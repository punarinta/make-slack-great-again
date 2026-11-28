// The macOS transport (transport.h) on NSURLSession: data tasks for HTTP,
// NSURLSessionWebSocketTask (10.15+) for WebSockets. TLS, proxies, HTTP/2
// and connection reuse are the OS's.
//
// What NSURLSession would otherwise do on its own and the common layer
// does instead: redirects (the delegate answers nil, so the 3xx comes back
// as the response) and cookies (no cookie storage, no Cookie header added).
// It does add Accept-Encoding and decode the body itself; we let it (less
// on the wire) and drop the Content-Encoding / Content-Length headers that
// no longer describe the body we return.
//
// Every wait is a semaphore that whoever ends the wait signals: the
// completion block, or the request's Cancel (it is that very semaphore), or
// a WebSocket's abort. Only download progress wakes it in 250 ms slices.
// Completion blocks only store into __block storage and signal; the blocks
// keep that storage alive, so giving up early (cancel → [task cancel])
// never leaves a block writing into a dead frame.
#import <Foundation/Foundation.h>
#import <Security/Security.h>

#include "base/str.h"
#include "base/time.h"
#include "net/transport.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <mutex>

@interface MsgaNoRedirect : NSObject <NSURLSessionTaskDelegate>
@end

@implementation MsgaNoRedirect
- (void)URLSession:(NSURLSession *)session
                          task:(NSURLSessionTask *)task
    willPerformHTTPRedirection:(NSHTTPURLResponse *)response
                    newRequest:(NSURLRequest *)request
             completionHandler:(void (^)(NSURLRequest *))completionHandler {
    completionHandler(nil); // hand the 3xx to the common layer (client.cpp)
}
@end

// One WebSocket task's delegate: the open/close events land here.
@interface MsgaWsDelegate : NSObject <NSURLSessionWebSocketDelegate> {
@public
    dispatch_semaphore_t opened; // didOpen or didComplete (failed handshake)
    dispatch_semaphore_t ended;  // didClose or didComplete
    BOOL                 isOpen;
    NSInteger            closeCode;
    NSData              *closeReason;
    NSError             *error;
}
@end

@implementation MsgaWsDelegate
- (instancetype)init {
    if ((self = [super init])) {
        opened = dispatch_semaphore_create(0);
        ended  = dispatch_semaphore_create(0);
    }
    return self;
}
- (void)URLSession:(NSURLSession *)session
          webSocketTask:(NSURLSessionWebSocketTask *)task
    didOpenWithProtocol:(NSString *)protocol {
    isOpen = YES;
    dispatch_semaphore_signal(opened);
}
- (void)URLSession:(NSURLSession *)session
       webSocketTask:(NSURLSessionWebSocketTask *)task
    didCloseWithCode:(NSURLSessionWebSocketCloseCode)code
              reason:(NSData *)reason {
    closeCode   = code;
    closeReason = reason;
    dispatch_semaphore_signal(ended);
}
- (void)URLSession:(NSURLSession *)session
                    task:(NSURLSessionTask *)task
    didCompleteWithError:(NSError *)err {
    error = err;
    dispatch_semaphore_signal(opened);
    dispatch_semaphore_signal(ended);
}
@end

