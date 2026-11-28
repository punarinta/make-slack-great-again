// Cocoa app services beyond windows and input: monitors, single instance and
// URL delivery (Apple Events), network reachability (Network.framework),
// sleep/wake, file panels, standard directories and
// accessibility/appearance settings.
//
// Everything reaches the loop the way the rest of the backend does: AppKit
// notifications and Apple Events are dispatched from the main run loop
// (inside pump()'s -nextEventMatchingMask:), fd watches are CFFileDescriptor
// sources, and the one thing on its own queue — the path monitor — posts.
#include "cocoa/cocoa_internal.h"

#include "core/hash.h"
#include "core/wire.h"

#import <CoreServices/CoreServices.h> // AESendMessage, kInternetEventClass/kAEGetURL
#import <Network/Network.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

using plat::EventType;
using plat::cocoa::CocoaApp;

// One target for every notification and Apple Event this file listens to.
@interface                                          PlatServicesObserver : NSObject
@property(nonatomic, assign) plat::cocoa::CocoaApp *owner;
@end

@implementation PlatServicesObserver
- (void)getUrl:(NSAppleEventDescriptor *)event withReply:(NSAppleEventDescriptor *)reply {
    NSString *url = [event paramDescriptorForKeyword:keyDirectObject].stringValue;
    if (_owner && url.length)
        _owner->onUrls({url.UTF8String});
}
- (void)screensChanged:(NSNotification *)n {
    if (!_owner)
        return;
    _owner->emitEvent({.type = EventType::MonitorsChanged});
    _owner->noteWork();
}
- (void)willSleep:(NSNotification *)n {
    if (_owner)
        _owner->onSystemEvent(EventType::Suspending);
}
- (void)didWake:(NSNotification *)n {
    if (_owner)
        _owner->onSystemEvent(EventType::Resumed);
}
- (void)settingsChanged:(NSNotification *)n {
    // Reduce motion / increase contrast / accent colour: systemSettings()
    // answers differently now, which ThemeChanged tells every window.
    if (_owner)
        _owner->onThemeChanged();
}
@end

