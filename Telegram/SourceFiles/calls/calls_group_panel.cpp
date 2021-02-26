/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "calls/calls_group_panel.h"

#include "calls/calls_group_common.h"
#include "calls/calls_group_members.h"
#include "calls/calls_group_settings.h"
#include "ui/platform/ui_platform_window_title.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/window.h"
#include "ui/widgets/call_button.h"
#include "ui/widgets/call_mute_button.h"
#include "ui/widgets/checkbox.h"
#include "ui/layers/layer_manager.h"
#include "ui/layers/generic_box.h"
#include "ui/text/text_utilities.h"
#include "ui/toast/toast.h"
#include "info/profile/info_profile_values.h" // Info::Profile::Value.
#include "core/application.h"
#include "lang/lang_keys.h"
#include "data/data_channel.h"
#include "data/data_chat.h"
#include "data/data_user.h"
#include "data/data_group_call.h"
#include "data/data_session.h"
#include "main/main_session.h"
#include "base/event_filter.h"
#include "boxes/peers/edit_participants_box.h"
#include "boxes/peers/add_participants_box.h"
#include "boxes/peer_lists_box.h"
#include "boxes/confirm_box.h"
#include "app.h"
#include "apiwrap.h" // api().kickParticipant.
#include "styles/style_calls.h"
#include "styles/style_layers.h"

#include <QtWidgets/QDesktopWidget>
#include <QtWidgets/QApplication>
#include <QtGui/QWindow>

namespace Calls {
namespace {

constexpr auto kSpacePushToTalkDelay = crl::time(250);

class InviteController final : public ParticipantsBoxController {
public:
	InviteController(
		not_null<PeerData*> peer,
		base::flat_set<not_null<UserData*>> alreadyIn);

	void prepare() override;

	void rowClicked(not_null<PeerListRow*> row) override;
	base::unique_qptr<Ui::PopupMenu> rowContextMenu(
		QWidget *parent,
		not_null<PeerListRow*> row) override;

	void itemDeselectedHook(not_null<PeerData*> peer) override;

	[[nodiscard]] auto peersWithRows() const
		-> not_null<const base::flat_set<not_null<UserData*>>*>;
	[[nodiscard]] rpl::producer<not_null<UserData*>> rowAdded() const;

	[[nodiscard]] bool hasRowFor(not_null<PeerData*> peer) const;

private:
	[[nodiscard]] bool isAlreadyIn(not_null<UserData*> user) const;

	std::unique_ptr<PeerListRow> createRow(
		not_null<UserData*> user) const override;

	not_null<PeerData*> _peer;
	const base::flat_set<not_null<UserData*>> _alreadyIn;
	mutable base::flat_set<not_null<UserData*>> _inGroup;
	rpl::event_stream<not_null<UserData*>> _rowAdded;

};

class InviteContactsController final : public AddParticipantsBoxController {
public:
	InviteContactsController(
		not_null<PeerData*> peer,
		base::flat_set<not_null<UserData*>> alreadyIn,
		not_null<const base::flat_set<not_null<UserData*>>*> inGroup,
		rpl::producer<not_null<UserData*>> discoveredInGroup);

private:
	void prepareViewHook() override;

	std::unique_ptr<PeerListRow> createRow(
		not_null<UserData*> user) override;

	bool needsInviteLinkButton() override {
		return false;
	}

	const not_null<const base::flat_set<not_null<UserData*>>*> _inGroup;
	rpl::producer<not_null<UserData*>> _discoveredInGroup;

