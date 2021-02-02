/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "calls/calls_volume_item.h"

#include "calls/calls_group_common.h"
#include "ui/effects/animation_value.h"
#include "ui/effects/cross_line.h"
#include "ui/widgets/continuous_sliders.h"
#include "styles/style_calls.h"

#include "ui/paint/arcs.h"

namespace Calls {
namespace {

constexpr auto kMaxVolumePercent = 200;

const auto kSpeakerThreshold = std::vector<float>{
	10.0f / kMaxVolumePercent,
	50.0f / kMaxVolumePercent,
	150.0f / kMaxVolumePercent };

constexpr auto kVolumeStickedValues =
	std::array<std::pair<float64, float64>, 7>{{
		{ 25. / kMaxVolumePercent, 2. / kMaxVolumePercent },
		{ 50. / kMaxVolumePercent, 2. / kMaxVolumePercent },
		{ 75. / kMaxVolumePercent, 2. / kMaxVolumePercent },
		{ 100. / kMaxVolumePercent, 5. / kMaxVolumePercent },
		{ 125. / kMaxVolumePercent, 2. / kMaxVolumePercent },
		{ 150. / kMaxVolumePercent, 2. / kMaxVolumePercent },
		{ 175. / kMaxVolumePercent, 2. / kMaxVolumePercent },
	}};

QString VolumeString(int volumePercent) {
	return u"%1%"_q.arg(volumePercent);
}

} // namespace

MenuVolumeItem::MenuVolumeItem(
	not_null<RpWidget*> parent,
	const style::Menu &st,
	rpl::producer<Group::ParticipantState> participantState,
	int startVolume,
	int maxVolume,
	bool muted)
: Ui::Menu::ItemBase(parent, st)
, _maxVolume(maxVolume)
, _cloudMuted(muted)
, _localMuted(muted)
, _slider(base::make_unique_q<Ui::MediaSlider>(
	this,
	st::groupCallMenuVolumeSlider))
, _dummyAction(new QAction(parent))
, _st(st)
, _stCross(st::groupCallMuteCrossLine)
, _crossLineMute(std::make_unique<Ui::CrossLineAnimation>(_stCross, true))
, _arcs(std::make_unique<Ui::Paint::ArcsAnimation>(
	st::groupCallSpeakerArcsAnimation,
	kSpeakerThreshold,
	_localMuted ? 0. : (startVolume / float(maxVolume)),
	Ui::Paint::ArcsAnimation::HorizontalDirection::Right)) {

	initResizeHook(parent->sizeValue());
	enableMouseSelecting();
	enableMouseSelecting(_slider.get());

	_slider->setAlwaysDisplayMarker(true);

	sizeValue(
	) | rpl::start_with_next([=](const QSize &size) {
		const auto geometry = QRect(QPoint(), size);
		_itemRect = geometry - _st.itemPadding;
		_speakerRect = QRect(_itemRect.topLeft(), _stCross.icon.size());
		_arcPosition = _speakerRect.center()
			+ QPoint(0, st::groupCallMenuSpeakerArcsSkip);
		_volumeRect = QRect(
			_arcPosition.x()
				+ st::groupCallMenuVolumeSkip
				+ _arcs->finishedWidth(),
			_speakerRect.y(),
			_st.itemStyle.font->width(VolumeString(kMaxVolumePercent)),
			_speakerRect.height());

		_slider->setGeometry(_itemRect
			- style::margins(0, contentHeight() / 2, 0, 0));
	}, lifetime());

	setCloudVolume(startVolume);

	paintRequest(
	) | rpl::start_with_next([=](const QRect &clip) {
		Painter p(this);

		const auto volume = _localMuted
			? 0
			: std::round(_slider->value() * kMaxVolumePercent);
		const auto muteProgress =
			_crossLineAnimation.value((!volume) ? 1. : 0.);

		const auto selected = isSelected();
		p.fillRect(clip, selected ? st.itemBgOver : st.itemBg);

		const auto mutePen = anim::color(
			unmuteColor(),
			muteColor(),
			muteProgress);
		p.setPen(mutePen);
		p.setFont(_st.itemStyle.font);
		p.drawText(_volumeRect, VolumeString(volume), style::al_left);

		_crossLineMute->paint(
			p,
			_speakerRect.topLeft(),
			muteProgress,
			(!muteProgress) ? std::nullopt : std::optional<QColor>(mutePen));

		{
			p.translate(_arcPosition);
			_arcs->paint(p);
		}
	}, lifetime());

	_slider->setChangeProgressCallback([=](float64 value) {
		const auto newMuted = (value == 0);
		if (_localMuted != newMuted) {
			_localMuted = newMuted;
			_toggleMuteLocallyRequests.fire_copy(newMuted);

			_crossLineAnimation.start(
				[=] { update(_speakerRect.united(_volumeRect)); },
				_localMuted ? 0. : 1.,
				_localMuted ? 1. : 0.,
				st::callPanelDuration);
		}
		if (value > 0) {
			_changeVolumeLocallyRequests.fire(value * _maxVolume);
		}
		update(_volumeRect);
		_arcs->setValue(value);
	});

	const auto returnVolume = [=] {
		_changeVolumeLocallyRequests.fire_copy(_cloudVolume);
	};

	_slider->setChangeFinishedCallback([=](float64 value) {
		const auto newVolume = std::round(value * _maxVolume);
		const auto muted = (value == 0);

		if (!_cloudMuted && muted) {
			returnVolume();
			_localMuted = true;
			_toggleMuteRequests.fire(true);
		}
		if (_cloudMuted && muted) {
			returnVolume();
		}
		if (_cloudMuted && !muted) {
			_waitingForUpdateVolume = true;
			_localMuted = false;
			_toggleMuteRequests.fire(false);
		}
		if (!_cloudMuted && !muted) {
			_changeVolumeRequests.fire_copy(newVolume);
		}
	});

	std::move(
		participantState
	) | rpl::start_with_next([=](const Group::ParticipantState &state) {
		const auto newMuted = state.mutedByMe;
		const auto newVolume = state.volume.value_or(0);

		_cloudMuted = _localMuted = newMuted;

		if (!newVolume) {
			return;
		}
		if (_waitingForUpdateVolume) {
			const auto localVolume =
				std::round(_slider->value() * _maxVolume);
			if ((localVolume != newVolume)
				&& (_cloudVolume == newVolume)) {
				_changeVolumeRequests.fire(int(localVolume));
			}
		} else {
			setCloudVolume(newVolume);
		}
		_waitingForUpdateVolume = false;
	}, lifetime());

	_slider->setAdjustCallback([=](float64 value) {
		for (const auto &snap : kVolumeStickedValues) {
			if (value > (snap.first - snap.second)
				&& value < (snap.first + snap.second)) {
				return snap.first;
			}
		}
		return value;
	});

	initArcsAnimation();
}

void MenuVolumeItem::initArcsAnimation() {
	const auto volumeLeftWas = lifetime().make_state<int>(0);
	const auto lastTime = lifetime().make_state<int>(0);
	_arcsAnimation.init([=](crl::time now) {
		_arcs->update(now);
		update(_speakerRect);

		const auto wasRect = _volumeRect;
		_volumeRect.moveLeft(anim::interpolate(
			*volumeLeftWas,
			_arcPosition.x()
				+ st::groupCallMenuVolumeSkip
				+ _arcs->finishedWidth(),
			std::clamp(
				(now - (*lastTime))
					/ float64(st::groupCallSpeakerArcsAnimation.duration),
				0.,
				1.)));
		update(_speakerRect.united(wasRect.united(_volumeRect)));
	});

	_arcs->startUpdateRequests(
	) | rpl::start_with_next([=] {
		if (!_arcsAnimation.animating()) {
			*volumeLeftWas = _volumeRect.left();
			*lastTime = crl::now();
			_arcsAnimation.start();
		}
	}, lifetime());

	_arcs->stopUpdateRequests(
	) | rpl::start_with_next([=] {
		_arcsAnimation.stop();
	}, lifetime());
}

QColor MenuVolumeItem::unmuteColor() const {
	return (isSelected()
		? _st.itemFgOver
		: isEnabled()
		? _st.itemFg
		: _st.itemFgDisabled)->c;
}

QColor MenuVolumeItem::muteColor() const {
	return (isSelected()
		? st::attentionButtonFgOver
		: st::attentionButtonFg)->c;
}

void MenuVolumeItem::setCloudVolume(int volume) {
	if (_cloudVolume == volume) {
		return;
	}
	_cloudVolume = volume;
	if (!_slider->isChanging()) {
		setSliderVolume(_cloudMuted ? 0. : volume);
	}
}

void MenuVolumeItem::setSliderVolume(int volume) {
	_slider->setValue(float64(volume) / _maxVolume);
	update(_volumeRect);
}

not_null<QAction*> MenuVolumeItem::action() const {
	return _dummyAction;
}

bool MenuVolumeItem::isEnabled() const {
	return true;
}

int MenuVolumeItem::contentHeight() const {
	return _st.itemPadding.top()
		+ _st.itemPadding.bottom()
		+ _stCross.icon.height() * 2;
}

rpl::producer<bool> MenuVolumeItem::toggleMuteRequests() const {
	return _toggleMuteRequests.events();
}

rpl::producer<bool> MenuVolumeItem::toggleMuteLocallyRequests() const {
	return _toggleMuteLocallyRequests.events();
}

rpl::producer<int> MenuVolumeItem::changeVolumeRequests() const {
	return _changeVolumeRequests.events();
}

rpl::producer<int> MenuVolumeItem::changeVolumeLocallyRequests() const {
	return _changeVolumeLocallyRequests.events();
}

} // namespace Calls
