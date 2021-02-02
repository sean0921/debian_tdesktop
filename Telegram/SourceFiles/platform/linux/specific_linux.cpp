/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "platform/linux/specific_linux.h"

#include "base/platform/base_platform_info.h"
#include "base/platform/linux/base_xcb_utilities_linux.h"
#include "platform/linux/linux_desktop_environment.h"
#include "platform/linux/linux_gtk_integration.h"
#include "platform/linux/linux_wayland_integration.h"
#include "base/qt_adapters.h"
#include "lang/lang_keys.h"
#include "mainwindow.h"
#include "storage/localstorage.h"
#include "core/update_checker.h"
#include "window/window_controller.h"
#include "core/application.h"

#ifndef DESKTOP_APP_DISABLE_DBUS_INTEGRATION
#include "platform/linux/linux_notification_service_watcher.h"
#include "platform/linux/linux_gsd_media_keys.h"
#endif // !DESKTOP_APP_DISABLE_DBUS_INTEGRATION

#include <QtWidgets/QApplication>
#include <QtWidgets/QDesktopWidget>
#include <QtCore/QStandardPaths>
#include <QtCore/QProcess>
#include <QtCore/QVersionNumber>
#include <QtCore/QLibraryInfo>
#include <QtGui/QWindow>

#ifndef DESKTOP_APP_DISABLE_DBUS_INTEGRATION
#include <QtDBus/QDBusInterface>
#include <QtDBus/QDBusConnection>
#include <QtDBus/QDBusConnectionInterface>
#include <QtDBus/QDBusMessage>
#include <QtDBus/QDBusReply>
#include <QtDBus/QDBusError>
#endif // !DESKTOP_APP_DISABLE_DBUS_INTEGRATION

#include <xcb/xcb.h>

#include <glib.h>

extern "C" {
#undef signals
#include <gio/gio.h>
#define signals public
} // extern "C"

#include <sys/stat.h>
#include <sys/types.h>
#include <cstdlib>
#include <unistd.h>
#include <dirent.h>
#include <pwd.h>

#include <iostream>

using namespace Platform;
using Platform::internal::WaylandIntegration;
using Platform::internal::GtkIntegration;

Q_DECLARE_METATYPE(QMargins);

