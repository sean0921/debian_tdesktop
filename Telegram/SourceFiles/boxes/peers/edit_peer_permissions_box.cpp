/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "boxes/peers/edit_peer_permissions_box.h"

#include "lang/lang_keys.h"
#include "data/data_channel.h"
#include "data/data_chat.h"
#include "ui/wrap/vertical_layout.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/checkbox.h"
#include "ui/toast/toast.h"
#include "info/profile/info_profile_button.h"
#include "info/profile/info_profile_icon.h"
#include "info/profile/info_profile_values.h"
#include "boxes/peers/edit_participants_box.h"
#include "boxes/peers/edit_peer_info_box.h"
#include "window/window_session_controller.h"
#include "mainwindow.h"
#include "styles/style_boxes.h"
#include "styles/style_info.h"

namespace {

template <typename CheckboxesMap, typename DependenciesMap>
void ApplyDependencies(
		const CheckboxesMap &checkboxes,
		const DependenciesMap &dependencies,
		QPointer<Ui::Checkbox> changed) {
	const auto checkAndApply = [&](
			auto &&current,
			auto dependency,
			bool isChecked) {
		for (auto &&checkbox : checkboxes) {
			if ((checkbox.first & dependency)
				&& (checkbox.second->checked() == isChecked)) {
				current->setChecked(isChecked);
				return true;
			}
		}
		return false;
	};
	const auto applySomeDependency = [&] {
		auto result = false;
		for (auto &&entry : checkboxes) {
			if (entry.second == changed) {
				continue;
			}
			auto isChecked = entry.second->checked();
			for (auto &&dependency : dependencies) {
				const auto check = isChecked
					? dependency.first
					: dependency.second;
				if (entry.first & check) {
					if (checkAndApply(
							entry.second,
							(isChecked
								? dependency.second
								: dependency.first),
							!isChecked)) {
						result = true;
						break;
					}
				}
			}
		}
		return result;
	};

	const auto maxFixesCount = int(checkboxes.size());
	for (auto i = 0; i != maxFixesCount; ++i) {
		if (!applySomeDependency()) {
			break;
		}
	};
}

std::vector<std::pair<ChatRestrictions, QString>> RestrictionLabels() {
	const auto langKeys = {
		tr::lng_rights_chat_send_text,
		tr::lng_rights_chat_send_media,
		tr::lng_rights_chat_send_stickers,
		tr::lng_rights_chat_send_links,
		tr::lng_rights_chat_send_polls,
		tr::lng_rights_chat_add_members,
		tr::lng_rights_group_pin,
		tr::lng_rights_group_info,
	};

	std::vector<std::pair<ChatRestrictions, QString>> vector;
	const auto restrictions = Data::ListOfRestrictions();
	auto i = 0;
	for (const auto &key : langKeys) {
		vector.emplace_back(restrictions[i++], key(tr::now));
	}
	return vector;
}

std::vector<std::pair<ChatAdminRights, QString>> AdminRightLabels(
		bool isGroup,
		bool anyoneCanAddMembers) {
	using Flag = ChatAdminRight;

	if (isGroup) {
		return {
			{ Flag::f_change_info, tr::lng_rights_group_info(tr::now) },
			{ Flag::f_delete_messages, tr::lng_rights_group_delete(tr::now) },
			{ Flag::f_ban_users, tr::lng_rights_group_ban(tr::now) },
			{ Flag::f_invite_users, anyoneCanAddMembers
				? tr::lng_rights_group_invite_link(tr::now)
				: tr::lng_rights_group_invite(tr::now) },
			{ Flag::f_pin_messages, tr::lng_rights_group_pin(tr::now) },
			{ Flag::f_add_admins, tr::lng_rights_add_admins(tr::now) },
		};
	} else {
		return {
			{ Flag::f_change_info, tr::lng_rights_channel_info(tr::now) },
			{ Flag::f_post_messages, tr::lng_rights_channel_post(tr::now) },
			{ Flag::f_edit_messages, tr::lng_rights_channel_edit(tr::now) },
			{ Flag::f_delete_messages, tr::lng_rights_channel_delete(tr::now) },
			{ Flag::f_invite_users, tr::lng_rights_group_invite(tr::now) },
			{ Flag::f_add_admins, tr::lng_rights_add_admins(tr::now) }
		};
	}
}

auto Dependencies(ChatRestrictions)
-> std::vector<std::pair<ChatRestriction, ChatRestriction>> {
	using Flag = ChatRestriction;

	return {
		// stickers <-> gifs
		{ Flag::f_send_gifs, Flag::f_send_stickers },
		{ Flag::f_send_stickers, Flag::f_send_gifs },

		// stickers <-> games
		{ Flag::f_send_games, Flag::f_send_stickers },
		{ Flag::f_send_stickers, Flag::f_send_games },

		// stickers <-> inline
		{ Flag::f_send_inline, Flag::f_send_stickers },
		{ Flag::f_send_stickers, Flag::f_send_inline },

		// stickers -> send_media
		{ Flag::f_send_stickers, Flag::f_send_messages },

		// embed_links -> send_media
		{ Flag::f_embed_links, Flag::f_send_messages },

		// send_media -> send_messages
		{ Flag::f_send_media, Flag::f_send_messages },

		// send_polls -> send_messages
		{ Flag::f_send_polls, Flag::f_send_messages },

		// send_messages -> view_messages
		{ Flag::f_send_messages, Flag::f_view_messages },
	};
}

ChatRestrictions NegateRestrictions(ChatRestrictions value) {
	using Flag = ChatRestriction;

	return (~value) & (Flag(0)
		// view_messages is always allowed, so it is never in restrictions.
		//| Flag::f_view_messages
		| Flag::f_change_info
		| Flag::f_embed_links
		| Flag::f_invite_users
		| Flag::f_pin_messages
		| Flag::f_send_games
		| Flag::f_send_gifs
		| Flag::f_send_inline
		| Flag::f_send_media
		| Flag::f_send_messages
		| Flag::f_send_polls
		| Flag::f_send_stickers);
}

auto Dependencies(ChatAdminRights)
-> std::vector<std::pair<ChatAdminRight, ChatAdminRight>> {
	return {};
}

auto ToPositiveNumberString() {
	return rpl::map([](int count) {
		return count ? QString::number(count) : QString();
	});
}

ChatRestrictions DisabledByAdminRights(not_null<PeerData*> peer) {
	using Flag = ChatRestriction;
	using Admin = ChatAdminRight;
	using Admins = ChatAdminRights;

	const auto adminRights = [&] {
		const auto full = ~Admins(0);
		if (const auto chat = peer->asChat()) {
			return chat->amCreator() ? full : chat->adminRights();
		} else if (const auto channel = peer->asChannel()) {
			return channel->amCreator() ? full : channel->adminRights();
		}
		Unexpected("User in DisabledByAdminRights.");
	}();
	return Flag(0)
		| ((adminRights & Admin::f_pin_messages)
			? Flag(0)
			: Flag::f_pin_messages)
		| ((adminRights & Admin::f_invite_users)
			? Flag(0)
			: Flag::f_invite_users)
		| ((adminRights & Admin::f_change_info)
			? Flag(0)
			: Flag::f_change_info);
}

} // namespace

