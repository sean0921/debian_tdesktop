/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "platform/linux/specific_linux.h"

#include "platform/linux/linux_libs.h"
#include "lang/lang_keys.h"
#include "mainwidget.h"
#include "mainwindow.h"
#include "platform/linux/linux_desktop_environment.h"
#include "platform/linux/file_utilities_linux.h"
#include "platform/platform_notifications_manager.h"
#include "storage/localstorage.h"
#include "core/crash_reports.h"
#include "core/update_checker.h"

#include <QtWidgets/QApplication>
#include <QtWidgets/QDesktopWidget>
#include <QtCore/QStandardPaths>
#include <QtCore/QProcess>
#include <QtCore/QVersionNumber>

#ifndef TDESKTOP_DISABLE_DBUS_INTEGRATION
#include <QtDBus/QDBusInterface>
#endif

#include <sys/stat.h>
#include <sys/types.h>
#include <cstdlib>
#include <unistd.h>
#include <dirent.h>
#include <pwd.h>

#include <iostream>

using namespace Platform;
using Platform::File::internal::EscapeShell;

namespace {

constexpr auto kDesktopFile = ":/misc/telegramdesktop.desktop"_cs;
constexpr auto kSnapLauncherDir = "/var/lib/snapd/desktop/applications/"_cs;

bool XDGDesktopPortalPresent = false;

#ifndef TDESKTOP_DISABLE_DBUS_INTEGRATION
void SandboxAutostart(bool autostart) {
	QVariantMap options;
	options["reason"] = tr::lng_settings_auto_start(tr::now);
	options["autostart"] = autostart;
	options["commandline"] = QStringList({
		cExeName(),
		qsl("-autostart")
	});
	options["dbus-activatable"] = false;

	const auto requestBackgroundReply = QDBusInterface(
		qsl("org.freedesktop.portal.Desktop"),
		qsl("/org/freedesktop/portal/desktop"),
		qsl("org.freedesktop.portal.Background")
	).call(qsl("RequestBackground"), QString(), options);

	if (requestBackgroundReply.type() == QDBusMessage::ErrorMessage) {
		LOG(("Flatpak autostart error: %1")
			.arg(requestBackgroundReply.errorMessage()));
	} else if (requestBackgroundReply.type() != QDBusMessage::ReplyMessage) {
		LOG(("Flatpak autostart error: invalid reply"));
	}
}
#endif

bool RunShellCommand(const QByteArray &command) {
	auto result = system(command.constData());
	if (result) {
		DEBUG_LOG(("App Error: command failed, code: %1, command (in utf8): %2").arg(result).arg(command.constData()));
		return false;
	}
	DEBUG_LOG(("App Info: command succeeded, command (in utf8): %1").arg(command.constData()));
	return true;
}

void FallbackFontConfig() {
#ifdef TDESKTOP_USE_FONT_CONFIG_FALLBACK
	const auto custom = cWorkingDir() + "tdata/fc-custom-1.conf";
	const auto finish = gsl::finally([&] {
		if (QFile(custom).exists()) {
			LOG(("Custom FONTCONFIG_FILE: ") + custom);
			qputenv("FONTCONFIG_FILE", QFile::encodeName(custom));
		}
	});

	QProcess process;
	process.setProcessChannelMode(QProcess::MergedChannels);
	process.start("fc-list", QStringList() << "--version");
	process.waitForFinished();
	if (process.exitCode() > 0) {
		LOG(("App Error: Could not start fc-list. Process exited with code: %1.").arg(process.exitCode()));
		return;
	}

	QString result(process.readAllStandardOutput());
	DEBUG_LOG(("Fontconfig version string: ") + result);

	QVersionNumber version = QVersionNumber::fromString(result.split("version ").last());
	if (version.isNull()) {
		LOG(("App Error: Could not get version from fc-list output."));
		return;
	}

	LOG(("Fontconfig version: %1.").arg(version.toString()));
	if (version < QVersionNumber::fromString("2.13")) {
		if (qgetenv("TDESKTOP_FORCE_CUSTOM_FONTCONFIG").isEmpty()) {
			return;
		}
	}

	QFile(":/fc/fc-custom.conf").copy(custom);
#endif // TDESKTOP_USE_FONT_CONFIG_FALLBACK
}

bool GenerateDesktopFile(const QString &targetPath, const QString &args) {
	DEBUG_LOG(("App Info: placing .desktop file to %1").arg(targetPath));
	if (!QDir(targetPath).exists()) QDir().mkpath(targetPath);

	const auto sourceFile = [&] {
		if (InSnap()) {
			return kSnapLauncherDir.utf16() + GetLauncherFilename();
		} else {
			return kDesktopFile.utf16();
		}
	}();

	const auto targetFile = targetPath + GetLauncherFilename();

	QString fileText;

	QFile source(sourceFile);
	if (source.open(QIODevice::ReadOnly)) {
		QTextStream s(&source);
		fileText = s.readAll();
		source.close();
	} else {
		LOG(("App Error: Could not open '%1' for read").arg(sourceFile));
		return false;
	}

	QFile target(targetFile);
	if (target.open(QIODevice::WriteOnly)) {
#ifdef DESKTOP_APP_USE_PACKAGED
		fileText = fileText.replace(
			QRegularExpression(qsl("^Exec=(.*) -- %u$"),
				QRegularExpression::MultilineOption),
			qsl("Exec=\\1")
				+ (args.isEmpty() ? QString() : ' ' + args));
#else
		fileText = fileText.replace(
			QRegularExpression(qsl("^TryExec=.*$"),
				QRegularExpression::MultilineOption),
			qsl("TryExec=")
				+ EscapeShell(QFile::encodeName(cExeDir() + cExeName())));
		fileText = fileText.replace(
			QRegularExpression(qsl("^Exec=.*$"),
				QRegularExpression::MultilineOption),
			qsl("Exec=")
				+ EscapeShell(QFile::encodeName(cExeDir() + cExeName()))
				+ (args.isEmpty() ? QString() : ' ' + args));
#endif
		target.write(fileText.toUtf8());
		target.close();

		DEBUG_LOG(("App Info: removing old .desktop file"));
		QFile(qsl("%1telegram.desktop").arg(targetPath)).remove();

		return true;
	} else {
		LOG(("App Error: Could not open '%1' for write").arg(targetFile));
		return false;
	}
}

} // namespace

