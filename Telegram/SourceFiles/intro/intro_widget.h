/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "mtproto/sender.h"
#include "ui/rp_widget.h"
#include "ui/effects/animations.h"
#include "window/window_lock_widgets.h"
#include "core/core_cloud_password.h"

namespace Main {
class Account;
} // namespace Main

namespace Ui {
class IconButton;
class RoundButton;
class LinkButton;
class FlatLabel;
template <typename Widget>
class FadeWrap;
} // namespace Ui

namespace Window {
class ConnectionState;
} // namespace Window

namespace Intro {
namespace details {

enum class CallStatus {
	Waiting,
	Calling,
	Called,
	Disabled,
};

struct Data {
	QString country;
	QString phone;
	QByteArray phoneHash;

	CallStatus callStatus = CallStatus::Disabled;
	int callTimeout = 0;

	int codeLength = 5;
	bool codeByTelegram = false;

	Core::CloudPasswordCheckRequest pwdRequest;
	bool hasRecovery = false;
	QString pwdHint;
	bool pwdNotEmptyPassport = false;

	Window::TermsLock termsLock;

	base::Observable<void> updated;

};

enum class Direction {
	Back,
	Forward,
	Replace,
};

class Step;

} // namespace details

class Widget : public Ui::RpWidget, private base::Subscriber {
public:
	Widget(QWidget *parent, not_null<Main::Account*> account);

	void showAnimated(const QPixmap &bgAnimCache, bool back = false);

	void setInnerFocus();

	~Widget();

protected:
	void paintEvent(QPaintEvent *e) override;
	void resizeEvent(QResizeEvent *e) override;
	void keyPressEvent(QKeyEvent *e) override;

private:
	void refreshLang();
	void animationCallback();
	void createLanguageLink();
	void checkUpdateStatus();
	void setupNextButton();
	void handleUpdates(const MTPUpdates &updates);
	void handleUpdate(const MTPUpdate &update);

	void updateControlsGeometry();
	[[nodiscard]] not_null<details::Data*> getData() {
		return &_data;
	}

	void fixOrder();
	void showControls();
	void hideControls();

	void showResetButton();
	void resetAccount();

	void showTerms();
	void acceptTerms(Fn<void()> callback);
	void hideAndDestroy(object_ptr<Ui::FadeWrap<Ui::RpWidget>> widget);

	[[nodiscard]] details::Step *getStep(int skip = 0) const {
		Expects(skip >= 0);
		Expects(skip < _stepHistory.size());

		return _stepHistory[_stepHistory.size() - skip - 1];
	}
	void historyMove(details::Direction direction);
	void moveToStep(details::Step *step, details::Direction direction);
	void appendStep(details::Step *step);

	void getNearestDC();
	void showTerms(Fn<void()> callback);

	const not_null<Main::Account*> _account;
	std::optional<MTP::Sender> _api;

	Ui::Animations::Simple _a_show;
	bool _showBack = false;
	QPixmap _cacheUnder, _cacheOver;

	std::vector<details::Step*> _stepHistory;
	rpl::lifetime _stepLifetime;

	details::Data _data;

	Ui::Animations::Simple _coverShownAnimation;
	int _nextTopFrom = 0;
	int _controlsTopFrom = 0;

	object_ptr<Ui::FadeWrap<Ui::IconButton>> _back;
	object_ptr<Ui::FadeWrap<Ui::RoundButton>> _update = { nullptr };
	object_ptr<Ui::FadeWrap<Ui::RoundButton>> _settings;

	object_ptr<Ui::FadeWrap<Ui::RoundButton>> _next;
	object_ptr<Ui::FadeWrap<Ui::LinkButton>> _changeLanguage = { nullptr };
	object_ptr<Ui::FadeWrap<Ui::RoundButton>> _resetAccount = { nullptr };
	object_ptr<Ui::FadeWrap<Ui::FlatLabel>> _terms = { nullptr };

	std::unique_ptr<Window::ConnectionState> _connecting;

	bool _nextShown = true;
	Ui::Animations::Simple _nextShownAnimation;

	mtpRequestId _resetRequest = 0;

};

} // namespace Intro