ChatAdminRights DisabledByDefaultRestrictions(not_null<PeerData*> peer) {
	using Flag = ChatAdminRight;
	using Restriction = ChatRestriction;

	const auto restrictions = FixDependentRestrictions([&] {
		if (const auto chat = peer->asChat()) {
			return chat->defaultRestrictions();
		} else if (const auto channel = peer->asChannel()) {
			return channel->defaultRestrictions();
		}
		Unexpected("User in DisabledByDefaultRestrictions.");
	}());
	return Flag(0)
		| ((restrictions & Restriction::f_pin_messages)
			? Flag(0)
			: Flag::f_pin_messages)
		//
		// We allow to edit 'invite_users' admin right no matter what
		// is chosen in default permissions for 'invite_users', because
		// if everyone can 'invite_users' it handles invite link for admins.
		//
		//| ((restrictions & Restriction::f_invite_users)
		//	? Flag(0)
		//	: Flag::f_invite_users)
		//
		| ((restrictions & Restriction::f_change_info)
			? Flag(0)
			: Flag::f_change_info);
}

ChatRestrictions FixDependentRestrictions(ChatRestrictions restrictions) {
	const auto &dependencies = Dependencies(restrictions);

	// Fix iOS bug of saving send_inline like embed_links.
	// We copy send_stickers to send_inline.
	if (restrictions & ChatRestriction::f_send_stickers) {
		restrictions |= ChatRestriction::f_send_inline;
	} else {
		restrictions &= ~ChatRestriction::f_send_inline;
	}

	// Apply the strictest.
	const auto fixOne = [&] {
		for (const auto [first, second] : dependencies) {
			if ((restrictions & second) && !(restrictions & first)) {
				restrictions |= first;
				return true;
			}
		}
		return false;
	};
	while (fixOne()) {
	}
	return restrictions;
}