namespace Platform {
namespace {

constexpr auto kIgnoreGtkIncompatibility = "TDESKTOP_I_KNOW_ABOUT_GTK_INCOMPATIBILITY"_cs;

constexpr auto kDesktopFile = ":/misc/telegramdesktop.desktop"_cs;
constexpr auto kIconName = "telegram"_cs;
constexpr auto kHandlerTypeName = "x-scheme-handler/tg"_cs;

constexpr auto kXDGDesktopPortalService = "org.freedesktop.portal.Desktop"_cs;
constexpr auto kXDGDesktopPortalObjectPath = "/org/freedesktop/portal/desktop"_cs;
constexpr auto kPropertiesInterface = "org.freedesktop.DBus.Properties"_cs;

constexpr auto kXCBFrameExtentsAtomName = "_GTK_FRAME_EXTENTS"_cs;

QStringList PlatformThemes;

#ifndef DESKTOP_APP_DISABLE_DBUS_INTEGRATION
std::unique_ptr<internal::NotificationServiceWatcher> NSWInstance;

QStringList ListDBusActivatableNames() {
	static const auto Result = [&] {
		const auto message = QDBusMessage::createMethodCall(
			qsl("org.freedesktop.DBus"),
			qsl("/org/freedesktop/DBus"),
			qsl("org.freedesktop.DBus"),
			qsl("ListActivatableNames"));

		const QDBusReply<QStringList> reply = QDBusConnection::sessionBus()
			.call(message);

		if (reply.isValid()) {
			return reply.value();
		} else if (reply.error().type() != QDBusError::Disconnected) {
			LOG(("ListActivatableNames Error: %1: %2")
				.arg(reply.error().name())
				.arg(reply.error().message()));
		}

		return QStringList{};
	}();

	return Result;
}

void PortalAutostart(bool autostart, bool silent = false) {
	if (cExeName().isEmpty()) {
		return;
	}

	QVariantMap options;
	options["reason"] = tr::lng_settings_auto_start(tr::now);
	options["autostart"] = autostart;
	options["commandline"] = QStringList{
		cExeName(),
		qsl("-workdir"),
		cWorkingDir(),
		qsl("-autostart")
	};
	options["dbus-activatable"] = false;

	auto message = QDBusMessage::createMethodCall(
		kXDGDesktopPortalService.utf16(),
		kXDGDesktopPortalObjectPath.utf16(),
		qsl("org.freedesktop.portal.Background"),
		qsl("RequestBackground"));

	const auto parentWindowId = [&] {
		if (const auto activeWindow = Core::App().activeWindow()) {
			if (!IsWayland()) {
				return qsl("x11:%1").arg(QString::number(
					activeWindow->widget().get()->windowHandle()->winId(),
					16));
			}
		}
		return QString();
	}();

	message.setArguments({
		parentWindowId,
		options
	});

	if (silent) {
		QDBusConnection::sessionBus().send(message);
		return;
	}

	const QDBusError error = QDBusConnection::sessionBus().call(message);
	if (error.isValid()) {
		LOG(("Flatpak Autostart Error: %1: %2")
			.arg(error.name())
			.arg(error.message()));
	}
}

bool IsXDGDesktopPortalPresent() {
	static const auto Result = QDBusInterface(
		kXDGDesktopPortalService.utf16(),
		kXDGDesktopPortalObjectPath.utf16()).isValid();

	return Result;
}

bool IsXDGDesktopPortalKDEPresent() {
	static const auto Result = QDBusInterface(
		qsl("org.freedesktop.impl.portal.desktop.kde"),
		kXDGDesktopPortalObjectPath.utf16()).isValid();

	return Result;
}

bool IsIBusPortalPresent() {
	static const auto Result = [&] {
		const auto interface = QDBusConnection::sessionBus().interface();
		const auto activatableNames = ListDBusActivatableNames();

		const auto serviceRegistered = interface
			&& interface->isServiceRegistered(
				qsl("org.freedesktop.portal.IBus"));

		const auto serviceActivatable = activatableNames.contains(
			qsl("org.freedesktop.portal.IBus"));

		return serviceRegistered || serviceActivatable;
	}();

	return Result;
}

uint FileChooserPortalVersion() {
	static const auto Result = [&]() -> uint {
		auto message = QDBusMessage::createMethodCall(
			kXDGDesktopPortalService.utf16(),
			kXDGDesktopPortalObjectPath.utf16(),
			kPropertiesInterface.utf16(),
			qsl("Get"));

		message.setArguments({
			qsl("org.freedesktop.portal.FileChooser"),
			qsl("version")
		});

		const QDBusReply<QVariant> reply = QDBusConnection::sessionBus().call(
			message);

		if (reply.isValid()) {
			return reply.value().toUInt();
		}

		LOG(("Error getting FileChooser portal version: %1: %2")
			.arg(reply.error().name())
			.arg(reply.error().message()));

		return 0;
	}();

	return Result;
}
#endif // !DESKTOP_APP_DISABLE_DBUS_INTEGRATION

QByteArray EscapeShell(const QByteArray &content) {
	auto result = QByteArray();

	auto b = content.constData(), e = content.constEnd();
	for (auto ch = b; ch != e; ++ch) {
		if (*ch == ' ' || *ch == '"' || *ch == '\'' || *ch == '\\') {
			if (result.isEmpty()) {
				result.reserve(content.size() * 2);
			}
			if (ch > b) {
				result.append(b, ch - b);
			}
			result.append('\\');
			b = ch;
		}
	}
	if (result.isEmpty()) {
		return content;
	}

	if (e > b) {
		result.append(b, e - b);
	}
	return result;
}

QString EscapeShellInLauncher(const QString &content) {
	return EscapeShell(content.toUtf8()).replace('\\', "\\\\");
}

QString FlatpakID() {
	static const auto Result = [] {
		if (!qEnvironmentVariableIsEmpty("FLATPAK_ID")) {
			return QString::fromLatin1(qgetenv("FLATPAK_ID"));
		} else {
			return GetLauncherBasename();
		}
	}();

	return Result;
}

bool RunShellCommand(const QString &program, const QStringList &arguments) {
	const auto result = QProcess::execute(program, arguments);

	const auto command = qsl("%1 %2")
		.arg(program)
		.arg(arguments.join(' '));

	if (result) {
		DEBUG_LOG(("App Error: command failed, code: %1, command: %2")
			.arg(result)
			.arg(command));

		return false;
	}

	DEBUG_LOG(("App Info: command succeeded, command: %1")
		.arg(command));

	return true;
}

bool GenerateDesktopFile(
		const QString &targetPath,
		const QString &args,
		bool silent = false) {
	if (targetPath.isEmpty() || cExeName().isEmpty()) {
		return false;
	}

	DEBUG_LOG(("App Info: placing .desktop file to %1").arg(targetPath));
	if (!QDir(targetPath).exists()) QDir().mkpath(targetPath);

	const auto sourceFile = kDesktopFile.utf16();
	const auto targetFile = targetPath + GetLauncherFilename();

	QString fileText;

	QFile source(sourceFile);
	if (source.open(QIODevice::ReadOnly)) {
		QTextStream s(&source);
		fileText = s.readAll();
		source.close();
	} else {
		if (!silent) {
			LOG(("App Error: Could not open '%1' for read").arg(sourceFile));
		}
		return false;
	}

	QFile target(targetFile);
	if (target.open(QIODevice::WriteOnly)) {
		fileText = fileText.replace(
			QRegularExpression(
				qsl("^TryExec=.*$"),
				QRegularExpression::MultilineOption),
			qsl("TryExec=%1").arg(
				QString(cExeDir() + cExeName()).replace('\\', "\\\\")));

		fileText = fileText.replace(
			QRegularExpression(
				qsl("^Exec=.*$"),
				QRegularExpression::MultilineOption),
			qsl("Exec=%1 -workdir %2").arg(
				EscapeShellInLauncher(cExeDir() + cExeName()),
				EscapeShellInLauncher(cWorkingDir()))
				+ (args.isEmpty() ? QString() : ' ' + args));

		target.write(fileText.toUtf8());
		target.close();

		if (!Core::UpdaterDisabled()) {
			DEBUG_LOG(("App Info: removing old .desktop files"));
			QFile::remove(qsl("%1telegram.desktop").arg(targetPath));
			QFile::remove(qsl("%1telegramdesktop.desktop").arg(targetPath));
		}

		return true;
	} else {
		if (!silent) {
			LOG(("App Error: Could not open '%1' for write").arg(targetFile));
		}
		return false;
	}
}

uint XCBMoveResizeFromEdges(Qt::Edges edges) {
	if (edges == (Qt::TopEdge | Qt::LeftEdge))
		return 0;
	if (edges == Qt::TopEdge)
		return 1;
	if (edges == (Qt::TopEdge | Qt::RightEdge))
		return 2;
	if (edges == Qt::RightEdge)
		return 3;
	if (edges == (Qt::RightEdge | Qt::BottomEdge))
		return 4;
	if (edges == Qt::BottomEdge)
		return 5;
	if (edges == (Qt::BottomEdge | Qt::LeftEdge))
		return 6;
	if (edges == Qt::LeftEdge)
		return 7;

	return 0;
}

bool StartXCBMoveResize(QWindow *window, int edges) {
	const auto connection = base::Platform::XCB::GetConnectionFromQt();
	if (!connection) {
		return false;
	}

	const auto root = base::Platform::XCB::GetRootWindowFromQt();
	if (!root.has_value()) {
		return false;
	}

	const auto moveResizeAtom = base::Platform::XCB::GetAtom(
		connection,
		"_NET_WM_MOVERESIZE");

	if (!moveResizeAtom.has_value()) {
		return false;
	}

	const auto globalPos = QCursor::pos();

	xcb_client_message_event_t xev;
	xev.response_type = XCB_CLIENT_MESSAGE;
	xev.type = *moveResizeAtom;
	xev.sequence = 0;
	xev.window = window->winId();
	xev.format = 32;
	xev.data.data32[0] = globalPos.x();
	xev.data.data32[1] = globalPos.y();
	xev.data.data32[2] = (edges == 16)
		? 8 // move
		: XCBMoveResizeFromEdges(Qt::Edges(edges));
	xev.data.data32[3] = XCB_BUTTON_INDEX_1;
	xev.data.data32[4] = 0;

	xcb_ungrab_pointer(connection, XCB_CURRENT_TIME);
	xcb_send_event(
		connection,
		false,
		*root,
		XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT
			| XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY,
		reinterpret_cast<const char*>(&xev));

	return true;
}

bool ShowXCBWindowMenu(QWindow *window) {
	const auto connection = base::Platform::XCB::GetConnectionFromQt();
	if (!connection) {
		return false;
	}

	const auto root = base::Platform::XCB::GetRootWindowFromQt();
	if (!root.has_value()) {
		return false;
	}

	const auto showWindowMenuAtom = base::Platform::XCB::GetAtom(
		connection,
		"_GTK_SHOW_WINDOW_MENU");

	if (!showWindowMenuAtom.has_value()) {
		return false;
	}

	const auto globalPos = QCursor::pos();

	xcb_client_message_event_t xev;
	xev.response_type = XCB_CLIENT_MESSAGE;
	xev.type = *showWindowMenuAtom;
	xev.sequence = 0;
	xev.window = window->winId();
	xev.format = 32;
	xev.data.data32[0] = 0;
	xev.data.data32[1] = globalPos.x();
	xev.data.data32[2] = globalPos.y();
	xev.data.data32[3] = 0;
	xev.data.data32[4] = 0;

	xcb_ungrab_pointer(connection, XCB_CURRENT_TIME);
	xcb_send_event(
		connection,
		false,
		*root,
		XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT
			| XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY,
		reinterpret_cast<const char*>(&xev));

	return true;
}

bool SetXCBFrameExtents(QWindow *window, const QMargins &extents) {
	const auto connection = base::Platform::XCB::GetConnectionFromQt();
	if (!connection) {
		return false;
	}

	const auto frameExtentsAtom = base::Platform::XCB::GetAtom(
		connection,
		kXCBFrameExtentsAtomName.utf16());

	if (!frameExtentsAtom.has_value()) {
		return false;
	}

	const auto extentsVector = std::vector<uint>{
		uint(extents.left()),
		uint(extents.right()),
		uint(extents.top()),
		uint(extents.bottom()),
	};

	xcb_change_property(
		connection,
		XCB_PROP_MODE_REPLACE,
		window->winId(),
		*frameExtentsAtom,
		XCB_ATOM_CARDINAL,
		32,
		extentsVector.size(),
		extentsVector.data());

	return true;
}

bool UnsetXCBFrameExtents(QWindow *window) {
	const auto connection = base::Platform::XCB::GetConnectionFromQt();
	if (!connection) {
		return false;
	}

	const auto frameExtentsAtom = base::Platform::XCB::GetAtom(
		connection,
		kXCBFrameExtentsAtomName.utf16());

	if (!frameExtentsAtom.has_value()) {
		return false;
	}

	xcb_delete_property(
		connection,
		window->winId(),
		*frameExtentsAtom);

	return true;
}

Window::Control GtkKeywordToWindowControl(const QString &keyword) {
	if (keyword == qstr("minimize")) {
		return Window::Control::Minimize;
	} else if (keyword == qstr("maximize")) {
		return Window::Control::Maximize;
	} else if (keyword == qstr("close")) {
		return Window::Control::Close;
	}

	return Window::Control::Unknown;
}

} // namespace

void SetWatchingMediaKeys(bool watching) {
#ifndef DESKTOP_APP_DISABLE_DBUS_INTEGRATION
	static std::unique_ptr<internal::GSDMediaKeys> Instance;

	if (watching && !Instance) {
		Instance = std::make_unique<internal::GSDMediaKeys>();
	} else if (!watching && Instance) {
		Instance = nullptr;
	}
#endif // !DESKTOP_APP_DISABLE_DBUS_INTEGRATION
}

void SetApplicationIcon(const QIcon &icon) {
	QApplication::setWindowIcon(icon);
}

bool InFlatpak() {
	static const auto Result = QFileInfo::exists(qsl("/.flatpak-info"));
	return Result;
}

bool InSnap() {
	static const auto Result = qEnvironmentVariableIsSet("SNAP");
	return Result;
}

bool IsStaticBinary() {
#ifdef DESKTOP_APP_USE_PACKAGED
		return false;
#else // DESKTOP_APP_USE_PACKAGED
		return true;
#endif // !DESKTOP_APP_USE_PACKAGED
}

bool IsGtkIntegrationForced() {
	static const auto Result = [&] {
		if (!GtkIntegration::Instance()) {
			return false;
		}

		return PlatformThemes.contains(qstr("gtk3"), Qt::CaseInsensitive)
			|| PlatformThemes.contains(qstr("gtk2"), Qt::CaseInsensitive);
	}();

	return Result;
}

bool AreQtPluginsBundled() {
#if !defined DESKTOP_APP_USE_PACKAGED || defined DESKTOP_APP_USE_PACKAGED_LAZY
	return true;
#else // !DESKTOP_APP_USE_PACKAGED || DESKTOP_APP_USE_PACKAGED_LAZY
	return false;
#endif // DESKTOP_APP_USE_PACKAGED && !DESKTOP_APP_USE_PACKAGED_LAZY
}

bool UseXDGDesktopPortal() {
#ifndef DESKTOP_APP_DISABLE_DBUS_INTEGRATION
	static const auto Result = [&] {
		const auto onlyIn = AreQtPluginsBundled()
			// it is handled by Qt for flatpak and snap
			&& !InFlatpak()
			&& !InSnap();

		const auto envVar = qEnvironmentVariableIsSet("TDESKTOP_USE_PORTAL");
		const auto portalPresent = IsXDGDesktopPortalPresent();
		const auto neededForKde = DesktopEnvironment::IsKDE()
			&& IsXDGDesktopPortalKDEPresent();

		return onlyIn
			&& portalPresent
			&& (neededForKde || envVar);
	}();

	return Result;
#endif // !DESKTOP_APP_DISABLE_DBUS_INTEGRATION

	return false;
}

bool CanOpenDirectoryWithPortal() {
#ifndef DESKTOP_APP_DISABLE_DBUS_INTEGRATION
	static const auto Result = [&] {
#ifdef DESKTOP_APP_QT_PATCHED
		return FileChooserPortalVersion() >= 3;
#else // DESKTOP_APP_QT_PATCHED
		return QLibraryInfo::version() >= QVersionNumber(5, 15, 0)
			&& FileChooserPortalVersion() >= 3;
#endif // !DESKTOP_APP_QT_PATCHED
	}();

	return Result;
#endif // !DESKTOP_APP_DISABLE_DBUS_INTEGRATION

	return false;
}

bool IsNotificationServiceActivatable() {
#ifndef DESKTOP_APP_DISABLE_DBUS_INTEGRATION
	static const auto Result = ListDBusActivatableNames().contains(
		qsl("org.freedesktop.Notifications"));

	return Result;
#endif // !DESKTOP_APP_DISABLE_DBUS_INTEGRATION

	return false;
}

QString AppRuntimeDirectory() {
	static const auto Result = [&] {
		auto runtimeDir = QStandardPaths::writableLocation(
			QStandardPaths::RuntimeLocation);

		if (InFlatpak()) {
			runtimeDir += qsl("/app/") + FlatpakID();
		}

		if (!QFileInfo::exists(runtimeDir)) { // non-systemd distros
			runtimeDir = QDir::tempPath();
		}

		if (!runtimeDir.endsWith('/')) {
			runtimeDir += '/';
		}

		return runtimeDir;
	}();

	return Result;
}

QString SingleInstanceLocalServerName(const QString &hash) {
	const auto idealSocketPath = AppRuntimeDirectory()
		+ hash
		+ '-'
		+ cGUIDStr();

	if (idealSocketPath.size() >= 108) {
		return AppRuntimeDirectory() + hash;
	} else {
		return idealSocketPath;
	}
}

QString GetLauncherBasename() {
	static const auto Result = [&] {
		if (!Core::UpdaterDisabled() && !cExeName().isEmpty()) {
			const auto appimagePath = qsl("file://%1%2")
				.arg(cExeDir())
				.arg(cExeName())
				.toUtf8();

			char md5Hash[33] = { 0 };
			hashMd5Hex(
				appimagePath.constData(),
				appimagePath.size(),
				md5Hash);

			return qsl("appimagekit_%1-%2")
				.arg(md5Hash)
				.arg(AppName.utf16().replace(' ', '_'));
		}

		return qsl(MACRO_TO_STRING(TDESKTOP_LAUNCHER_BASENAME));
	}();

	return Result;
}

QString GetLauncherFilename() {
	static const auto Result = GetLauncherBasename()
		+ qsl(".desktop");
	return Result;
}

QString GetIconName() {
	static const auto Result = InFlatpak()
		? FlatpakID()
		: kIconName.utf16();
	return Result;
}

QImage GetImageFromClipboard() {
	if (const auto integration = GtkIntegration::Instance()) {
		return integration->getImageFromClipboard();
	}

	return {};
}

std::optional<bool> IsDarkMode() {
	const auto integration = GtkIntegration::Instance();
	if (!integration) {
		return std::nullopt;
	}

	if (integration->checkVersion(3, 0, 0)) {
		const auto preferDarkTheme = integration->getBoolSetting(
			qsl("gtk-application-prefer-dark-theme"));
		
		if (!preferDarkTheme.has_value()) {
			return std::nullopt;
		} else if (*preferDarkTheme) {
			return true;
		}
	}

	const auto themeName = integration->getStringSetting(qsl("gtk-theme-name"));
	if (!themeName.has_value()) {
		return std::nullopt;
	} else if (themeName->toLower().contains(qsl("-dark"))) {
		return true;
	}

	return false;
}

bool AutostartSupported() {
	// snap sandbox doesn't allow creating files
	// in folders with names started with a dot
	// and doesn't provide any api to add an app to autostart
	// thus, autostart isn't supported in snap
	return !InSnap();
}

bool TrayIconSupported() {
	return App::wnd()
		? App::wnd()->trayAvailable()
		: false;
}

bool SkipTaskbarSupported() {
	return !IsWayland()
		&& base::Platform::XCB::IsSupportedByWM("_NET_WM_STATE_SKIP_TASKBAR");
}

bool StartSystemMove(QWindow *window) {
	if (const auto integration = WaylandIntegration::Instance()) {
		return integration->startMove(window);
	} else {
		return StartXCBMoveResize(window, 16);
	}
}

bool StartSystemResize(QWindow *window, Qt::Edges edges) {
	if (const auto integration = WaylandIntegration::Instance()) {
		return integration->startResize(window, edges);
	} else {
		return StartXCBMoveResize(window, edges);
	}
}

bool ShowWindowMenu(QWindow *window) {
	if (const auto integration = WaylandIntegration::Instance()) {
		return integration->showWindowMenu(window);
	} else {
		return ShowXCBWindowMenu(window);
	}
}

bool SetWindowExtents(QWindow *window, const QMargins &extents) {
	if (IsWayland()) {
#ifdef DESKTOP_APP_QT_PATCHED
		window->setProperty("WaylandCustomMargins", QVariant::fromValue<QMargins>(extents));
		return true;
#else // DESKTOP_APP_QT_PATCHED
		return false;
#endif // !DESKTOP_APP_QT_PATCHED
	} else {
		return SetXCBFrameExtents(window, extents);
	}
}

bool UnsetWindowExtents(QWindow *window) {
	if (IsWayland()) {
#ifdef DESKTOP_APP_QT_PATCHED
		window->setProperty("WaylandCustomMargins", QVariant());
		return true;
#else // DESKTOP_APP_QT_PATCHED
		return false;
#endif // !DESKTOP_APP_QT_PATCHED
	} else {
		return UnsetXCBFrameExtents(window);
	}
}

bool WindowsNeedShadow() {
#ifdef DESKTOP_APP_QT_PATCHED
	if (IsWayland()) {
		return true;
	}
#endif // DESKTOP_APP_QT_PATCHED

	namespace XCB = base::Platform::XCB;
	if (!IsWayland()
		&& XCB::IsSupportedByWM(kXCBFrameExtentsAtomName.utf16())) {
		return true;
	}

	return false;
}

Window::ControlsLayout WindowControlsLayout() {
	const auto gtkResult = []() -> std::optional<Window::ControlsLayout> {
		const auto integration = GtkIntegration::Instance();
		if (!integration || !integration->checkVersion(3, 12, 0)) {
			return std::nullopt;
		}

		const auto decorationLayoutSetting = integration->getStringSetting(
			qsl("gtk-decoration-layout"));
		
		if (!decorationLayoutSetting.has_value()) {
			return std::nullopt;
		}

		const auto decorationLayout = decorationLayoutSetting->split(':');

		std::vector<Window::Control> controlsLeft;
		ranges::transform(
			decorationLayout[0].split(','),
			ranges::back_inserter(controlsLeft),
			GtkKeywordToWindowControl);

		std::vector<Window::Control> controlsRight;
		if (decorationLayout.size() > 1) {
			ranges::transform(
				decorationLayout[1].split(','),
				ranges::back_inserter(controlsRight),
				GtkKeywordToWindowControl);
		}

		return Window::ControlsLayout{
			.left = controlsLeft,
			.right = controlsRight
		};
	}();

	if (gtkResult.has_value()) {
		return *gtkResult;
	} else if (DesktopEnvironment::IsUnity()) {
		return Window::ControlsLayout{
			.left = {
				Window::Control::Close,
				Window::Control::Minimize,
				Window::Control::Maximize,
			}
		};
	} else {
		return Window::ControlsLayout{
			.right = {
				Window::Control::Minimize,
				Window::Control::Maximize,
				Window::Control::Close,
			}
		};
	}
}

} // namespace Platform