	rpl::lifetime _lifetime;

};

[[nodiscard]] object_ptr<Ui::RpWidget> CreateSectionSubtitle(
		QWidget *parent,
		rpl::producer<QString> text) {
	auto result = object_ptr<Ui::FixedHeightWidget>(
		parent,
		st::searchedBarHeight);

	const auto raw = result.data();
	raw->paintRequest(
	) | rpl::start_with_next([=](QRect clip) {
		auto p = QPainter(raw);
		p.fillRect(clip, st::groupCallMembersBgOver);
	}, raw->lifetime());

	const auto label = Ui::CreateChild<Ui::FlatLabel>(
		raw,
		std::move(text),
		st::groupCallBoxLabel);
	raw->widthValue(
	) | rpl::start_with_next([=](int width) {
		const auto padding = st::groupCallInviteDividerPadding;
		const auto available = width - padding.left() - padding.right();
		label->resizeToNaturalWidth(available);
		label->moveToLeft(padding.left(), padding.top(), width);
	}, label->lifetime());

	return result;
}

InviteController::InviteController(
	not_null<PeerData*> peer,
	base::flat_set<not_null<UserData*>> alreadyIn)
: ParticipantsBoxController(CreateTag{}, nullptr, peer, Role::Members)
, _peer(peer)
, _alreadyIn(std::move(alreadyIn)) {
	SubscribeToMigration(
		_peer,
		lifetime(),
		[=](not_null<ChannelData*> channel) { _peer = channel; });
}

void InviteController::prepare() {
	delegate()->peerListSetHideEmpty(true);
	ParticipantsBoxController::prepare();
	delegate()->peerListSetAboveWidget(CreateSectionSubtitle(
		nullptr,
		tr::lng_group_call_invite_members()));
	delegate()->peerListSetAboveSearchWidget(CreateSectionSubtitle(
		nullptr,
		tr::lng_group_call_invite_members()));
}

void InviteController::rowClicked(not_null<PeerListRow*> row) {
	delegate()->peerListSetRowChecked(row, !row->checked());
}

base::unique_qptr<Ui::PopupMenu> InviteController::rowContextMenu(
		QWidget *parent,
		not_null<PeerListRow*> row) {
	return nullptr;
}

void InviteController::itemDeselectedHook(not_null<PeerData*> peer) {
}

bool InviteController::hasRowFor(not_null<PeerData*> peer) const {
	return (delegate()->peerListFindRow(peer->id) != nullptr);
}

bool InviteController::isAlreadyIn(not_null<UserData*> user) const {
	return _alreadyIn.contains(user);
}

std::unique_ptr<PeerListRow> InviteController::createRow(
		not_null<UserData*> user) const {
	if (user->isSelf() || user->isBot()) {
		return nullptr;
	}
	auto result = std::make_unique<PeerListRow>(user);
	_rowAdded.fire_copy(user);
	_inGroup.emplace(user);
	if (isAlreadyIn(user)) {
		result->setDisabledState(PeerListRow::State::DisabledChecked);
	}
	return result;
}

auto InviteController::peersWithRows() const
-> not_null<const base::flat_set<not_null<UserData*>>*> {
	return &_inGroup;
}

rpl::producer<not_null<UserData*>> InviteController::rowAdded() const {
	return _rowAdded.events();
}

InviteContactsController::InviteContactsController(
	not_null<PeerData*> peer,
	base::flat_set<not_null<UserData*>> alreadyIn,
	not_null<const base::flat_set<not_null<UserData*>>*> inGroup,
	rpl::producer<not_null<UserData*>> discoveredInGroup)
: AddParticipantsBoxController(peer, std::move(alreadyIn))
, _inGroup(inGroup)
, _discoveredInGroup(std::move(discoveredInGroup)) {
}

void InviteContactsController::prepareViewHook() {
	AddParticipantsBoxController::prepareViewHook();

	delegate()->peerListSetAboveWidget(CreateSectionSubtitle(
		nullptr,
		tr::lng_contacts_header()));
	delegate()->peerListSetAboveSearchWidget(CreateSectionSubtitle(
		nullptr,
		tr::lng_group_call_invite_search_results()));

	std::move(
		_discoveredInGroup
	) | rpl::start_with_next([=](not_null<UserData*> user) {
		if (auto row = delegate()->peerListFindRow(user->id)) {
			delegate()->peerListRemoveRow(row);
		}
	}, _lifetime);
}

std::unique_ptr<PeerListRow> InviteContactsController::createRow(
		not_null<UserData*> user) {
	return _inGroup->contains(user)
		? nullptr
		: AddParticipantsBoxController::createRow(user);
}

} // namespace

void LeaveGroupCallBox(
		not_null<Ui::GenericBox*> box,
		not_null<GroupCall*> call,
		bool discardChecked,
		BoxContext context) {
	box->setTitle(tr::lng_group_call_leave_title());
	const auto inCall = (context == BoxContext::GroupCallPanel);
	box->addRow(object_ptr<Ui::FlatLabel>(
		box.get(),
		tr::lng_group_call_leave_sure(),
		(inCall ? st::groupCallBoxLabel : st::boxLabel)));
	const auto discard = call->peer()->canManageGroupCall()
		? box->addRow(object_ptr<Ui::Checkbox>(
			box.get(),
			tr::lng_group_call_end(),
			discardChecked,
			(inCall ? st::groupCallCheckbox : st::defaultBoxCheckbox),
			(inCall ? st::groupCallCheck : st::defaultCheck)),
			style::margins(
				st::boxRowPadding.left(),
				st::boxRowPadding.left(),
				st::boxRowPadding.right(),
				st::boxRowPadding.bottom()))
		: nullptr;
	const auto weak = base::make_weak(call.get());
	box->addButton(tr::lng_group_call_leave(), [=] {
		const auto discardCall = (discard && discard->checked());
		box->closeBox();

		if (!weak) {
			return;
		} else if (discardCall) {
			call->discard();
		} else {
			call->hangup();
		}
	});
	box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
}

void GroupCallConfirmBox(
		not_null<Ui::GenericBox*> box,
		const QString &text,
		rpl::producer<QString> button,
		Fn<void()> callback) {
	box->addRow(
		object_ptr<Ui::FlatLabel>(
			box.get(),
			text,
			st::groupCallBoxLabel),
		st::boxPadding);
	box->addButton(std::move(button), callback);
	box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
}

GroupPanel::GroupPanel(not_null<GroupCall*> call)
: _call(call)
, _peer(call->peer())
, _window(std::make_unique<Ui::Window>(Core::App().getModalParent()))
, _layerBg(std::make_unique<Ui::LayerManager>(_window->body()))
#ifndef Q_OS_MAC
, _controls(std::make_unique<Ui::Platform::TitleControls>(
	_window->body(),
	st::groupCallTitle))
#endif // !Q_OS_MAC
, _members(widget(), call)
, _settings(widget(), st::groupCallSettings)
, _mute(std::make_unique<Ui::CallMuteButton>(
	widget(),
	Core::App().appDeactivatedValue(),
	Ui::CallMuteButtonState{
		.text = tr::lng_group_call_connecting(tr::now),
		.type = Ui::CallMuteButtonType::Connecting,
	}))
, _hangup(widget(), st::groupCallHangup) {
	_layerBg->setStyleOverrides(&st::groupCallBox, &st::groupCallLayerBox);
	_settings->setColorOverrides(_mute->colorOverrides());
	_layerBg->setHideByBackgroundClick(true);

	SubscribeToMigration(
		_peer,
		_window->lifetime(),
		[=](not_null<ChannelData*> channel) { migrate(channel); });

	initWindow();
	initWidget();
	initControls();
	initLayout();
	showAndActivate();
}

GroupPanel::~GroupPanel() = default;

bool GroupPanel::isActive() const {
	return _window->isActiveWindow()
		&& _window->isVisible()
		&& !(_window->windowState() & Qt::WindowMinimized);
}

void GroupPanel::minimize() {
	_window->setWindowState(_window->windowState() | Qt::WindowMinimized);
}

void GroupPanel::close() {
	_window->close();
}

void GroupPanel::showAndActivate() {
	if (_window->isHidden()) {
		_window->show();
	}
	const auto state = _window->windowState();
	if (state & Qt::WindowMinimized) {
		_window->setWindowState(state & ~Qt::WindowMinimized);
	}
	_window->raise();
	_window->activateWindow();
	_window->setFocus();
}

void GroupPanel::migrate(not_null<ChannelData*> channel) {
	_peer = channel;
	_peerLifetime.destroy();
	subscribeToPeerChanges();
	_title.destroy();
	refreshTitle();
}

void GroupPanel::subscribeToPeerChanges() {
	Info::Profile::NameValue(
		_peer
	) | rpl::start_with_next([=](const TextWithEntities &name) {
		_window->setTitle(name.text);
	}, _peerLifetime);
}

void GroupPanel::initWindow() {
	_window->setAttribute(Qt::WA_OpaquePaintEvent);
	_window->setAttribute(Qt::WA_NoSystemBackground);
	_window->setWindowIcon(
		QIcon(QPixmap::fromImage(Image::Empty()->original(), Qt::ColorOnly)));
	_window->setTitleStyle(st::groupCallTitle);

	subscribeToPeerChanges();

	base::install_event_filter(_window.get(), [=](not_null<QEvent*> e) {
		if (e->type() == QEvent::Close && handleClose()) {
			e->ignore();
			return base::EventFilterResult::Cancel;
		} else if (e->type() == QEvent::KeyPress
			|| e->type() == QEvent::KeyRelease) {
			if (static_cast<QKeyEvent*>(e.get())->key() == Qt::Key_Space) {
				if (_call) {
					_call->pushToTalk(
						e->type() == QEvent::KeyPress,
						kSpacePushToTalkDelay);
				}
			}
		}
		return base::EventFilterResult::Continue;
	});

	_window->setBodyTitleArea([=](QPoint widgetPoint) {
		using Flag = Ui::WindowTitleHitTestFlag;
		const auto titleRect = QRect(
			0,
			0,
			widget()->width(),
			computeMembersListTop());
		return titleRect.contains(widgetPoint)
			? (Flag::Move | Flag::Maximize)
			: Flag::None;
	});
}

void GroupPanel::initWidget() {
	widget()->setMouseTracking(true);

	widget()->paintRequest(
	) | rpl::start_with_next([=](QRect clip) {
		paint(clip);
	}, widget()->lifetime());

	widget()->sizeValue(
	) | rpl::skip(1) | rpl::start_with_next([=] {
		updateControlsGeometry();

		// title geometry depends on _controls->geometry,
		// which is not updated here yet.
		crl::on_main(widget(), [=] { refreshTitle(); });
	}, widget()->lifetime());
}

void GroupPanel::endCall() {
	if (!_call) {
		return;
	} else if (!_call->peer()->canManageGroupCall()) {
		_call->hangup();
		return;
	}
	_layerBg->showBox(Box(
		LeaveGroupCallBox,
		_call,
		false,
		BoxContext::GroupCallPanel));
}

void GroupPanel::initControls() {
	_mute->clicks(
	) | rpl::filter([=](Qt::MouseButton button) {
		return (button == Qt::LeftButton) && (_call != nullptr);
	}) | rpl::start_with_next([=] {
		if (_call->muted() == MuteState::ForceMuted) {
			_mute->shake();
		} else {
			_call->setMuted((_call->muted() == MuteState::Muted)
				? MuteState::Active
				: MuteState::Muted);
		}
	}, _mute->lifetime());

	_hangup->setClickedCallback([=] { endCall(); });
	_settings->setClickedCallback([=] {
		if (_call) {
			_layerBg->showBox(Box(GroupCallSettingsBox, _call));
		}
	});

	_settings->setText(tr::lng_menu_settings());
	_hangup->setText(tr::lng_group_call_leave());

	_members->desiredHeightValue(
	) | rpl::start_with_next([=] {
		updateControlsGeometry();
	}, _members->lifetime());

	initWithCall(_call);
}

void GroupPanel::initWithCall(GroupCall *call) {
	_callLifetime.destroy();
	_call = call;
	if (!_call) {
		return;
	}

	_peer = _call->peer();

	call->stateValue(
	) | rpl::filter([](State state) {
		return (state == State::HangingUp)
			|| (state == State::Ended)
			|| (state == State::FailedHangingUp)
			|| (state == State::Failed);
	}) | rpl::start_with_next([=] {
		closeBeforeDestroy();
	}, _callLifetime);

	call->levelUpdates(
	) | rpl::filter([=](const LevelUpdate &update) {
		return update.self;
	}) | rpl::start_with_next([=](const LevelUpdate &update) {
		_mute->setLevel(update.value);
	}, _callLifetime);

	_members->toggleMuteRequests(
	) | rpl::start_with_next([=](Group::MuteRequest request) {
		if (_call) {
			_call->toggleMute(request);
		}
	}, _callLifetime);

	_members->changeVolumeRequests(
	) | rpl::start_with_next([=](Group::VolumeRequest request) {
		if (_call) {
			_call->changeVolume(request);
		}
	}, _callLifetime);

	_members->kickMemberRequests(
	) | rpl::start_with_next([=](not_null<UserData*> user) {
		kickMember(user);
	}, _callLifetime);

	_members->addMembersRequests(
	) | rpl::start_with_next([=] {
		if (_call) {
			addMembers();
		}
	}, _callLifetime);

	rpl::combine(
		_call->mutedValue() | MapPushToTalkToActive(),
		_call->connectingValue()
	) | rpl::distinct_until_changed(
	) | rpl::start_with_next([=](MuteState mute, bool connecting) {
		_mute->setState(Ui::CallMuteButtonState{
			.text = (connecting
				? tr::lng_group_call_connecting(tr::now)
				: mute == MuteState::ForceMuted
				? tr::lng_group_call_force_muted(tr::now)
				: mute == MuteState::Muted
				? tr::lng_group_call_unmute(tr::now)
				: tr::lng_group_call_you_are_live(tr::now)),
			.subtext = (connecting
				? QString()
				: mute == MuteState::ForceMuted
				? tr::lng_group_call_force_muted_sub(tr::now)
				: mute == MuteState::Muted
				? tr::lng_group_call_unmute_sub(tr::now)
				: QString()),
			.type = (connecting
				? Ui::CallMuteButtonType::Connecting
				: mute == MuteState::ForceMuted
				? Ui::CallMuteButtonType::ForceMuted
				: mute == MuteState::Muted
				? Ui::CallMuteButtonType::Muted
				: Ui::CallMuteButtonType::Active),
		});
	}, _callLifetime);
}

void GroupPanel::addMembers() {
	const auto real = _peer->groupCall();
	if (!_call || !real || real->id() != _call->id()) {
		return;
	}
	auto alreadyIn = _peer->owner().invitedToCallUsers(real->id());
	for (const auto &participant : real->participants()) {
		alreadyIn.emplace(participant.user);
	}
	alreadyIn.emplace(_peer->session().user());
	auto controller = std::make_unique<InviteController>(
		_peer,
		alreadyIn);
	controller->setStyleOverrides(
		&st::groupCallInviteMembersList,
		&st::groupCallMultiSelect);

	auto contactsController = std::make_unique<InviteContactsController>(
		_peer,
		std::move(alreadyIn),
		controller->peersWithRows(),
		controller->rowAdded());
	contactsController->setStyleOverrides(
		&st::groupCallInviteMembersList,
		&st::groupCallMultiSelect);

	const auto weak = base::make_weak(_call);
	const auto invite = [=](const std::vector<not_null<UserData*>> &users) {
		const auto call = weak.get();
		if (!call) {
			return;
		}
		const auto result = call->inviteUsers(users);
		if (const auto user = std::get_if<not_null<UserData*>>(&result)) {
			Ui::Toast::Show(
				widget(),
				Ui::Toast::Config{
					.text = tr::lng_group_call_invite_done_user(
						tr::now,
						lt_user,
						Ui::Text::Bold((*user)->firstName),
						Ui::Text::WithEntities),
					.st = &st::defaultToast,
				});
		} else if (const auto count = std::get_if<int>(&result)) {
			if (*count > 0) {
				Ui::Toast::Show(
					widget(),
					Ui::Toast::Config{
						.text = tr::lng_group_call_invite_done_many(
							tr::now,
							lt_count,
							*count,
							Ui::Text::RichLangValue),
						.st = &st::defaultToast,
					});
			}
		} else {
			Unexpected("Result in GroupCall::inviteUsers.");
		}
	};
	const auto inviteWithAdd = [=](
			const std::vector<not_null<UserData*>> &users,
			const std::vector<not_null<UserData*>> &nonMembers,
			Fn<void()> finish) {
		_peer->session().api().addChatParticipants(
			_peer,
			nonMembers,
			[=](bool) { invite(users); finish(); });
	};
	const auto inviteWithConfirmation = [=](
			const std::vector<not_null<UserData*>> &users,
			const std::vector<not_null<UserData*>> &nonMembers,
			Fn<void()> finish) {
		if (nonMembers.empty()) {
			invite(users);
			finish();
			return;
		}
		const auto name = _peer->name;
		const auto text = (nonMembers.size() == 1)
			? tr::lng_group_call_add_to_group_one(
				tr::now,
				lt_user,
				nonMembers.front()->shortName(),
				lt_group,
				name)
			: (nonMembers.size() < users.size())
			? tr::lng_group_call_add_to_group_some(tr::now, lt_group, name)
			: tr::lng_group_call_add_to_group_all(tr::now, lt_group, name);
		const auto shared = std::make_shared<QPointer<Ui::GenericBox>>();
		const auto finishWithConfirm = [=] {
			if (*shared) {
				(*shared)->closeBox();
			}
			finish();
		};
		auto box = Box(
			GroupCallConfirmBox,
			text,
			tr::lng_participant_invite(),
			[=] { inviteWithAdd(users, nonMembers, finishWithConfirm); });
		*shared = box.data();
		_layerBg->showBox(std::move(box));
	};
	auto initBox = [=, controller = controller.get()](
			not_null<PeerListsBox*> box) {
		box->setTitle(tr::lng_group_call_invite_title());
		box->addButton(tr::lng_group_call_invite_button(), [=] {
			const auto rows = box->collectSelectedRows();

			const auto users = ranges::view::all(
				rows
			) | ranges::view::transform([](not_null<PeerData*> peer) {
				return not_null<UserData*>(peer->asUser());
			}) | ranges::to_vector;

			const auto nonMembers = ranges::view::all(
				users
			) | ranges::view::filter([&](not_null<UserData*> user) {
				return !controller->hasRowFor(user);
			}) | ranges::to_vector;

			const auto finish = [box = Ui::MakeWeak(box)]() {
				if (box) {
					box->closeBox();
				}
			};
			inviteWithConfirmation(users, nonMembers, finish);
		});
		box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
	};

	auto controllers = std::vector<std::unique_ptr<PeerListController>>();
	controllers.push_back(std::move(controller));
	controllers.push_back(std::move(contactsController));
	_layerBg->showBox(Box<PeerListsBox>(std::move(controllers), initBox));
}

void GroupPanel::kickMember(not_null<UserData*> user) {
	_layerBg->showBox(Box([=](not_null<Ui::GenericBox*> box) {
		box->addRow(
			object_ptr<Ui::FlatLabel>(
				box.get(),
				tr::lng_profile_sure_kick(
					tr::now,
					lt_user,
					user->firstName),
				st::groupCallBoxLabel),
			style::margins(
				st::boxRowPadding.left(),
				st::boxPadding.top(),
				st::boxRowPadding.right(),
				st::boxPadding.bottom()));
		box->addButton(tr::lng_box_remove(), [=] {
			box->closeBox();
			kickMemberSure(user);
		});
		box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
	}));
}

void GroupPanel::kickMemberSure(not_null<UserData*> user) {
	if (const auto chat = _peer->asChat()) {
		chat->session().api().kickParticipant(chat, user);
	} else if (const auto channel = _peer->asChannel()) {
		const auto currentRestrictedRights = [&]() -> MTPChatBannedRights {
			const auto it = channel->mgInfo->lastRestricted.find(user);
			return (it != channel->mgInfo->lastRestricted.cend())
				? it->second.rights
				: MTP_chatBannedRights(MTP_flags(0), MTP_int(0));
		}();

		channel->session().api().kickParticipant(
			channel,
			user,
			currentRestrictedRights);
	}
}

void GroupPanel::initLayout() {
	initGeometry();

#ifndef Q_OS_MAC
	_controls->raise();
#endif // !Q_OS_MAC
}

void GroupPanel::showControls() {
	Expects(_call != nullptr);

	widget()->showChildren();
}

void GroupPanel::closeBeforeDestroy() {
	_window->close();
	initWithCall(nullptr);
}

void GroupPanel::initGeometry() {
	const auto center = Core::App().getPointForCallPanelCenter();
	const auto rect = QRect(0, 0, st::groupCallWidth, st::groupCallHeight);
	_window->setGeometry(rect.translated(center - rect.center()));
	_window->setMinimumSize(rect.size());
	_window->show();
	updateControlsGeometry();
}

int GroupPanel::computeMembersListTop() const {
	if (computeTitleRect().has_value()) {
		return st::groupCallMembersTop;
	}
	return st::groupCallMembersTop
		- (st::groupCallSubtitleTop - st::groupCallTitleTop);
}

std::optional<QRect> GroupPanel::computeTitleRect() const {
#ifdef Q_OS_MAC
	return QRect(70, 0, widget()->width() - 70, 28);
#else // Q_OS_MAC
	const auto controls = _controls->geometry();
	return QRect(0, 0, controls.x(), controls.height());
#endif // !Q_OS_MAC
}

void GroupPanel::updateControlsGeometry() {
	if (widget()->size().isEmpty()) {
		return;
	}
	const auto desiredHeight = _members->desiredHeight();
	const auto membersWidthAvailable = widget()->width()
		- st::groupCallMembersMargin.left()
		- st::groupCallMembersMargin.right();
	const auto membersWidthMin = st::groupCallWidth
		- st::groupCallMembersMargin.left()
		- st::groupCallMembersMargin.right();
	const auto membersWidth = std::clamp(
		membersWidthAvailable,
		membersWidthMin,
		st::groupCallMembersWidthMax);
	const auto muteTop = widget()->height() - st::groupCallMuteBottomSkip;
	const auto buttonsTop = widget()->height() - st::groupCallButtonBottomSkip;
	const auto membersTop = computeMembersListTop();
	const auto availableHeight = muteTop
		- membersTop
		- st::groupCallMembersMargin.bottom();
	_members->setGeometry(
		(widget()->width() - membersWidth) / 2,
		membersTop,
		membersWidth,
		std::min(desiredHeight, availableHeight));
	const auto muteSize = _mute->innerSize().width();
	const auto fullWidth = muteSize
		+ 2 * _settings->width()
		+ 2 * st::groupCallButtonSkip;
	_mute->moveInner({ (widget()->width() - muteSize) / 2, muteTop });
	_settings->moveToLeft((widget()->width() - fullWidth) / 2, buttonsTop);
	_hangup->moveToRight((widget()->width() - fullWidth) / 2, buttonsTop);
	refreshTitle();
}

void GroupPanel::refreshTitle() {
	if (const auto titleRect = computeTitleRect()) {
		if (!_title) {
			_title.create(
				widget(),
				Info::Profile::NameValue(_peer),
				st::groupCallTitleLabel);
			_title->show();
			_title->setAttribute(Qt::WA_TransparentForMouseEvents);
		}
		const auto best = _title->naturalWidth();
		const auto from = (widget()->width() - best) / 2;
		const auto top = st::groupCallTitleTop;
		const auto left = titleRect->x();
		if (from >= left && from + best <= left + titleRect->width()) {
			_title->resizeToWidth(best);
			_title->moveToLeft(from, top);
		} else if (titleRect->width() < best) {
			_title->resizeToWidth(titleRect->width());
			_title->moveToLeft(left, top);
		} else if (from < left) {
			_title->resizeToWidth(best);
			_title->moveToLeft(left, top);
		} else {
			_title->resizeToWidth(best);
			_title->moveToLeft(left + titleRect->width() - best, top);
		}
	} else if (_title) {
		_title.destroy();
	}
	if (!_subtitle) {
		_subtitle.create(
			widget(),
			tr::lng_group_call_members(
				lt_count_decimal,
				_members->fullCountValue() | tr::to_count()),
			st::groupCallSubtitleLabel);
		_subtitle->show();
		_subtitle->setAttribute(Qt::WA_TransparentForMouseEvents);
	}
	const auto middle = _title
		? (_title->x() + _title->width() / 2)
		: (widget()->width() / 2);
	const auto top = _title
		? st::groupCallSubtitleTop
		: st::groupCallTitleTop;
	_subtitle->moveToLeft(
		(widget()->width() - _subtitle->width()) / 2,
		top);
}

void GroupPanel::paint(QRect clip) {
	Painter p(widget());

	auto region = QRegion(clip);
	for (const auto rect : region) {
		p.fillRect(rect, st::groupCallBg);
	}
}

bool GroupPanel::handleClose() {
	if (_call) {
		_window->hide();
		return true;
	}
	return false;
}

not_null<Ui::RpWidget*> GroupPanel::widget() const {
	return _window->body();
}

} // namespace Calls
