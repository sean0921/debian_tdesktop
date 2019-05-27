/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "window/window_peer_menu.h"

#include "lang/lang_keys.h"
#include "boxes/confirm_box.h"
#include "boxes/mute_settings_box.h"
#include "boxes/add_contact_box.h"
#include "boxes/report_box.h"
#include "boxes/create_poll_box.h"
#include "boxes/peers/add_participants_box.h"
#include "ui/toast/toast.h"
#include "auth_session.h"
#include "apiwrap.h"
#include "mainwidget.h"
#include "mainwindow.h"
#include "observer_peer.h"
#include "history/history.h"
#include "window/window_controller.h"
#include "support/support_helper.h"
#include "info/info_memento.h"
#include "info/info_controller.h"
//#include "info/feed/info_feed_channels_controllers.h" // #feed
#include "info/profile/info_profile_values.h"
#include "data/data_session.h"
#include "data/data_folder.h"
#include "data/data_poll.h"
#include "data/data_channel.h"
#include "data/data_chat.h"
#include "data/data_drafts.h"
#include "data/data_user.h"
#include "dialogs/dialogs_key.h"
#include "boxes/peers/edit_peer_info_box.h"
#include "styles/style_boxes.h"
#include "styles/style_window.h" // st::windowMinWidth

namespace Window {
namespace {

constexpr auto kArchivedToastDuration = crl::time(3000);

class Filler {
public:
	Filler(
		not_null<Controller*> controller,
		not_null<PeerData*> peer,
		const PeerMenuCallback &addAction,
		PeerMenuSource source);
	void fill();

private:
	bool showInfo();
	bool showToggleArchived();
	bool showTogglePin();
	void addTogglePin();
	void addInfo();
	//void addSearch();
	void addToggleUnreadMark();
	void addToggleArchive();
	void addUserActions(not_null<UserData*> user);
	void addBlockUser(not_null<UserData*> user);
	void addChatActions(not_null<ChatData*> chat);
	void addChannelActions(not_null<ChannelData*> channel);

	not_null<Controller*> _controller;
	not_null<PeerData*> _peer;
	const PeerMenuCallback &_addAction;
	PeerMenuSource _source;

};

class FolderFiller {
public:
	FolderFiller(
		not_null<Controller*> controller,
		not_null<Data::Folder*> folder,
		const PeerMenuCallback &addAction,
		PeerMenuSource source);
	void fill();

private:
	void addToggleCollapse();
	//bool showInfo();
	//void addTogglePin();
	//void addInfo();
	//void addSearch();
	//void addNotifications();
	//void addUngroup();

