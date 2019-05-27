/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "data/data_user.h"

#include "observer_peer.h"
#include "storage/localstorage.h"
#include "data/data_session.h"
#include "ui/text_options.h"
#include "apiwrap.h"
#include "lang/lang_keys.h"

namespace {

// User with hidden last seen stays online in UI for such amount of seconds.
constexpr auto kSetOnlineAfterActivity = TimeId(30);

using UpdateFlag = Notify::PeerUpdate::Flag;

} // namespace

BotCommand::BotCommand(
	const QString &command,
	const QString &description)
: command(command)
, _description(description) {
}

bool BotCommand::setDescription(const QString &description) {
	if (_description != description) {
		_description = description;
		_descriptionText = Text();
		return true;
	}
	return false;
}

const Text &BotCommand::descriptionText() const {
	if (_descriptionText.isEmpty() && !_description.isEmpty()) {
		_descriptionText.setText(
			st::defaultTextStyle,
			_description,
			Ui::NameTextOptions());
	}
	return _descriptionText;
}

UserData::UserData(not_null<Data::Session*> owner, PeerId id)
: PeerData(owner, id) {
}

bool UserData::canShareThisContact() const {
	return canShareThisContactFast()
		|| !owner().findContactPhone(peerToUser(id)).isEmpty();
}

void UserData::setContactStatus(ContactStatus status) {
	if (_contactStatus != status) {
		const auto changed = (_contactStatus == ContactStatus::Contact)
			!= (status == ContactStatus::Contact);
		_contactStatus = status;
		if (changed) {
			Notify::peerUpdatedDelayed(
				this,
				Notify::PeerUpdate::Flag::UserIsContact);
		}
	}
	if (_contactStatus == ContactStatus::Contact
		&& cReportSpamStatuses().value(id, dbiprsHidden) != dbiprsHidden) {
		cRefReportSpamStatuses().insert(id, dbiprsHidden);
		Local::writeReportSpamStatuses();
	}
}

// see Local::readPeer as well
void UserData::setPhoto(const MTPUserProfilePhoto &photo) {
	if (photo.type() == mtpc_userProfilePhoto) {
		const auto &data = photo.c_userProfilePhoto();
		updateUserpic(data.vphoto_id.v, data.vdc_id.v, data.vphoto_small);
	} else {
		clearUserpic();
	}
}

QString UserData::unavailableReason() const {
	return _unavailableReason;
}

void UserData::setUnavailableReason(const QString &text) {
	if (_unavailableReason != text) {
		_unavailableReason = text;
		Notify::peerUpdatedDelayed(
			this,
			Notify::PeerUpdate::Flag::UnavailableReasonChanged);
	}
}

void UserData::setCommonChatsCount(int count) {
	if (_commonChatsCount != count) {
		_commonChatsCount = count;
		Notify::peerUpdatedDelayed(this, UpdateFlag::UserCommonChatsChanged);
	}
}

void UserData::setName(const QString &newFirstName, const QString &newLastName, const QString &newPhoneName, const QString &newUsername) {
	bool changeName = !newFirstName.isEmpty() || !newLastName.isEmpty();

	QString newFullName;
	if (changeName && newFirstName.trimmed().isEmpty()) {
		firstName = newLastName;
		lastName = QString();
		newFullName = firstName;
	} else {
		if (changeName) {
			firstName = newFirstName;
			lastName = newLastName;
		}
		newFullName = lastName.isEmpty() ? firstName : lng_full_name(lt_first_name, firstName, lt_last_name, lastName);
	}
	updateNameDelayed(newFullName, newPhoneName, newUsername);
}

void UserData::setPhone(const QString &newPhone) {
	if (_phone != newPhone) {
		_phone = newPhone;
	}
}

void UserData::setBotInfoVersion(int version) {
	if (version < 0) {
		if (botInfo) {
			if (!botInfo->commands.isEmpty()) {
				botInfo->commands.clear();
				Notify::botCommandsChanged(this);
			}
			botInfo = nullptr;
			Notify::userIsBotChanged(this);
		}
	} else if (!botInfo) {
		botInfo = std::make_unique<BotInfo>();
		botInfo->version = version;
		Notify::userIsBotChanged(this);
	} else if (botInfo->version < version) {
		if (!botInfo->commands.isEmpty()) {
			botInfo->commands.clear();
			Notify::botCommandsChanged(this);
		}
		botInfo->description.clear();
		botInfo->version = version;
		botInfo->inited = false;
	}
}