QRect psDesktopRect() {
	static QRect _monitorRect;
	static auto _monitorLastGot = 0LL;
	auto tnow = crl::now();
	if (tnow > _monitorLastGot + 1000LL || tnow < _monitorLastGot) {
		_monitorLastGot = tnow;
		_monitorRect = QApplication::desktop()->availableGeometry(App::wnd());
	}
	return _monitorRect;
}

void psWriteDump() {
}

void psActivateProcess(uint64 pid) {
//	objc_activateProgram();
}

namespace {

QString getHomeDir() {
	const auto home = QString(g_get_home_dir());

	if (!home.isEmpty() && !home.endsWith('/')) {
		return home + '/';
	}

	return home;
}

} // namespace

QString psAppDataPath() {
	// Previously we used ~/.TelegramDesktop, so look there first.
	// If we find data there, we should still use it.
	auto home = getHomeDir();
	if (!home.isEmpty()) {
		auto oldPath = home + qsl(".TelegramDesktop/");
		auto oldSettingsBase = oldPath + qsl("tdata/settings");
		if (QFile::exists(oldSettingsBase + '0')
			|| QFile::exists(oldSettingsBase + '1')
			|| QFile::exists(oldSettingsBase + 's')) {
			return oldPath;
		}
	}

	return QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + '/';
}