	not_null<Controller*> _controller;
	not_null<Data::Folder*> _folder;
	const PeerMenuCallback &_addAction;
	PeerMenuSource _source;

};

History *FindWastedPin(Data::Folder *folder) {
	const auto &order = Auth().data().pinnedChatsOrder(folder);
	for (const auto &pinned : order) {
		if (const auto history = pinned.history()) {
			if (history->peer->isChat()
				&& history->peer->asChat()->isDeactivated()
				&& !history->inChatList()) {
				return history;
			}
		}
	}
	return nullptr;
}

void AddChatMembers(not_null<ChatData*> chat) {
	AddParticipantsBoxController::Start(chat);
}

bool PinnedLimitReached(Dialogs::Key key) {
	Expects(key.entry()->folderKnown());

	const auto folder = key.entry()->folder();
	const auto pinnedCount = Auth().data().pinnedChatsCount(folder);
	const auto pinnedMax = Auth().data().pinnedChatsLimit(folder);
	if (pinnedCount < pinnedMax) {
		return false;
	}
	// Some old chat, that was converted, maybe is still pinned.
	if (const auto wasted = FindWastedPin(folder)) {
		Auth().data().setChatPinned(wasted, false);
		Auth().data().setChatPinned(key, true);
		Auth().api().savePinnedOrder(folder);
	} else {
		auto errorText = lng_error_pinned_max(
			lt_count,
			pinnedMax);
		Ui::show(Box<InformBox>(errorText));
	}
	return true;
}

void TogglePinnedDialog(Dialogs::Key key) {
	if (!key.entry()->folderKnown()) {
		return;
	}
	const auto isPinned = !key.entry()->isPinnedDialog();
	if (isPinned && PinnedLimitReached(key)) {
		return;
	}

	Auth().data().setChatPinned(key, isPinned);
	const auto flags = isPinned
		? MTPmessages_ToggleDialogPin::Flag::f_pinned
		: MTPmessages_ToggleDialogPin::Flag(0);
	if (const auto history = key.history()) {
		history->session().api().request(MTPmessages_ToggleDialogPin(
			MTP_flags(flags),
			MTP_inputDialogPeer(key.history()->peer->input)
		)).send();
	} else if (const auto folder = key.folder()) {
		folder->session().api().request(MTPmessages_ToggleDialogPin(
			MTP_flags(flags),
			MTP_inputDialogPeerFolder(MTP_int(folder->id()))
		)).send();
	}
	if (isPinned) {
		if (const auto main = App::main()) {
			main->dialogsToUp();
		}
	}
}

Filler::Filler(
	not_null<Controller*> controller,
	not_null<PeerData*> peer,
	const PeerMenuCallback &addAction,
	PeerMenuSource source)
: _controller(controller)
, _peer(peer)
, _addAction(addAction)
, _source(source) {
}

bool Filler::showInfo() {
	if (_source == PeerMenuSource::Profile
		|| _source == PeerMenuSource::ChatsList
		|| _peer->isSelf()) {
		return false;
	} else if (_controller->activeChatCurrent().peer() != _peer) {
		return true;
	} else if (!Adaptive::ThreeColumn()) {
		return true;
	} else if (
		!Auth().settings().thirdSectionInfoEnabled() &&
		!Auth().settings().tabbedReplacedWithInfo()) {
		return true;
	}
	return false;
}

bool Filler::showToggleArchived() {
	if (_source != PeerMenuSource::ChatsList) {
		return false;
	}
	const auto history = _peer->owner().historyLoaded(_peer);
	if (history && history->useProxyPromotion()) {
		return false;
	} else if (!_peer->isNotificationsUser() && !_peer->isSelf()) {
		return true;
	}
	return history && (history->folder() != nullptr);
}

bool Filler::showTogglePin() {
	if (_source != PeerMenuSource::ChatsList) {
		return false;
	}
	const auto history = _peer->owner().historyLoaded(_peer);
	return history && !history->fixedOnTopIndex();
}

void Filler::addTogglePin() {
	auto peer = _peer;
	auto isPinned = false;
	if (auto history = peer->owner().historyLoaded(peer)) {
		isPinned = history->isPinnedDialog();
	}
	auto pinText = [](bool isPinned) {
		return lang(isPinned
			? lng_context_unpin_from_top
			: lng_context_pin_to_top);
	};
	auto pinToggle = [=] {
		TogglePinnedDialog(peer->owner().history(peer));
	};
	auto pinAction = _addAction(pinText(isPinned), pinToggle);

	const auto lifetime = Ui::CreateChild<rpl::lifetime>(pinAction);
	Notify::PeerUpdateViewer(
		peer,
		Notify::PeerUpdate::Flag::ChatPinnedChanged
	) | rpl::start_with_next([peer, pinAction, pinText] {
		auto isPinned = peer->owner().history(peer)->isPinnedDialog();
		pinAction->setText(pinText(isPinned));
	}, *lifetime);
}

void Filler::addInfo() {
	auto controller = _controller;
	auto peer = _peer;
	auto infoKey = (peer->isChat() || peer->isMegagroup())
		? lng_context_view_group
		: (peer->isUser()
			? lng_context_view_profile
			: lng_context_view_channel);
	_addAction(lang(infoKey), [=] {
		controller->showPeerInfo(peer);
	});
}

//void Filler::addSearch() {
//	_addAction(lang(lng_profile_search_messages), [peer = _peer] {
//		App::main()->searchInChat(peer->owner().history(peer));
//	});
//}

void Filler::addToggleUnreadMark() {
	const auto peer = _peer;
	const auto isUnread = [](not_null<PeerData*> peer) {
		if (const auto history = peer->owner().historyLoaded(peer)) {
			return (history->chatListUnreadCount() > 0)
				|| (history->chatListUnreadMark());
		}
		return false;
	};
	const auto label = [=](not_null<PeerData*> peer) {
		return lang(isUnread(peer)
			? lng_context_mark_read
			: lng_context_mark_unread);
	};
	auto action = _addAction(label(peer), [=] {
		const auto markAsRead = isUnread(peer);
		const auto handle = [&](not_null<History*> history) {
			if (markAsRead) {
				Auth().api().readServerHistory(history);
			} else {
				Auth().api().changeDialogUnreadMark(history, !markAsRead);
			}
		};
		const auto history = peer->owner().history(peer);
		handle(history);
		if (markAsRead) {
			if (const auto migrated = history->migrateSibling()) {
				handle(migrated);
			}
		}
	});

	const auto lifetime = Ui::CreateChild<rpl::lifetime>(action);
	Notify::PeerUpdateViewer(
		_peer,
		Notify::PeerUpdate::Flag::UnreadViewChanged
	) | rpl::start_with_next([=] {
		action->setText(label(peer));
	}, *lifetime);
}

void Filler::addToggleArchive() {
	const auto peer = _peer;
	const auto archived = [&] {
		const auto history = peer->owner().historyLoaded(peer);
		return history && history->folder();
	}();
	const auto toggle = [=] {
		ToggleHistoryArchived(
			peer->owner().history(peer),
			!archived);
	};
	_addAction(
		lang(archived ? lng_archived_remove : lng_archived_add),
		toggle);
}

void Filler::addBlockUser(not_null<UserData*> user) {
	auto blockText = [](not_null<UserData*> user) {
		return lang(user->isBlocked()
			? ((user->isBot() && !user->isSupport())
				? lng_profile_restart_bot
				: lng_profile_unblock_user)
			: ((user->isBot() && !user->isSupport())
				? lng_profile_block_bot
				: lng_profile_block_user));
	};
	auto blockAction = _addAction(blockText(user), [=] {
		if (user->isBlocked()) {
			Auth().api().unblockUser(user);
		} else {
			Auth().api().blockUser(user);
		}
	});

	const auto lifetime = Ui::CreateChild<rpl::lifetime>(blockAction);
	Notify::PeerUpdateViewer(
		_peer,
		Notify::PeerUpdate::Flag::UserIsBlocked
	) | rpl::start_with_next([=] {
		blockAction->setText(blockText(user));
	}, *lifetime);

	if (user->blockStatus() == UserData::BlockStatus::Unknown) {
		Auth().api().requestFullPeer(user);
	}
}

void Filler::addUserActions(not_null<UserData*> user) {
	if (_source != PeerMenuSource::ChatsList) {
		if (Auth().supportMode()) {
			_addAction("Edit support info", [=] {
				Auth().supportHelper().editInfo(user);
			});
		}
		if (user->isContact()) {
			if (!user->isSelf()) {
				_addAction(
					lang(lng_info_share_contact),
					[user] { PeerMenuShareContactBox(user); });
				_addAction(
					lang(lng_info_edit_contact),
					[user] { Ui::show(Box<AddContactBox>(user)); });
				_addAction(
					lang(lng_info_delete_contact),
					[user] { PeerMenuDeleteContact(user); });
			}
		} else if (user->canShareThisContact()) {
			if (!user->isSelf()) {
				_addAction(
					lang(lng_info_add_as_contact),
					[user] { PeerMenuAddContact(user); });
			}
			_addAction(
				lang(lng_info_share_contact),
				[user] { PeerMenuShareContactBox(user); });
		} else if (user->botInfo && !user->botInfo->cantJoinGroups) {
			_addAction(
				lang(lng_profile_invite_to_group),
				[user] { AddBotToGroupBoxController::Start(user); });
		}
		if (user->canExportChatHistory()) {
			_addAction(
				lang(lng_profile_export_chat),
				[=] { PeerMenuExportChat(user); });
		}
	}
	_addAction(
		lang(lng_profile_delete_conversation),
		DeleteAndLeaveHandler(user));
	_addAction(
		lang(lng_profile_clear_history),
		ClearHistoryHandler(user));
	if (!user->isInaccessible()
		&& user != Auth().user()
		&& _source != PeerMenuSource::ChatsList) {
		addBlockUser(user);
	}
}

void Filler::addChatActions(not_null<ChatData*> chat) {
	if (_source != PeerMenuSource::ChatsList) {
		if (EditPeerInfoBox::Available(chat)) {
			const auto text = lang(lng_manage_group_title);
			_addAction(text, [=] {
				App::wnd()->controller()->showEditPeerBox(chat);
			});
		}
		if (chat->canAddMembers()) {
			_addAction(
				lang(lng_profile_add_participant),
				[chat] { AddChatMembers(chat); });
		}
		if (chat->canSendPolls()) {
			_addAction(
				lang(lng_polls_create),
				[=] { PeerMenuCreatePoll(chat); });
		}
		if (chat->canExportChatHistory()) {
			_addAction(
				lang(lng_profile_export_chat),
				[=] { PeerMenuExportChat(chat); });
		}
	}
	_addAction(
		lang(lng_profile_clear_and_exit),
		DeleteAndLeaveHandler(_peer));
	_addAction(
		lang(lng_profile_clear_history),
		ClearHistoryHandler(_peer));
}

void Filler::addChannelActions(not_null<ChannelData*> channel) {
	auto isGroup = channel->isMegagroup();
	//if (!isGroup) { // #feed
	//	const auto feed = channel->feed();
	//	const auto grouped = (feed != nullptr);
	//	if (!grouped || feed->channels().size() > 1) {
	//		_addAction( // #feed
	//			lang(grouped ? lng_feed_ungroup : lng_feed_group),
	//			[=] { ToggleChannelGrouping(channel, !grouped); });
	//	}
	//}
	if (_source != PeerMenuSource::ChatsList) {
		if (EditPeerInfoBox::Available(channel)) {
			const auto text = lang(isGroup
				? lng_manage_group_title
				: lng_manage_channel_title);
			_addAction(text, [channel] {
				App::wnd()->controller()->showEditPeerBox(channel);
			});
		}
		if (channel->canAddMembers()) {
			_addAction(
				lang(lng_channel_add_members),
				[channel] { PeerMenuAddChannelMembers(channel); });
		}
		if (channel->canSendPolls()) {
			_addAction(
				lang(lng_polls_create),
				[=] { PeerMenuCreatePoll(channel); });
		}
		if (channel->canExportChatHistory()) {
			_addAction(
				lang(isGroup
					? lng_profile_export_chat
					: lng_profile_export_channel),
				[=] { PeerMenuExportChat(channel); });
		}
	}
	if (channel->amIn()) {
		if (isGroup && !channel->isPublic()) {
			_addAction(
				lang(lng_profile_clear_history),
				ClearHistoryHandler(channel));
		}
		auto text = lang(isGroup
			? lng_profile_leave_group
			: lng_profile_leave_channel);
		_addAction(text, DeleteAndLeaveHandler(channel));
	} else {
		auto text = lang(isGroup
			? lng_profile_join_group
			: lng_profile_join_channel);
		_addAction(
			text,
			[channel] { Auth().api().joinChannel(channel); });
	}
	if (_source != PeerMenuSource::ChatsList) {
		auto needReport = !channel->amCreator()
			&& (!isGroup || channel->isPublic());
		if (needReport) {
			_addAction(lang(lng_profile_report), [channel] {
				Ui::show(Box<ReportBox>(channel));
			});
		}
	}
}

void Filler::fill() {
	if (showToggleArchived()) {
		addToggleArchive();
	}
	if (showTogglePin()) {
		addTogglePin();
	}
	if (showInfo()) {
		addInfo();
	}
	if (_source != PeerMenuSource::Profile && !_peer->isSelf()) {
		PeerMenuAddMuteAction(_peer, _addAction);
	}
	if (_source == PeerMenuSource::ChatsList) {
		//addSearch();
		addToggleUnreadMark();
	}

	if (const auto user = _peer->asUser()) {
		addUserActions(user);
	} else if (const auto chat = _peer->asChat()) {
		addChatActions(chat);
	} else if (const auto channel = _peer->asChannel()) {
		addChannelActions(channel);
	}
}

FolderFiller::FolderFiller(
	not_null<Controller*> controller,
	not_null<Data::Folder*> folder,
	const PeerMenuCallback &addAction,
	PeerMenuSource source)
: _controller(controller)
, _folder(folder)
, _addAction(addAction)
, _source(source) {
}

void FolderFiller::fill() {
	if (_source == PeerMenuSource::ChatsList) {
		addToggleCollapse();
	}
}

void FolderFiller::addToggleCollapse() {
	if (_folder->id() != Data::Folder::kId) {
		return;
	}
	const auto controller = _controller;
	const auto hidden = controller->session().settings().archiveCollapsed();
	const auto text = lang(hidden
		? lng_context_archive_expand
		: lng_context_archive_collapse);
	_addAction(text, [=] {
		controller->session().settings().setArchiveCollapsed(!hidden);
		controller->session().saveSettingsDelayed();
	});
}
//
//void FolderFiller::addInfo() {
//	auto controller = _controller;
//	auto feed = _feed;
//	_addAction(lang(lng_context_view_feed_info), [=] {
//		controller->showSection(Info::Memento(
//			feed,
//			Info::Section(Info::Section::Type::Profile)));
//	});
//}
//
//void FolderFiller::addNotifications() {
//	const auto feed = _feed;
//	_addAction(lang(lng_feed_notifications), [=] {
//		Info::FeedProfile::NotificationsController::Start(feed);
//	});
//}
//
//void FolderFiller::addSearch() {
//	const auto feed = _feed;
//	_addAction(lang(lng_profile_search_messages), [=] {
//		App::main()->searchInChat(feed);
//	});
//}
//
//void FolderFiller::addUngroup() {
//	const auto feed = _feed;
//	//_addAction(lang(lng_feed_ungroup_all), [=] { // #feed
//	//	PeerMenuUngroupFeed(feed);
//	//});
//}

} // namespace

void PeerMenuExportChat(not_null<PeerData*> peer) {
	Auth().data().startExport(peer);
}

void PeerMenuDeleteContact(not_null<UserData*> user) {
	const auto text = lng_sure_delete_contact(
		lt_contact,
		App::peerName(user));
	const auto deleteSure = [=] {
		Ui::hideLayer();
		user->session().api().request(MTPcontacts_DeleteContact(
			user->inputUser
		)).done([=](const MTPcontacts_Link &result) {
			result.match([&](const MTPDcontacts_link &data) {
				user->owner().processUser(data.vuser);
				App::feedUserLink(
					MTP_int(peerToUser(user->id)),
					data.vmy_link,
					data.vforeign_link);
			});
		}).send();
	};
	Ui::show(Box<ConfirmBox>(
		text,
		lang(lng_box_delete),
		deleteSure));
}

void PeerMenuAddContact(not_null<UserData*> user) {
	Ui::show(Box<AddContactBox>(
		user->firstName,
		user->lastName,
		Auth().data().findContactPhone(user)));
}

void PeerMenuShareContactBox(not_null<UserData*> user) {
	const auto weak = std::make_shared<QPointer<PeerListBox>>();
	auto callback = [=](not_null<PeerData*> peer) {
		if (!peer->canWrite()) {
			Ui::show(Box<InformBox>(
				lang(lng_forward_share_cant)),
				LayerOption::KeepOther);
			return;
		} else if (peer->isSelf()) {
			auto options = ApiWrap::SendOptions(peer->owner().history(peer));
			Auth().api().shareContact(user, options);
			Ui::Toast::Show(lang(lng_share_done));
			if (auto strong = *weak) {
				strong->closeBox();
			}
			return;
		}
		auto recipient = peer->isUser()
			? peer->name
			: '\xAB' + peer->name + '\xBB';
		Ui::show(Box<ConfirmBox>(
			lng_forward_share_contact(lt_recipient, recipient),
			lang(lng_forward_send),
			[peer, user] {
				const auto history = peer->owner().history(peer);
				Ui::showPeerHistory(history, ShowAtTheEndMsgId);
				auto options = ApiWrap::SendOptions(history);
				Auth().api().shareContact(user, options);
			}), LayerOption::KeepOther);
	};
	*weak = Ui::show(Box<PeerListBox>(
		std::make_unique<ChooseRecipientBoxController>(std::move(callback)),
		[](not_null<PeerListBox*> box) {
			box->addButton(langFactory(lng_cancel), [box] {
				box->closeBox();
			});
		}));
}

void PeerMenuCreatePoll(not_null<PeerData*> peer) {
	const auto box = Ui::show(Box<CreatePollBox>());
	const auto lock = box->lifetime().make_state<bool>(false);
	box->submitRequests(
	) | rpl::start_with_next([=](const PollData &result) {
		if (std::exchange(*lock, true)) {
			return;
		}
		auto options = ApiWrap::SendOptions(peer->owner().history(peer));
		if (const auto id = App::main()->currentReplyToIdFor(options.history)) {
			options.replyTo = id;
		}
		if (const auto localDraft = options.history->localDraft()) {
			options.clearDraft = localDraft->textWithTags.text.isEmpty();
		}

		Auth().api().createPoll(result, options, crl::guard(box, [=] {
			box->closeBox();
		}), crl::guard(box, [=](const RPCError &error) {
			*lock = false;
			box->submitFailed(lang(lng_attach_failed));
		}));
	}, box->lifetime());
}

QPointer<Ui::RpWidget> ShowForwardMessagesBox(
		MessageIdsList &&items,
		FnMut<void()> &&successCallback) {
	const auto weak = std::make_shared<QPointer<PeerListBox>>();
	auto callback = [
		ids = std::move(items),
		callback = std::move(successCallback),
		weak
	](not_null<PeerData*> peer) mutable {
		if (peer->isSelf()) {
			auto items = Auth().data().idsToItems(ids);
			if (!items.empty()) {
				auto options = ApiWrap::SendOptions(peer->owner().history(peer));
				options.generateLocal = false;
				Auth().api().forwardMessages(std::move(items), options, [] {
					Ui::Toast::Show(lang(lng_share_done));
				});
			}
		} else {
			App::main()->setForwardDraft(peer->id, std::move(ids));
		}
		if (const auto strong = *weak) {
			strong->closeBox();
		}
		if (callback) {
			callback();
		}
	};
	auto initBox = [](not_null<PeerListBox*> box) {
		box->addButton(langFactory(lng_cancel), [box] {
			box->closeBox();
		});
	};
	*weak = Ui::show(Box<PeerListBox>(
		std::make_unique<ChooseRecipientBoxController>(std::move(callback)),
		std::move(initBox)), LayerOption::KeepOther);
	return weak->data();
}

void PeerMenuAddChannelMembers(not_null<ChannelData*> channel) {
	if (!channel->isMegagroup()
		&& channel->membersCount() >= Global::ChatSizeMax()) {
		Ui::show(
			Box<MaxInviteBox>(channel),
			LayerOption::KeepOther);
		return;
	}
	auto callback = [=](const MTPchannels_ChannelParticipants &result) {
		Auth().api().parseChannelParticipants(channel, result, [&](
				int availableCount,
				const QVector<MTPChannelParticipant> &list) {
			auto already = (
				list
			) | ranges::view::transform([](const MTPChannelParticipant &p) {
				return p.match([](const auto &data) {
					return data.vuser_id.v;
				});
			}) | ranges::view::transform([](UserId userId) {
				return Auth().data().userLoaded(userId);
			}) | ranges::view::filter([](UserData *user) {
				return (user != nullptr);
			}) | ranges::to_vector;

			AddParticipantsBoxController::Start(
				channel,
				{ already.begin(), already.end() });
		});
	};
	Auth().api().requestChannelMembersForAdd(channel, callback);
}

void PeerMenuAddMuteAction(
		not_null<PeerData*> peer,
		const PeerMenuCallback &addAction) {
	Auth().data().requestNotifySettings(peer);
	const auto muteText = [](bool isMuted) {
		return lang(isMuted
			? lng_enable_notifications_from_tray
			: lng_disable_notifications_from_tray);
	};
	const auto muteAction = addAction(QString("-"), [=] {
		if (!Auth().data().notifyIsMuted(peer)) {
			Ui::show(Box<MuteSettingsBox>(peer));
		} else {
			Auth().data().updateNotifySettings(peer, 0);
		}
	});

	const auto lifetime = Ui::CreateChild<rpl::lifetime>(muteAction);
	Info::Profile::NotificationsEnabledValue(
		peer
	) | rpl::start_with_next([=](bool enabled) {
		muteAction->setText(muteText(!enabled));
	}, *lifetime);
}
// #feed
//void PeerMenuUngroupFeed(not_null<Data::Feed*> feed) {
//	Ui::show(Box<ConfirmBox>(
//		lang(lng_feed_sure_ungroup_all),
//		lang(lng_feed_ungroup_sure),
//		[=] { Ui::hideLayer(); Auth().api().ungroupAllFromFeed(feed); }));
//}
//
void ToggleHistoryArchived(not_null<History*> history, bool archived) {
	const auto callback = [=] {
		Ui::Toast::Config toast;
		toast.text = lang(archived
			? lng_archived_added
			: lng_archived_removed);
		toast.maxWidth = st::boxWideWidth;
		if (archived) {
			toast.durationMs = kArchivedToastDuration;
		}
		Ui::Toast::Show(toast);
	};
	history->session().api().toggleHistoryArchived(
		history,
		archived,
		callback);
}

Fn<void()> ClearHistoryHandler(not_null<PeerData*> peer) {
	return [=] {
		Ui::show(Box<DeleteMessagesBox>(peer, true), LayerOption::KeepOther);
	};
}

Fn<void()> DeleteAndLeaveHandler(not_null<PeerData*> peer) {
	return [=] {
		Ui::show(Box<DeleteMessagesBox>(peer, false), LayerOption::KeepOther);
	};
}

void FillPeerMenu(
		not_null<Controller*> controller,
		not_null<PeerData*> peer,
		const PeerMenuCallback &callback,
		PeerMenuSource source) {
	Filler filler(controller, peer, callback, source);
	filler.fill();
}

void FillFolderMenu(
		not_null<Controller*> controller,
		not_null<Data::Folder*> folder,
		const PeerMenuCallback &callback,
		PeerMenuSource source) {
	FolderFiller filler(controller, folder, callback, source);
	filler.fill();
}

} // namespace Window