namespace Platform {

void SetApplicationIcon(const QIcon &icon) {
	QApplication::setWindowIcon(icon);
}

bool InSandbox() {
	static const auto Sandbox = QFileInfo::exists(
		QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation)
			+ qsl("/flatpak-info"));
	return Sandbox;
}

bool InSnap() {
	static const auto Snap = qEnvironmentVariableIsSet("SNAP");
	return Snap;
}

bool IsXDGDesktopPortalPresent() {
	return XDGDesktopPortalPresent;
}

QString ProcessNameByPID(const QString &pid) {
	constexpr auto kMaxPath = 1024;
	char result[kMaxPath] = { 0 };
	auto count = readlink("/proc/" + pid.toLatin1() + "/exe", result, kMaxPath);
	if (count > 0) {
		auto filename = QFile::decodeName(result);
		auto deletedPostfix = qstr(" (deleted)");
		if (filename.endsWith(deletedPostfix) && !QFileInfo(filename).exists()) {
			filename.chop(deletedPostfix.size());
		}
		return filename;
	}

	return QString();
}

QString CurrentExecutablePath(int argc, char *argv[]) {
	const auto processName = ProcessNameByPID(qsl("self"));

	// Fallback to the first command line argument.
	return !processName.isEmpty()
		? processName
		: argc
			? QFile::decodeName(argv[0])
			: QString();
}

QString AppRuntimeDirectory() {
	static const auto RuntimeDirectory = [&] {
		auto runtimeDir = QStandardPaths::writableLocation(
			QStandardPaths::RuntimeLocation);

		if (InSandbox()) {
			runtimeDir += qsl("/app/")
				+ QString::fromLatin1(qgetenv("FLATPAK_ID"));
		}

		if (!QFileInfo::exists(runtimeDir)) { // non-systemd distros
			runtimeDir = QDir::tempPath();
		}

		if (runtimeDir.isEmpty()) {
			runtimeDir = qsl("/tmp/");
		}

		if (!runtimeDir.endsWith('/')) {
			runtimeDir += '/';
		}

		return runtimeDir;
	}();

	return RuntimeDirectory;
}