ChatAdminRights FullAdminRights(bool isGroup) {
	auto result = ChatAdminRights();
	for (const auto &[flag, label] : AdminRightLabels(isGroup, true)) {
		result |= flag;
	}
	return result;
}

EditPeerPermissionsBox::EditPeerPermissionsBox(
	QWidget*,
	not_null<PeerData*> peer)
: _peer(peer->migrateToOrMe()) {
}

auto EditPeerPermissionsBox::saveEvents() const
-> rpl::producer<MTPDchatBannedRights::Flags> {
	Expects(_save != nullptr);

	return _save->clicks() | rpl::map(_value);
}

void EditPeerPermissionsBox::prepare() {
	setTitle(tr::lng_manage_peer_permissions());

	const auto inner = setInnerWidget(object_ptr<Ui::VerticalLayout>(this));

	using Flag = ChatRestriction;
	using Flags = ChatRestrictions;

	const auto disabledByAdminRights = DisabledByAdminRights(_peer);
	const auto restrictions = FixDependentRestrictions([&] {
		if (const auto chat = _peer->asChat()) {
			return chat->defaultRestrictions()
				| disabledByAdminRights;
		} else if (const auto channel = _peer->asChannel()) {
			return channel->defaultRestrictions()
				| (channel->isPublic()
					? (Flag::f_change_info | Flag::f_pin_messages)
					: Flags(0))
				| disabledByAdminRights;
		}
		Unexpected("User in EditPeerPermissionsBox.");
	}());
	const auto disabledMessages = [&] {
		auto result = std::map<Flags, QString>();
			result.emplace(
				disabledByAdminRights,
				tr::lng_rights_permission_cant_edit(tr::now));
		if (const auto channel = _peer->asChannel()) {
			if (channel->isPublic()) {
				result.emplace(
					Flag::f_change_info | Flag::f_pin_messages,
					tr::lng_rights_permission_unavailable(tr::now));
			}
		}
		return result;
	}();

	auto [checkboxes, getRestrictions, changes] = CreateEditRestrictions(
		this,
		tr::lng_rights_default_restrictions_header(),
		restrictions,
		disabledMessages);

	inner->add(std::move(checkboxes));

	addBannedButtons(inner);

	_value = getRestrictions;
	_save = addButton(tr::lng_settings_save());
	addButton(tr::lng_cancel(), [=] { closeBox(); });

	setDimensionsToContent(st::boxWidth, inner);
}

void EditPeerPermissionsBox::addBannedButtons(
		not_null<Ui::VerticalLayout*> container) {
	if (const auto chat = _peer->asChat()) {
		if (!chat->amCreator()) {
			return;
		}
	}
	const auto channel = _peer->asChannel();

	container->add(
		object_ptr<BoxContentDivider>(container),
		{ 0, st::infoProfileSkip, 0, st::infoProfileSkip });

	const auto navigation = App::wnd()->sessionController();
	container->add(EditPeerInfoBox::CreateButton(
		container,
		tr::lng_manage_peer_exceptions(),
		(channel
			? Info::Profile::RestrictedCountValue(channel)
			: rpl::single(0)) | ToPositiveNumberString(),
		[=] {
			ParticipantsBoxController::Start(
				navigation,
				_peer,
				ParticipantsBoxController::Role::Restricted);
		},
		st::peerPermissionsButton));
	if (channel) {
		container->add(EditPeerInfoBox::CreateButton(
			container,
			tr::lng_manage_peer_removed_users(),
			Info::Profile::KickedCountValue(channel)
			| ToPositiveNumberString(),
			[=] {
				ParticipantsBoxController::Start(
					navigation,
					_peer,
					ParticipantsBoxController::Role::Kicked);
			},
			st::peerPermissionsButton));
	}
}

