/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "history/admin_log/history_admin_log_item.h"

#include "history/admin_log/history_admin_log_inner.h"
#include "history/view/history_view_element.h"
#include "history/history_service.h"
#include "history/history_message.h"
#include "history/history.h"
#include "data/data_channel.h"
#include "data/data_user.h"
#include "data/data_session.h"
#include "lang/lang_keys.h"
#include "boxes/sticker_set_box.h"
#include "core/application.h"
#include "auth_session.h"

namespace AdminLog {
namespace {

TextWithEntities PrepareText(const QString &value, const QString &emptyValue) {
	auto result = TextWithEntities { TextUtilities::Clean(value) };
	if (result.text.isEmpty()) {
		result.text = emptyValue;
		if (!emptyValue.isEmpty()) {
			result.entities.push_back({
				EntityType::Italic,
				0,
				emptyValue.size() });
		}
	} else {
		TextUtilities::ParseEntities(result, TextParseLinks | TextParseMentions | TextParseHashtags | TextParseBotCommands);
	}
	return result;
}

MTPMessage PrepareLogMessage(
		const MTPMessage &message,
		MsgId newId,
		TimeId newDate) {
	return message.match([&](const MTPDmessageEmpty &) {
		return MTP_messageEmpty(MTP_int(newId));
	}, [&](const MTPDmessageService &message) {
		const auto removeFlags = MTPDmessageService::Flag::f_out
			| MTPDmessageService::Flag::f_post
			/* | MTPDmessageService::Flag::f_reply_to_msg_id*/;
		const auto flags = message.vflags.v & ~removeFlags;
		return MTP_messageService(
			MTP_flags(flags),
			MTP_int(newId),
			message.vfrom_id,
			message.vto_id,
			message.vreply_to_msg_id,
			MTP_int(newDate),
			message.vaction);
	}, [&](const MTPDmessage &message) {
		const auto removeFlags = MTPDmessage::Flag::f_out
			| MTPDmessage::Flag::f_post
			| MTPDmessage::Flag::f_reply_to_msg_id
			| MTPDmessage::Flag::f_edit_date
			| MTPDmessage::Flag::f_grouped_id;
		const auto flags = message.vflags.v & ~removeFlags;
		return MTP_message(
			MTP_flags(flags),
			MTP_int(newId),
			message.vfrom_id,
			message.vto_id,
			message.vfwd_from,
			message.vvia_bot_id,
			message.vreply_to_msg_id,
			MTP_int(newDate),
			message.vmessage,
			message.vmedia,
			message.vreply_markup,
			message.ventities,
			message.vviews,
			message.vedit_date,
			MTP_string(""),
			message.vgrouped_id);
	});
}

bool MediaCanHaveCaption(const MTPMessage &message) {
	if (message.type() != mtpc_message) {
		return false;
	}
	auto &data = message.c_message();
	auto mediaType = data.has_media() ? data.vmedia.type() : mtpc_messageMediaEmpty;
	return (mediaType == mtpc_messageMediaDocument || mediaType == mtpc_messageMediaPhoto);
}

TextWithEntities ExtractEditedText(const MTPMessage &message) {
	if (message.type() != mtpc_message) {
		return TextWithEntities();
	}
	auto &data = message.c_message();
	auto text = TextUtilities::Clean(qs(data.vmessage));
	auto entities = data.has_entities()
		? TextUtilities::EntitiesFromMTP(data.ventities.v)
		: EntitiesInText();
	return { text, entities };
}

const auto CollectChanges = [](auto &phraseMap, auto plusFlags, auto minusFlags) {
	auto withPrefix = [&phraseMap](auto flags, QChar prefix) {
		auto result = QString();
		for (auto &phrase : phraseMap) {
			if (flags & phrase.first) {
				result.append('\n' + (prefix + lang(phrase.second)));
			}
		}
		return result;
	};
	const auto kMinus = QChar(0x2212);
	return withPrefix(plusFlags & ~minusFlags, '+') + withPrefix(minusFlags & ~plusFlags, kMinus);
};

TextWithEntities GenerateAdminChangeText(
		not_null<ChannelData*> channel,
		const TextWithEntities &user,
		const MTPChatAdminRights *newRights,
		const MTPChatAdminRights *prevRights) {
	Expects(!newRights || newRights->type() == mtpc_chatAdminRights);
	Expects(!prevRights || prevRights->type() == mtpc_chatAdminRights);

	using Flag = MTPDchatAdminRights::Flag;
	using Flags = MTPDchatAdminRights::Flags;

	auto newFlags = newRights ? newRights->c_chatAdminRights().vflags.v : MTPDchatAdminRights::Flags(0);
	auto prevFlags = prevRights ? prevRights->c_chatAdminRights().vflags.v : MTPDchatAdminRights::Flags(0);
	auto result = lng_admin_log_promoted__generic(lt_user, user);

	auto useInviteLinkPhrase = channel->isMegagroup() && channel->anyoneCanAddMembers();
	auto invitePhrase = (useInviteLinkPhrase ? lng_admin_log_admin_invite_link : lng_admin_log_admin_invite_users);
	static auto phraseMap = std::map<Flags, LangKey> {
		{ Flag::f_change_info, lng_admin_log_admin_change_info },
		{ Flag::f_post_messages, lng_admin_log_admin_post_messages },
		{ Flag::f_edit_messages, lng_admin_log_admin_edit_messages },
		{ Flag::f_delete_messages, lng_admin_log_admin_delete_messages },
		{ Flag::f_ban_users, lng_admin_log_admin_ban_users },
		{ Flag::f_invite_users, invitePhrase },
		{ Flag::f_pin_messages, lng_admin_log_admin_pin_messages },
		{ Flag::f_add_admins, lng_admin_log_admin_add_admins },
	};
	phraseMap[Flag::f_invite_users] = invitePhrase;

	if (!channel->isMegagroup()) {
		// Don't display "Ban users" changes in channels.
		newFlags &= ~Flag::f_ban_users;
		prevFlags &= ~Flag::f_ban_users;
	}

	auto changes = CollectChanges(phraseMap, newFlags, prevFlags);
	if (!changes.isEmpty()) {
		result.text.append('\n' + changes);
	}

	return result;
};

QString GenerateBannedChangeText(
		const MTPChatBannedRights *newRights,
		const MTPChatBannedRights *prevRights) {
	Expects(!newRights || newRights->type() == mtpc_chatBannedRights);
	Expects(!prevRights || prevRights->type() == mtpc_chatBannedRights);

	using Flag = MTPDchatBannedRights::Flag;
	using Flags = MTPDchatBannedRights::Flags;

	auto newFlags = newRights ? newRights->c_chatBannedRights().vflags.v : Flags(0);
	auto prevFlags = prevRights ? prevRights->c_chatBannedRights().vflags.v : Flags(0);
	static auto phraseMap = std::map<Flags, LangKey>{
		{ Flag::f_view_messages, lng_admin_log_banned_view_messages },
		{ Flag::f_send_messages, lng_admin_log_banned_send_messages },
		{ Flag::f_send_media, lng_admin_log_banned_send_media },
		{ Flag::f_send_stickers | Flag::f_send_gifs | Flag::f_send_inline | Flag::f_send_games, lng_admin_log_banned_send_stickers },
		{ Flag::f_embed_links, lng_admin_log_banned_embed_links },
		{ Flag::f_send_polls, lng_admin_log_banned_send_polls },
		{ Flag::f_change_info, lng_admin_log_admin_change_info },
		{ Flag::f_invite_users, lng_admin_log_admin_invite_users },
		{ Flag::f_pin_messages, lng_admin_log_admin_pin_messages },
	};
	return CollectChanges(phraseMap, prevFlags, newFlags);
}

TextWithEntities GenerateBannedChangeText(
		const TextWithEntities &user,
		const MTPChatBannedRights *newRights,
		const MTPChatBannedRights *prevRights) {
	Expects(!newRights || newRights->type() == mtpc_chatBannedRights);

	using Flag = MTPDchatBannedRights::Flag;
	using Flags = MTPDchatBannedRights::Flags;

	auto newFlags = newRights ? newRights->c_chatBannedRights().vflags.v : Flags(0);
	auto newUntil = newRights ? newRights->c_chatBannedRights().vuntil_date.v : TimeId(0);
	auto indefinitely = ChannelData::IsRestrictedForever(newUntil);
	if (newFlags & Flag::f_view_messages) {
		return lng_admin_log_banned__generic(lt_user, user);
	}
	auto untilText = indefinitely
		? lang(lng_admin_log_restricted_forever)
		: lng_admin_log_restricted_until(
			lt_date,
			langDateTime(ParseDateTime(newUntil)));
	auto result = lng_admin_log_restricted__generic(
		lt_user,
		user,
		lt_until,
		TextWithEntities { untilText });
	const auto changes = GenerateBannedChangeText(newRights, prevRights);
	if (!changes.isEmpty()) {
		result.text.append('\n' + changes);
	}
	return result;
}

auto GenerateUserString(MTPint userId) {
	// User name in "User name (@username)" format with entities.
	auto user = Auth().data().user(userId.v);
	auto name = TextWithEntities { App::peerName(user) };
	auto entityData = QString::number(user->id)
		+ '.'
		+ QString::number(user->accessHash());
	name.entities.push_back({
		EntityType::MentionName,
		0,
		name.text.size(),
		entityData });
	auto username = user->userName();
	if (username.isEmpty()) {
		return name;
	}
	auto mention = TextWithEntities { '@' + username };
	mention.entities.push_back({
		EntityType::Mention,
		0,
		mention.text.size() });
	return lng_admin_log_user_with_username__generic(lt_name, name, lt_mention, mention);
}

auto GenerateParticipantChangeTextInner(
		not_null<ChannelData*> channel,
		const MTPChannelParticipant &participant,
		const MTPChannelParticipant *oldParticipant) {
	const auto oldType = oldParticipant ? oldParticipant->type() : 0;
	return participant.match([&](const MTPDchannelParticipantCreator &data) {
		// No valid string here :(
		return lng_admin_log_invited__generic(
			lt_user,
			GenerateUserString(data.vuser_id));
	}, [&](const MTPDchannelParticipantAdmin &data) {
		auto user = GenerateUserString(data.vuser_id);
		return GenerateAdminChangeText(
			channel,
			user,
			&data.vadmin_rights,
			(oldType == mtpc_channelParticipantAdmin)
				? &oldParticipant->c_channelParticipantAdmin().vadmin_rights
				: nullptr);
	}, [&](const MTPDchannelParticipantBanned &data) {
		auto user = GenerateUserString(data.vuser_id);
		return GenerateBannedChangeText(
			user,
			&data.vbanned_rights,
			(oldType == mtpc_channelParticipantBanned)
				? &oldParticipant->c_channelParticipantBanned().vbanned_rights
				: nullptr);
	}, [&](const auto &data) {
		auto user = GenerateUserString(data.vuser_id);
		if (oldType == mtpc_channelParticipantAdmin) {
			return GenerateAdminChangeText(
				channel,
				user,
				nullptr,
				&oldParticipant->c_channelParticipantAdmin().vadmin_rights);
		} else if (oldType == mtpc_channelParticipantBanned) {
			return GenerateBannedChangeText(
				user,
				nullptr,
				&oldParticipant->c_channelParticipantBanned().vbanned_rights);
		}
		return lng_admin_log_invited__generic(lt_user, user);
	});
}

TextWithEntities GenerateParticipantChangeText(not_null<ChannelData*> channel, const MTPChannelParticipant &participant, const MTPChannelParticipant *oldParticipant = nullptr) {
	auto result = GenerateParticipantChangeTextInner(channel, participant, oldParticipant);
	result.entities.push_front(EntityInText(EntityType::Italic, 0, result.text.size()));
	return result;
}

TextWithEntities GenerateDefaultBannedRightsChangeText(not_null<ChannelData*> channel, const MTPChatBannedRights &rights, const MTPChatBannedRights &oldRights) {
	auto result = TextWithEntities{ lang(lng_admin_log_changed_default_permissions) };
	const auto changes = GenerateBannedChangeText(&rights, &oldRights);
	if (!changes.isEmpty()) {
		result.text.append('\n' + changes);
	}
	result.entities.push_front(EntityInText(EntityType::Italic, 0, result.text.size()));
	return result;
}

} // namespace

OwnedItem::OwnedItem(std::nullptr_t) {
}

OwnedItem::OwnedItem(
	not_null<HistoryView::ElementDelegate*> delegate,
	not_null<HistoryItem*> data)
: _data(data)
, _view(_data->createView(delegate)) {
}

OwnedItem::OwnedItem(OwnedItem &&other)
: _data(base::take(other._data))
, _view(base::take(other._view)) {
}

OwnedItem &OwnedItem::operator=(OwnedItem &&other) {
	_data = base::take(other._data);
	_view = base::take(other._view);
	return *this;
}

OwnedItem::~OwnedItem() {
	_view = nullptr;
	if (_data) {
		_data->destroy();
	}
}

void OwnedItem::refreshView(
		not_null<HistoryView::ElementDelegate*> delegate) {
	_view = _data->createView(delegate);
}

void GenerateItems(
		not_null<HistoryView::ElementDelegate*> delegate,
		not_null<History*> history,
		not_null<LocalIdManager*> idManager,
		const MTPDchannelAdminLogEvent &event,
		Fn<void(OwnedItem item)> callback) {
	Expects(history->peer->isChannel());

	auto id = event.vid.v;
	auto from = Auth().data().user(event.vuser_id.v);
	auto channel = history->peer->asChannel();
	auto &action = event.vaction;
	auto date = event.vdate.v;
	auto addPart = [&](not_null<HistoryItem*> item) {
		return callback(OwnedItem(delegate, item));
	};

	using Flag = MTPDmessage::Flag;
	auto fromName = App::peerName(from);
	auto fromLink = from->createOpenLink();
	auto fromLinkText = textcmdLink(1, fromName);

	auto addSimpleServiceMessage = [&](const QString &text, PhotoData *photo = nullptr) {
		auto message = HistoryService::PreparedText { text };
		message.links.push_back(fromLink);
		addPart(history->owner().makeServiceMessage(history, idManager->next(), date, message, MTPDmessage::Flags(0), peerToUser(from->id), photo));
	};

	auto createChangeTitle = [&](const MTPDchannelAdminLogEventActionChangeTitle &action) {
		auto text = (channel->isMegagroup() ? lng_action_changed_title : lng_admin_log_changed_title_channel)(lt_from, fromLinkText, lt_title, qs(action.vnew_value));
		addSimpleServiceMessage(text);
	};

	auto createChangeAbout = [&](const MTPDchannelAdminLogEventActionChangeAbout &action) {
		auto newValue = qs(action.vnew_value);
		auto oldValue = qs(action.vprev_value);
		auto text = (channel->isMegagroup()
			? (newValue.isEmpty() ? lng_admin_log_removed_description_group : lng_admin_log_changed_description_group)
			: (newValue.isEmpty() ? lng_admin_log_removed_description_channel : lng_admin_log_changed_description_channel)
			)(lt_from, fromLinkText);
		addSimpleServiceMessage(text);

		auto bodyFlags = Flag::f_entities | Flag::f_from_id;
		auto bodyReplyTo = 0;
		auto bodyViaBotId = 0;
		auto newDescription = PrepareText(newValue, QString());
		auto body = history->owner().makeMessage(history, idManager->next(), bodyFlags, bodyReplyTo, bodyViaBotId, date, peerToUser(from->id), QString(), newDescription);
		if (!oldValue.isEmpty()) {
			auto oldDescription = PrepareText(oldValue, QString());
			body->addLogEntryOriginal(id, lang(lng_admin_log_previous_description), oldDescription);
		}
		addPart(body);
	};

	auto createChangeUsername = [&](const MTPDchannelAdminLogEventActionChangeUsername &action) {
		auto newValue = qs(action.vnew_value);
		auto oldValue = qs(action.vprev_value);
		auto text = (channel->isMegagroup()
			? (newValue.isEmpty() ? lng_admin_log_removed_link_group : lng_admin_log_changed_link_group)
			: (newValue.isEmpty() ? lng_admin_log_removed_link_channel : lng_admin_log_changed_link_channel)
			)(lt_from, fromLinkText);
		addSimpleServiceMessage(text);

		auto bodyFlags = Flag::f_entities | Flag::f_from_id;
		auto bodyReplyTo = 0;
		auto bodyViaBotId = 0;
		auto newLink = newValue.isEmpty() ? TextWithEntities() : PrepareText(Core::App().createInternalLinkFull(newValue), QString());
		auto body = history->owner().makeMessage(history, idManager->next(), bodyFlags, bodyReplyTo, bodyViaBotId, date, peerToUser(from->id), QString(), newLink);
		if (!oldValue.isEmpty()) {
			auto oldLink = PrepareText(Core::App().createInternalLinkFull(oldValue), QString());
			body->addLogEntryOriginal(id, lang(lng_admin_log_previous_link), oldLink);
		}
		addPart(body);
	};

	auto createChangePhoto = [&](const MTPDchannelAdminLogEventActionChangePhoto &action) {
		action.vnew_photo.match([&](const MTPDphoto &data) {
			auto photo = Auth().data().processPhoto(data);
			auto text = (channel->isMegagroup() ? lng_admin_log_changed_photo_group : lng_admin_log_changed_photo_channel)(lt_from, fromLinkText);
			addSimpleServiceMessage(text, photo);
		}, [&](const MTPDphotoEmpty &data) {
			auto text = (channel->isMegagroup() ? lng_admin_log_removed_photo_group : lng_admin_log_removed_photo_channel)(lt_from, fromLinkText);
			addSimpleServiceMessage(text);
		});
	};

	auto createToggleInvites = [&](const MTPDchannelAdminLogEventActionToggleInvites &action) {
		auto enabled = (action.vnew_value.type() == mtpc_boolTrue);
		auto text = (enabled
			? lng_admin_log_invites_enabled
			: lng_admin_log_invites_disabled);
		addSimpleServiceMessage(text(lt_from, fromLinkText));
	};

	auto createToggleSignatures = [&](const MTPDchannelAdminLogEventActionToggleSignatures &action) {
		auto enabled = (action.vnew_value.type() == mtpc_boolTrue);
		auto text = (enabled
			? lng_admin_log_signatures_enabled
			: lng_admin_log_signatures_disabled);
		addSimpleServiceMessage(text(lt_from, fromLinkText));
	};

	auto createUpdatePinned = [&](const MTPDchannelAdminLogEventActionUpdatePinned &action) {
		if (action.vmessage.type() == mtpc_messageEmpty) {
			auto text = lng_admin_log_unpinned_message(lt_from, fromLinkText);
			addSimpleServiceMessage(text);
		} else {
			auto text = lng_admin_log_pinned_message(lt_from, fromLinkText);
			addSimpleServiceMessage(text);

			auto detachExistingItem = false;
			addPart(history->createItem(
				PrepareLogMessage(
					action.vmessage,
					idManager->next(),
					date),
				detachExistingItem));
		}
	};

	auto createEditMessage = [&](const MTPDchannelAdminLogEventActionEditMessage &action) {
		auto newValue = ExtractEditedText(action.vnew_message);
		auto canHaveCaption = MediaCanHaveCaption(action.vnew_message);
		auto text = (canHaveCaption
			? (newValue.text.isEmpty() ? lng_admin_log_removed_caption : lng_admin_log_edited_caption)
			: lng_admin_log_edited_message
			)(lt_from, fromLinkText);
		addSimpleServiceMessage(text);

		auto oldValue = ExtractEditedText(action.vprev_message);
		auto detachExistingItem = false;
		auto body = history->createItem(
			PrepareLogMessage(
				action.vnew_message,
				idManager->next(),
				date),
			detachExistingItem);
		if (oldValue.text.isEmpty()) {
			oldValue = PrepareText(QString(), lang(lng_admin_log_empty_text));
		}
		body->addLogEntryOriginal(id, lang(canHaveCaption ? lng_admin_log_previous_caption : lng_admin_log_previous_message), oldValue);
		addPart(body);
	};

	auto createDeleteMessage = [&](const MTPDchannelAdminLogEventActionDeleteMessage &action) {
		auto text = lng_admin_log_deleted_message(lt_from, fromLinkText);
		addSimpleServiceMessage(text);

		auto detachExistingItem = false;
		addPart(history->createItem(
			PrepareLogMessage(action.vmessage, idManager->next(), date),
			detachExistingItem));
	};

	auto createParticipantJoin = [&]() {
		auto text = (channel->isMegagroup()
			? lng_admin_log_participant_joined
			: lng_admin_log_participant_joined_channel);
		addSimpleServiceMessage(text(lt_from, fromLinkText));
	};

	auto createParticipantLeave = [&]() {
		auto text = (channel->isMegagroup()
			? lng_admin_log_participant_left
			: lng_admin_log_participant_left_channel);
		addSimpleServiceMessage(text(lt_from, fromLinkText));
	};

	auto createParticipantInvite = [&](const MTPDchannelAdminLogEventActionParticipantInvite &action) {
		auto bodyFlags = Flag::f_entities | Flag::f_from_id;
		auto bodyReplyTo = 0;
		auto bodyViaBotId = 0;
		auto bodyText = GenerateParticipantChangeText(channel, action.vparticipant);
		addPart(history->owner().makeMessage(history, idManager->next(), bodyFlags, bodyReplyTo, bodyViaBotId, date, peerToUser(from->id), QString(), bodyText));
	};

	auto createParticipantToggleBan = [&](const MTPDchannelAdminLogEventActionParticipantToggleBan &action) {
		auto bodyFlags = Flag::f_entities | Flag::f_from_id;
		auto bodyReplyTo = 0;
		auto bodyViaBotId = 0;
		auto bodyText = GenerateParticipantChangeText(channel, action.vnew_participant, &action.vprev_participant);
		addPart(history->owner().makeMessage(history, idManager->next(), bodyFlags, bodyReplyTo, bodyViaBotId, date, peerToUser(from->id), QString(), bodyText));
	};

	auto createParticipantToggleAdmin = [&](const MTPDchannelAdminLogEventActionParticipantToggleAdmin &action) {
		auto bodyFlags = Flag::f_entities | Flag::f_from_id;
		auto bodyReplyTo = 0;
		auto bodyViaBotId = 0;
		auto bodyText = GenerateParticipantChangeText(channel, action.vnew_participant, &action.vprev_participant);
		addPart(history->owner().makeMessage(history, idManager->next(), bodyFlags, bodyReplyTo, bodyViaBotId, date, peerToUser(from->id), QString(), bodyText));
	};

	auto createChangeStickerSet = [&](const MTPDchannelAdminLogEventActionChangeStickerSet &action) {
		auto set = action.vnew_stickerset;
		auto removed = (set.type() == mtpc_inputStickerSetEmpty);
		if (removed) {
			auto text = lng_admin_log_removed_stickers_group(lt_from, fromLinkText);
			addSimpleServiceMessage(text);
		} else {
			auto text = lng_admin_log_changed_stickers_group(
				lt_from,
				fromLinkText,
				lt_sticker_set,
				textcmdLink(2, lang(lng_admin_log_changed_stickers_set)));
			auto setLink = std::make_shared<LambdaClickHandler>([set] {
				Ui::show(Box<StickerSetBox>(set));
			});
			auto message = HistoryService::PreparedText { text };
			message.links.push_back(fromLink);
			message.links.push_back(setLink);
			addPart(history->owner().makeServiceMessage(history, idManager->next(), date, message, MTPDmessage::Flags(0), peerToUser(from->id)));
		}
	};

	auto createTogglePreHistoryHidden = [&](const MTPDchannelAdminLogEventActionTogglePreHistoryHidden &action) {
		auto hidden = (action.vnew_value.type() == mtpc_boolTrue);
		auto text = (hidden
			? lng_admin_log_history_made_hidden
			: lng_admin_log_history_made_visible);
		addSimpleServiceMessage(text(lt_from, fromLinkText));
	};

	auto createDefaultBannedRights = [&](const MTPDchannelAdminLogEventActionDefaultBannedRights &action) {
		auto bodyFlags = Flag::f_entities | Flag::f_from_id;
		auto bodyReplyTo = 0;
		auto bodyViaBotId = 0;
		auto bodyText = GenerateDefaultBannedRightsChangeText(channel, action.vnew_banned_rights, action.vprev_banned_rights);
		addPart(history->owner().makeMessage(history, idManager->next(), bodyFlags, bodyReplyTo, bodyViaBotId, date, peerToUser(from->id), QString(), bodyText));
	};

	auto createStopPoll = [&](const MTPDchannelAdminLogEventActionStopPoll &action) {
		auto text = lng_admin_log_stopped_poll(lt_from, fromLinkText);
		addSimpleServiceMessage(text);

		auto detachExistingItem = false;
		addPart(history->createItem(
			PrepareLogMessage(action.vmessage, idManager->next(), date),
			detachExistingItem));
	};

	action.match([&](const MTPDchannelAdminLogEventActionChangeTitle &data) {
		createChangeTitle(data);
	}, [&](const MTPDchannelAdminLogEventActionChangeAbout &data) {
		createChangeAbout(data);
	}, [&](const MTPDchannelAdminLogEventActionChangeUsername &data) {
		createChangeUsername(data);
	}, [&](const MTPDchannelAdminLogEventActionChangePhoto &data) {
		createChangePhoto(data);
	}, [&](const MTPDchannelAdminLogEventActionToggleInvites &data) {
		createToggleInvites(data);
	}, [&](const MTPDchannelAdminLogEventActionToggleSignatures &data) {
		createToggleSignatures(data);
	}, [&](const MTPDchannelAdminLogEventActionUpdatePinned &data) {
		createUpdatePinned(data);
	}, [&](const MTPDchannelAdminLogEventActionEditMessage &data) {
		createEditMessage(data);
	}, [&](const MTPDchannelAdminLogEventActionDeleteMessage &data) {
		createDeleteMessage(data);
	}, [&](const MTPDchannelAdminLogEventActionParticipantJoin &) {
		createParticipantJoin();
	}, [&](const MTPDchannelAdminLogEventActionParticipantLeave &) {
		createParticipantLeave();
	}, [&](const MTPDchannelAdminLogEventActionParticipantInvite &data) {
		createParticipantInvite(data);
	}, [&](const MTPDchannelAdminLogEventActionParticipantToggleBan &data) {
		createParticipantToggleBan(data);
	}, [&](const MTPDchannelAdminLogEventActionParticipantToggleAdmin &data) {
		createParticipantToggleAdmin(data);
	}, [&](const MTPDchannelAdminLogEventActionChangeStickerSet &data) {
		createChangeStickerSet(data);
	}, [&](const MTPDchannelAdminLogEventActionTogglePreHistoryHidden &data) {
		createTogglePreHistoryHidden(data);
	}, [&](const MTPDchannelAdminLogEventActionDefaultBannedRights &data) {
		createDefaultBannedRights(data);
	}, [&](const MTPDchannelAdminLogEventActionStopPoll &data) {
		createStopPoll(data);
	});
}

} // namespace AdminLog
