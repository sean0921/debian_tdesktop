/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "calls/calls_group_members.h"

#include "calls/calls_group_call.h"
#include "calls/calls_group_common.h"
#include "calls/calls_volume_item.h"
#include "data/data_channel.h"
#include "data/data_chat.h"
#include "data/data_user.h"
#include "data/data_changes.h"
#include "data/data_group_call.h"
#include "data/data_peer_values.h" // Data::CanWriteValue.
#include "data/data_session.h" // Data::Session::invitedToCallUsers.
#include "settings/settings_common.h" // Settings::CreateButton.
#include "ui/paint/arcs.h"
#include "ui/paint/blobs.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/scroll_area.h"
#include "ui/widgets/popup_menu.h"
#include "ui/text/text_utilities.h"
#include "ui/effects/ripple_animation.h"
#include "ui/effects/cross_line.h"
#include "core/application.h" // Core::App().domain, Core::App().activeWindow.
#include "main/main_domain.h" // Core::App().domain().activate.
#include "main/main_session.h"
#include "base/timer.h"
#include "boxes/peers/edit_participants_box.h" // SubscribeToMigration.
#include "lang/lang_keys.h"
#include "window/window_controller.h" // Controller::sessionController.
#include "window/window_session_controller.h"
#include "styles/style_calls.h"

namespace Calls {
namespace {

constexpr auto kBlobsEnterDuration = crl::time(250);
constexpr auto kLevelDuration = 100. + 500. * 0.23;
constexpr auto kBlobScale = 0.605;
constexpr auto kMinorBlobFactor = 0.9f;
constexpr auto kUserpicMinScale = 0.8;
constexpr auto kMaxLevel = 1.;
constexpr auto kWideScale = 5;

const auto kSpeakerThreshold = std::vector<float>{
	Group::kDefaultVolume * 0.1f / Group::kMaxVolume,
	Group::kDefaultVolume * 0.9f / Group::kMaxVolume };

constexpr auto kArcsStrokeRatio = 0.8;

auto RowBlobs() -> std::array<Ui::Paint::Blobs::BlobData, 2> {
	return { {
		{
			.segmentsCount = 6,
			.minScale = kBlobScale * kMinorBlobFactor,
			.minRadius = st::groupCallRowBlobMinRadius * kMinorBlobFactor,
			.maxRadius = st::groupCallRowBlobMaxRadius * kMinorBlobFactor,
			.speedScale = 1.,
			.alpha = .5,
		},
		{
			.segmentsCount = 8,
			.minScale = kBlobScale,
			.minRadius = (float)st::groupCallRowBlobMinRadius,
			.maxRadius = (float)st::groupCallRowBlobMaxRadius,
			.speedScale = 1.,
			.alpha = .2,
		},
	} };
}

class Row;

class RowDelegate {
public:
	virtual bool rowCanMuteMembers() = 0;
	virtual void rowUpdateRow(not_null<Row*> row) = 0;
	virtual void rowPaintIcon(
		Painter &p,
		QRect rect,
		float64 speaking,
		float64 active,
		float64 muted,
		bool mutedByMe) = 0;
};

class Row final : public PeerListRow {
public:
	Row(not_null<RowDelegate*> delegate, not_null<UserData*> user);

	enum class State {
		Active,
		Inactive,
		Muted,
		MutedByMe,
		Invited,
	};

	void setSkipLevelUpdate(bool value);
	void updateState(const Data::GroupCall::Participant *participant);
	void updateLevel(float level);
	void updateBlobAnimation(crl::time now);
	[[nodiscard]] State state() const {
		return _state;
	}
	[[nodiscard]] uint32 ssrc() const {
		return _ssrc;
	}
	[[nodiscard]] bool sounding() const {
		return _sounding;
	}
	[[nodiscard]] bool speaking() const {
		return _speaking;
	}
	[[nodiscard]] int volume() const {
		return _volume;
	}

	void addActionRipple(QPoint point, Fn<void()> updateCallback) override;
	void stopLastActionRipple() override;

	int nameIconWidth() const override {
		return 0;
	}
	QSize actionSize() const override {
		return QSize(
			st::groupCallActiveButton.width,
			st::groupCallActiveButton.height);
	}
	bool actionDisabled() const override {
		return peer()->isSelf()
			|| (_state == State::Invited)
			|| !_delegate->rowCanMuteMembers();
	}
	QMargins actionMargins() const override {
		return QMargins(
			0,
			0,
			st::groupCallMemberButtonSkip,
			0);
	}
	void paintAction(
		Painter &p,
		int x,
		int y,
		int outerWidth,
		bool selected,
		bool actionSelected) override;

	auto generatePaintUserpicCallback() -> PaintRoundImageCallback override;

	void paintStatusText(
		Painter &p,
		const style::PeerListItem &st,
		int x,
		int y,
		int availableWidth,
		int outerWidth,
		bool selected) override;

private:
	struct BlobsAnimation {
		BlobsAnimation(
			std::vector<Ui::Paint::Blobs::BlobData> blobDatas,
			float levelDuration,
			float maxLevel)
		: blobs(std::move(blobDatas), levelDuration, maxLevel) {
			style::PaletteChanged(
			) | rpl::start_with_next([=] {
				userpicCache = QImage();
			}, lifetime);
		}

		Ui::Paint::Blobs blobs;
		crl::time lastTime = 0;
		crl::time lastSoundingUpdateTime = 0;
		float64 enter = 0.;

		QImage userpicCache;
		InMemoryKey userpicKey;

		rpl::lifetime lifetime;
	};

	struct StatusIcon {
		StatusIcon(float volume)
		: speaker(st::groupCallStatusSpeakerIcon)
		, arcs(std::make_unique<Ui::Paint::ArcsAnimation>(
			st::groupCallStatusSpeakerArcsAnimation,
			kSpeakerThreshold,
			volume,
			Ui::Paint::ArcsAnimation::Direction::Right)) {
		}
		const style::icon &speaker;
		const std::unique_ptr<Ui::Paint::ArcsAnimation> arcs;
		int arcsWidth = 0;

		rpl::lifetime lifetime;
	};

	int statusIconWidth() const;
	int statusIconHeight() const;
	void paintStatusIcon(
		Painter &p,
		const style::PeerListItem &st,
		const style::font &font,
		bool selected);

	void refreshStatus() override;
	void setSounding(bool sounding);
	void setSpeaking(bool speaking);
	void setState(State state);
	void setSsrc(uint32 ssrc);
	void setVolume(int volume);

	void ensureUserpicCache(
		std::shared_ptr<Data::CloudImageView> &view,
		int size);