namespace plat::cocoa {

namespace {

CGFloat primaryHeight() {
    return NSScreen.screens.firstObject.frame.size.height;
}

// ── single-instance wire format ────────────────────────────────────────────
// "PLSI", version byte, u32 payload length, then the payload: u32 count and
// that many u32-length-prefixed UTF-8 strings (working directory first, then
// the arguments). Little-endian. The primary answers one byte (kAck) once
// it has taken the message, so the second instance knows it can exit.
constexpr char     kMagic[4]   = {'P', 'L', 'S', 'I'};
constexpr uint8_t  kVersion    = 1;
constexpr size_t   kHeader     = 9;
constexpr uint32_t kMaxPayload = 1u << 20;
constexpr char     kAck        = 0x06;

std::string encodeMessage(const std::vector<std::string> &strings) {
    std::string payload;
    core::putU32(payload, uint32_t(strings.size()));
    for (const auto &s : strings)
        core::putString(payload, s);
    std::string out(kMagic, 4);
    out += char(kVersion);
    core::putU32(out, uint32_t(payload.size()));
    return out + payload;
}

// False when malformed; *complete false while more bytes are needed.
bool decodeMessage(const std::string &buf, bool *complete, std::vector<std::string> *strings) {
    *complete = false;
    if (buf.size() < kHeader)
        return buf.compare(0, buf.size(), kMagic, std::min<size_t>(buf.size(), 4)) == 0;
    if (buf.compare(0, 4, kMagic, 4) != 0 || uint8_t(buf[4]) != kVersion)
        return false;
    const uint32_t len = core::getU32(buf.data() + 5);
    if (len > kMaxPayload || len < 4)
        return false;
    if (buf.size() < kHeader + len)
        return true;
    std::string_view p(buf.data() + kHeader, len);
    const uint32_t   n = core::getU32(p.data());
    p.remove_prefix(4);
    strings->clear();
    for (uint32_t i = 0; i < n; ++i) {
        std::string s;
        if (!core::takeString(p, &s))
            return false;
        strings->push_back(std::move(s));
    }
    *complete = true;
    return p.empty();
}

// The per-user temporary directory (/var/folders/…/T/): created by the system
// 0700 for this user, so a socket in it is reachable by our own processes
// only. confstr gives it even when $TMPDIR was changed.
std::string userTempDir() {
    char         dir[PATH_MAX];
    const size_t n = confstr(_CS_DARWIN_USER_TEMP_DIR, dir, sizeof dir);
    std::string  d = n > 0 && n <= sizeof dir ? std::string(dir) : std::string();
    if (d.empty())
        return {};
    struct stat st{};
    if (lstat(d.c_str(), &st) != 0 || !S_ISDIR(st.st_mode) || st.st_uid != getuid() ||
        (st.st_mode & 077))
        return {}; // not private: no channel rather than an unsafe one
    if (d.back() != '/')
        d += '/';
    return d;
}

// $dir/plat-<key>.sock; a hash stands in for keys with other characters or
// too long for sun_path (104 bytes on macOS).
std::string socketPathFor(std::string_view key) {
    const std::string dir = userTempDir();
    if (dir.empty())
        return {};
    const uint64_t h = core::fnv1a(key);
    char           hex[17];
    std::snprintf(hex, sizeof hex, "%016llx", (unsigned long long)h);
    bool plain = !key.empty();
    for (char c : key)
        plain &= std::isalnum(uint8_t(c)) || c == '.' || c == '-' || c == '_';
    std::string p = dir + "plat-" + (plain ? std::string(key) : std::string(hex)) + ".sock";
    if (p.size() >= sizeof(sockaddr_un::sun_path))
        p = dir + "plat-" + hex + ".sock";
    return p.size() < sizeof(sockaddr_un::sun_path) ? p : std::string();
}

sockaddr_un unixAddr(const std::string &path) {
    sockaddr_un a{};
    a.sun_family = AF_UNIX;
    std::memcpy(a.sun_path, path.c_str(), path.size() + 1);
    return a;
}

enum class Forward : uint8_t { Sent, NoListener, NoAnswer };

// Second instance: hand our arguments to the primary.
Forward forwardToPrimary(const std::string &path, const std::vector<std::string> &strings) {
    const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return Forward::NoAnswer;
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    const int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
    // Bounded: a hung primary must not hang every later launch.
    const timeval tv{3, 0};
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    sockaddr_un a = unixAddr(path);
    if (connect(fd, reinterpret_cast<sockaddr *>(&a), sizeof a) != 0) {
        close(fd);
        return Forward::NoListener;
    }
    const std::string msg = encodeMessage(strings);
    size_t            off = 0;
    while (off < msg.size()) {
        const ssize_t n = write(fd, msg.data() + off, msg.size() - off);
        if (n <= 0 && errno != EINTR)
            break;
        if (n > 0)
            off += size_t(n);
    }
    char ack = 0;
    bool ok  = off == msg.size() && read(fd, &ack, 1) == 1 && ack == kAck;
    close(fd);
    return ok ? Forward::Sent : Forward::NoAnswer;
}

// Filter globs → content types. nil (no restriction) as soon as one pattern
// is not a plain "*.ext": the panel cannot express it, and an over-wide
// panel beats one that hides the file the user wants.
NSArray<UTType *> *contentTypes(const std::vector<FileFilter> &filters) {
    NSMutableArray<UTType *> *types = [NSMutableArray array];
    for (const auto &f : filters) {
        for (const auto &p : f.patterns) {
            if (p.size() < 3 || p.compare(0, 2, "*.") != 0 ||
                p.find_first_of("*?[", 2) != std::string::npos)
                return nil;
            if (UTType *t = [UTType typeWithFilenameExtension:nsString(p.substr(2))])
                [types addObject:t];
        }
    }
    return types.count ? types : nil;
}

NSString *directoryPath(NSSearchPathDirectory d) {
    return [NSFileManager.defaultManager URLsForDirectory:d inDomains:NSUserDomainMask]
        .firstObject.path;
}

} // namespace

// ── Screens ─────────────────────────────────────────────────────────────────

Rect flipRect(NSRect r) {
    // Round the edges, not origin and size separately, so a work area
    // computed from the same screen never pokes out of its bounds.
    const CGFloat H  = primaryHeight();
    const long    x0 = std::lround(r.origin.x), x1 = std::lround(r.origin.x + r.size.width);
    const long    y0 = std::lround(H - (r.origin.y + r.size.height));
    const long    y1 = std::lround(H - r.origin.y);
    return {int(x0), int(y0), int(x1 - x0), int(y1 - y0)};
}

NSRect unflipRect(Rect r) {
    return NSMakeRect(r.x, primaryHeight() - r.y - r.h, r.w, r.h);
}

uint64_t screenId(NSScreen *s) {
    NSNumber *n = s.deviceDescription[@"NSScreenNumber"];
    return n ? n.unsignedLongLongValue : 0;
}

std::vector<Monitor> CocoaApp::monitors() const {
    std::vector<Monitor> out;
    @autoreleasepool {
        NSArray<NSScreen *> *screens = NSScreen.screens;
        for (NSScreen *s in screens) {
            Monitor m;
            m.id                  = screenId(s);
            m.name                = s.localizedName.UTF8String ?: "";
            m.bounds              = flipRect(s.frame);
            m.workArea            = flipRect(s.visibleFrame); // minus menu bar and Dock
            m.scale               = s.backingScaleFactor;
            // The mode's rate is exact (59.94 Hz → 59940); built-in panels
            // report 0 there, and ProMotion ones their maximum here.
            double           hz   = 0;
            CGDisplayModeRef mode = CGDisplayCopyDisplayMode(CGDirectDisplayID(m.id));
            if (mode) {
                hz = CGDisplayModeGetRefreshRate(mode);
                CGDisplayModeRelease(mode);
            }
            if (hz <= 0)
                hz = double(s.maximumFramesPerSecond);
            m.refreshMilliHz = int(std::lround(hz * 1000));
            m.primary        = s == screens.firstObject; // the menu-bar screen, origin 0,0
            out.push_back(std::move(m));
        }
    }
    return out;
}

// ── setup ───────────────────────────────────────────────────────────────────

void CocoaApp::setUpServices() {
    PlatServicesObserver *obs = [PlatServicesObserver new];
    obs.owner                 = this;
    _services                 = obs;

    NSNotificationCenter *nc = NSNotificationCenter.defaultCenter;
    [nc addObserver:obs
           selector:@selector(screensChanged:)
               name:NSApplicationDidChangeScreenParametersNotification
             object:nil];
    [nc addObserver:obs
           selector:@selector(settingsChanged:)
               name:NSSystemColorsDidChangeNotification
             object:nil];

    NSNotificationCenter *wc = NSWorkspace.sharedWorkspace.notificationCenter;
    [wc addObserver:obs
           selector:@selector(willSleep:)
               name:NSWorkspaceWillSleepNotification
             object:nil];
    [wc addObserver:obs
           selector:@selector(didWake:)
               name:NSWorkspaceDidWakeNotification
             object:nil];
    [wc addObserver:obs
           selector:@selector(settingsChanged:)
               name:NSWorkspaceAccessibilityDisplayOptionsDidChangeNotification
             object:nil];

    installUrlHandler();

    // Reachability: the first update (right after start) sets the state
    // without an event; later changes are NetworkChanged.
    nw_path_monitor_t mon = nw_path_monitor_create();
    dispatch_queue_t  q   = dispatch_queue_create("org.nisdos.plat.network", DISPATCH_QUEUE_SERIAL);
    auto              alive = _alive;
    nw_path_monitor_set_queue(mon, q);
    nw_path_monitor_set_update_handler(mon, ^(nw_path_t path) {
      const bool      on = nw_path_get_status(path) == nw_path_status_satisfied;
      std::lock_guard lock(alive->mutex);
      if (CocoaApp *app = alive->app)
          app->post([app, on] { app->onNetwork(on); });
    });
    nw_path_monitor_start(mon);
    _pathMonitor = mon;
}

void CocoaApp::tearDownServices() {
    if (_pathMonitor) {
        nw_path_monitor_cancel((nw_path_monitor_t)_pathMonitor);
        _pathMonitor = nil;
    }
#ifdef PLAT_TEST_HOOKS
    [(NSTimer *)_dialogDriver invalidate];
    _dialogDriver = nil;
#endif
    for (auto &[fd, c] : _instanceConns) {
        unwatchFd(c.watch);
        close(fd);
    }
    _instanceConns.clear();
    if (_instanceFd >= 0) {
        unwatchFd(_instanceWatch);
        close(_instanceFd);
        unlink(_instancePath.c_str()); // the lock (still ours) makes this safe
        _instanceFd = -1;
    }
    if (_instanceLock >= 0) {
        // Unlinked while still held: a launch that opened this inode a moment
        // ago notices it is gone (see claimSingleInstance) and retries.
        unlink(_instanceLockPath.c_str());
        close(_instanceLock); // releases the flock: the next launch is primary
        _instanceLock = -1;
    }
    if (!_services)
        return;
    NSAppleEventManager *aem = NSAppleEventManager.sharedAppleEventManager;
    [aem removeEventHandlerForEventClass:kInternetEventClass andEventID:kAEGetURL];
    [NSNotificationCenter.defaultCenter removeObserver:_services];
    [NSWorkspace.sharedWorkspace.notificationCenter removeObserver:_services];
    _services.owner = nullptr;
    _services       = nil;
}

void CocoaApp::onSystemEvent(EventType t) {
    emit({.type = t});
    noteWork();
}

void CocoaApp::onNetwork(bool online) {
    const bool first = !_online;
    if (_online == online)
        return;
    _online = online;
    if (!first)
        emit({.type = EventType::NetworkChanged, .online = online});
}

// ── URLs ────────────────────────────────────────────────────────────────────

void CocoaApp::installUrlHandler() {
    // Our own GetURL handler rather than -application:openURLs:, so it is in
    // place before launch (AppKit only routes URLs to the delegate from
    // -finishLaunching on) and works the same for events sent to ourselves.
    [NSAppleEventManager.sharedAppleEventManager setEventHandler:_services
                                                     andSelector:@selector(getUrl:withReply:)
                                                   forEventClass:kInternetEventClass
                                                      andEventID:kAEGetURL];
}

void CocoaApp::onUrls(std::vector<std::string> urls) {
    // Posted: launch-time URLs arrive inside ensureLaunched(), before the app
    // may have set its handler, and never re-entrantly.
    post([this, urls = std::move(urls)] { emit({.type = EventType::OpenUrls, .strings = urls}); });
}

void CocoaApp::onReopen() {
    post([this] { emit({.type = EventType::InstanceActivated}); });
}

bool CocoaApp::registerUrlScheme(std::string_view scheme) {
    // macOS registers schemes from the bundle's Info.plist (CFBundleURLTypes)
    // when Launch Services sees the bundle; nothing to write at run time.
    // Remembered either way, for second-instance arguments.
    core::rememberScheme(_schemes, scheme);
    const std::string s = core::asciiLower(scheme);
    @autoreleasepool {
        NSString *want = nsString(s);
        for (NSDictionary *type in NSBundle.mainBundle.infoDictionary[@"CFBundleURLTypes"]) {
            if (![type isKindOfClass:NSDictionary.class])
                continue;
            for (NSString *declared in type[@"CFBundleURLSchemes"])
                if ([declared isKindOfClass:NSString.class] &&
                    [declared caseInsensitiveCompare:want] == NSOrderedSame)
                    return true;
        }
    }
    return false;
}

#ifdef PLAT_TEST_HOOKS
bool CocoaApp::deliverUrl(std::string_view url) {
    @autoreleasepool {
        // A real GetURL event, addressed to our pid, so it takes the same path
        // as one Launch Services sends: the Apple Event Manager's port, the
        // run loop, then our handler.
        NSAppleEventDescriptor *target =
            [NSAppleEventDescriptor descriptorWithProcessIdentifier:getpid()];
        NSAppleEventDescriptor *ev =
            [NSAppleEventDescriptor appleEventWithEventClass:kInternetEventClass
                                                     eventID:kAEGetURL
                                            targetDescriptor:target
                                                    returnID:kAutoGenerateReturnID
                                               transactionID:kAnyTransactionID];
        [ev setParamDescriptor:[NSAppleEventDescriptor descriptorWithString:nsString(url)]
                    forKeyword:keyDirectObject];
        const OSStatus st =
            AESendMessage(ev.aeDesc, nullptr, kAENoReply | kAENeverInteract, kAEDefaultTimeout);
        if (st != noErr) {
            std::fprintf(stderr, "plat/cocoa: AESendMessage(GetURL) failed: %d\n", int(st));
            return false;
        }
        return true;
    }
}
#endif

// ── single instance ─────────────────────────────────────────────────────────
// A Unix socket in the per-user temp dir, guarded by an flock on a sibling
// file that the primary holds for its lifetime: the lock says whether a
// primary lives (the kernel drops it when that process dies, however it
// dies), the socket carries the arguments. Launch Services launches of the
// bundle never get here — they reach the running process as a reopen or a
// GetURL event instead.

bool CocoaApp::claimSingleInstance(std::string_view key, const std::vector<std::string> &args) {
    if (_instanceLock >= 0) {
        if (_instanceKey != key)
            std::fprintf(stderr, "plat/cocoa: one single-instance key per process\n");
        return true;
    }
    const std::string path = socketPathFor(key);
    if (path.empty()) {
        std::fprintf(stderr, "plat/cocoa: no private directory for the instance socket\n");
        return true;
    }
    const std::string lockPath = path.substr(0, path.size() - 5) + ".lock";
    int               lock     = -1;
    for (int attempt = 0; attempt < 5 && lock < 0; ++attempt) {
        lock = open(lockPath.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
        if (lock < 0)
            return true;
        if (flock(lock, LOCK_EX | LOCK_NB) != 0) {
            close(lock);
            // A primary lives. It may have taken the lock a moment ago and
            // not be listening yet: retry the connection briefly.
            std::vector<std::string> strings;
            char                     cwd[PATH_MAX];
            strings.emplace_back(getcwd(cwd, sizeof cwd) ? cwd : "");
            strings.insert(strings.end(), args.begin(), args.end());
            for (int i = 0; i < 20; ++i) {
                const Forward f = forwardToPrimary(path, strings);
                if (f == Forward::Sent)
                    return false;
                if (f == Forward::NoAnswer)
                    break;
                usleep(50 * 1000);
            }
            std::fprintf(stderr, "plat/cocoa: the running instance did not answer\n");
            return false; // still not ours to run beside a live (if hung) primary
        }
        // Locked — but maybe an inode a quitting primary just unlinked: then
        // the lock proves nothing and a fresh file decides.
        struct stat held{}, now{};
        if (fstat(lock, &held) != 0 || stat(lockPath.c_str(), &now) != 0 ||
            held.st_ino != now.st_ino || held.st_dev != now.st_dev) {
            close(lock);
            lock = -1;
        }
    }
    if (lock < 0)
        return true;
    // We are primary; whatever socket file is there is stale.
    unlink(path.c_str());
    const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        close(lock);
        return true;
    }
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    sockaddr_un a = unixAddr(path);
    if (bind(fd, reinterpret_cast<sockaddr *>(&a), sizeof a) != 0 || listen(fd, 8) != 0) {
        std::fprintf(stderr, "plat/cocoa: instance socket: %s\n", std::strerror(errno));
        close(fd);
        close(lock);
        return true;
    }
    chmod(path.c_str(), 0600);
    _instanceLock     = lock;
    _instanceLockPath = lockPath;
    _instanceFd       = fd;
    _instancePath     = path;
    _instanceKey      = std::string(key);
    _instanceWatch    = watchFd(fd, FdRead, [this](uint32_t) { onInstanceAccept(); });
    return true;
}

void CocoaApp::onInstanceAccept() {
    for (;;) {
        const int c = accept(_instanceFd, nullptr, nullptr);
        if (c < 0)
            return; // EAGAIN: drained
        uid_t uid = 0;
        gid_t gid = 0;
        if (getpeereid(c, &uid, &gid) != 0 || uid != getuid()) {
            close(c); // the directory is private; belt and braces
            continue;
        }
        fcntl(c, F_SETFD, FD_CLOEXEC);
        fcntl(c, F_SETFL, fcntl(c, F_GETFL) | O_NONBLOCK);
        const int one = 1;
        setsockopt(c, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
        _instanceConns[c].watch = watchFd(c, FdRead, [this, c](uint32_t) { onInstanceData(c); });
    }
}

void CocoaApp::onInstanceData(int fd) {
    auto it = _instanceConns.find(fd);
    if (it == _instanceConns.end())
        return;
    char    chunk[4096];
    ssize_t n;
    bool    eof = false;
    while ((n = read(fd, chunk, sizeof chunk)) > 0)
        it->second.buf.append(chunk, size_t(n));
    if (n == 0)
        eof = true;
    else if (errno != EAGAIN && errno != EINTR)
        eof = true;
    bool                     complete = false;
    std::vector<std::string> strings;
    const bool               valid = decodeMessage(it->second.buf, &complete, &strings);
    if (!complete && valid && !eof)
        return; // more to come
    unwatchFd(it->second.watch);
    _instanceConns.erase(it);
    if (complete && valid) {
        const char ack = kAck;
        (void)!write(fd, &ack, 1);
    }
    close(fd);
    if (!complete || !valid || strings.empty())
        return;
    Event e{.type = EventType::InstanceActivated};
    e.text = std::move(strings.front());
    e.strings.assign(strings.begin() + 1, strings.end());
    std::vector<std::string> urls = core::schemeUrls(e.strings, _schemes);
    emit(e);
    if (!urls.empty())
        emit({.type = EventType::OpenUrls, .strings = std::move(urls)});
}

// ── file panels ─────────────────────────────────────────────────────────────

void CocoaApp::showFileDialog(
    const FileDialogDesc &d, std::function<void(std::vector<std::string>)> cb
) {
    ensureLaunched();
    @autoreleasepool {
        using Mode         = FileDialogDesc::Mode;
        NSSavePanel *panel = nil;
        if (d.mode == Mode::Save) {
            panel                      = [NSSavePanel savePanel];
            panel.nameFieldStringValue = nsString(d.suggestedName);
        } else {
            NSOpenPanel *open            = [NSOpenPanel openPanel];
            open.canChooseFiles          = d.mode != Mode::PickFolder;
            open.canChooseDirectories    = d.mode == Mode::PickFolder;
            open.allowsMultipleSelection = d.mode == Mode::OpenMultiple;
            open.resolvesAliases         = YES;
            panel                        = open;
        }
        panel.canCreateDirectories = YES;
        if (!d.title.empty()) {
            // A sheet has no title bar; the message line is what shows.
            panel.title   = nsString(d.title);
            panel.message = nsString(d.title);
        }
        if (!d.initialDir.empty())
            panel.directoryURL = [NSURL fileURLWithPath:nsString(d.initialDir) isDirectory:YES];
        // One union of every filter's types: the plain panel has no filter
        // chooser (that would take an accessory view).
        if (NSArray<UTType *> *types = contentTypes(d.filters))
            panel.allowedContentTypes = types;

#ifdef PLAT_TEST_HOOKS
        // A pending test answer is entered the way a user would before
        // confirming: navigate to its folder and type its name (Save), or
        // navigate to the file itself, which the panel selects (Open). Set
        // before -begin…: the panel's contents live in the open/save panel
        // service, and changes made once it is up are not reflected in what
        // it reports (measured on macOS 26).
        std::optional<std::vector<std::string>> answer;
        if (_dialogAnswer) {
            answer = std::move(*_dialogAnswer);
            _dialogAnswer.reset();
        }
        if (answer && !answer->empty()) {
            NSString *first = nsString(answer->front());
            if (d.mode == Mode::Save) {
                panel.directoryURL = [NSURL fileURLWithPath:first.stringByDeletingLastPathComponent
                                                isDirectory:YES];
                panel.nameFieldStringValue = first.lastPathComponent;
            } else {
                panel.directoryURL = [NSURL fileURLWithPath:first];
            }
        }
#endif

        auto alive = _alive;
        auto done  = std::make_shared<std::function<void(std::vector<std::string>)>>(std::move(cb));
        void (^finish)(NSModalResponse) = ^(NSModalResponse r) {
          // Standardised: the panel resolves /var and /tmp to /private/...
          // for files it selected, where every other API (NSTemporaryDirectory,
          // standardDir) says /var/...; this is Apple's own normal form.
          std::vector<std::string> paths;
          if (r == NSModalResponseOK) {
              if ([panel isKindOfClass:NSOpenPanel.class]) {
                  for (NSURL *u in ((NSOpenPanel *)panel).URLs)
                      if (u.isFileURL)
                          paths.emplace_back(u.path.stringByStandardizingPath.UTF8String);
              } else if (panel.URL.isFileURL) {
                  paths.emplace_back(panel.URL.path.stringByStandardizingPath.UTF8String);
              }
          }
          std::lock_guard lock(alive->mutex);
          if (CocoaApp *app = alive->app)
              app->post([done, paths = std::move(paths)] { (*done)(paths); });
        };

        auto      *parent = static_cast<CocoaWindow *>(d.parent);
        NSWindow  *pw     = parent ? parent->window : nil;
        const bool sheet  = pw.isVisible && !pw.attachedSheet;
        if (sheet)
            [panel beginSheetModalForWindow:pw completionHandler:finish];
        else
            [panel beginWithCompletionHandler:finish];
#ifdef PLAT_TEST_HOOKS
        if (answer)
            confirmPanel(panel, sheet, !answer->empty());
#endif
    }
}

#ifdef PLAT_TEST_HOOKS

// Test driver, second half: once the panel is up (and the service has had a
// moment to fill it), press its button. Only two public calls do anything
// on a service-backed panel: -cancel:, and for a sheet -[NSApp endSheet:
// returnCode:]; -ok: raises "not implemented", and keystrokes sent to the
// panel never reach the service (tried: -sendEvent:, -postEvent:,
// CGEventPostToPid, the remote view's -keyDown:). So a parentless panel can
// be cancelled but not confirmed.
void CocoaApp::confirmPanel(NSSavePanel *panel, bool sheet, bool accept) {
    [(NSTimer *)_dialogDriver invalidate];
    const CFAbsoluteTime   start   = CFAbsoluteTimeGetCurrent();
    __block CFAbsoluteTime visible = 0;
    NSTimer               *t       = [NSTimer
        timerWithTimeInterval:0.05
                      repeats:YES
                        block:^(NSTimer *timer) {
                          const CFAbsoluteTime now = CFAbsoluteTimeGetCurrent();
                          if (!visible) {
                              if (panel.isVisible)
                                  visible = now;
                              else if (now - start > 8) {
                                  std::fprintf(
                                      stderr, "plat/cocoa: the file panel never appeared\n"
                                  );
                                  [timer invalidate];
                              }
                              return;
                          }
                          if (now - visible < 1.0)
                              return; // the service is still loading the folder
                          [timer invalidate];
                          if (accept && sheet) {
                              [NSApp endSheet:panel returnCode:NSModalResponseOK];
                              return;
                          }
                          if (accept)
                              std::fprintf(
                                  stderr,
                                  "plat/cocoa: a file panel without a parent window cannot be "
                                  "confirmed programmatically; cancelling it\n"
                              );
                          [panel cancel:nil];
                        }];
    [NSRunLoop.mainRunLoop addTimer:t forMode:NSRunLoopCommonModes];
    _dialogDriver = t;
}

bool CocoaApp::fileDialogRespond(std::vector<std::string> paths) {
    // One file (or none) can be chosen through the panel's public API; there
    // is no public way to select several, and faking the answer would test
    // nothing. (A non-empty answer also needs the dialog to have a parent
    // window: see confirmPanel.)
    if (paths.size() > 1)
        return false;
    _dialogAnswer = std::move(paths);
    return true;
}
#endif

// ── directories, settings ───────────────────────────────────────────────────

std::vector<std::string> CocoaApp::preferredLanguages() const {
    @autoreleasepool {
        std::vector<std::string> out;
        for (NSString *l in NSLocale.preferredLanguages) // "ja-JP", "en-US", …
            out.emplace_back(l.UTF8String);
        return out;
    }
}

std::string CocoaApp::standardDir(StandardDir d) const {
    @autoreleasepool {
        NSString *p = nil;
        switch (d) {
        case StandardDir::Config: // macOS keeps settings, data and state together
        case StandardDir::Data:
        case StandardDir::State:
            p = directoryPath(NSApplicationSupportDirectory);
            break;
        case StandardDir::Cache:
            p = directoryPath(NSCachesDirectory);
            break;
        case StandardDir::Temp:
            p = NSTemporaryDirectory();
            break;
        case StandardDir::Home:
            p = NSHomeDirectory();
            break;
        case StandardDir::Desktop:
            p = directoryPath(NSDesktopDirectory);
            break;
        case StandardDir::Documents:
            p = directoryPath(NSDocumentDirectory);
            break;
        case StandardDir::Downloads:
            p = directoryPath(NSDownloadsDirectory);
            break;
        case StandardDir::Pictures:
            p = directoryPath(NSPicturesDirectory);
            break;
        }
        std::string s = p.UTF8String ?: "";
        while (s.size() > 1 && s.back() == '/')
            s.pop_back();
        return s;
    }
}

SystemSettings CocoaApp::systemSettings() const {
    SystemSettings s;
    @autoreleasepool {
        NSWorkspace *ws         = NSWorkspace.sharedWorkspace;
        s.reducedMotion         = ws.accessibilityDisplayShouldReduceMotion;
        s.highContrast          = ws.accessibilityDisplayShouldIncreaseContrast;
        // macOS has no app-wide text size preference for AppKit apps (only
        // per-app ones like Mail's); stays 1.
        s.textScale             = 1.0;
        // Resolved in the app's appearance: the "multicolour" accent is a
        // dynamic colour that only has components inside one.
        __block uint32_t accent = 0;
        [NSApp.effectiveAppearance performAsCurrentDrawingAppearance:^{
          NSColor *c =
              [NSColor.controlAccentColor colorUsingColorSpace:NSColorSpace.sRGBColorSpace];
          if (!c)
              return;
          auto ch = [](CGFloat v) {
              return uint32_t(std::lround(std::clamp(v, CGFloat(0), CGFloat(1)) * 255));
          };
          accent = ch(c.alphaComponent) << 24 | ch(c.redComponent) << 16 |
                   ch(c.greenComponent) << 8 | ch(c.blueComponent);
        }];
        s.accentColor      = accent;
        // NSTextView's blink period when the user set one (the
        // NSTextInsertionPointBlinkPeriodOn default, ms), else AppKit's own
        // ~560 ms. A huge value is the usual "don't blink" trick.
        NSUserDefaults *ud = NSUserDefaults.standardUserDefaults;
        NSInteger       on = [ud integerForKey:@"NSTextInsertionPointBlinkPeriodOn"];
        if (on <= 0)
            on = [ud integerForKey:@"NSTextInsertionPointBlinkPeriod"];
        s.caretBlinkMs = on <= 0 ? 560 : on > 5000 ? 0 : int(on);
    }
    return s;
}

#ifdef PLAT_TEST_HOOKS
// ── system-event test hook ──────────────────────────────────────────────────

bool CocoaApp::simulateSystemEvent(EventType type, bool online) {
    @autoreleasepool {
        NSNotificationCenter *wc = NSWorkspace.sharedWorkspace.notificationCenter;
        switch (type) {
        case EventType::Suspending:
            // NSWorkspace's centre is per process: only we hear this one.
            [wc postNotificationName:NSWorkspaceWillSleepNotification
                              object:NSWorkspace.sharedWorkspace];
            return true;
        case EventType::Resumed:
            [wc postNotificationName:NSWorkspaceDidWakeNotification
                              object:NSWorkspace.sharedWorkspace];
            return true;
        default:
            // Network: nw_path_monitor has no way to be fed a fake path, and
            // taking an interface down is not a test's business.
            return false;
        }
    }
}
#endif

} // namespace plat::cocoa
