/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "window/notifications_manager.h"
#include "base/timer.h"

namespace Window {
namespace Notifications {

class CachedUserpics : public QObject {
	Q_OBJECT

public:
	enum class Type {
		Rounded,
		Circled,
	};
	CachedUserpics(Type type);

	QString get(const InMemoryKey &key, PeerData *peer);

	~CachedUserpics();

private slots:
	void onClear();

private:
	void clearInMs(int ms);
	crl::time clear(crl::time ms);

	Type _type = Type::Rounded;
	struct Image {
		crl::time until;
		QString path;
	};
	using Images = QMap<InMemoryKey, Image>;
	Images _images;
	bool _someSavedFlag = false;
	base::Timer _clearTimer;

};

} // namesapce Notifications
} // namespace Window