QString SingleInstanceLocalServerName(const QString &hash) {
	if (InSandbox() || InSnap()) {
		return AppRuntimeDirectory() + hash;
	} else {
		return AppRuntimeDirectory() + hash + '-' + cGUIDStr();
	}
}

QString GetLauncherBasename() {
	static const auto LauncherBasename = [&] {
		if (!InSnap()) {
			return qsl(MACRO_TO_STRING(TDESKTOP_LAUNCHER_BASENAME));
		}

		const auto snapNameKey =
			qEnvironmentVariableIsSet("SNAP_INSTANCE_NAME")
				? "SNAP_INSTANCE_NAME"
				: "SNAP_NAME";

		const auto result = qsl("%1_%2")
			.arg(QString::fromLatin1(qgetenv(snapNameKey)))
			.arg(cExeName());

		LOG(("SNAP Environment detected, launcher filename is %1.desktop")
			.arg(result));

		return result;
	}();

	return LauncherBasename;
}

QString GetLauncherFilename() {
	static const auto LauncherFilename = GetLauncherBasename()
		+ qsl(".desktop");
	return LauncherFilename;
}

} // namespace Platform

namespace {

QRect _monitorRect;
auto _monitorLastGot = 0LL;

} // namespace

QRect psDesktopRect() {
	auto tnow = crl::now();
	if (tnow > _monitorLastGot + 1000LL || tnow < _monitorLastGot) {
		_monitorLastGot = tnow;
		_monitorRect = QApplication::desktop()->availableGeometry(App::wnd());
	}
	return _monitorRect;
}

void psWriteDump() {
}

bool _removeDirectory(const QString &path) { // from http://stackoverflow.com/questions/2256945/removing-a-non-empty-directory-programmatically-in-c-or-c
	QByteArray pathRaw = QFile::encodeName(path);
	DIR *d = opendir(pathRaw.constData());
	if (!d) return false;

	while (struct dirent *p = readdir(d)) {
		/* Skip the names "." and ".." as we don't want to recurse on them. */
		if (!strcmp(p->d_name, ".") || !strcmp(p->d_name, "..")) continue;

		QString fname = path + '/' + p->d_name;
		QByteArray fnameRaw = QFile::encodeName(fname);
		struct stat statbuf;
		if (!stat(fnameRaw.constData(), &statbuf)) {
			if (S_ISDIR(statbuf.st_mode)) {
				if (!_removeDirectory(fname)) {
					closedir(d);
					return false;
				}
			} else {
				if (unlink(fnameRaw.constData())) {
					closedir(d);
					return false;
				}
			}
		}
	}
	closedir(d);

	return !rmdir(pathRaw.constData());
}

void psDeleteDir(const QString &dir) {
	_removeDirectory(dir);
}

void psActivateProcess(uint64 pid) {
//	objc_activateProgram();
}

namespace {

QString getHomeDir() {
	auto home = QDir::homePath();

	if (home != QDir::rootPath())
		return home + '/';

	struct passwd *pw = getpwuid(getuid());
	return (pw && pw->pw_dir && strlen(pw->pw_dir)) ? (QFile::decodeName(pw->pw_dir) + '/') : QString();
}

} // namespace

