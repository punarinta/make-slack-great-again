// TLS for the POSIX transport: mbedTLS client sessions over our non-blocking
// sockets, verified against the system's CA bundle.
//
// The last session per host:port (a TLS 1.3 ticket, or a TLS 1.2 session id
// or ticket) is kept and offered on the next connection there. When the
// server takes it, the handshake skips the certificate chain and its
// signature checks: a reconnect after the keep-alive pool let go (30 s) or a
// recycled WebSocket costs one key exchange instead of a full handshake.
//
// The CA store is kept as DER only. Parsing every root up front (what
// mbedtls_x509_crt_parse_file does) costs ~2.5× the DER in RAM for ~150
// certificates of which a session touches one; instead mbedTLS asks us for
// the issuer of the top certificate it was sent (the trusted-CA callback)
// and we parse just the candidates whose subject matches, in place.
#include "base/crypto.h"
#include "base/file.h"
#include "base/str.h"
#include "net/posix/posix.h"

#include <mbedtls/asn1.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>
#include <psa/crypto.h>
extern "C" { // psa_util.h lacks its own C++ guard in 3.6
#include <mbedtls/psa_util.h>
}

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <mutex>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

// MBEDTLS_PSA_CRYPTO_EXTERNAL_RNG: PSA (and through it TLS) draws all its
// randomness from the kernel — no entropy pool, no DRBG in the binary.
extern "C" psa_status_t mbedtls_psa_external_get_random(
    mbedtls_psa_external_random_context_t *, uint8_t *out, size_t size, size_t *outLen
) {
    if (!crypto::randomBytes(out, size))
        return PSA_ERROR_INSUFFICIENT_ENTROPY;
    *outLen = size;
    return PSA_SUCCESS;
}

namespace net::detail {

namespace {

struct CaStore {
    std::string der; // every certificate, DER, back to back
    struct Entry {
        uint32_t off, len, subjectOff, subjectLen;
    };
    std::vector<Entry> entries;