template <
	typename Flags,
	typename DisabledMessagePairs,
	typename FlagLabelPairs>
EditFlagsControl<Flags> CreateEditFlags(
		QWidget *parent,
		rpl::producer<QString> header,
		Flags checked,
		const DisabledMessagePairs &disabledMessagePairs,
		const FlagLabelPairs &flagLabelPairs) {
	auto widget = object_ptr<Ui::VerticalLayout>(parent);
	const auto container = widget.data();

	const auto checkboxes = container->lifetime(
	).make_state<std::map<Flags, QPointer<Ui::Checkbox>>>();

	const auto value = [=] {
		auto result = Flags(0);
		for (const auto &[flags, checkbox] : *checkboxes) {
			if (checkbox->checked()) {
				result |= flags;
			} else {
				result &= ~flags;
			}
		}
		return result;
	};

	const auto changes = container->lifetime(
	).make_state<rpl::event_stream<>>();

	const auto applyDependencies = [=](Ui::Checkbox *control) {
		static const auto dependencies = Dependencies(Flags());
		ApplyDependencies(*checkboxes, dependencies, control);
	};

	container->add(
		object_ptr<Ui::FlatLabel>(
			container,
			std::move(header),
			st::rightsHeaderLabel),
		st::rightsHeaderMargin);

	auto addCheckbox = [&](Flags flags, const QString &text) {
		const auto lockedIt = ranges::find_if(
			disabledMessagePairs,
			[&](const auto &pair) { return (pair.first & flags) != 0; });
		const auto locked = (lockedIt != end(disabledMessagePairs))
			? std::make_optional(lockedIt->second)
			: std::nullopt;
		const auto toggled = ((checked & flags) != 0);
		auto toggle = std::make_unique<Ui::ToggleView>(
			st::rightsToggle,
			toggled);
		toggle->setLocked(locked.has_value());
		const auto control = container->add(
			object_ptr<Ui::Checkbox>(
				container,
				text,
				st::rightsCheckbox,
				std::move(toggle)),
			st::rightsToggleMargin);
		control->checkedChanges(
		) | rpl::start_with_next([=](bool checked) {
			if (locked.has_value()) {
				if (checked != toggled) {
					Ui::Toast::Show(*locked);
					control->setChecked(toggled);
				}
			} else {
				InvokeQueued(control, [=] {
					applyDependencies(control);
					changes->fire({});
				});
			}
		}, control->lifetime());
		checkboxes->emplace(flags, control);
	};
	for (const auto &[flags, label] : flagLabelPairs) {
		addCheckbox(flags, label);
	}

	applyDependencies(nullptr);
	for (const auto &[flags, checkbox] : *checkboxes) {
		checkbox->finishAnimating();
	}

	return {
		std::move(widget),
		value,
		changes->events() | rpl::map(value)
	};
}

EditFlagsControl<MTPDchatBannedRights::Flags> CreateEditRestrictions(
		QWidget *parent,
		rpl::producer<QString> header,
		MTPDchatBannedRights::Flags restrictions,
		std::map<MTPDchatBannedRights::Flags, QString> disabledMessages) {
	auto result = CreateEditFlags(
		parent,
		header,
		NegateRestrictions(restrictions),
		disabledMessages,
		RestrictionLabels());
	result.value = [original = std::move(result.value)]{
		return NegateRestrictions(original());
	};
	result.changes = std::move(
		result.changes
	) | rpl::map(NegateRestrictions);

	return result;
}

EditFlagsControl<MTPDchatAdminRights::Flags> CreateEditAdminRights(
		QWidget *parent,
	rpl::producer<QString> header,
		MTPDchatAdminRights::Flags rights,
		std::map<MTPDchatAdminRights::Flags, QString> disabledMessages,
		bool isGroup,
		bool anyoneCanAddMembers) {
	return CreateEditFlags(
		parent,
		header,
		rights,
		disabledMessages,
		AdminRightLabels(isGroup, anyoneCanAddMembers));
}
