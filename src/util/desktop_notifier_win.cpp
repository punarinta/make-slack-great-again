#include "util/desktop_notifier.h"

#include <QImage>

// The toast needs the WinRT notification + XML headers and WRL. The AUMID /
// shortcut / scheme registration is plain Win32. If the toolchain lacks the
// WinRT headers we keep the class but report unavailable, so the caller falls
// back to the Shell_NotifyIcon balloon (which already shows the avatar small).
#if __has_include(<windows.ui.notifications.h>) && __has_include(<wrl/client.h>)
#define MSGA_WINRT_TOAST 1
#endif

#include <windows.h>

// Registering the msga:// URL scheme is plain Win32 (registry only) and must
// happen on EVERY Windows build, including the mingw cross builds that lack the
// WinRT toast headers — otherwise the browser has nowhere to deliver the OAuth
// redirect (msga://oauth/callback) and sign-in hangs "loading forever". So this
// lives OUTSIDE the MSGA_WINRT_TOAST guard, unlike the toast machinery below.
#include <QCoreApplication>
#include <QDir>
#include <QSettings>

namespace {
// Register HKCU\Software\Classes\msga so the OAuth redirect (and the toast's
// `launch` protocol-activation URL) re-launches us with the msga:// argument —
// which SingleInstance forwards to the running app. Mirrors the Linux .desktop
// scheme handler.
void registerUriScheme() {
    const QString exe = QDir::toNativeSeparators(QCoreApplication::applicationFilePath());
    QSettings     cls("HKEY_CURRENT_USER\\Software\\Classes", QSettings::NativeFormat);
    cls.setValue("msga/.", "URL:MSGA Protocol"); // "." → the key's default value
    cls.setValue("msga/URL Protocol", "");
    cls.setValue("msga/DefaultIcon/.", QString("\"%1\",0").arg(exe));
    cls.setValue("msga/shell/open/command/.", QString("\"%1\" \"%2\"").arg(exe, "%1"));
}
} // namespace

#if defined(MSGA_WINRT_TOAST)

#include <QFileInfo>
#include <QStandardPaths>
#include <QUrl>

#include <propsys.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <roapi.h>
#include <windows.data.xml.dom.h>
#include <windows.ui.notifications.h>
#include <winstring.h>
#include <wrl/client.h>

using namespace ABI::Windows::Data::Xml::Dom;
using namespace ABI::Windows::UI::Notifications;
using Microsoft::WRL::ComPtr;

namespace {
constexpr wchar_t kAumid[] = L"com.nisdos.msga";

// PKEY_AppUserModel_ID = {9F4C2855-9F79-4B39-A8D0-E1D42DE1D5F3}, pid 5.
// Defined locally so we don't depend on the propkey/uuid lib carrying it (the
// mingw cross toolchain doesn't always provide the symbol).
const PROPERTYKEY kPkeyAumid = {
    {0x9F4C2855, 0x9F79, 0x4B39, {0xA8, 0xD0, 0xE1, 0xD4, 0x2D, 0xE1, 0xD5, 0xF3}}, 5
};

// XML-escape for attribute/text content in the toast payload.
QString xmlEscape(const QString &s) {
    QString out = s;
    out.replace('&', "&amp;");
    out.replace('<', "&lt;");
    out.replace('>', "&gt;");
    out.replace('"', "&quot;");
    return out;
}

// Unpackaged Win32 apps only get toasts if a Start-Menu shortcut carries the
// same AppUserModelID the process publishes under. Create it once (idempotent).
bool ensureStartMenuShortcut() {
    PWSTR programs = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_Programs, 0, nullptr, &programs)))
        return false;
    const QString lnk =
        QDir(QString::fromWCharArray(programs)).filePath(QStringLiteral("MSGA.lnk"));
    CoTaskMemFree(programs);
    if (QFileInfo::exists(lnk))
        return true;

    const std::wstring lnkW = QDir::toNativeSeparators(lnk).toStdWString();
    const std::wstring exeW =
        QDir::toNativeSeparators(QCoreApplication::applicationFilePath()).toStdWString();

    ComPtr<IShellLinkW> link;
    if (FAILED(
            CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link))
        ))
        return false;
    link->SetPath(exeW.c_str());

    ComPtr<IPropertyStore> props;
    if (SUCCEEDED(link.As(&props))) {
        PROPVARIANT pv;
        PropVariantInit(&pv);
        pv.vt      = VT_LPWSTR;
        pv.pwszVal = const_cast<wchar_t *>(kAumid);
        props->SetValue(kPkeyAumid, pv);
        props->Commit();
        // pv borrows kAumid (static) — don't PropVariantClear (would free it).
    }
    ComPtr<IPersistFile> file;
    if (FAILED(link.As(&file)))
        return false;
    return SUCCEEDED(file->Save(lnkW.c_str(), TRUE));
}

