/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "history/media/history_media_file.h"

#include "lang/lang_keys.h"
#include "layout.h"
#include "history/history_item.h"
#include "history/history.h"
#include "data/data_document.h"
#include "data/data_session.h"
#include "styles/style_history.h"

void HistoryFileMedia::clickHandlerActiveChanged(const ClickHandlerPtr &p, bool active) {
	if (p == _savel || p == _cancell) {
		if (active && !dataLoaded()) {
			ensureAnimation();
			_animation->a_thumbOver.start([this] { thumbAnimationCallback(); }, 0., 1., st::msgFileOverDuration);
		} else if (!active && _animation && !dataLoaded()) {
			_animation->a_thumbOver.start([this] { thumbAnimationCallback(); }, 1., 0., st::msgFileOverDuration);
		}
	}
}

void HistoryFileMedia::thumbAnimationCallback() {
	history()->owner().requestViewRepaint(_parent);
}

void HistoryFileMedia::clickHandlerPressedChanged(
		const ClickHandlerPtr &handler,
		bool pressed) {
	history()->owner().requestViewRepaint(_parent);
}

void HistoryFileMedia::setLinks(
		FileClickHandlerPtr &&openl,
		FileClickHandlerPtr &&savel,
		FileClickHandlerPtr &&cancell) {
	_openl = std::move(openl);
	_savel = std::move(savel);
	_cancell = std::move(cancell);
}

void HistoryFileMedia::refreshParentId(not_null<HistoryItem*> realParent) {
	const auto contextId = realParent->fullId();
	_openl->setMessageId(contextId);
	_savel->setMessageId(contextId);
	_cancell->setMessageId(contextId);
}

void HistoryFileMedia::setStatusSize(int newSize, int fullSize, int duration, qint64 realDuration) const {
	_statusSize = newSize;
	if (_statusSize == FileStatusSizeReady) {
		_statusText = (duration >= 0) ? formatDurationAndSizeText(duration, fullSize) : (duration < -1 ? formatGifAndSizeText(fullSize) : formatSizeText(fullSize));
	} else if (_statusSize == FileStatusSizeLoaded) {
		_statusText = (duration >= 0) ? formatDurationText(duration) : (duration < -1 ? qsl("GIF") : formatSizeText(fullSize));
	} else if (_statusSize == FileStatusSizeFailed) {
		_statusText = tr::lng_attach_failed(tr::now);
	} else if (_statusSize >= 0) {
		_statusText = formatDownloadText(_statusSize, fullSize);
	} else {
		_statusText = formatPlayedText(-_statusSize - 1, realDuration);
	}
}

void HistoryFileMedia::radialAnimationCallback(crl::time now) const {
	const auto updated = [&] {
		return _animation->radial.update(
			dataProgress(),
			dataFinished(),
			now);
	}();
	if (!anim::Disabled() || updated) {
		history()->owner().requestViewRepaint(_parent);
	}
	if (!_animation->radial.animating()) {
		checkAnimationFinished();
	}
}

void HistoryFileMedia::ensureAnimation() const {
	if (!_animation) {
		_animation = std::make_unique<AnimationData>([=](crl::time now) {
			radialAnimationCallback(now);
		});
	}
}

void HistoryFileMedia::checkAnimationFinished() const {
	if (_animation && !_animation->a_thumbOver.animating() && !_animation->radial.animating()) {
		if (dataLoaded()) {
			_animation.reset();
		}
	}
}
void HistoryFileMedia::setDocumentLinks(
		not_null<DocumentData*> document,
		not_null<HistoryItem*> realParent) {
	const auto context = realParent->fullId();
	setLinks(
		std::make_shared<DocumentOpenClickHandler>(document, context),
		std::make_shared<DocumentSaveClickHandler>(document, context),
		std::make_shared<DocumentCancelClickHandler>(document, context));
}

HistoryFileMedia::~HistoryFileMedia() = default;