    std::string_view cert(const Entry &e) const { return {der.data() + e.off, e.len}; }
    std::string_view subject(const Entry &e) const {
        return {der.data() + e.subjectOff, e.subjectLen};
    }
    void add(std::string_view cert);
    void addPem(std::string_view pem);
};

// The DER of a certificate's subject Name (tag and length included, as
// mbedTLS's issuer_raw), found by walking the TBSCertificate's fields.
bool subjectOf(std::string_view cert, size_t *off, size_t *len) {
    auto          *start = reinterpret_cast<unsigned char *>(const_cast<char *>(cert.data()));
    unsigned char *p = start, *end = start + cert.size();
    size_t         n;
    const int      seq = MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SEQUENCE;
    if (mbedtls_asn1_get_tag(&p, end, &n, seq) || mbedtls_asn1_get_tag(&p, end, &n, seq))
        return false;
    end = p + n; // the TBSCertificate
    if (p < end && *p == (MBEDTLS_ASN1_CONTEXT_SPECIFIC | MBEDTLS_ASN1_CONSTRUCTED)) {
        if (mbedtls_asn1_get_tag(&p, end, &n, *p))
            return false;
        p += n; // [0] version
    }
    if (mbedtls_asn1_get_tag(&p, end, &n, MBEDTLS_ASN1_INTEGER))
        return false;
    p += n;                                // serial
    for (int skip = 0; skip < 3; ++skip) { // signature algorithm, issuer, validity
        if (mbedtls_asn1_get_tag(&p, end, &n, seq))
            return false;
        p += n;
    }
    unsigned char *subject = p;
    if (mbedtls_asn1_get_tag(&p, end, &n, seq))
        return false;
    *off = size_t(subject - start);
    *len = size_t(p + n - subject);
    return true;
}

void CaStore::add(std::string_view cert) {
    size_t so, sl;
    if (cert.size() > 64 * 1024 || !subjectOf(cert, &so, &sl))
        return;
    for (const auto &e : entries) // /etc/ssl/certs holds each root under several names
        if (e.len == cert.size() && this->cert(e) == cert)
            return;
    const auto off = uint32_t(der.size());
    der.append(cert);
    entries.push_back({off, uint32_t(cert.size()), uint32_t(off + so), uint32_t(sl)});
}

void CaStore::addPem(std::string_view pem) {
    static constexpr std::string_view kBegin = "-----BEGIN CERTIFICATE-----";
    static constexpr std::string_view kEnd   = "-----END CERTIFICATE-----";
    std::string                       der;
    for (size_t at = pem.find(kBegin); at != std::string_view::npos; at = pem.find(kBegin, at)) {
        at += kBegin.size();
        const size_t stop = pem.find(kEnd, at);
        if (stop == std::string_view::npos)
            break;
        der.clear();
        if (crypto::base64Decode(pem.substr(at, stop - at), &der))
            add(der);
        at = stop + kEnd.size();
    }
}

// A certificate file (a regular one): a bundle is a few hundred KB, so
// anything past 16 MB is not one.
bool readFile(std::string_view path, std::string *out) {
    return file::size(path) <= 16 * 1024 * 1024 && file::readAll(path, out);
}

// $SSL_CERT_FILE, then where the distributions put their bundle, then every
// *.pem / *.crt of a certificate directory ($SSL_CERT_DIR or /etc/ssl/certs,
// then FreeBSD 14's base trust store, which /etc/ssl/certs only links to by
// hash names).
void loadSystemCas(CaStore *store) {
    std::string text;
    const char *env = std::getenv("SSL_CERT_FILE");
    if (env && *env && readFile(env, &text))
        store->addPem(text);
    static const char *const kBundles[] = {
        "/etc/ssl/certs/ca-certificates.crt",     // Debian, Ubuntu, Arch, Gentoo, Alpine
        "/etc/pki/tls/certs/ca-bundle.crt",       // Fedora, RHEL
        "/etc/ssl/ca-bundle.pem",                 // openSUSE
        "/etc/pki/tls/cacert.pem",                // OpenELEC
        "/etc/ssl/cert.pem",                      // Alpine, OpenBSD, FreeBSD 15
        "/usr/local/share/certs/ca-root-nss.crt", // FreeBSD's ca_root_nss package
    };
    for (const char *path : kBundles) {
        if (!store->entries.empty())
            return;
        if (readFile(path, &text))
            store->addPem(text);
    }
    if (!store->entries.empty())
        return;
    const char *dirEnv = std::getenv("SSL_CERT_DIR");
    for (const char *dir :
         {dirEnv && *dirEnv ? dirEnv : "/etc/ssl/certs", "/usr/share/certs/trusted"}) {
        if (!store->entries.empty())
            return;
        DIR *d = opendir(dir);
        if (!d)
            continue;
        while (const dirent *e = readdir(d)) {
            const std::string_view name = e->d_name;
            if (!str::endsWith(name, ".pem") && !str::endsWith(name, ".crt"))
                continue;
            if (readFile(str::concat({dir, "/", name}), &text))
                store->addPem(text);
        }
        closedir(d);
    }
}

struct Shared {
    mbedtls_ssl_config conf;
    CaStore            cas;
    bool               ok = false;
};

// mbedTLS asks for the trusted certificates that may have issued `child`.
int caCallback(void *ctx, const mbedtls_x509_crt *child, mbedtls_x509_crt **out) {
    const auto            &store = static_cast<Shared *>(ctx)->cas;
    const std::string_view issuer(
        reinterpret_cast<const char *>(child->issuer_raw.p), child->issuer_raw.len
    );
    mbedtls_x509_crt *chain = nullptr;
    for (const auto &e : store.entries) {
        if (store.subject(e) != issuer)
            continue;
        if (!chain) {
            chain = static_cast<mbedtls_x509_crt *>(std::calloc(1, sizeof *chain));
            if (!chain)
                break;
            mbedtls_x509_crt_init(chain);
        }
        // No copy: the store lives as long as the process.
        const std::string_view der = store.cert(e);
        mbedtls_x509_crt_parse_der_nocopy(
            chain, reinterpret_cast<const unsigned char *>(der.data()), der.size()
        );
    }
    if (chain && !chain->raw.p) { // nothing parsed
        mbedtls_x509_crt_free(chain);
        std::free(chain);
        chain = nullptr;
    }
    *out = chain;
    return 0;
}

// Built on the first TLS connection, then shared read-only by every thread
// (an mbedtls_ssl_config may serve many contexts at once). Never destroyed:
// sessions may still be closing while static destructors run. releaseTls()
// frees it once no stream is left; the next connection builds it again.
struct SharedSlot {
    std::mutex mutex;
    Shared    *s = nullptr; // in mem
    alignas(Shared) unsigned char mem[sizeof(Shared)];
};

Shared *build(void *mem) {
    auto *sh = new (mem) Shared;
    mbedtls_ssl_config_init(&sh->conf);
    if (psa_crypto_init() != PSA_SUCCESS)
        return sh;
    loadSystemCas(&sh->cas);
    if (sh->cas.entries.empty())
        return sh;
    if (mbedtls_ssl_config_defaults(
            &sh->conf,
            MBEDTLS_SSL_IS_CLIENT,
            MBEDTLS_SSL_TRANSPORT_STREAM,
            MBEDTLS_SSL_PRESET_DEFAULT
        ) != 0)
        return sh;
    mbedtls_ssl_conf_authmode(&sh->conf, MBEDTLS_SSL_VERIFY_REQUIRED);
    mbedtls_ssl_conf_ca_cb(&sh->conf, caCallback, sh);
    mbedtls_ssl_conf_rng(&sh->conf, mbedtls_psa_get_random, MBEDTLS_PSA_RANDOM_STATE);
    // TLS 1.3 tickets come after the handshake; mbedTLS hands them over
    // only when asked to (tlsRead/tlsWrite then keep them).
    mbedtls_ssl_conf_tls13_enable_signal_new_session_tickets(
        &sh->conf, MBEDTLS_SSL_TLS1_3_SIGNAL_NEW_SESSION_TICKETS_ENABLED
    );
    sh->ok = true;
    return sh;
}

Shared *shared() {
    SharedSlot                 &slot = immortal<SharedSlot>();
    std::lock_guard<std::mutex> lock(slot.mutex);
    if (!slot.s)
        slot.s = build(slot.mem);
    return slot.s;
}

std::string hexCode(int code) {
    unsigned v = unsigned(code < 0 ? -code : code);
    char     buf[8];
    int      n = 0;
    do {
        buf[n++] = str::kHexLower[v & 15];
        v >>= 4;
    } while (v && n < 8);
    std::string out = code < 0 ? "-0x" : "0x";
    while (n)
        out += buf[--n];
    return out;
}

} // namespace

struct TlsConn {
    mbedtls_ssl_context ssl;
    int                 fd       = -1;
    int                 sysErr   = 0;     // errno of the last failed socket call
    size_t              inflight = 0;     // length of a write that returned WANT_*
    std::string         key;              // "host:port", for the session cache
    bool                offered  = false; // a cached session went into the ClientHello
    bool                verified = false; // a certificate chain was checked: a full handshake
};

namespace {

// ── Session cache ───────────────────────────────────────────────────────────

constexpr size_t kMaxSessions = 16; // hosts; the least recently stored goes first

struct SessionCache {
    struct Entry {
        std::string          key;
        mbedtls_ssl_session *session;
    };
    std::mutex         mutex;
    std::vector<Entry> entries; // the most recently stored last
};

SessionCache &sessions() {
    return immortal<SessionCache>();
}

std::atomic<int64_t> g_resumed{0};

void freeSession(mbedtls_ssl_session *s) {
    if (!s)
        return;
    mbedtls_ssl_session_free(s);
    delete s;
}

// After a TLS 1.2 handshake, or whenever a TLS 1.3 ticket arrived.
void keepSession(TlsConn *c) {
    auto *s = new mbedtls_ssl_session;
    mbedtls_ssl_session_init(s);
    if (mbedtls_ssl_get_session(&c->ssl, s) != 0) {
        freeSession(s);
        return;
    }
    mbedtls_ssl_session *drop[2] = {nullptr, nullptr}; // freed outside the lock
    {
        SessionCache               &sc = sessions();
        std::lock_guard<std::mutex> lock(sc.mutex);
        for (size_t i = 0; i < sc.entries.size(); ++i)
            if (sc.entries[i].key == c->key) {
                drop[0] = sc.entries[i].session;
                sc.entries.erase(sc.entries.begin() + ptrdiff_t(i));
                break;
            }
        if (sc.entries.size() >= kMaxSessions) {
            drop[1] = sc.entries.front().session;
            sc.entries.erase(sc.entries.begin());
        }
        sc.entries.push_back({c->key, s});
    }
    freeSession(drop[0]);
    freeSession(drop[1]);
}

// Puts the host's session (if any) into the ClientHello. Copied: the cache
// keeps it for the next connection too (servers accept a ticket more than
// once; one that doesn't simply runs a full handshake).
void offerSession(TlsConn *c) {
    SessionCache               &sc = sessions();
    std::lock_guard<std::mutex> lock(sc.mutex);
    for (const auto &e : sc.entries)
        if (e.key == c->key) {
            c->offered = mbedtls_ssl_set_session(&c->ssl, e.session) == 0;
            return;
        }
}

void forgetSession(const std::string &key) {
    mbedtls_ssl_session *drop = nullptr;
    {
        SessionCache               &sc = sessions();
        std::lock_guard<std::mutex> lock(sc.mutex);
        for (size_t i = 0; i < sc.entries.size(); ++i)
            if (sc.entries[i].key == key) {
                drop = sc.entries[i].session;
                sc.entries.erase(sc.entries.begin() + ptrdiff_t(i));
                break;
            }
    }
    freeSession(drop);
}

// Runs for every certificate of a chain being verified, which a resumed
// handshake never has.
int onVerify(void *ctx, mbedtls_x509_crt *, int, uint32_t *) {
    static_cast<TlsConn *>(ctx)->verified = true;
    return 0; // the flags stay as mbedTLS found them
}

// mbedTLS's I/O callbacks: a byte count, WANT_* when it would block.
int bioResult(long r, int want) {
    return r >= 0 ? int(r) : r == Stream::Again ? want : MBEDTLS_ERR_SSL_INTERNAL_ERROR;
}

int bioSend(void *ctx, const unsigned char *buf, size_t len) {
    auto *c = static_cast<TlsConn *>(ctx);
    return bioResult(sockSend(c->fd, buf, len, &c->sysErr), MBEDTLS_ERR_SSL_WANT_WRITE);
}

int bioRecv(void *ctx, unsigned char *buf, size_t len) {
    auto *c = static_cast<TlsConn *>(ctx);
    return bioResult(sockRecv(c->fd, buf, len, &c->sysErr), MBEDTLS_ERR_SSL_WANT_READ);
}

// Our short reasons for mbedTLS codes (MBEDTLS_ERROR_C, its string table, is
// not built).
std::string describe(TlsConn *c, int rc) {
    if (c->sysErr)
        return str::concat({"connect: ", std::strerror(c->sysErr)});
    if (rc == MBEDTLS_ERR_X509_CERT_VERIFY_FAILED) {
        const uint32_t flags = mbedtls_ssl_get_verify_result(&c->ssl);
        if (flags & MBEDTLS_X509_BADCERT_CN_MISMATCH)
            return "tls: hostname mismatch";
        if (flags & (MBEDTLS_X509_BADCERT_EXPIRED | MBEDTLS_X509_BADCERT_FUTURE))
            return "tls: certificate expired";
        if (flags & MBEDTLS_X509_BADCERT_NOT_TRUSTED)
            return "tls: untrusted certificate";
        return str::concat({"tls: bad certificate ", hexCode(int(flags))});
    }
    if (rc == MBEDTLS_ERR_SSL_CONN_EOF)
        return "tls: connection closed";
    if (rc == MBEDTLS_ERR_SSL_FATAL_ALERT_MESSAGE)
        return str::concat(
            {"tls: alert from server ", str::number(c->ssl.MBEDTLS_PRIVATE(in_msg)[1])}
        );
    if (rc == MBEDTLS_ERR_SSL_HANDSHAKE_FAILURE || rc == MBEDTLS_ERR_SSL_BAD_PROTOCOL_VERSION)
        return "tls: no common parameters";
    return str::concat({"tls: ", hexCode(rc)});
}

long result(TlsConn *c, int rc, short *want, std::string *error) {
    if (rc == MBEDTLS_ERR_SSL_WANT_READ) {
        *want = POLLIN;
        return Stream::Again;
    }
    if (rc == MBEDTLS_ERR_SSL_WANT_WRITE) {
        *want = POLLOUT;
        return Stream::Again;
    }
    *error = describe(c, rc);
    return Stream::Fail;
}

} // namespace

int64_t tlsResumptions() {
    return g_resumed.load(std::memory_order_relaxed);
}

void releaseTls() {
    std::vector<SessionCache::Entry> old;
    {
        SessionCache               &sc = sessions();
        std::lock_guard<std::mutex> lock(sc.mutex);
        old.swap(sc.entries);
    }
    for (const auto &e : old)
        freeSession(e.session);
    SharedSlot                 &slot = immortal<SharedSlot>();
    std::lock_guard<std::mutex> lock(slot.mutex);
    if (!slot.s)
        return;
    mbedtls_ssl_config_free(&slot.s->conf);
    slot.s->~Shared();
    slot.s = nullptr;
    mbedtls_psa_crypto_free();
}

TlsConn *tlsNew(int fd, const Url &url, std::string *error) {
    Shared *sh = shared();
    if (!sh->ok) {
        *error = sh->cas.entries.empty() ? "tls: no CA certificates" : "tls: setup failed";
        return nullptr;
    }
    auto *c = new TlsConn;
    c->fd   = fd;
    c->key  = str::concat({url.host, ":", str::number(url.port)});
    mbedtls_ssl_init(&c->ssl);
    // set_hostname turns on both SNI and the certificate name check.
    if (mbedtls_ssl_setup(&c->ssl, &sh->conf) != 0 ||
        mbedtls_ssl_set_hostname(&c->ssl, url.host.c_str()) != 0) {
        mbedtls_ssl_free(&c->ssl);
        delete c;
        *error = "tls: setup failed";
        return nullptr;
    }
    mbedtls_ssl_set_bio(&c->ssl, c, bioSend, bioRecv, nullptr);
    mbedtls_ssl_set_verify(&c->ssl, onVerify, c);
    offerSession(c);
    return c;
}

void tlsFree(TlsConn *c) {
    if (mbedtls_ssl_is_handshake_over(&c->ssl))
        mbedtls_ssl_close_notify(&c->ssl); // best effort, never waits
    mbedtls_ssl_free(&c->ssl);
    delete c;
}

long tlsHandshake(TlsConn *c, short *want, std::string *error) {
    int rc;
    while ((rc = mbedtls_ssl_handshake(&c->ssl)) == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET)
        keepSession(c);
    if (rc == 0) {
        if (c->offered && !c->verified)
            g_resumed.fetch_add(1, std::memory_order_relaxed);
        // TLS 1.2 sessions are whole now; TLS 1.3 tickets arrive later.
        if (mbedtls_ssl_get_version_number(&c->ssl) == MBEDTLS_SSL_VERSION_TLS1_2)
            keepSession(c);
        return 0;
    }
    const long r = result(c, rc, want, error);
    if (r == Stream::Fail && c->offered)
        forgetSession(c->key); // in case the server chokes on it: not again
    return r;
}

long tlsRead(TlsConn *c, char *buf, size_t n, short *want, std::string *error) {
    int rc;
    while ((rc = mbedtls_ssl_read(&c->ssl, reinterpret_cast<unsigned char *>(buf), n)) ==
           MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET)
        keepSession(c);
    if (rc >= 0)
        return rc;
    // Most servers skip close_notify; a bare TCP close is the end too.
    if (rc == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY || rc == MBEDTLS_ERR_SSL_CONN_EOF)
        return 0;
    return result(c, rc, want, error);
}

long tlsWrite(TlsConn *c, const char *buf, size_t n, short *want, std::string *error) {
    // mbedtls_ssl_write must be repeated with the same length after WANT_*.
    if (c->inflight && c->inflight <= n)
        n = c->inflight;
    int rc;
    while ((rc = mbedtls_ssl_write(&c->ssl, reinterpret_cast<const unsigned char *>(buf), n)) ==
           MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET)
        keepSession(c);
    if (rc >= 0) {
        c->inflight = 0;
        return rc;
    }
    if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE)
        c->inflight = n;
    return result(c, rc, want, error);
}

bool tlsPending(const TlsConn *c) {
    return mbedtls_ssl_check_pending(&c->ssl) != 0;
}

} // namespace net::detail