// Owning RAII for an HSTRING built from a QString.
struct HStr {
    HSTRING h = nullptr;
    explicit HStr(const QString &s) {
        WindowsCreateString(reinterpret_cast<PCWSTR>(s.utf16()), UINT32(s.size()), &h);
    }
    ~HStr() { WindowsDeleteString(h); }
    HStr(const HStr &)            = delete;
    HStr &operator=(const HStr &) = delete;
};

bool showToast(const QString &aumid, const QString &xml) {
    HStr xmlH(xml);

    ComPtr<IXmlDocument> doc;
    {
        ComPtr<IInspectable> inspectable;
        HStr                 cls(QStringLiteral("Windows.Data.Xml.Dom.XmlDocument"));
        if (FAILED(RoActivateInstance(cls.h, &inspectable)) || FAILED(inspectable.As(&doc)))
            return false;
        ComPtr<IXmlDocumentIO> io;
        if (FAILED(doc.As(&io)) || FAILED(io->LoadXml(xmlH.h)))
            return false;
    }

    ComPtr<IToastNotificationFactory> toastFactory;
    {
        HStr id(QStringLiteral("Windows.UI.Notifications.ToastNotification"));
        if (FAILED(RoGetActivationFactory(id.h, IID_PPV_ARGS(&toastFactory))))
            return false;
    }
    ComPtr<IToastNotification> toast;
    if (FAILED(toastFactory->CreateToastNotification(doc.Get(), &toast)))
        return false;

    ComPtr<IToastNotificationManagerStatics> mgr;
    {
        HStr id(QStringLiteral("Windows.UI.Notifications.ToastNotificationManager"));
        if (FAILED(RoGetActivationFactory(id.h, IID_PPV_ARGS(&mgr))))
            return false;
    }
    ComPtr<IToastNotifier> notifier;
    HStr                   aumidH(aumid);
    if (FAILED(mgr->CreateToastNotifierWithId(aumidH.h, &notifier)))
        return false;
    return SUCCEEDED(notifier->Show(toast.Get()));
}
} // namespace

#endif // MSGA_WINRT_TOAST

DesktopNotifier::DesktopNotifier(QObject *parent) : QObject(parent) {
    // Always register the msga:// scheme so OAuth sign-in can complete, even on
    // builds without WinRT toast support.
    registerUriScheme();
#if defined(MSGA_WINRT_TOAST)
    // RoInitialize may already be done by Qt's COM init on this thread — that's
    // fine (RPC_E_CHANGED_MODE just means a different apartment, still usable).
    RoInitialize(RO_INIT_SINGLETHREADED);
    SetCurrentProcessExplicitAppUserModelID(kAumid);
    if (ensureStartMenuShortcut()) {
        _aumid     = QString::fromWCharArray(kAumid);
        _available = true;
    }
#endif
}

DesktopNotifier::~DesktopNotifier() = default;

bool DesktopNotifier::notify(
    const QString            &title,
    const QString            &body,
    const QImage             &image,
    const QString            &token,
    const QList<NotifAction> &actions,
    int /*timeoutMs*/
) {
#if defined(MSGA_WINRT_TOAST)
    if (!_available)
        return false;

    // Avatar must be a file the toast renderer can read; rotate a few temp files
    // so a rapid burst never reads a half-written one.
    QString imgTag;
    if (!image.isNull()) {
        static int    rot = 0;
        const QString dir = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
        const QString path =
            QDir(dir).filePath(QStringLiteral("msga-notif-%1.png").arg((rot++) & 7));
        if (image.save(path, "PNG"))
            imgTag = QStringLiteral(
                         "<image placement=\"appLogoOverride\" hint-crop=\"circle\" "
                         "src=\"%1\"/>"
            )
                         .arg(xmlEscape(QUrl::fromLocalFile(path).toString()));
    }

    // Clicking fires protocol activation of this URL → SingleInstance forwards it.
    const auto launchUrl = [](const QString &t) {
        return QStringLiteral("msga://notif?token=%1")
            .arg(QString::fromLatin1(QUrl::toPercentEncoding(t)));
    };

    // Each action button is its own protocol-activation button carrying its own
    // token; the toast-level launch handles the body click.
    QString actionsTag;
    for (const auto &a : actions)
        actionsTag +=
            QStringLiteral("<action content=\"%1\" arguments=\"%2\" activationType=\"protocol\"/>")
                .arg(xmlEscape(a.label), xmlEscape(launchUrl(a.token)));
    if (!actionsTag.isEmpty())
        actionsTag = QStringLiteral("<actions>%1</actions>").arg(actionsTag);

    const QString xml =
        QStringLiteral(
            "<toast launch=\"%1\" activationType=\"protocol\"><visual>"
            "<binding template=\"ToastGeneric\"><text>%2</text><text>%3</text>%4"
            "</binding></visual>%5</toast>"
        )
            .arg(
                xmlEscape(launchUrl(token)), xmlEscape(title), xmlEscape(body), imgTag, actionsTag
            );

    return showToast(_aumid, xml);
#else
    Q_UNUSED(title);
    Q_UNUSED(body);
    Q_UNUSED(image);
    Q_UNUSED(token);
    Q_UNUSED(actions);
    return false;
#endif
}