namespace net::detail {

namespace {

constexpr int64_t kSliceMs = 250;

NSString *ns(std::string_view s) {
    return [[NSString alloc] initWithBytes:s.data() length:s.size() encoding:NSUTF8StringEncoding];
}

std::string utf8(NSString *s) {
    const char *c = s.UTF8String;
    return c ? std::string(c) : std::string();
}

NSURLSessionConfiguration *configuration() {
    NSURLSessionConfiguration *c = [NSURLSessionConfiguration ephemeralSessionConfiguration];
    c.HTTPShouldSetCookies       = NO;
    c.HTTPCookieAcceptPolicy     = NSHTTPCookieAcceptPolicyNever;
    c.HTTPCookieStorage          = nil;
    c.URLCache                   = nil;
    c.requestCachePolicy         = NSURLRequestReloadIgnoringLocalCacheData;
    c.TLSMinimumSupportedProtocolVersion = tls_protocol_version_TLSv12;
    c.HTTPAdditionalHeaders              = @{@"User-Agent" : @(kUserAgent)};
    return c;
}

// One session for every request (NSURLSession is thread-safe): it pools
// connections across them. Its delegate runs on the session's own serial
// queue. Never invalidated.
NSURLSession *session() {
    static NSURLSession *s = [NSURLSession sessionWithConfiguration:configuration()
                                                           delegate:[MsgaNoRedirect new]
                                                      delegateQueue:nil];
    return s;
}

// The names the posix transport gives the same faults. NSURLSession says
// only "untrusted" for both a foreign name and an unknown root; the trust
// object it attaches tells them apart.
std::string certificateFault(NSError *e) {
    SecTrustRef trust = (__bridge SecTrustRef)e.userInfo[NSURLErrorFailingURLPeerTrustErrorKey];
    CFErrorRef  err   = nullptr;
    if (trust && !SecTrustEvaluateWithError(trust, &err) && err) {
        const CFIndex code = CFErrorGetCode(err);
        CFRelease(err);
        if (code == errSecHostNameMismatch)
            return "tls: hostname mismatch";
        if (code == errSecCertificateExpired || code == errSecCertificateNotValidYet)
            return "tls: certificate expired";
    }
    return "tls: untrusted certificate";
}

std::string reason(NSError *e) {
    const char *why = "protocol";
    if ([e.domain isEqualToString:NSURLErrorDomain]) {
        switch (e.code) {
        case NSURLErrorCancelled:
            return "cancelled";
        case NSURLErrorTimedOut:
            return "timeout";
        case NSURLErrorCannotFindHost:
        case NSURLErrorDNSLookupFailed:
            why = "dns";
            break;
        case NSURLErrorCannotConnectToHost:
        case NSURLErrorNetworkConnectionLost:
        case NSURLErrorNotConnectedToInternet:
        case NSURLErrorInternationalRoamingOff:
        case NSURLErrorDataNotAllowed:
            why = "connect";
            break;
        case NSURLErrorServerCertificateHasBadDate:
        case NSURLErrorServerCertificateNotYetValid:
            return "tls: certificate expired";
        case NSURLErrorServerCertificateUntrusted:
        case NSURLErrorServerCertificateHasUnknownRoot:
            return certificateFault(e);
        case NSURLErrorSecureConnectionFailed:
        case NSURLErrorClientCertificateRejected:
        case NSURLErrorClientCertificateRequired:
            why = "tls";
            break;
        case NSURLErrorBadURL:
        case NSURLErrorUnsupportedURL:
            why = "url";
            break;
        case NSURLErrorAppTransportSecurityRequiresSecureConnection:
            why = "unsupported";
            break;
        }
    }
    return str::concat({why, ": ", utf8(e.domain), " ", str::number(int64_t(e.code))});
}

int64_t deadlineAfter(int timeoutMs) {
    return timeoutMs > 0 ? base::monotonicMs() + timeoutMs : INT64_MAX;
}

enum class Wait { Done, Cancelled, TimedOut };

// Whoever sets `cancel` must also signal `sem`. `tick`, when set, runs
// every kSliceMs while the wait goes on.
Wait waitFor(
    dispatch_semaphore_t         sem,
    int64_t                      deadline,
    const std::atomic<bool>     &cancel,
    const std::function<void()> &tick = {}
) {
    for (;;) {
        if (cancel.load())
            return Wait::Cancelled;
        const int64_t left = deadline - base::monotonicMs();
        if (left <= 0)
            return Wait::TimedOut;
        const int64_t         slice = tick && left > kSliceMs ? kSliceMs : left;
        const dispatch_time_t until = !tick && deadline == INT64_MAX
                                          ? DISPATCH_TIME_FOREVER
                                          : dispatch_time(DISPATCH_TIME_NOW, slice * NSEC_PER_MSEC);
        if (dispatch_semaphore_wait(sem, until) == 0)
            return cancel.load() ? Wait::Cancelled : Wait::Done;
        if (tick)
            tick();
    }
}

NSMutableURLRequest *
makeRequest(const Url &url, const std::vector<Header> &headers, int timeoutMs) {
    NSURL *u = [NSURL URLWithString:ns(url.str())];
    if (!u)
        return nil;
    // timeoutInterval is NSURLSession's idle timeout; our deadline bounds
    // the whole exchange, so it only must not fire earlier.
    NSMutableURLRequest *r =
        [NSMutableURLRequest requestWithURL:u
                                cachePolicy:NSURLRequestReloadIgnoringLocalCacheData
                            timeoutInterval:timeoutMs > 0 ? timeoutMs / 1000.0 : 365 * 86400.0];
    for (const auto &h : headers) {
        NSString *name = ns(h.name), *value = ns(h.value);
        if (name && value)
            [r addValue:value forHTTPHeaderField:name];
    }
    return r;
}

// NSHTTPCookie back into a Set-Cookie line, with what client.cpp's jar reads:
// the Domain attribute (a leading dot means one was given) and expiry.
std::string setCookieLine(NSHTTPCookie *c) {
    std::string line = str::concat({utf8(c.name), "=", utf8(c.value)});
    if ([c.domain hasPrefix:@"."])
        line += str::concat({"; Domain=", utf8([c.domain substringFromIndex:1])});
    if (c.path.length)
        line += str::concat({"; Path=", utf8(c.path)});
    if (c.expiresDate) {
        const double left = c.expiresDate.timeIntervalSinceNow;
        line += str::concat({"; Max-Age=", str::number(left > 0 ? int64_t(left) : 0)});
    }
    if (c.secure)
        line += "; Secure";
    if (c.HTTPOnly)
        line += "; HttpOnly";
    return line;
}

// Headers as a flat list. The OS keeps them in a dictionary (arrival order
// is lost) and joins repeated Set-Cookie lines with ", " — which commas in
// Expires dates make ambiguous — so cookies are re-split by the OS's own
// parser.
void readHeaders(NSHTTPURLResponse *http, std::vector<Header> &out) {
    NSDictionary<NSString *, NSString *> *fields = http.allHeaderFields;
    const bool decoded = [http valueForHTTPHeaderField:@"Content-Encoding"] != nil;
    for (NSString *key in fields) {
        NSString *lower = key.lowercaseString;
        if ([lower isEqualToString:@"set-cookie"])
            continue;
        if (decoded && ([lower isEqualToString:@"content-encoding"] ||
                        [lower isEqualToString:@"content-length"]))
            continue;
        out.push_back({utf8(key), utf8(fields[key])});
    }
    for (NSHTTPCookie *c in [NSHTTPCookie cookiesWithResponseHeaderFields:fields forURL:http.URL])
        out.push_back({"Set-Cookie", setCookieLine(c)});
}

class MacWsConn final : public WsConn {
public:
    ~MacWsConn() override {
        std::lock_guard<std::mutex> lock(_mutex);
        [_task cancel];
        [_session invalidateAndCancel]; // also releases the delegate
        _task    = nil;
        _session = nil;
    }

