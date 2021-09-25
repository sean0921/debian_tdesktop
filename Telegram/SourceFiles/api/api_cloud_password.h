/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "mtproto/sender.h"

namespace Core {
struct CloudPasswordState;
} // namespace Core

class ApiWrap;

namespace Main {
class Session;
} // namespace Main

namespace Api {

class CloudPassword final {
public:
	using ResetRetryDate = int;
	explicit CloudPassword(not_null<ApiWrap*> api);

	void reload();
	void clearUnconfirmedPassword();
	rpl::producer<Core::CloudPasswordState> state() const;
	std::optional<Core::CloudPasswordState> stateCurrent() const;

	rpl::producer<ResetRetryDate, QString> resetPassword();
	rpl::producer<rpl::no_value, QString> cancelResetPassword();

private:
	MTP::Sender _api;
	mtpRequestId _requestId = 0;
	std::unique_ptr<Core::CloudPasswordState> _state;
	rpl::event_stream<Core::CloudPasswordState> _stateChanges;

};

} // namespace Api
