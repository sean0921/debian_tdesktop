/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "history/media/history_media_call.h"

#include "lang/lang_keys.h"
#include "layout.h"
#include "history/history.h"
#include "history/history_item.h"
#include "history/view/history_view_element.h"
#include "history/view/history_view_cursor_state.h"
#include "calls/calls_instance.h"
#include "data/data_media_types.h"
#include "styles/style_history.h"

namespace {

using TextState = HistoryView::TextState;

} // namespace

HistoryCall::HistoryCall(
	not_null<Element*> parent,
	not_null<Data::Call*> call)
: HistoryMedia(parent) {
	_duration = call->duration;
	_reason = call->finishReason;

	const auto item = parent->data();
	_text = Data::MediaCall::Text(item, _reason);
	_status = parent->dateTime().time().toString(cTimeFormat());
	if (_duration) {
		if (_reason != FinishReason::Missed
			&& _reason != FinishReason::Busy) {
			_status = lng_call_duration_info(
				lt_time,
				_status,
				lt_duration,
				formatDurationWords(_duration));
		} else {
			_duration = 0;
		}
	}
}

QSize HistoryCall::countOptimalSize() {
	const auto user = _parent->data()->history()->peer->asUser();
	_link = std::make_shared<LambdaClickHandler>([=] {
		if (user) {
			Calls::Current().startOutgoingCall(user);
		}
	});

	auto maxWidth = st::historyCallWidth;
	auto minHeight = st::historyCallHeight;
	if (!isBubbleTop()) {
		minHeight -= st::msgFileTopMinus;
	}
	return { maxWidth, minHeight };
}

void HistoryCall::draw(Painter &p, const QRect &r, TextSelection selection, crl::time ms) const {
	if (width() < st::msgPadding.left() + st::msgPadding.right() + 1) return;
	auto paintx = 0, painty = 0, paintw = width(), painth = height();

	auto outbg = _parent->hasOutLayout();
	auto selected = (selection == FullSelection);

	accumulate_min(paintw, maxWidth());

	auto nameleft = 0, nametop = 0, nameright = 0, statustop = 0;
	auto topMinus = isBubbleTop() ? 0 : st::msgFileTopMinus;

	nameleft = st::historyCallLeft;
	nametop = st::historyCallTop - topMinus;
	nameright = st::msgFilePadding.left();
	statustop = st::historyCallStatusTop - topMinus;

	auto namewidth = paintw - nameleft - nameright;

	p.setFont(st::semiboldFont);
	p.setPen(outbg ? (selected ? st::historyFileNameOutFgSelected : st::historyFileNameOutFg) : (selected ? st::historyFileNameInFgSelected : st::historyFileNameInFg));
	p.drawTextLeft(nameleft, nametop, paintw, _text);

	auto statusleft = nameleft;
	auto missed = (_reason == FinishReason::Missed || _reason == FinishReason::Busy);
	auto &arrow = outbg ? (selected ? st::historyCallArrowOutSelected : st::historyCallArrowOut) : missed ? (selected ? st::historyCallArrowMissedInSelected : st::historyCallArrowMissedIn) : (selected ? st::historyCallArrowInSelected : st::historyCallArrowIn);
	arrow.paint(p, statusleft + st::historyCallArrowPosition.x(), statustop + st::historyCallArrowPosition.y(), paintw);
	statusleft += arrow.width() + st::historyCallStatusSkip;

	auto &statusFg = outbg ? (selected ? st::mediaOutFgSelected : st::mediaOutFg) : (selected ? st::mediaInFgSelected : st::mediaInFg);
	p.setFont(st::normalFont);
	p.setPen(statusFg);
	p.drawTextLeft(statusleft, statustop, paintw, _status);

	auto &icon = outbg ? (selected ? st::historyCallOutIconSelected : st::historyCallOutIcon) : (selected ? st::historyCallInIconSelected : st::historyCallInIcon);
	icon.paint(p, paintw - st::historyCallIconPosition.x() - icon.width(), st::historyCallIconPosition.y() - topMinus, paintw);
}

TextState HistoryCall::textState(QPoint point, StateRequest request) const {
	auto result = TextState(_parent);
	if (QRect(0, 0, width(), height()).contains(point)) {
		result.link = _link;
		return result;
	}
	return result;
}