    bool connect(
        const Url &url, const std::vector<Header> &headers, int timeoutMs, std::string *error
    ) override {
        @autoreleasepool {
            std::string e = start(url, headers, timeoutMs);
            if (!e.empty() && error)
                *error = e;
            return e.empty();
        }
    }

    bool recv(WsMessage &msg) override {
        @autoreleasepool {
            msg = WsMessage();
            NSURLSessionWebSocketTask *task;
            {
                std::lock_guard<std::mutex> lock(_mutex);
                task = _task;
            }
            if (!task || _aborted.load())
                return closed(msg);
            __block NSURLSessionWebSocketMessage *got = nil;
            __block NSError                      *err = nil;
            dispatch_semaphore_t                  sem = _recvSem;
            [task
                receiveMessageWithCompletionHandler:^(NSURLSessionWebSocketMessage *m, NSError *e) {
                  got = m;
                  err = e;
                  dispatch_semaphore_signal(sem);
                }];
            dispatch_semaphore_wait(sem, DISPATCH_TIME_FOREVER); // abort() signals it too
            if (_aborted.load() || !got)
                return closed(msg);
            if (got.type == NSURLSessionWebSocketMessageTypeString) {
                msg.kind = WsMessage::Text;
                msg.data = utf8(got.string);
            } else {
                msg.kind = WsMessage::Binary;
                msg.data.assign(static_cast<const char *>(got.data.bytes), got.data.length);
            }
            return true;
        }
    }

    bool send(std::string_view data, bool text) override {
        @autoreleasepool {
            NSURLSessionWebSocketMessage *m;
            if (text) {
                NSString *s = ns(data);
                if (!s)
                    return false; // not UTF-8: no valid text frame for it
                m = [[NSURLSessionWebSocketMessage alloc] initWithString:s];
            } else {
                m = [[NSURLSessionWebSocketMessage alloc]
                    initWithData:[NSData dataWithBytes:data.data() length:data.size()]];
            }
            std::lock_guard<std::mutex> lock(_mutex);
            if (!_task || _closeCode)
                return false;
            [_task sendMessage:m
                completionHandler:^(NSError *){
                    // A failed send means a broken socket; the receive ends it.
                }];
            return true;
        }
    }