QString psAppDataPath() {
	// Previously we used ~/.TelegramDesktop, so look there first.
	// If we find data there, we should still use it.
	auto home = getHomeDir();
	if (!home.isEmpty()) {
		auto oldPath = home + qsl(".TelegramDesktop/");
		auto oldSettingsBase = oldPath + qsl("tdata/settings");
		if (QFile(oldSettingsBase + '0').exists() || QFile(oldSettingsBase + '1').exists()) {
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
	FallbackFontConfig();

#if !defined(TDESKTOP_DISABLE_DBUS_INTEGRATION) && defined(TDESKTOP_FORCE_GTK_FILE_DIALOG)
	LOG(("Checking for XDG Desktop Portal..."));
	XDGDesktopPortalPresent = QDBusInterface(
		"org.freedesktop.portal.Desktop",
		"/org/freedesktop/portal/desktop").isValid();

	// this can give us a chance to use a proper file dialog for current session
	if(XDGDesktopPortalPresent) {
		LOG(("XDG Desktop Portal is present!"));
		qputenv("QT_QPA_PLATFORMTHEME", "xdgdesktopportal");
	} else {
		LOG(("XDG Desktop Portal is not present :("));
	}
#endif // !TDESKTOP_DISABLE_DBUS_INTEGRATION && TDESKTOP_FORCE_GTK_FILE_DIALOG
}

void finish() {
}

void RegisterCustomScheme() {
#ifndef TDESKTOP_DISABLE_REGISTER_CUSTOM_SCHEME
	auto home = getHomeDir();
	if (home.isEmpty() || cAlphaVersion() || cExeName().isEmpty())
		return; // don't update desktop file for alpha version
	if (Core::UpdaterDisabled())
		return;

	const auto applicationsPath = QStandardPaths::writableLocation(
		QStandardPaths::ApplicationsLocation) + '/';

#ifndef TDESKTOP_DISABLE_DESKTOP_FILE_GENERATION
	GenerateDesktopFile(applicationsPath, qsl("-- %u"));

	const auto icons =
		QStandardPaths::writableLocation(
			QStandardPaths::GenericDataLocation)
		+ qsl("/icons/");

	if (!QDir(icons).exists()) QDir().mkpath(icons);

	const auto icon = icons + qsl("telegram.png");
	auto iconExists = QFile(icon).exists();
	if (Local::oldSettingsVersion() < 10021 && iconExists) {
		// Icon was changed.
		if (QFile(icon).remove()) {
			iconExists = false;
		}
	}
	if (!iconExists) {
		if (QFile(qsl(":/gui/art/logo_256.png")).copy(icon)) {
			DEBUG_LOG(("App Info: Icon copied to 'tdata'"));
		}
	}
#endif // !TDESKTOP_DISABLE_DESKTOP_FILE_GENERATION

	RunShellCommand("update-desktop-database "
		+ EscapeShell(QFile::encodeName(applicationsPath)));

	RunShellCommand("xdg-mime default "
		+ GetLauncherFilename().toLatin1()
		+ " x-scheme-handler/tg");
#endif // !TDESKTOP_DISABLE_REGISTER_CUSTOM_SCHEME
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
		auto options = std::vector<QString>();
		const auto add = [&](const char *option) {
			options.emplace_back(option);
		};
		if (DesktopEnvironment::IsUnity()) {
			add("unity-control-center sound");
		} else if (DesktopEnvironment::IsKDE()) {
			add("kcmshell5 kcm_pulseaudio");
			add("kcmshell4 phonon");
		} else if (DesktopEnvironment::IsGnome()) {
			add("gnome-control-center sound");
		} else if (DesktopEnvironment::IsMATE()) {
			add("mate-volume-control");
		}
		add("pavucontrol-qt");
		add("pavucontrol");
		add("alsamixergui");
		return ranges::find_if(options, [](const QString &command) {
			return QProcess::startDetached(command);
		}) != end(options);
	}
	return true;
}

namespace ThirdParty {

void start() {
	Libs::start();
	MainWindow::LibsLoaded();
}

void finish() {
}

} // namespace ThirdParty

} // namespace Platform

void psNewVersion() {
	Platform::RegisterCustomScheme();
}

bool psShowOpenWithMenu(int x, int y, const QString &file) {
	return false;
}

void psAutoStart(bool start, bool silent) {
	auto home = getHomeDir();
	if (home.isEmpty() || cAlphaVersion() || cExeName().isEmpty())
		return;

	if (InSandbox()) {
#ifndef TDESKTOP_DISABLE_DBUS_INTEGRATION
		SandboxAutostart(start);
#endif
	} else {
		const auto autostart = [&] {
			if (InSnap()) {
				QDir realHomeDir(home);
				realHomeDir.cd(qsl("../../.."));

				return realHomeDir
					.absoluteFilePath(qsl(".config/autostart/"));
			} else {
				return QStandardPaths::writableLocation(
					QStandardPaths::GenericConfigLocation)
				+ qsl("/autostart/");
			}
		}();

		if (start) {
			GenerateDesktopFile(autostart, qsl("-autostart"));
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