void UserData::setBotInfo(const MTPBotInfo &info) {
	switch (info.type()) {
	case mtpc_botInfo: {
		const auto &d(info.c_botInfo());
		if (peerFromUser(d.vuser_id.v) != id || !botInfo) return;

		QString desc = qs(d.vdescription);
		if (botInfo->description != desc) {
			botInfo->description = desc;
			botInfo->text = Text(st::msgMinWidth);
		}

		auto &v = d.vcommands.v;
		botInfo->commands.reserve(v.size());
		auto changedCommands = false;
		int32 j = 0;
		for (int32 i = 0, l = v.size(); i < l; ++i) {
			if (v.at(i).type() != mtpc_botCommand) continue;

			QString cmd = qs(v.at(i).c_botCommand().vcommand), desc = qs(v.at(i).c_botCommand().vdescription);
			if (botInfo->commands.size() <= j) {
				botInfo->commands.push_back(BotCommand(cmd, desc));
				changedCommands = true;
			} else {
				if (botInfo->commands[j].command != cmd) {
					botInfo->commands[j].command = cmd;
					changedCommands = true;
				}
				if (botInfo->commands[j].setDescription(desc)) {
					changedCommands = true;
				}
			}
			++j;
		}
		while (j < botInfo->commands.size()) {
			botInfo->commands.pop_back();
			changedCommands = true;
		}

		botInfo->inited = true;

		if (changedCommands) {
			Notify::botCommandsChanged(this);
		}
	} break;
	}
}

void UserData::setNameOrPhone(const QString &newNameOrPhone) {
	if (nameOrPhone != newNameOrPhone) {
		nameOrPhone = newNameOrPhone;
		phoneText.setText(
			st::msgNameStyle,
			nameOrPhone,
			Ui::NameTextOptions());
	}
}

void UserData::madeAction(TimeId when) {
	if (isBot() || isServiceUser() || when <= 0) {
		return;
	} else if (onlineTill <= 0 && -onlineTill < when) {
		onlineTill = -when - kSetOnlineAfterActivity;
		Notify::peerUpdatedDelayed(this, Notify::PeerUpdate::Flag::UserOnlineChanged);
	} else if (onlineTill > 0 && onlineTill < when + 1) {
		onlineTill = when + kSetOnlineAfterActivity;
		Notify::peerUpdatedDelayed(this, Notify::PeerUpdate::Flag::UserOnlineChanged);
	}
}

void UserData::setAccessHash(uint64 accessHash) {
	if (accessHash == kInaccessibleAccessHashOld) {
		_accessHash = 0;
//		_flags.add(MTPDuser_ClientFlag::f_inaccessible | 0);
		_flags.add(MTPDuser::Flag::f_deleted);
	} else {
		_accessHash = accessHash;
	}
}

void UserData::setBlockStatus(BlockStatus blockStatus) {
	if (blockStatus != _blockStatus) {
		_blockStatus = blockStatus;
		Notify::peerUpdatedDelayed(this, UpdateFlag::UserIsBlocked);
	}
}

void UserData::setCallsStatus(CallsStatus callsStatus) {
	if (callsStatus != _callsStatus) {
		_callsStatus = callsStatus;
		Notify::peerUpdatedDelayed(this, UpdateFlag::UserHasCalls);
	}
}

bool UserData::hasCalls() const {
	return (callsStatus() != CallsStatus::Disabled)
		&& (callsStatus() != CallsStatus::Unknown);
}

namespace Data {

void ApplyUserUpdate(not_null<UserData*> user, const MTPDuserFull &update) {
	user->owner().processUser(update.vuser);
	if (update.has_profile_photo()) {
		user->owner().processPhoto(update.vprofile_photo);
	}
	update.vlink.match([&](const MTPDcontacts_link & link) {
		App::feedUserLink(
			MTP_int(peerToUser(user->id)),
			link.vmy_link,
			link.vforeign_link);
	});
	user->session().api().applyNotifySettings(
		MTP_inputNotifyPeer(user->input),
		update.vnotify_settings);

	if (update.has_bot_info()) {
		user->setBotInfo(update.vbot_info);
	} else {
		user->setBotInfoVersion(-1);
	}
	if (update.has_pinned_msg_id()) {
		user->setPinnedMessageId(update.vpinned_msg_id.v);
	} else {
		user->clearPinnedMessage();
	}
	user->setFullFlags(update.vflags.v);
	user->setBlockStatus(update.is_blocked()
		? UserData::BlockStatus::Blocked
		: UserData::BlockStatus::NotBlocked);
	user->setCallsStatus(update.is_phone_calls_private()
		? UserData::CallsStatus::Private
		: update.is_phone_calls_available()
		? UserData::CallsStatus::Enabled
		: UserData::CallsStatus::Disabled);
	user->setAbout(update.has_about() ? qs(update.vabout) : QString());
	user->setCommonChatsCount(update.vcommon_chats_count.v);
	user->checkFolder(update.has_folder_id() ? update.vfolder_id.v : 0);
	user->fullUpdated();
}

} // namespace Data