	const not_null<RowDelegate*> _delegate;
	State _state = State::Inactive;
	std::unique_ptr<Ui::RippleAnimation> _actionRipple;
	std::unique_ptr<BlobsAnimation> _blobsAnimation;
	std::unique_ptr<StatusIcon> _statusIcon;
	Ui::Animations::Simple _speakingAnimation; // For gray-red/green icon.
	Ui::Animations::Simple _mutedAnimation; // For gray/red icon.
	Ui::Animations::Simple _activeAnimation; // For icon cross animation.
	Ui::Animations::Simple _arcsAnimation; // For volume arcs animation.
	uint32 _ssrc = 0;
	int _volume = Group::kDefaultVolume;
	bool _sounding = false;
	bool _speaking = false;
	bool _skipLevelUpdate = false;

};

class MembersController final
	: public PeerListController
	, public RowDelegate
	, public base::has_weak_ptr {
public:
	MembersController(
		not_null<GroupCall*> call,
		not_null<QWidget*> menuParent);
	~MembersController();

	using MuteRequest = Group::MuteRequest;
	using VolumeRequest = Group::VolumeRequest;

	Main::Session &session() const override;
	void prepare() override;
	void rowClicked(not_null<PeerListRow*> row) override;
	void rowActionClicked(not_null<PeerListRow*> row) override;
	base::unique_qptr<Ui::PopupMenu> rowContextMenu(
		QWidget *parent,
		not_null<PeerListRow*> row) override;
	void loadMoreRows() override;

	[[nodiscard]] rpl::producer<int> fullCountValue() const {
		return _fullCount.value();
	}
	[[nodiscard]] rpl::producer<MuteRequest> toggleMuteRequests() const;
	[[nodiscard]] rpl::producer<VolumeRequest> changeVolumeRequests() const;
	[[nodiscard]] auto kickMemberRequests() const
		-> rpl::producer<not_null<UserData*>>;

	bool rowCanMuteMembers() override;
	void rowUpdateRow(not_null<Row*> row) override;
	void rowPaintIcon(
		Painter &p,
		QRect rect,
		float64 speaking,
		float64 active,
		float64 muted,
		bool mutedByMe) override;

private:
	[[nodiscard]] std::unique_ptr<Row> createSelfRow();
	[[nodiscard]] std::unique_ptr<Row> createRow(
		const Data::GroupCall::Participant &participant);
	[[nodiscard]] std::unique_ptr<Row> createInvitedRow(
		not_null<UserData*> user);

	void prepareRows(not_null<Data::GroupCall*> real);
	//void repaintByTimer();

	[[nodiscard]] base::unique_qptr<Ui::PopupMenu> createRowContextMenu(
		QWidget *parent,
		not_null<PeerListRow*> row);
	void addMuteActionsToContextMenu(
		not_null<Ui::PopupMenu*> menu,
		not_null<UserData*> user,
		bool userIsCallAdmin,
		not_null<Row*> row);
	void setupListChangeViewers(not_null<GroupCall*> call);
	void subscribeToChanges(not_null<Data::GroupCall*> real);
	void updateRow(
		const std::optional<Data::GroupCall::Participant> &was,
		const Data::GroupCall::Participant &now);
	void updateRow(
		not_null<Row*> row,
		const Data::GroupCall::Participant *participant);
	void removeRow(not_null<Row*> row);
	void updateRowLevel(not_null<Row*> row, float level);
	void checkSpeakingRowPosition(not_null<Row*> row);
	Row *findRow(not_null<UserData*> user) const;

	[[nodiscard]] Data::GroupCall *resolvedRealCall() const;
	void appendInvitedUsers();

	const base::weak_ptr<GroupCall> _call;
	not_null<PeerData*> _peer;

	// Use only resolvedRealCall() method, not this value directly.
	Data::GroupCall *_realCallRawValue = nullptr;
	uint64 _realId = 0;
	bool _prepared = false;

	rpl::event_stream<MuteRequest> _toggleMuteRequests;
	rpl::event_stream<VolumeRequest> _changeVolumeRequests;
	rpl::event_stream<not_null<UserData*>> _kickMemberRequests;
	rpl::variable<int> _fullCount = 1;
	rpl::variable<int> _fullCountMin = 0;
	rpl::variable<int> _fullCountMax = std::numeric_limits<int>::max();

	not_null<QWidget*> _menuParent;
	base::unique_qptr<Ui::PopupMenu> _menu;
	base::flat_set<not_null<PeerData*>> _menuCheckRowsAfterHidden;

	base::flat_map<uint32, not_null<Row*>> _soundingRowBySsrc;
	Ui::Animations::Basic _soundingAnimation;

	crl::time _soundingAnimationHideLastTime = 0;
	bool _skipRowLevelUpdate = false;

	Ui::CrossLineAnimation _inactiveCrossLine;
	Ui::CrossLineAnimation _coloredCrossLine;

	rpl::lifetime _lifetime;

};

Row::Row(not_null<RowDelegate*> delegate, not_null<UserData*> user)
: PeerListRow(user)
, _delegate(delegate) {
	refreshStatus();
}

void Row::setSkipLevelUpdate(bool value) {
	_skipLevelUpdate = value;
}

void Row::updateState(const Data::GroupCall::Participant *participant) {
	setSsrc(participant ? participant->ssrc : 0);
	setVolume(participant
		? participant->volume
		: Group::kDefaultVolume);
	if (!participant) {
		setState(State::Invited);
		setSounding(false);
		setSpeaking(false);
	} else if (!participant->muted
		|| (participant->sounding && participant->ssrc != 0)) {
		setState(participant->mutedByMe ? State::MutedByMe : State::Active);
		setSounding(participant->sounding && participant->ssrc != 0);
		setSpeaking(participant->speaking && participant->ssrc != 0);
	} else if (participant->canSelfUnmute) {
		setState(participant->mutedByMe ? State::MutedByMe : State::Inactive);
		setSounding(false);
		setSpeaking(false);
	} else {
		setState(State::Muted);
		setSounding(false);
		setSpeaking(false);
	}
}

void Row::setSpeaking(bool speaking) {
	if (_speaking == speaking) {
		return;
	}
	_speaking = speaking;
	_speakingAnimation.start(
		[=] { _delegate->rowUpdateRow(this); },
		_speaking ? 0. : 1.,
		_speaking ? 1. : 0.,
		st::widgetFadeDuration);

	if (!_speaking
		|| (_state == State::MutedByMe)
		|| (_state == State::Muted)) {
		_statusIcon = nullptr;
	} else if (!_statusIcon) {
		_statusIcon = std::make_unique<StatusIcon>(
			(float)_volume / Group::kMaxVolume);
		_statusIcon->arcs->setStrokeRatio(kArcsStrokeRatio);
		_statusIcon->arcsWidth = _statusIcon->arcs->finishedWidth();

		const auto wasArcsWidth = _statusIcon->lifetime.make_state<int>(0);

		_statusIcon->arcs->startUpdateRequests(
		) | rpl::start_with_next([=] {
			if (!_arcsAnimation.animating()) {
				*wasArcsWidth = _statusIcon->arcsWidth;
			}
			auto callback = [=](float64 value) {
				if (_statusIcon) {
					_statusIcon->arcs->update(crl::now());

					_statusIcon->arcsWidth = anim::interpolate(
						*wasArcsWidth,
						_statusIcon->arcs->finishedWidth(),
						value);
				}
				_delegate->rowUpdateRow(this);
			};
			_arcsAnimation.start(
				std::move(callback),
				0.,
				1.,
				st::groupCallSpeakerArcsAnimation.duration);
		}, _statusIcon->lifetime);
	}
}

void Row::setSounding(bool sounding) {
	if (_sounding == sounding) {
		return;
	}
	_sounding = sounding;
	if (!_sounding) {
		_blobsAnimation = nullptr;
	} else if (!_blobsAnimation) {
		_blobsAnimation = std::make_unique<BlobsAnimation>(
			RowBlobs() | ranges::to_vector,
			kLevelDuration,
			kMaxLevel);
		_blobsAnimation->lastTime = crl::now();
		updateLevel(GroupCall::kSpeakLevelThreshold);
	}
	refreshStatus();
}

void Row::setState(State state) {
	if (_state == state) {
		return;
	}
	const auto wasActive = (_state == State::Active);
	const auto wasMuted = (_state == State::Muted);
	_state = state;
	const auto nowActive = (_state == State::Active);
	const auto nowMuted = (_state == State::Muted);
	if (nowActive != wasActive) {
		_activeAnimation.start(
			[=] { _delegate->rowUpdateRow(this); },
			nowActive ? 0. : 1.,
			nowActive ? 1. : 0.,
			st::widgetFadeDuration);
	}
	if (nowMuted != wasMuted) {
		_mutedAnimation.start(
			[=] { _delegate->rowUpdateRow(this); },
			nowMuted ? 0. : 1.,
			nowMuted ? 1. : 0.,
			st::widgetFadeDuration);
	}
}

void Row::setSsrc(uint32 ssrc) {
	_ssrc = ssrc;
}

void Row::setVolume(int volume) {
	_volume = volume;
	if (_statusIcon) {
		_statusIcon->arcs->setValue((float)volume / Group::kMaxVolume);
	}
}

void Row::updateLevel(float level) {
	Expects(_blobsAnimation != nullptr);

	if (_skipLevelUpdate) {
		return;
	}

	if (level >= GroupCall::kSpeakLevelThreshold) {
		_blobsAnimation->lastSoundingUpdateTime = crl::now();
	}
	_blobsAnimation->blobs.setLevel(level);
}

void Row::updateBlobAnimation(crl::time now) {
	Expects(_blobsAnimation != nullptr);

	const auto soundingFinishesAt = _blobsAnimation->lastSoundingUpdateTime
		+ Data::GroupCall::kSoundStatusKeptFor;
	const auto soundingStartsFinishing = soundingFinishesAt
		- kBlobsEnterDuration;
	const auto soundingFinishes = (soundingStartsFinishing < now);
	if (soundingFinishes) {
		_blobsAnimation->enter = std::clamp(
			(soundingFinishesAt - now) / float64(kBlobsEnterDuration),
			0.,
			1.);
	} else if (_blobsAnimation->enter < 1.) {
		_blobsAnimation->enter = std::clamp(
			(_blobsAnimation->enter
				+ ((now - _blobsAnimation->lastTime)
					/ float64(kBlobsEnterDuration))),
			0.,
			1.);
	}
	_blobsAnimation->blobs.updateLevel(now - _blobsAnimation->lastTime);
	_blobsAnimation->lastTime = now;
}

void Row::ensureUserpicCache(
		std::shared_ptr<Data::CloudImageView> &view,
		int size) {
	Expects(_blobsAnimation != nullptr);

	const auto user = peer();
	const auto key = user->userpicUniqueKey(view);
	const auto full = QSize(size, size) * kWideScale * cIntRetinaFactor();
	auto &cache = _blobsAnimation->userpicCache;
	if (cache.isNull()) {
		cache = QImage(full, QImage::Format_ARGB32_Premultiplied);
		cache.setDevicePixelRatio(cRetinaFactor());
	} else if (_blobsAnimation->userpicKey == key
		&& cache.size() == full) {
		return;
	}
	_blobsAnimation->userpicKey = key;
	cache.fill(Qt::transparent);
	{
		Painter p(&cache);
		const auto skip = (kWideScale - 1) / 2 * size;
		user->paintUserpicLeft(p, view, skip, skip, kWideScale * size, size);
	}
}

auto Row::generatePaintUserpicCallback() -> PaintRoundImageCallback {
	auto userpic = ensureUserpicView();
	return [=](Painter &p, int x, int y, int outerWidth, int size) mutable {
		if (_blobsAnimation) {
			const auto mutedByMe = (_state == State::MutedByMe);
			const auto shift = QPointF(x + size / 2., y + size / 2.);
			auto hq = PainterHighQualityEnabler(p);
			p.translate(shift);
			const auto brush = mutedByMe
				? st::groupCallMemberMutedIcon->b
				: anim::brush(
					st::groupCallMemberInactiveStatus,
					st::groupCallMemberActiveStatus,
					_speakingAnimation.value(_speaking ? 1. : 0.));
			_blobsAnimation->blobs.paint(p, brush);
			p.translate(-shift);
			p.setOpacity(1.);

			const auto enter = _blobsAnimation->enter;
			const auto &minScale = kUserpicMinScale;
			const auto scaleUserpic = minScale
				+ (1. - minScale) * _blobsAnimation->blobs.currentLevel();
			const auto scale = scaleUserpic * enter + 1. * (1. - enter);
			if (scale == 1.) {
				peer()->paintUserpicLeft(p, userpic, x, y, outerWidth, size);
			} else {
				ensureUserpicCache(userpic, size);

				PainterHighQualityEnabler hq(p);

				auto target = QRect(
					x + (1 - kWideScale) / 2 * size,
					y + (1 - kWideScale) / 2 * size,
					kWideScale * size,
					kWideScale * size);
				auto shrink = anim::interpolate(
					(1 - kWideScale) / 2 * size,
					0,
					scale);
				auto margins = QMargins(shrink, shrink, shrink, shrink);
				p.drawImage(
					target.marginsAdded(margins),
					_blobsAnimation->userpicCache);
			}
		} else {
			peer()->paintUserpicLeft(p, userpic, x, y, outerWidth, size);
		}
	};
}

int Row::statusIconWidth() const {
	if (!_statusIcon) {
		return 0;
	}
	return _speaking
		? (_statusIcon->speaker.width() + _statusIcon->arcsWidth)
		: 0;
}

int Row::statusIconHeight() const {
	if (!_statusIcon) {
		return 0;
	}
	return _speaking
		? _statusIcon->speaker.height()
		: 0;
}

void Row::paintStatusIcon(
		Painter &p,
		const style::PeerListItem &st,
		const style::font &font,
		bool selected) {
	if (!_statusIcon) {
		return;
	}
	p.setFont(font);
	const auto color = (_speaking
		? st.statusFgActive
		: (selected ? st.statusFgOver : st.statusFg))->c;
	p.setPen(color);

	const auto speakerRect = QRect(
		st.statusPosition
			+ QPoint(0, (font->height - statusIconHeight()) / 2),
		_statusIcon->speaker.size());
	const auto arcPosition = speakerRect.topLeft()
		+ QPoint(
			speakerRect.width() - st::groupCallStatusSpeakerArcsSkip,
			speakerRect.height() / 2);

	const auto volume = std::round(_volume / 100.);
	_statusIcon->speaker.paint(
		p,
		speakerRect.topLeft(),
		speakerRect.width(),
		color);

	p.save();
	p.translate(arcPosition);
	_statusIcon->arcs->paint(p, color);
	p.restore();
}

void Row::paintStatusText(
		Painter &p,
		const style::PeerListItem &st,
		int x,
		int y,
		int availableWidth,
		int outerWidth,
		bool selected) {
	const auto &font = st::normalFont;
	if (_state != State::Invited && _state != State::MutedByMe) {
		p.save();
		paintStatusIcon(p, st, font, selected);
		const auto translatedWidth = statusIconWidth();
		p.translate(translatedWidth, 0);
		const auto guard = gsl::finally([&] { p.restore(); });
		PeerListRow::paintStatusText(
			p,
			st,
			x,
			y,
			availableWidth - translatedWidth,
			outerWidth,
			selected);
		return;
	}
	p.setFont(font);
	if (_state == State::MutedByMe) {
		p.setPen(st::groupCallMemberMutedIcon);
	} else {
		p.setPen(st::groupCallMemberNotJoinedStatus);
	}
	p.drawTextLeft(
		x,
		y,
		outerWidth,
		(_state == State::MutedByMe
			? tr::lng_group_call_muted_by_me_status(tr::now)
			: peer()->isSelf()
			? tr::lng_status_connecting(tr::now)
			: tr::lng_group_call_invited_status(tr::now)));
}

void Row::paintAction(
		Painter &p,
		int x,
		int y,
		int outerWidth,
		bool selected,
		bool actionSelected) {
	auto size = actionSize();
	const auto iconRect = style::rtlrect(
		x,
		y,
		size.width(),
		size.height(),
		outerWidth);
	if (_state == State::Invited) {
		_actionRipple = nullptr;
		st::groupCallMemberInvited.paint(
			p,
			QPoint(x, y) + st::groupCallMemberInvitedPosition,
			outerWidth);
		return;
	}
	if (_actionRipple) {
		_actionRipple->paint(
			p,
			x + st::groupCallActiveButton.rippleAreaPosition.x(),
			y + st::groupCallActiveButton.rippleAreaPosition.y(),
			outerWidth);
		if (_actionRipple->empty()) {
			_actionRipple.reset();
		}
	}
	const auto speaking = _speakingAnimation.value(_speaking ? 1. : 0.);
	const auto active = _activeAnimation.value(
		(_state == State::Active) ? 1. : 0.);
	const auto muted = _mutedAnimation.value(
		(_state == State::Muted) ? 1. : 0.);
	const auto mutedByMe = (_state == State::MutedByMe);
	_delegate->rowPaintIcon(p, iconRect, speaking, active, muted, mutedByMe);
}

void Row::refreshStatus() {
	setCustomStatus(
		(_speaking
			? u"%1% %2"_q
				.arg(std::round(_volume / 100.))
				.arg(tr::lng_group_call_active(tr::now))
			: tr::lng_group_call_inactive(tr::now)),
		_speaking);
}

void Row::addActionRipple(QPoint point, Fn<void()> updateCallback) {
	if (!_actionRipple) {
		auto mask = Ui::RippleAnimation::ellipseMask(QSize(
			st::groupCallActiveButton.rippleAreaSize,
			st::groupCallActiveButton.rippleAreaSize));
		_actionRipple = std::make_unique<Ui::RippleAnimation>(
			st::groupCallActiveButton.ripple,
			std::move(mask),
			std::move(updateCallback));
	}
	_actionRipple->add(point - st::groupCallActiveButton.rippleAreaPosition);
}

void Row::stopLastActionRipple() {
	if (_actionRipple) {
		_actionRipple->lastStop();
	}
}

MembersController::MembersController(
	not_null<GroupCall*> call,
	not_null<QWidget*> menuParent)
: _call(call)
, _peer(call->peer())
, _menuParent(menuParent)
, _inactiveCrossLine(st::groupCallMemberInactiveCrossLine)
, _coloredCrossLine(st::groupCallMemberColoredCrossLine) {
	setupListChangeViewers(call);

	style::PaletteChanged(
	) | rpl::start_with_next([=] {
		_inactiveCrossLine.invalidate();
		_coloredCrossLine.invalidate();
	}, _lifetime);

	rpl::combine(
		rpl::single(anim::Disabled()) | rpl::then(anim::Disables()),
		Core::App().appDeactivatedValue()
	) | rpl::start_with_next([=](bool animDisabled, bool deactivated) {
		const auto hide = !(!animDisabled && !deactivated);

		if (!(hide && _soundingAnimationHideLastTime)) {
			_soundingAnimationHideLastTime = hide ? crl::now() : 0;
		}
		for (const auto [_, row] : _soundingRowBySsrc) {
			if (hide) {
				updateRowLevel(row, 0.);
			}
			row->setSkipLevelUpdate(hide);
		}
		if (!hide && !_soundingAnimation.animating()) {
			_soundingAnimation.start();
		}
		_skipRowLevelUpdate = hide;
	}, _lifetime);

	_soundingAnimation.init([=](crl::time now) {
		if (const auto &last = _soundingAnimationHideLastTime; (last > 0)
			&& (now - last >= kBlobsEnterDuration)) {
			_soundingAnimation.stop();
			return false;
		}
		for (const auto [ssrc, row] : _soundingRowBySsrc) {
			row->updateBlobAnimation(now);
			delegate()->peerListUpdateRow(row);
		}
		return true;
	});
}

MembersController::~MembersController() {
	base::take(_menu);
}

void MembersController::setupListChangeViewers(not_null<GroupCall*> call) {
	const auto peer = call->peer();
	peer->session().changes().peerFlagsValue(
		peer,
		Data::PeerUpdate::Flag::GroupCall
	) | rpl::map([=] {
		return peer->groupCall();
	}) | rpl::filter([=](Data::GroupCall *real) {
		const auto call = _call.get();
		return call && real && (real->id() == call->id());
	}) | rpl::take(
		1
	) | rpl::start_with_next([=](not_null<Data::GroupCall*> real) {
		subscribeToChanges(real);
	}, _lifetime);

	call->stateValue(
	) | rpl::start_with_next([=] {
		const auto call = _call.get();
		const auto real = peer->groupCall();
		if (call && real && (real->id() == call->id())) {
			//updateRow(channel->session().user());
		}
	}, _lifetime);

	call->levelUpdates(
	) | rpl::start_with_next([=](const LevelUpdate &update) {
		const auto i = _soundingRowBySsrc.find(update.ssrc);
		if (i != end(_soundingRowBySsrc)) {
			updateRowLevel(i->second, update.value);
		}
	}, _lifetime);
}

void MembersController::subscribeToChanges(not_null<Data::GroupCall*> real) {
	_realCallRawValue = real;
	_realId = real->id();

	_fullCount = rpl::combine(
		real->fullCountValue(),
		_fullCountMin.value(),
		_fullCountMax.value()
	) | rpl::map([](int value, int min, int max) {
		return std::max(std::clamp(value, min, max), 1);
	});

	real->participantsSliceAdded(
	) | rpl::start_with_next([=] {
		prepareRows(real);
	}, _lifetime);

	using Update = Data::GroupCall::ParticipantUpdate;
	real->participantUpdated(
	) | rpl::start_with_next([=](const Update &update) {
		Expects(update.was.has_value() || update.now.has_value());

		const auto user = update.was ? update.was->user : update.now->user;
		if (!update.now) {
			if (const auto row = findRow(user)) {
				const auto owner = &user->owner();
				if (user->isSelf()) {
					updateRow(row, nullptr);
				} else {
					removeRow(row);
					delegate()->peerListRefreshRows();
				}
			}
		} else {
			updateRow(update.was, *update.now);
		}
	}, _lifetime);

	if (_prepared) {
		appendInvitedUsers();
	}
}

void MembersController::appendInvitedUsers() {
	for (const auto user : _peer->owner().invitedToCallUsers(_realId)) {
		if (auto row = createInvitedRow(user)) {
			delegate()->peerListAppendRow(std::move(row));
		}
	}
	delegate()->peerListRefreshRows();

	using Invite = Data::Session::InviteToCall;
	_peer->owner().invitesToCalls(
	) | rpl::filter([=](const Invite &invite) {
		return (invite.id == _realId);
	}) | rpl::start_with_next([=](const Invite &invite) {
		if (auto row = createInvitedRow(invite.user)) {
			delegate()->peerListAppendRow(std::move(row));
			delegate()->peerListRefreshRows();
		}
	}, _lifetime);
}

void MembersController::updateRow(
		const std::optional<Data::GroupCall::Participant> &was,
		const Data::GroupCall::Participant &now) {
	auto reorderIfInvitedBeforeIndex = 0;
	auto countChange = 0;
	if (const auto row = findRow(now.user)) {
		if (now.speaking && (!was || !was->speaking)) {
			checkSpeakingRowPosition(row);
		}
		if (row->state() == Row::State::Invited) {
			reorderIfInvitedBeforeIndex = row->absoluteIndex();
			countChange = 1;
		}
		updateRow(row, &now);
	} else if (auto row = createRow(now)) {
		if (row->speaking()) {
			delegate()->peerListPrependRow(std::move(row));
		} else {
			reorderIfInvitedBeforeIndex = delegate()->peerListFullRowsCount();
			delegate()->peerListAppendRow(std::move(row));
		}
		delegate()->peerListRefreshRows();
		countChange = 1;
	}
	static constexpr auto kInvited = Row::State::Invited;
	const auto reorder = [&] {
		const auto count = reorderIfInvitedBeforeIndex;
		if (count <= 0) {
			return false;
		}
		const auto row = delegate()->peerListRowAt(
			reorderIfInvitedBeforeIndex - 1).get();
		return (static_cast<Row*>(row)->state() == kInvited);
	}();
	if (reorder) {
		delegate()->peerListPartitionRows([](const PeerListRow &row) {
			return static_cast<const Row&>(row).state() != kInvited;
		});
	}
	if (countChange) {
		const auto fullCountMin = _fullCountMin.current() + countChange;
		if (_fullCountMax.current() < fullCountMin) {
			_fullCountMax = fullCountMin;
		}
		_fullCountMin = fullCountMin;
	}
}

void MembersController::checkSpeakingRowPosition(not_null<Row*> row) {
	if (_menu) {
		// Don't reorder rows while we show the popup menu.
		_menuCheckRowsAfterHidden.emplace(row->peer());
		return;
	}
	// Check if there are non-speaking rows above this one.
	const auto count = delegate()->peerListFullRowsCount();
	for (auto i = 0; i != count; ++i) {
		const auto above = delegate()->peerListRowAt(i);
		if (above == row) {
			// All rows above are speaking.
			return;
		} else if (!static_cast<Row*>(above.get())->speaking()) {
			break;
		}
	}
	// Someone started speaking and has a non-speaking row above him. Sort.
	const auto proj = [&](const PeerListRow &other) {
		if (&other == row.get()) {
			// Bring this new one to the top.
			return 0;
		} else if (static_cast<const Row&>(other).speaking()) {
			// Bring all the speaking ones below him.
			return 1;
		} else {
			return 2;
		}
	};
	delegate()->peerListSortRows([&](
			const PeerListRow &a,
			const PeerListRow &b) {
		return proj(a) < proj(b);
	});
}

void MembersController::updateRow(
		not_null<Row*> row,
		const Data::GroupCall::Participant *participant) {
	const auto wasSounding = row->sounding();
	const auto wasSsrc = row->ssrc();
	row->setSkipLevelUpdate(_skipRowLevelUpdate);
	row->updateState(participant);
	const auto nowSounding = row->sounding();
	const auto nowSsrc = row->ssrc();

	const auto wasNoSounding = _soundingRowBySsrc.empty();
	if (wasSsrc == nowSsrc) {
		if (nowSounding != wasSounding) {
			if (nowSounding) {
				_soundingRowBySsrc.emplace(nowSsrc, row);
			} else {
				_soundingRowBySsrc.remove(nowSsrc);
			}
		}
	} else {
		_soundingRowBySsrc.remove(wasSsrc);
		if (nowSounding) {
			Assert(nowSsrc != 0);
			_soundingRowBySsrc.emplace(nowSsrc, row);
		}
	}
	const auto nowNoSounding = _soundingRowBySsrc.empty();
	if (wasNoSounding && !nowNoSounding) {
		_soundingAnimation.start();
	} else if (nowNoSounding && !wasNoSounding) {
		_soundingAnimation.stop();
	}

	delegate()->peerListUpdateRow(row);
}

void MembersController::removeRow(not_null<Row*> row) {
	_soundingRowBySsrc.remove(row->ssrc());
	delegate()->peerListRemoveRow(row);
}

void MembersController::updateRowLevel(
		not_null<Row*> row,
		float level) {
	if (_skipRowLevelUpdate) {
		return;
	}
	row->updateLevel(level);
}

Row *MembersController::findRow(not_null<UserData*> user) const {
	return static_cast<Row*>(delegate()->peerListFindRow(user->id));
}

Data::GroupCall *MembersController::resolvedRealCall() const {
	return (_realCallRawValue
		&& (_peer->groupCall() == _realCallRawValue)
		&& (_realCallRawValue->id() == _realId))
		? _realCallRawValue
		: nullptr;
}

Main::Session &MembersController::session() const {
	return _call->peer()->session();
}

void MembersController::prepare() {
	delegate()->peerListSetSearchMode(PeerListSearchMode::Disabled);
	//delegate()->peerListSetTitle(std::move(title));
	setDescriptionText(tr::lng_contacts_loading(tr::now));
	setSearchNoResultsText(tr::lng_blocked_list_not_found(tr::now));

	const auto call = _call.get();
	if (const auto real = _peer->groupCall()
		; real && call && real->id() == call->id()) {
		prepareRows(real);
	} else if (auto row = createSelfRow()) {
		_fullCountMin = (row->state() == Row::State::Invited) ? 0 : 1;
		delegate()->peerListAppendRow(std::move(row));
		delegate()->peerListRefreshRows();
	}

	loadMoreRows();
	if (_realId) {
		appendInvitedUsers();
	}
	_prepared = true;
}

void MembersController::prepareRows(not_null<Data::GroupCall*> real) {
	auto foundSelf = false;
	auto changed = false;
	const auto &participants = real->participants();
	auto fullCountMin = 0;
	auto count = delegate()->peerListFullRowsCount();
	for (auto i = 0; i != count;) {
		auto row = delegate()->peerListRowAt(i);
		auto user = row->peer()->asUser();
		if (user->isSelf()) {
			foundSelf = true;
			++i;
			continue;
		}
		const auto contains = ranges::contains(
			participants,
			not_null{ user },
			&Data::GroupCall::Participant::user);
		if (contains) {
			++fullCountMin;
			++i;
		} else {
			changed = true;
			removeRow(static_cast<Row*>(row.get()));
			--count;
		}
	}
	if (!foundSelf) {
		const auto self = _peer->session().user();
		const auto i = ranges::find(
			participants,
			_peer->session().user(),
			&Data::GroupCall::Participant::user);
		auto row = (i != end(participants)) ? createRow(*i) : createSelfRow();
		if (row) {
			if (row->state() != Row::State::Invited) {
				++fullCountMin;
			}
			changed = true;
			delegate()->peerListAppendRow(std::move(row));
		}
	}
	for (const auto &participant : participants) {
		if (auto row = createRow(participant)) {
			++fullCountMin;
			changed = true;
			delegate()->peerListAppendRow(std::move(row));
		}
	}
	if (changed) {
		delegate()->peerListRefreshRows();
		if (_fullCountMax.current() < fullCountMin) {
			_fullCountMax = fullCountMin;
		}
		_fullCountMin = fullCountMin;
		if (real->participantsLoaded()) {
			_fullCountMax = fullCountMin;
		}
	}
}

void MembersController::loadMoreRows() {
	if (const auto real = _peer->groupCall()) {
		real->requestParticipants();
	}
}

auto MembersController::toggleMuteRequests() const
-> rpl::producer<MuteRequest> {
	return _toggleMuteRequests.events();
}

auto MembersController::changeVolumeRequests() const
-> rpl::producer<VolumeRequest> {
	return _changeVolumeRequests.events();
}

bool MembersController::rowCanMuteMembers() {
	return _peer->canManageGroupCall();
}

void MembersController::rowUpdateRow(not_null<Row*> row) {
	delegate()->peerListUpdateRow(row);
}

void MembersController::rowPaintIcon(
		Painter &p,
		QRect rect,
		float64 speaking,
		float64 active,
		float64 muted,
		bool mutedByMe) {
	const auto &greenIcon = st::groupCallMemberColoredCrossLine.icon;
	const auto left = rect.x() + (rect.width() - greenIcon.width()) / 2;
	const auto top = rect.y() + (rect.height() - greenIcon.height()) / 2;
	if (speaking == 1. && !mutedByMe) {
		// Just green icon, no cross, no coloring.
		greenIcon.paintInCenter(p, rect);
		return;
	} else if (speaking == 0.) {
		if (active == 1.) {
			// Just gray icon, no cross, no coloring.
			st::groupCallMemberInactiveCrossLine.icon.paintInCenter(p, rect);
			return;
		} else if (active == 0.) {
			if (muted == 1.) {
				// Red crossed icon, colorized once, cached as last frame.
				_coloredCrossLine.paint(
					p,
					left,
					top,
					1.,
					st::groupCallMemberMutedIcon->c);
				return;
			} else if (muted == 0.) {
				// Gray crossed icon, no coloring, cached as last frame.
				_inactiveCrossLine.paint(p, left, top, 1.);
				return;
			}
		}
	}
	const auto activeInactiveColor = anim::color(
		st::groupCallMemberInactiveIcon,
		(mutedByMe
			? st::groupCallMemberMutedIcon
			: st::groupCallMemberActiveIcon),
		speaking);
	const auto iconColor = anim::color(
		activeInactiveColor,
		st::groupCallMemberMutedIcon,
		muted);

	// Don't use caching of the last frame, because 'muted' may animate color.
	const auto crossProgress = std::min(1. - active, 0.9999);
	_inactiveCrossLine.paint(p, left, top, crossProgress, iconColor);
}

auto MembersController::kickMemberRequests() const
-> rpl::producer<not_null<UserData*>>{
	return _kickMemberRequests.events();
}

void MembersController::rowClicked(not_null<PeerListRow*> row) {
	delegate()->peerListShowRowMenu(row, [=](not_null<Ui::PopupMenu*> menu) {
		if (!_menu || _menu.get() != menu) {
			return;
		}
		auto saved = base::take(_menu);
		for (const auto peer : base::take(_menuCheckRowsAfterHidden)) {
			if (const auto row = findRow(peer->asUser())) {
				if (row->speaking()) {
					checkSpeakingRowPosition(row);
				}
			}
		}
		_menu = std::move(saved);
	});
}

void MembersController::rowActionClicked(
		not_null<PeerListRow*> row) {
	rowClicked(row);
}

base::unique_qptr<Ui::PopupMenu> MembersController::rowContextMenu(
		QWidget *parent,
		not_null<PeerListRow*> row) {
	auto result = createRowContextMenu(parent, row);

	if (result) {
		// First clear _menu value, so that we don't check row positions yet.
		base::take(_menu);

		// Here unique_qptr is used like a shared pointer, where
		// not the last destroyed pointer destroys the object, but the first.
		_menu = base::unique_qptr<Ui::PopupMenu>(result.get());
	}

	return result;
}

base::unique_qptr<Ui::PopupMenu> MembersController::createRowContextMenu(
		QWidget *parent,
		not_null<PeerListRow*> row) {
	Expects(row->peer()->isUser());

	const auto real = static_cast<Row*>(row.get());
	if (row->peer()->isSelf()
		&& (!_peer->canManageGroupCall() || !real->ssrc())) {
		return nullptr;
	}
	const auto user = row->peer()->asUser();
	auto result = base::make_unique_q<Ui::PopupMenu>(
		parent,
		st::groupCallPopupMenu);

	const auto muteState = real->state();
	const auto admin = IsGroupCallAdmin(_peer, user);
	const auto session = &user->session();
	const auto getCurrentWindow = [=]() -> Window::SessionController* {
		if (const auto window = Core::App().activeWindow()) {
			if (const auto controller = window->sessionController()) {
				if (&controller->session() == session) {
					return controller;
				}
			}
		}
		return nullptr;
	};
	const auto getWindow = [=] {
		if (const auto current = getCurrentWindow()) {
			return current;
		} else if (&Core::App().domain().active() != &session->account()) {
			Core::App().domain().activate(&session->account());
		}
		return getCurrentWindow();
	};
	const auto performOnMainWindow = [=](auto callback) {
		if (const auto window = getWindow()) {
			if (_menu) {
				_menu->discardParentReActivate();

				// We must hide PopupMenu before we activate the MainWindow,
				// otherwise we set focus in field inside MainWindow and then
				// PopupMenu::hide activates back the group call panel :(
				_menu = nullptr;
			}
			callback(window);
			window->widget()->activate();
		}
	};
	const auto showProfile = [=] {
		performOnMainWindow([=](not_null<Window::SessionController*> window) {
			window->showPeerInfo(user);
		});
	};
	const auto showHistory = [=] {
		performOnMainWindow([=](not_null<Window::SessionController*> window) {
			window->showPeerHistory(
				user,
				Window::SectionShow::Way::Forward);
		});
	};
	const auto removeFromGroup = crl::guard(this, [=] {
		_kickMemberRequests.fire_copy(user);
	});

	if (real->ssrc() != 0) {
		addMuteActionsToContextMenu(result, user, admin, real);
	}

	if (!user->isSelf()) {
		result->addAction(
			tr::lng_context_view_profile(tr::now),
			showProfile);
		result->addAction(
			tr::lng_context_send_message(tr::now),
			showHistory);
		const auto canKick = [&] {
			if (static_cast<Row*>(row.get())->state() == Row::State::Invited) {
				return false;
			} else if (const auto chat = _peer->asChat()) {
				return chat->amCreator()
					|| (chat->canBanMembers() && !chat->admins.contains(user));
			} else if (const auto group = _peer->asMegagroup()) {
				return group->canRestrictUser(user);
			}
			return false;
		}();
		if (canKick) {
			result->addAction(
				tr::lng_context_remove_from_group(tr::now),
				removeFromGroup);
		}
	}
	return result;
}

void MembersController::addMuteActionsToContextMenu(
		not_null<Ui::PopupMenu*> menu,
		not_null<UserData*> user,
		bool userIsCallAdmin,
		not_null<Row*> row) {
	const auto muteString = [=] {
		return (_peer->canManageGroupCall()
			? tr::lng_group_call_context_mute
			: tr::lng_group_call_context_mute_for_me)(tr::now);
	};

	const auto unmuteString = [=] {
		return (_peer->canManageGroupCall()
			? tr::lng_group_call_context_unmute
			: tr::lng_group_call_context_unmute_for_me)(tr::now);
	};

	const auto toggleMute = crl::guard(this, [=](bool mute, bool local) {
		_toggleMuteRequests.fire(Group::MuteRequest{
			.user = user,
			.mute = mute,
			.locallyOnly = local,
		});
	});
	const auto changeVolume = crl::guard(this, [=](
			int volume,
			bool local) {
		_changeVolumeRequests.fire(Group::VolumeRequest{
			.user = user,
			.volume = std::clamp(volume, 1, Group::kMaxVolume),
			.locallyOnly = local,
		});
	});

	const auto muteState = row->state();
	const auto isMuted = (muteState == Row::State::Muted)
		|| (muteState == Row::State::MutedByMe);

	auto mutesFromVolume = rpl::never<bool>() | rpl::type_erased();

	if (!isMuted || user->isSelf()) {
		const auto call = _call.get();
		auto otherParticipantStateValue = call
			? call->otherParticipantStateValue(
				) | rpl::filter([=](const Group::ParticipantState &data) {
					return data.user == user;
				})
			: rpl::never<Group::ParticipantState>() | rpl::type_erased();

		auto volumeItem = base::make_unique_q<MenuVolumeItem>(
			menu->menu(),
			st::groupCallPopupMenu.menu,
			otherParticipantStateValue,
			row->volume(),
			Group::kMaxVolume,
			isMuted);

		mutesFromVolume = volumeItem->toggleMuteRequests();

		volumeItem->toggleMuteRequests(
		) | rpl::start_with_next([=](bool muted) {
			if (muted) {
				// Slider value is changed after the callback is called.
				// To capture good state inside the slider frame we postpone.
				crl::on_main(menu, [=] {
					menu->hideMenu();
				});
			}
			toggleMute(muted, false);
		}, volumeItem->lifetime());

		volumeItem->toggleMuteLocallyRequests(
		) | rpl::start_with_next([=](bool muted) {
			if (!user->isSelf()) {
				toggleMute(muted, true);
			}
		}, volumeItem->lifetime());

		volumeItem->changeVolumeRequests(
		) | rpl::start_with_next([=](int volume) {
			changeVolume(volume, false);
		}, volumeItem->lifetime());

		volumeItem->changeVolumeLocallyRequests(
		) | rpl::start_with_next([=](int volume) {
			if (!user->isSelf()) {
				changeVolume(volume, true);
			}
		}, volumeItem->lifetime());

		menu->addAction(std::move(volumeItem));
	};

	const auto muteAction = [&]() -> QAction* {
		if (muteState == Row::State::Invited
			|| user->isSelf()
			|| (muteState == Row::State::Muted
				&& userIsCallAdmin
				&& _peer->canManageGroupCall())) {
			return nullptr;
		}
		auto callback = [=] {
			const auto state = row->state();
			const auto muted = (state == Row::State::Muted)
				|| (state == Row::State::MutedByMe);
			toggleMute(!muted, false);
		};
		return menu->addAction(
			isMuted ? unmuteString() : muteString(),
			std::move(callback));
	}();

	if (muteAction) {
		std::move(
			mutesFromVolume
		) | rpl::start_with_next([=](bool muted) {
			muteAction->setText(muted ? unmuteString() : muteString());
		}, menu->lifetime());
	}
}

std::unique_ptr<Row> MembersController::createSelfRow() {
	const auto self = _peer->session().user();
	auto result = std::make_unique<Row>(this, self);
	updateRow(result.get(), nullptr);
	return result;
}

std::unique_ptr<Row> MembersController::createRow(
		const Data::GroupCall::Participant &participant) {
	auto result = std::make_unique<Row>(this, participant.user);
	updateRow(result.get(), &participant);
	return result;
}

std::unique_ptr<Row> MembersController::createInvitedRow(
		not_null<UserData*> user) {
	if (findRow(user)) {
		return nullptr;
	}
	auto result = std::make_unique<Row>(this, user);
	updateRow(result.get(), nullptr);
	return result;
}

} // namespace


GroupMembers::GroupMembers(
	not_null<QWidget*> parent,
	not_null<GroupCall*> call)
: RpWidget(parent)
, _call(call)
, _scroll(this, st::defaultSolidScroll)
, _listController(std::make_unique<MembersController>(call, parent)) {
	setupAddMember(call);
	setupList();
	setContent(_list);
	setupFakeRoundCorners();
	_listController->setDelegate(static_cast<PeerListDelegate*>(this));
}

auto GroupMembers::toggleMuteRequests() const
-> rpl::producer<Group::MuteRequest> {
	return static_cast<MembersController*>(
		_listController.get())->toggleMuteRequests();
}

auto GroupMembers::changeVolumeRequests() const
-> rpl::producer<Group::VolumeRequest> {
	return static_cast<MembersController*>(
		_listController.get())->changeVolumeRequests();
}

auto GroupMembers::kickMemberRequests() const
-> rpl::producer<not_null<UserData*>> {
	return static_cast<MembersController*>(
		_listController.get())->kickMemberRequests();
}

int GroupMembers::desiredHeight() const {
	const auto top = _addMember ? _addMember->height() : 0;
	auto count = [&] {
		if (const auto call = _call.get()) {
			if (const auto real = call->peer()->groupCall()) {
				if (call->id() == real->id()) {
					return real->fullCount();
				}
			}
		}
		return 0;
	}();
	const auto use = std::max(count, _list->fullRowsCount());
	return top
		+ (use * st::groupCallMembersList.item.height)
		+ (use ? st::lineWidth : 0);
}

rpl::producer<int> GroupMembers::desiredHeightValue() const {
	const auto controller = static_cast<MembersController*>(
		_listController.get());
	return rpl::combine(
		heightValue(),
		_addMemberButton.value(),
		controller->fullCountValue()
	) | rpl::map([=] {
		return desiredHeight();
	});
}

void GroupMembers::setupAddMember(not_null<GroupCall*> call) {
	using namespace rpl::mappers;

	_canAddMembers = Data::CanWriteValue(call->peer().get());
	SubscribeToMigration(
		call->peer(),
		lifetime(),
		[=](not_null<ChannelData*> channel) {
			_canAddMembers = Data::CanWriteValue(channel.get());
		});

	_canAddMembers.value(
	) | rpl::start_with_next([=](bool can) {
		if (!can) {
			_addMemberButton = nullptr;
			_addMember.destroy();
			updateControlsGeometry();
			return;
		}
		_addMember = Settings::CreateButton(
			this,
			tr::lng_group_call_invite(),
			st::groupCallAddMember,
			&st::groupCallAddMemberIcon,
			st::groupCallAddMemberIconLeft);
		_addMember->show();

		_addMember->addClickHandler([=] { // TODO throttle(ripple duration)
			_addMemberRequests.fire({});
		});
		_addMemberButton = _addMember.data();

		resizeToList();
	}, lifetime());
}

rpl::producer<int> GroupMembers::fullCountValue() const {
	return static_cast<MembersController*>(
		_listController.get())->fullCountValue();
}

//tr::lng_chat_status_members(
//	lt_count_decimal,
//	controller->fullCountValue() | tr::to_count(),
//	Ui::Text::Upper
//),

void GroupMembers::setupList() {
	_listController->setStyleOverrides(&st::groupCallMembersList);
	_list = _scroll->setOwnedWidget(object_ptr<ListWidget>(
		this,
		_listController.get()));

	_list->heightValue(
	) | rpl::start_with_next([=] {
		resizeToList();
	}, _list->lifetime());

	rpl::combine(
		_scroll->scrollTopValue(),
		_scroll->heightValue()
	) | rpl::start_with_next([=](int scrollTop, int scrollHeight) {
		_list->setVisibleTopBottom(scrollTop, scrollTop + scrollHeight);
	}, _scroll->lifetime());

	updateControlsGeometry();
}

void GroupMembers::resizeEvent(QResizeEvent *e) {
	updateControlsGeometry();
}

void GroupMembers::resizeToList() {
	if (!_list) {
		return;
	}
	const auto listHeight = _list->height();
	const auto newHeight = (listHeight > 0)
		? ((_addMember ? _addMember->height() : 0)
			+ listHeight
			+ st::lineWidth)
		: 0;
	if (height() == newHeight) {
		updateControlsGeometry();
	} else {
		resize(width(), newHeight);
	}
}

void GroupMembers::updateControlsGeometry() {
	if (!_list) {
		return;
	}
	auto topSkip = 0;
	if (_addMember) {
		_addMember->resizeToWidth(width());
		_addMember->move(0, 0);
		topSkip = _addMember->height();
	}
	_scroll->setGeometry(0, topSkip, width(), height() - topSkip);
	_list->resizeToWidth(width());
}

void GroupMembers::setupFakeRoundCorners() {
	const auto size = st::roundRadiusLarge;
	const auto full = 3 * size;
	const auto imagePartSize = size * cIntRetinaFactor();
	const auto imageSize = full * cIntRetinaFactor();
	const auto image = std::make_shared<QImage>(
		QImage(imageSize, imageSize, QImage::Format_ARGB32_Premultiplied));
	image->setDevicePixelRatio(cRetinaFactor());

	const auto refreshImage = [=] {
		image->fill(st::groupCallBg->c);
		{
			QPainter p(image.get());
			PainterHighQualityEnabler hq(p);
			p.setCompositionMode(QPainter::CompositionMode_Source);
			p.setPen(Qt::NoPen);
			p.setBrush(Qt::transparent);
			p.drawRoundedRect(0, 0, full, full, size, size);
		}
	};

	const auto create = [&](QPoint imagePartOrigin) {
		const auto result = Ui::CreateChild<Ui::RpWidget>(this);
		result->show();
		result->resize(size, size);
		result->setAttribute(Qt::WA_TransparentForMouseEvents);
		result->paintRequest(
		) | rpl::start_with_next([=] {
			QPainter(result).drawImage(
				result->rect(),
				*image,
				QRect(imagePartOrigin, QSize(imagePartSize, imagePartSize)));
		}, result->lifetime());
		result->raise();
		return result;
	};
	const auto shift = imageSize - imagePartSize;
	const auto topleft = create({ 0, 0 });
	const auto topright = create({ shift, 0 });
	const auto bottomleft = create({ 0, shift });
	const auto bottomright = create({ shift, shift });

	sizeValue(
	) | rpl::start_with_next([=](QSize size) {
		topleft->move(0, 0);
		topright->move(size.width() - topright->width(), 0);
		bottomleft->move(0, size.height() - bottomleft->height());
		bottomright->move(
			size.width() - bottomright->width(),
			size.height() - bottomright->height());
	}, lifetime());

	refreshImage();
	style::PaletteChanged(
	) | rpl::start_with_next([=] {
		refreshImage();
		topleft->update();
		topright->update();
		bottomleft->update();
		bottomright->update();
	}, lifetime());
}

void GroupMembers::peerListSetTitle(rpl::producer<QString> title) {
}

void GroupMembers::peerListSetAdditionalTitle(rpl::producer<QString> title) {
}

void GroupMembers::peerListSetHideEmpty(bool hide) {
}

bool GroupMembers::peerListIsRowChecked(not_null<PeerListRow*> row) {
	return false;
}

void GroupMembers::peerListScrollToTop() {
}

int GroupMembers::peerListSelectedRowsCount() {
	return 0;
}

void GroupMembers::peerListAddSelectedPeerInBunch(not_null<PeerData*> peer) {
	Unexpected("Item selection in Calls::GroupMembers.");
}

void GroupMembers::peerListAddSelectedRowInBunch(not_null<PeerListRow*> row) {
	Unexpected("Item selection in Calls::GroupMembers.");
}

void GroupMembers::peerListFinishSelectedRowsBunch() {
}

void GroupMembers::peerListSetDescription(
		object_ptr<Ui::FlatLabel> description) {
	description.destroy();
}

} // namespace Calls