    bool ping(std::string_view) override {
        // NSURLSession picks the payload itself. Slack never answers client
        // pings: the handler then runs once, with an error, when the task ends.
        std::lock_guard<std::mutex> lock(_mutex);
        if (!_task || _closeCode)
            return false;
        [_task sendPingWithPongReceiveHandler:^(NSError *){
        }];
        return true;
    }

    void close(int code) override {
        std::lock_guard<std::mutex> lock(_mutex);
        if (!_task || _closeCode)
            return;
        _closeCode = code > 0 ? code : 1000;
        [_task cancelWithCloseCode:NSURLSessionWebSocketCloseCode(_closeCode) reason:nil];
    }

    void abort() override {
        _aborted.store(true);
        {
            std::lock_guard<std::mutex> lock(_mutex);
            [_task cancel];
            if (_delegate) // a connect waiting for the handshake
                dispatch_semaphore_signal(_delegate->opened);
        }
        dispatch_semaphore_signal(_recvSem);
    }

private:
    std::string start(const Url &url, const std::vector<Header> &headers, int timeoutMs) {
        const int64_t        deadline = deadlineAfter(timeoutMs);
        NSMutableURLRequest *r        = makeRequest(url, headers, timeoutMs);
        if (!r)
            return "url";
        MsgaWsDelegate *d = [MsgaWsDelegate new];
        {
            std::lock_guard<std::mutex> lock(_mutex);
            if (_aborted.load())
                return "cancelled";
            _delegate                = d;
            _session                 = [NSURLSession sessionWithConfiguration:configuration()
                                                                     delegate:d
                                                                delegateQueue:nil];
            _task                    = [_session webSocketTaskWithRequest:r];
            // Slack's larger events go past the 1 MB default.
            _task.maximumMessageSize = 64 * 1024 * 1024;
            [_task resume];
        }
        switch (waitFor(d->opened, deadline, _aborted)) {
        case Wait::Done:
            break;
        case Wait::Cancelled:
            return "cancelled";
        case Wait::TimedOut:
            abort();
            return "timeout";
        }
        if (d->isOpen)
            return {};
        // The handshake failed: an HTTP answer other than 101, or no answer.
        NSURLResponse *resp = _task.response;
        if ([resp isKindOfClass:[NSHTTPURLResponse class]]) {
            const NSInteger status = ((NSHTTPURLResponse *)resp).statusCode;
            if (status && status != 101)
                return str::concat({"handshake: status ", str::number(int64_t(status))});
        }
        if (!d->error)
            return "handshake";
        const std::string why = reason(d->error);
        return str::startsWith(why, "protocol") ? str::concat({"handshake: ", why}) : why;
    }

    // The end: the peer's close code and reason once the OS has them.
    bool closed(WsMessage &msg) {
        msg      = WsMessage();
        msg.kind = WsMessage::Closed;
        msg.code = 1006;
        if (_aborted.load() || !_delegate)
            return false;
        // The failed receive can come before the delegate hears the close.
        dispatch_semaphore_wait(_delegate->ended, dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC));
        if (_delegate->closeCode > 0) {
            msg.code = int(_delegate->closeCode);
            if (NSData *r = _delegate->closeReason)
                msg.data.assign(static_cast<const char *>(r.bytes), r.length);
        } else if (_closeCode) {
            msg.code = _closeCode; // we closed and the OS did not report the echo
        }
        return false;
    }

    std::mutex                 _mutex; // guards _task/_session/_closeCode against send/close/abort
    MsgaWsDelegate            *_delegate  = nil;
    NSURLSession              *_session   = nil;
    NSURLSessionWebSocketTask *_task      = nil;
    dispatch_semaphore_t       _recvSem   = dispatch_semaphore_create(0);
    int                        _closeCode = 0;
    std::atomic<bool>          _aborted{false};
};

} // namespace