void psDoCleanup() {
	try {
		psAutoStart(false, true);
		psSendToMenu(false, true);
	} catch (...) {
	}
}

int psCleanup() {
	psDoCleanup();
	return 0;
}

void psDoFixPrevious() {
}

int psFixPrevious() {
	psDoFixPrevious();
	return 0;
}

namespace Platform {

void start() {
	PlatformThemes = QString::fromUtf8(qgetenv("QT_QPA_PLATFORMTHEME"))
		.split(':', base::QStringSkipEmptyParts);

	LOG(("Launcher filename: %1").arg(GetLauncherFilename()));

	qputenv("PULSE_PROP_application.name", AppName.utf8());
	qputenv("PULSE_PROP_application.icon_name", GetIconName().toLatin1());

	// if gtk integration and qgtk3/qgtk2 platformtheme (or qgtk2 style)
	// is used at the same time, the app will crash
	if (GtkIntegration::Instance()
		&& !IsStaticBinary()
		&& !qEnvironmentVariableIsSet(
			kIgnoreGtkIncompatibility.utf8())) {
		g_warning(
			"Unfortunately, GTK integration "
			"conflicts with qgtk2 platformtheme and style. "
			"Therefore, QT_QPA_PLATFORMTHEME "
			"and QT_STYLE_OVERRIDE will be unset.");

		g_message(
			"This can be ignored by setting %s environment variable "
			"to any value, however, if qgtk2 theme or style is used, "
			"this will lead to a crash.",
			kIgnoreGtkIncompatibility.utf8().constData());

		g_message(
			"GTK integration can be disabled by setting %s to any value. "
			"Keep in mind that this will lead to clipboard issues "
			"and tdesktop will be unable to get settings from GTK "
			"(such as decoration layout, dark mode & more).",
			internal::kDisableGtkIntegration.utf8().constData());

		qunsetenv("QT_QPA_PLATFORMTHEME");
		qunsetenv("QT_STYLE_OVERRIDE");

		// Don't allow qgtk3 to init gtk earlier than us
		if (DesktopEnvironment::IsGtkBased()) {
			QApplication::setDesktopSettingsAware(false);
		}
	}

	if (!GtkIntegration::Instance()) {
		g_warning(
			"GTK integration was disabled on build or in runtime. "
			"This will lead to clipboard issues and a lack of some features "
			"(like Auto-Night Mode or system window controls layout).");
	}

#ifdef DESKTOP_APP_USE_PACKAGED_RLOTTIE
	g_warning(
		"Application has been built with foreign rlottie, "
		"animated emojis won't be colored to the selected pack.");
#endif // DESKTOP_APP_USE_PACKAGED_RLOTTIE

#ifdef DESKTOP_APP_USE_PACKAGED_FONTS
	g_warning(
		"Application was built without embedded fonts, "
		"this may lead to font issues.");
#endif // DESKTOP_APP_USE_PACKAGED_FONTS

	if (AreQtPluginsBundled()) {
		qputenv("QT_WAYLAND_DECORATION", "material");
	}

#ifndef DESKTOP_APP_DISABLE_DBUS_INTEGRATION
	// this can give us a chance to use
	// a proper file dialog for current session
	DEBUG_LOG(("Checking for XDG Desktop Portal..."));
	if (IsXDGDesktopPortalPresent()) {
		DEBUG_LOG(("XDG Desktop Portal is present!"));
		if (UseXDGDesktopPortal()) {
			LOG(("Using XDG Desktop Portal."));
			qputenv("QT_QPA_PLATFORMTHEME", "xdgdesktopportal");
		} else {
			DEBUG_LOG(("Not using XDG Desktop Portal."));
		}
	} else {
		DEBUG_LOG(("XDG Desktop Portal is not present :("));
	}

	// IBus has changed its socket path several times
	// and each change should be synchronized with Qt.
	// Moreover, the last time Qt changed the path,
	// they didn't introduce a fallback to the old path
	// and made the new Qt incompatible with IBus from older distributions.
	// Since tdesktop is distributed in static binary form,
	// it makes sense to use ibus portal whenever it present
	// to ensure compatibility with the maximum range of distributions.
	if (AreQtPluginsBundled()
		&& !InFlatpak()
		&& !InSnap()
		&& IsIBusPortalPresent()) {
		LOG(("IBus portal is present! Using it."));
		qputenv("IBUS_USE_PORTAL", "1");
	}
#endif // !DESKTOP_APP_DISABLE_DBUS_INTEGRATION
}

void finish() {
}

void InstallLauncher(bool force) {
	static const auto DisabledByEnv = qEnvironmentVariableIsSet(
		"TDESKTOP_DISABLE_DESKTOP_FILE_GENERATION");

	// don't update desktop file for alpha version or if updater is disabled
	if ((cAlphaVersion() || Core::UpdaterDisabled() || DisabledByEnv)
		&& !force) {
		return;
	}

	const auto applicationsPath = QStandardPaths::writableLocation(
		QStandardPaths::ApplicationsLocation) + '/';

	GenerateDesktopFile(applicationsPath, qsl("-- %u"));

	const auto icons = QStandardPaths::writableLocation(
		QStandardPaths::GenericDataLocation) + qsl("/icons/");

	if (!QDir(icons).exists()) QDir().mkpath(icons);

	const auto icon = icons + kIconName.utf16() + qsl(".png");
	auto iconExists = QFile::exists(icon);
	if (Local::oldSettingsVersion() < 10021 && iconExists) {
		// Icon was changed.
		if (QFile::remove(icon)) {
			iconExists = false;
		}
	}
	if (!iconExists) {
		if (QFile::copy(qsl(":/gui/art/logo_256.png"), icon)) {
			DEBUG_LOG(("App Info: Icon copied to '%1'").arg(icon));
		}
	}

	RunShellCommand("update-desktop-database", {
		applicationsPath
	});
}

void RegisterCustomScheme(bool force) {
	if (cExeName().isEmpty()) {
		return;
	}

	GError *error = nullptr;

	const auto neededCommandlineBuilder = qsl("%1 -workdir %2 --").arg(
		QString(EscapeShell(QFile::encodeName(cExeDir() + cExeName()))),
		QString(EscapeShell(QFile::encodeName(cWorkingDir()))));

	const auto neededCommandline = qsl("%1 %u")
		.arg(neededCommandlineBuilder);

	auto currentAppInfo = g_app_info_get_default_for_type(
		kHandlerTypeName.utf8(),
		true);

	if (currentAppInfo) {
		const auto currentCommandline = QString(
			g_app_info_get_commandline(currentAppInfo));

		g_object_unref(currentAppInfo);

		if (currentCommandline == neededCommandline) {
			return;
		}
	}

	auto registeredAppInfoList = g_app_info_get_recommended_for_type(
		kHandlerTypeName.utf8());

	for (auto l = registeredAppInfoList; l != nullptr; l = l->next) {
		const auto currentRegisteredAppInfo = reinterpret_cast<GAppInfo*>(
			l->data);

		const auto currentAppInfoId = QString(
			g_app_info_get_id(currentRegisteredAppInfo));

		const auto currentCommandline = QString(
			g_app_info_get_commandline(currentRegisteredAppInfo));

		if (currentCommandline == neededCommandline
			&& currentAppInfoId.startsWith(qsl("userapp-"))) {
			g_app_info_delete(currentRegisteredAppInfo);
		}
	}

	if (registeredAppInfoList) {
		g_list_free_full(registeredAppInfoList, g_object_unref);
	}

	auto newAppInfo = g_app_info_create_from_commandline(
		neededCommandlineBuilder.toUtf8(),
		AppName.utf8(),
		G_APP_INFO_CREATE_SUPPORTS_URIS,
		&error);

	if (newAppInfo) {
		g_app_info_set_as_default_for_type(
			newAppInfo,
			kHandlerTypeName.utf8(),
			&error);

		g_object_unref(newAppInfo);
	}

	if (error) {
		LOG(("App Error: %1").arg(error->message));
		g_error_free(error);
	}
}

PermissionStatus GetPermissionStatus(PermissionType type) {
	return PermissionStatus::Granted;
}

void RequestPermission(PermissionType type, Fn<void(PermissionStatus)> resultCallback) {
	resultCallback(PermissionStatus::Granted);
}

void OpenSystemSettingsForPermission(PermissionType type) {
}

bool OpenSystemSettings(SystemSettingsType type) {
	if (type == SystemSettingsType::Audio) {
		struct Command {
			QString command;
			QStringList arguments;
		};
		auto options = std::vector<Command>();
		const auto add = [&](const char *option, const char *arg = nullptr) {
			auto command = Command{ .command = option };
			if (arg) {
				command.arguments.push_back(arg);
			}
			options.push_back(std::move(command));
		};
		if (DesktopEnvironment::IsUnity()) {
			add("unity-control-center", "sound");
		} else if (DesktopEnvironment::IsKDE()) {
			add("kcmshell5", "kcm_pulseaudio");
			add("kcmshell4", "phonon");
		} else if (DesktopEnvironment::IsGnome()) {
			add("gnome-control-center", "sound");
		} else if (DesktopEnvironment::IsCinnamon()) {
			add("cinnamon-settings", "sound");
		} else if (DesktopEnvironment::IsMATE()) {
			add("mate-volume-control");
		}
		add("pavucontrol-qt");
		add("pavucontrol");
		add("alsamixergui");
		return ranges::any_of(options, [](const Command &command) {
			return QProcess::startDetached(command.command, command.arguments);
		});
	}
	return true;
}

namespace ThirdParty {

void start() {
	DEBUG_LOG(("Icon theme: %1").arg(QIcon::themeName()));
	DEBUG_LOG(("Fallback icon theme: %1").arg(QIcon::fallbackThemeName()));

	if (const auto integration = GtkIntegration::Instance()) {
		integration->load();
	}

	// wait for interface announce to know if native window frame is supported
	if (const auto waylandIntegration = WaylandIntegration::Instance()) {
		waylandIntegration->waitForInterfaceAnnounce();
	}

#ifndef DESKTOP_APP_DISABLE_DBUS_INTEGRATION
	NSWInstance = std::make_unique<internal::NotificationServiceWatcher>();
#endif // !DESKTOP_APP_DISABLE_DBUS_INTEGRATION
}

void finish() {
#ifndef DESKTOP_APP_DISABLE_DBUS_INTEGRATION
	NSWInstance = nullptr;
#endif // !DESKTOP_APP_DISABLE_DBUS_INTEGRATION
}

} // namespace ThirdParty

} // namespace Platform

