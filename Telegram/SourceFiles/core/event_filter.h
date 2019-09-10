/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

namespace Core {

class EventFilter : public QObject {
public:
	enum Result {
		Continue,
		Cancel,
	};

	EventFilter(
		not_null<QObject*> parent,
		not_null<QObject*> object,
		Fn<Result(not_null<QEvent*>)> filter);

protected:
	bool eventFilter(QObject *watched, QEvent *event);

private:
	Fn<Result(not_null<QEvent*>)> _filter;

};

not_null<QObject*> InstallEventFilter(
	not_null<QObject*> object,
	Fn<EventFilter::Result(not_null<QEvent*>)> filter);

not_null<QObject*> InstallEventFilter(
	not_null<QObject*> context,
	not_null<QObject*> object,
	Fn<EventFilter::Result(not_null<QEvent*>)> filter);

} // namespace Core