// The semaphore an exchange waits on: its completion block signals it, and
// so does set(). A stale signal (set() while nothing waits) only makes the
// next wait look at the flag, which is then set.
Cancel::Cancel() : _os(intptr_t((__bridge_retained void *)dispatch_semaphore_create(0))) {}

Cancel::~Cancel() {
    dispatch_semaphore_t sem = (__bridge_transfer dispatch_semaphore_t)(void *)_os;
    (void)sem;
}

void Cancel::set() {
    _flag.store(true, std::memory_order_release);
    dispatch_semaphore_signal((__bridge dispatch_semaphore_t)(void *)_os);
}

void perform(
    const Url &url, Request &req, Response &resp, const Cancel &cancel, const Progress &progress
) {
    @autoreleasepool {
        const int64_t        deadline = deadlineAfter(req.timeoutMs);
        NSMutableURLRequest *r        = makeRequest(url, req.headers, req.timeoutMs);
        if (!r) {
            resp.error = "url";
            return;
        }
        r.HTTPMethod = ns(req.method);

        // The body moves into the NSData instead of being copied (an upload
        // can be tens of megabytes); it comes back once the server answered
        // (a 307 sends it again). The NSData frees what is left of it.
        std::string *held = nullptr;
        if (!req.body.empty()) {
            held       = new std::string(std::move(req.body));
            r.HTTPBody = [[NSData alloc] initWithBytesNoCopy:held->data()
                                                      length:held->size()
                                                 deallocator:^(void *, NSUInteger) {
                                                   delete held;
                                                 }];
        }
        __block NSData        *data     = nil;
        __block NSURLResponse *response = nil;
        __block NSError       *error    = nil;
        dispatch_semaphore_t   sem      = (__bridge dispatch_semaphore_t)(void *)cancel.os();
        NSURLSessionDataTask  *task =
            [session() dataTaskWithRequest:r
                         completionHandler:^(NSData *d, NSURLResponse *rr, NSError *e) {
                           data     = d;
                           response = rr;
                           error    = e;
                           dispatch_semaphore_signal(sem);
                         }];
        [task resume];
        // The task's counters (thread-safe properties) between the waits'
        // slices: the body only arrives whole, in the completion block.
        std::function<void()> tick;
        if (progress)
            tick = [task, &progress] {
                const int64_t got = task.countOfBytesReceived;
                if (got > 0) {
                    const int64_t total = task.countOfBytesExpectedToReceive;
                    progress(got, total > 0 ? total : 0);
                }
            };
        switch (waitFor(sem, deadline, cancel.flag(), tick)) {
        case Wait::Done:
            if (held) // the finished task reads it no more
                req.body = std::move(*held);
            break;
        case Wait::Cancelled:
            [task cancel];
            resp.error = "cancelled";
            return;
        case Wait::TimedOut:
            [task cancel];
            resp.error = "timeout";
            return;
        }
        if (error || ![response isKindOfClass:[NSHTTPURLResponse class]]) {
            resp.error = error ? reason(error) : "protocol";
            return;
        }
        NSHTTPURLResponse *http = (NSHTTPURLResponse *)response;
        resp.status             = int(http.statusCode);
        readHeaders(http, resp.headers);
        // A streamed body still arrives whole here (a data task with a
        // completion block): handed over from the NSData in pieces, never
        // copied into resp.body.
        if (Handler *sink = bodySink(req, resp.status)) {
            const char *p = static_cast<const char *>(data.bytes);
            for (NSUInteger at = 0; at < data.length;) {
                const size_t n = size_t(std::min<NSUInteger>(data.length - at, 256 * 1024));
                if (!sink->body(p + at, n)) {
                    resp       = Response();
                    resp.error = "sink";
                    return;
                }
                at += n;
            }
        } else if (data.length) {
            resp.body.assign(static_cast<const char *>(data.bytes), data.length);
        }
        if (progress && data.length) {
            // The size the server announced, as in the ticks: 0 when it
            // didn't say (chunked), whatever arrived.
            const int64_t total = task.countOfBytesExpectedToReceive;
            progress(int64_t(data.length), total > 0 ? total : 0);
        }
    }
}

// The next request opens a fresh connection; in-flight ones are untouched.
void closeIdleConnections() {
    [session() flushWithCompletionHandler:^{
    }];
}

void releaseCaches() {
    closeIdleConnections();
}

std::unique_ptr<WsConn> makeWsConn() {
    return std::make_unique<MacWsConn>();
}

} // namespace net::detail