void psNewVersion() {
	Platform::InstallLauncher();
	Platform::RegisterCustomScheme();
}

void psAutoStart(bool start, bool silent) {
	if (InFlatpak()) {
#ifndef DESKTOP_APP_DISABLE_DBUS_INTEGRATION
		PortalAutostart(start, silent);
#endif // !DESKTOP_APP_DISABLE_DBUS_INTEGRATION
	} else {
		const auto autostart = QStandardPaths::writableLocation(
			QStandardPaths::GenericConfigLocation)
			+ qsl("/autostart/");

		if (start) {
			GenerateDesktopFile(autostart, qsl("-autostart"), silent);
		} else {
			QFile::remove(autostart + GetLauncherFilename());
		}
	}
}

void psSendToMenu(bool send, bool silent) {
}

bool linuxMoveFile(const char *from, const char *to) {
	FILE *ffrom = fopen(from, "rb"), *fto = fopen(to, "wb");
	if (!ffrom) {
		if (fto) fclose(fto);
		return false;
	}
	if (!fto) {
		fclose(ffrom);
		return false;
	}
	static const int BufSize = 65536;
	char buf[BufSize];
	while (size_t size = fread(buf, 1, BufSize, ffrom)) {
		fwrite(buf, 1, size, fto);
	}

	struct stat fst; // from http://stackoverflow.com/questions/5486774/keeping-fileowner-and-permissions-after-copying-file-in-c
	//let's say this wont fail since you already worked OK on that fp
	if (fstat(fileno(ffrom), &fst) != 0) {
		fclose(ffrom);
		fclose(fto);
		return false;
	}
	//update to the same uid/gid
	if (fchown(fileno(fto), fst.st_uid, fst.st_gid) != 0) {
		fclose(ffrom);
		fclose(fto);
		return false;
	}
	//update the permissions
	if (fchmod(fileno(fto), fst.st_mode) != 0) {
		fclose(ffrom);
		fclose(fto);
		return false;
	}

	fclose(ffrom);
	fclose(fto);

	if (unlink(from)) {
		return false;
	}

	return true;
}

bool psLaunchMaps(const Data::LocationPoint &point) {
	return false;
}
