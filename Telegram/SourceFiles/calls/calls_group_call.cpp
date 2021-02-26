/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "calls/calls_group_call.h"

#include "calls/calls_group_common.h"
#include "main/main_session.h"
#include "api/api_send_progress.h"
#include "apiwrap.h"
#include "lang/lang_keys.h"
#include "lang/lang_hardcoded.h"
#include "boxes/peers/edit_participants_box.h" // SubscribeToMigration.
#include "ui/toasts/common_toasts.h"
#include "base/unixtime.h"
#include "core/application.h"
#include "core/core_settings.h"
#include "data/data_changes.h"
#include "data/data_user.h"
#include "data/data_chat.h"
#include "data/data_channel.h"
#include "data/data_group_call.h"
#include "data/data_session.h"
#include "base/global_shortcuts.h"
#include "base/openssl_help.h"
#include "webrtc/webrtc_media_devices.h"
#include "webrtc/webrtc_create_adm.h"

#include <tgcalls/group/GroupInstanceImpl.h>

#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QJsonArray>

namespace tgcalls {
class GroupInstanceImpl;
} // namespace tgcalls

namespace Calls {
namespace {

constexpr auto kMaxInvitePerSlice = 10;
constexpr auto kCheckLastSpokeInterval = crl::time(1000);
constexpr auto kCheckJoinedTimeout = 4 * crl::time(1000);
constexpr auto kUpdateSendActionEach = crl::time(500);
constexpr auto kPlayConnectingEach = crl::time(1056) + 2 * crl::time(1000);

[[nodiscard]] std::unique_ptr<Webrtc::MediaDevices> CreateMediaDevices() {
	const auto &settings = Core::App().settings();
	return Webrtc::CreateMediaDevices(
		settings.callInputDeviceId(),
		settings.callOutputDeviceId(),
		settings.callVideoInputDeviceId());
}

[[nodiscard]] const Data::GroupCall::Participant *LookupParticipant(
		not_null<PeerData*> chat,
		uint64 id,
		not_null<UserData*> user) {
	const auto call = chat->groupCall();
	if (!id || !call || call->id() != id) {
		return nullptr;
	}
	const auto &participants = call->participants();
	const auto i = ranges::find(
		participants,
		user,
		&Data::GroupCall::Participant::user);
	return (i != end(participants)) ? &*i : nullptr;
}

} // namespace

[[nodiscard]] bool IsGroupCallAdmin(
		not_null<PeerData*> peer,
		not_null<UserData*> user) {
	if (const auto chat = peer->asChat()) {
		return chat->admins.contains(user)
			|| (chat->creator == user->bareId());
	} else if (const auto group = peer->asMegagroup()) {
		if (const auto mgInfo = group->mgInfo.get()) {
			if (mgInfo->creator == user) {
				return true;
			}
			const auto i = mgInfo->lastAdmins.find(user);
			if (i == mgInfo->lastAdmins.end()) {
				return false;
			}
			const auto &rights = i->second.rights;
			return rights.c_chatAdminRights().is_manage_call();
		}
	}
	return false;
}

GroupCall::GroupCall(
	not_null<Delegate*> delegate,
	not_null<PeerData*> peer,
	const MTPInputGroupCall &inputCall)
: _delegate(delegate)
, _peer(peer)
, _history(peer->owner().history(peer))
, _api(&peer->session().mtp())
, _lastSpokeCheckTimer([=] { checkLastSpoke(); })
, _checkJoinedTimer([=] { checkJoined(); })
, _pushToTalkCancelTimer([=] { pushToTalkCancel(); })
, _connectingSoundTimer([=] { playConnectingSoundOnce(); })
, _mediaDevices(CreateMediaDevices()) {
	_muted.value(
	) | rpl::combine_previous(
	) | rpl::start_with_next([=](MuteState previous, MuteState state) {
		if (_instance) {
			updateInstanceMuteState();
		}
		if (_mySsrc) {
			maybeSendMutedUpdate(previous);
		}
	}, _lifetime);

	checkGlobalShortcutAvailability();

	const auto id = inputCall.c_inputGroupCall().vid().v;
	if (id) {
		if (const auto call = _peer->groupCall(); call && call->id() == id) {
			if (!_peer->canManageGroupCall() && call->joinMuted()) {
				_muted = MuteState::ForceMuted;
			}
		}
		_state = State::Joining;
		join(inputCall);
	} else {
		start();
	}

	_mediaDevices->audioInputId(
	) | rpl::start_with_next([=](QString id) {
		_audioInputId = id;
		if (_instance) {
			_instance->setAudioInputDevice(id.toStdString());
		}
	}, _lifetime);

	_mediaDevices->audioOutputId(
	) | rpl::start_with_next([=](QString id) {
		_audioOutputId = id;
		if (_instance) {
			_instance->setAudioOutputDevice(id.toStdString());
		}
	}, _lifetime);
}

GroupCall::~GroupCall() {
	destroyController();
}

void GroupCall::checkGlobalShortcutAvailability() {
	auto &settings = Core::App().settings();
	if (!settings.groupCallPushToTalk()) {
		return;
	} else if (!base::GlobalShortcutsAllowed()) {
		settings.setGroupCallPushToTalk(false);
		Core::App().saveSettingsDelayed();
	}
}

void GroupCall::setState(State state) {
	if (_state.current() == State::Failed) {
		return;
	} else if (_state.current() == State::FailedHangingUp
		&& state != State::Failed) {
		return;
	}
	if (_state.current() == state) {
		return;
	}
	_state = state;

	if (state == State::Joined) {
		stopConnectingSound();
		if (!_hadJoinedState) {
			_hadJoinedState = true;
			applyGlobalShortcutChanges();
			_delegate->groupCallPlaySound(Delegate::GroupCallSound::Started);
		}
		if (const auto call = _peer->groupCall(); call && call->id() == _id) {
			call->setInCall();
		}
	} else if (state == State::Connecting || state == State::Joining) {
		if (_hadJoinedState) {
			playConnectingSound();
		}
	} else {
		stopConnectingSound();
	}

	if (false
		|| state == State::Ended
		|| state == State::Failed) {
		// Destroy controller before destroying Call Panel,
		// so that the panel hide animation is smooth.
		destroyController();
	}
	switch (state) {
	case State::HangingUp:
	case State::FailedHangingUp:
		_delegate->groupCallPlaySound(Delegate::GroupCallSound::Ended);
		break;
	case State::Ended:
		_delegate->groupCallFinished(this);
		break;
	case State::Failed:
		_delegate->groupCallFailed(this);
		break;
	case State::Connecting:
		if (!_checkJoinedTimer.isActive()) {
			_checkJoinedTimer.callOnce(kCheckJoinedTimeout);
		}
		break;
	}
}

void GroupCall::playConnectingSound() {
	if (_connectingSoundTimer.isActive()) {
		return;
	}
	playConnectingSoundOnce();
	_connectingSoundTimer.callEach(kPlayConnectingEach);
}

void GroupCall::stopConnectingSound() {
	_connectingSoundTimer.cancel();
}

void GroupCall::playConnectingSoundOnce() {
	_delegate->groupCallPlaySound(Delegate::GroupCallSound::Connecting);
}

void GroupCall::start() {
	_createRequestId = _api.request(MTPphone_CreateGroupCall(
		_peer->input,
		MTP_int(openssl::RandomValue<int32>())
	)).done([=](const MTPUpdates &result) {
		_acceptFields = true;
		_peer->session().api().applyUpdates(result);
		_acceptFields = false;
	}).fail([=](const RPCError &error) {
		LOG(("Call Error: Could not create, error: %1"
			).arg(error.type()));
		hangup();
		if (error.type() == u"GROUPCALL_ANONYMOUS_FORBIDDEN"_q) {
			Ui::ShowMultilineToast({
				.text = { tr::lng_group_call_no_anonymous(tr::now) },
			});
		}
	}).send();
}

void GroupCall::join(const MTPInputGroupCall &inputCall) {
	setState(State::Joining);
	if (const auto chat = _peer->asChat()) {
		chat->setGroupCall(inputCall);
	} else if (const auto group = _peer->asMegagroup()) {
		group->setGroupCall(inputCall);
	} else {
		Unexpected("Peer type in GroupCall::join.");
	}

	inputCall.match([&](const MTPDinputGroupCall &data) {
		_id = data.vid().v;
		_accessHash = data.vaccess_hash().v;
		rejoin();
	});

	using Update = Data::GroupCall::ParticipantUpdate;
	_peer->groupCall()->participantUpdated(
	) | rpl::filter([=](const Update &update) {
		return (_instance != nullptr);
	}) | rpl::start_with_next([=](const Update &update) {
		if (!update.now) {
			_instance->removeSsrcs({ update.was->ssrc });
		} else {
			const auto &now = *update.now;
			const auto &was = update.was;
			const auto volumeChanged = was
				? (was->volume != now.volume || was->mutedByMe != now.mutedByMe)
				: (now.volume != Group::kDefaultVolume || now.mutedByMe);
			if (volumeChanged) {
				_instance->setVolume(
					now.ssrc,
					(now.mutedByMe
						? 0.
						: (now.volume
							/ float64(Group::kDefaultVolume))));
			}
		}
	}, _lifetime);

	SubscribeToMigration(_peer, _lifetime, [=](not_null<ChannelData*> group) {
		_peer = group;
	});
}

void GroupCall::rejoin() {
	if (state() != State::Joining
		&& state() != State::Joined
		&& state() != State::Connecting) {
		return;
	}

	_mySsrc = 0;
	setState(State::Joining);
	createAndStartController();
	applySelfInCallLocally();
	LOG(("Call Info: Requesting join payload."));

	const auto weak = base::make_weak(this);
	_instance->emitJoinPayload([=](tgcalls::GroupJoinPayload payload) {
		crl::on_main(weak, [=, payload = std::move(payload)]{
			auto fingerprints = QJsonArray();
			for (const auto print : payload.fingerprints) {
				auto object = QJsonObject();
				object.insert("hash", QString::fromStdString(print.hash));
				object.insert("setup", QString::fromStdString(print.setup));
				object.insert(
					"fingerprint",
					QString::fromStdString(print.fingerprint));
				fingerprints.push_back(object);
			}

			auto root = QJsonObject();
			const auto ssrc = payload.ssrc;
			root.insert("ufrag", QString::fromStdString(payload.ufrag));
			root.insert("pwd", QString::fromStdString(payload.pwd));
			root.insert("fingerprints", fingerprints);
			root.insert("ssrc", double(payload.ssrc));

			LOG(("Call Info: Join payload received, joining with ssrc: %1."
				).arg(ssrc));

			const auto json = QJsonDocument(root).toJson(
				QJsonDocument::Compact);
			const auto wasMuteState = muted();
			_api.request(MTPphone_JoinGroupCall(
				MTP_flags((wasMuteState != MuteState::Active)
					? MTPphone_JoinGroupCall::Flag::f_muted
					: MTPphone_JoinGroupCall::Flag(0)),
				inputCall(),
				MTP_dataJSON(MTP_bytes(json))
			)).done([=](const MTPUpdates &updates) {
				_mySsrc = ssrc;
				setState(_instanceConnected
					? State::Joined
					: State::Connecting);
				applySelfInCallLocally();
				maybeSendMutedUpdate(wasMuteState);
				_peer->session().api().applyUpdates(updates);
			}).fail([=](const RPCError &error) {
				const auto type = error.type();
				LOG(("Call Error: Could not join, error: %1").arg(type));

				if (type == u"GROUPCALL_SSRC_DUPLICATE_MUCH") {
					rejoin();
					return;
				}

				hangup();
				Ui::ShowMultilineToast({
					.text = { type == u"GROUPCALL_ANONYMOUS_FORBIDDEN"_q
						? tr::lng_group_call_no_anonymous(tr::now)
						: type == u"GROUPCALL_PARTICIPANTS_TOO_MUCH"_q
						? tr::lng_group_call_too_many(tr::now)
						: type == u"GROUPCALL_FORBIDDEN"_q
						? tr::lng_group_not_accessible(tr::now)
						: Lang::Hard::ServerError() },
				});
			}).send();
		});
	});
}

void GroupCall::applySelfInCallLocally() {
	const auto call = _peer->groupCall();
	if (!call || call->id() != _id) {
		return;
	}
	using Flag = MTPDgroupCallParticipant::Flag;
	const auto &participants = call->participants();
	const auto self = _peer->session().user();
	const auto i = ranges::find(
		participants,
		self,
		&Data::GroupCall::Participant::user);
	const auto date = (i != end(participants))
		? i->date
		: base::unixtime::now();
	const auto lastActive = (i != end(participants))
		? i->lastActive
		: TimeId(0);
	const auto volume = (i != end(participants))
		? i->volume
		: Group::kDefaultVolume;
	const auto canSelfUnmute = (muted() != MuteState::ForceMuted);
	const auto flags = (canSelfUnmute ? Flag::f_can_self_unmute : Flag(0))
		| (lastActive ? Flag::f_active_date : Flag(0))
		| (_mySsrc ? Flag(0) : Flag::f_left)
		| Flag::f_volume // Without flag the volume is reset to 100%.
		| Flag::f_volume_by_admin // Self volume can only be set by admin.
		| ((muted() != MuteState::Active) ? Flag::f_muted : Flag(0));
	call->applyUpdateChecked(
		MTP_updateGroupCallParticipants(
			inputCall(),
			MTP_vector<MTPGroupCallParticipant>(
				1,
				MTP_groupCallParticipant(
					MTP_flags(flags),
					MTP_int(self->bareId()),
					MTP_int(date),
					MTP_int(lastActive),
					MTP_int(_mySsrc),
					MTP_int(volume))),
			MTP_int(0)).c_updateGroupCallParticipants());
}

void GroupCall::applyParticipantLocally(
		not_null<UserData*> user,
		bool mute,
		std::optional<int> volume) {
	const auto participant = LookupParticipant(_peer, _id, user);
	if (!participant || !participant->ssrc) {
		return;
	}
	const auto canManageCall = _peer->canManageGroupCall();
	const auto isMuted = participant->muted || (mute && canManageCall);
	const auto canSelfUnmute = !canManageCall
		? participant->canSelfUnmute
		: (!mute || IsGroupCallAdmin(_peer, user));
	const auto isMutedByYou = mute && !canManageCall;
	const auto mutedCount = 0/*participant->mutedCount*/;
	using Flag = MTPDgroupCallParticipant::Flag;
	const auto flags = (canSelfUnmute ? Flag::f_can_self_unmute : Flag(0))
		| Flag::f_volume // Without flag the volume is reset to 100%.
		| ((participant->applyVolumeFromMin && !volume)
			? Flag::f_volume_by_admin
			: Flag(0))
		| (participant->lastActive ? Flag::f_active_date : Flag(0))
		| (isMuted ? Flag::f_muted : Flag(0))
		| (isMutedByYou ? Flag::f_muted_by_you : Flag(0));
	_peer->groupCall()->applyUpdateChecked(
		MTP_updateGroupCallParticipants(
			inputCall(),
			MTP_vector<MTPGroupCallParticipant>(
				1,
				MTP_groupCallParticipant(
					MTP_flags(flags),
					MTP_int(user->bareId()),
					MTP_int(participant->date),
					MTP_int(participant->lastActive),
					MTP_int(participant->ssrc),
					MTP_int(volume.value_or(participant->volume)))),
			MTP_int(0)).c_updateGroupCallParticipants());
}

void GroupCall::hangup() {
	finish(FinishType::Ended);
}

void GroupCall::discard() {
	if (!_id) {
		_api.request(_createRequestId).cancel();
		hangup();
		return;
	}
	_api.request(MTPphone_DiscardGroupCall(
		inputCall()
	)).done([=](const MTPUpdates &result) {
		// Here 'this' could be destroyed by updates, so we set Ended after
		// updates being handled, but in a guarded way.
		crl::on_main(this, [=] { hangup(); });
		_peer->session().api().applyUpdates(result);
	}).fail([=](const RPCError &error) {
		hangup();
	}).send();
}

void GroupCall::finish(FinishType type) {
	Expects(type != FinishType::None);

	const auto finalState = (type == FinishType::Ended)
		? State::Ended
		: State::Failed;
	const auto hangupState = (type == FinishType::Ended)
		? State::HangingUp
		: State::FailedHangingUp;
	const auto state = _state.current();
	if (state == State::HangingUp
		|| state == State::FailedHangingUp
		|| state == State::Ended
		|| state == State::Failed) {
		return;
	}
	if (!_mySsrc) {
		setState(finalState);
		return;
	}

	setState(hangupState);

	// We want to leave request still being sent and processed even if
	// the call is already destroyed.
	const auto session = &_peer->session();
	const auto weak = base::make_weak(this);
	session->api().request(MTPphone_LeaveGroupCall(
		inputCall(),
		MTP_int(_mySsrc)
	)).done([=](const MTPUpdates &result) {
		// Here 'this' could be destroyed by updates, so we set Ended after
		// updates being handled, but in a guarded way.
		crl::on_main(weak, [=] { setState(finalState); });
		session->api().applyUpdates(result);
	}).fail(crl::guard(weak, [=](const RPCError &error) {
		setState(finalState);
	})).send();
}

void GroupCall::setMuted(MuteState mute) {
	const auto set = [=] {
		const auto wasMuted = (muted() == MuteState::Muted)
			|| (muted() == MuteState::PushToTalk);
		_muted = mute;
		const auto nowMuted = (muted() == MuteState::Muted)
			|| (muted() == MuteState::PushToTalk);
		if (wasMuted != nowMuted) {
			applySelfInCallLocally();
		}
	};
	if (mute == MuteState::Active || mute == MuteState::PushToTalk) {
		_delegate->groupCallRequestPermissionsOrFail(crl::guard(this, set));
	} else {
		set();
	}
}

void GroupCall::handleUpdate(const MTPGroupCall &call) {
	return call.match([&](const MTPDgroupCall &data) {
		if (_acceptFields) {
			if (!_instance && !_id) {
				join(MTP_inputGroupCall(data.vid(), data.vaccess_hash()));
			}
			return;
		} else if (_id != data.vid().v
			|| _accessHash != data.vaccess_hash().v
			|| !_instance) {
			return;
		}
		if (const auto params = data.vparams()) {
			params->match([&](const MTPDdataJSON &data) {
				auto error = QJsonParseError{ 0, QJsonParseError::NoError };
				const auto document = QJsonDocument::fromJson(
					data.vdata().v,
					&error);
				if (error.error != QJsonParseError::NoError) {
					LOG(("API Error: "
						"Failed to parse group call params, error: %1."
						).arg(error.errorString()));
					return;
				} else if (!document.isObject()) {
					LOG(("API Error: "
						"Not an object received in group call params."));
					return;
				}
				const auto readString = [](
						const QJsonObject &object,
						const char *key) {
					return object.value(key).toString().toStdString();
				};
				const auto root = document.object().value("transport").toObject();
				auto payload = tgcalls::GroupJoinResponsePayload();
				payload.ufrag = readString(root, "ufrag");
				payload.pwd = readString(root, "pwd");
				const auto prints = root.value("fingerprints").toArray();
				const auto candidates = root.value("candidates").toArray();
				for (const auto &print : prints) {
					const auto object = print.toObject();
					payload.fingerprints.push_back(tgcalls::GroupJoinPayloadFingerprint{
						.hash = readString(object, "hash"),
						.setup = readString(object, "setup"),
						.fingerprint = readString(object, "fingerprint"),
					});
				}
				for (const auto &candidate : candidates) {
					const auto object = candidate.toObject();
					payload.candidates.push_back(tgcalls::GroupJoinResponseCandidate{
						.port = readString(object, "port"),
						.protocol = readString(object, "protocol"),
						.network = readString(object, "network"),
						.generation = readString(object, "generation"),
						.id = readString(object, "id"),
						.component = readString(object, "component"),
						.foundation = readString(object, "foundation"),
						.priority = readString(object, "priority"),
						.ip = readString(object, "ip"),
						.type = readString(object, "type"),
						.tcpType = readString(object, "tcpType"),
						.relAddr = readString(object, "relAddr"),
						.relPort = readString(object, "relPort"),
					});
				}
				_instance->setJoinResponsePayload(payload);
			});
		}
	}, [&](const MTPDgroupCallDiscarded &data) {
		if (data.vid().v == _id) {
			_mySsrc = 0;
			hangup();
		}
	});
}

void GroupCall::handleUpdate(const MTPDupdateGroupCallParticipants &data) {
	const auto state = _state.current();
	if (state != State::Joined && state != State::Connecting) {
		return;
	}

	const auto handleOtherParticipants = [=](
			const MTPDgroupCallParticipant &data) {
		if (data.is_min()) {
			// No real information about mutedByMe or my custom volume.
			return;
		}
		const auto user = _peer->owner().user(data.vuser_id().v);
		const auto participant = LookupParticipant(_peer, _id, user);
		if (!participant) {
			return;
		}
		_otherParticipantStateValue.fire(Group::ParticipantState{
			.user = user,
			.volume = data.vvolume().value_or_empty(),
			.mutedByMe = data.is_muted_by_you(),
		});
	};

	const auto self = _peer->session().userId();
	for (const auto &participant : data.vparticipants().v) {
		participant.match([&](const MTPDgroupCallParticipant &data) {
			if (data.vuser_id().v != self) {
				handleOtherParticipants(data);
				return;
			}
			if (data.is_left() && data.vsource().v == _mySsrc) {
				// I was removed from the call, rejoin.
				LOG(("Call Info: Rejoin after got 'left' with my ssrc."));
				setState(State::Joining);
				rejoin();
			} else if (!data.is_left() && data.vsource().v != _mySsrc) {
				// I joined from another device, hangup.
				LOG(("Call Info: Hangup after '!left' with ssrc %1, my %2."
					).arg(data.vsource().v
					).arg(_mySsrc));
				_mySsrc = 0;
				hangup();
			}
			if (data.is_muted() && !data.is_can_self_unmute()) {
				setMuted(MuteState::ForceMuted);
			} else if (muted() == MuteState::ForceMuted) {
				setMuted(MuteState::Muted);
			} else if (data.is_muted() && muted() != MuteState::Muted) {
				setMuted(MuteState::Muted);
			}
		});
	}
}

void GroupCall::createAndStartController() {
	const auto &settings = Core::App().settings();

	const auto weak = base::make_weak(this);
	const auto myLevel = std::make_shared<tgcalls::GroupLevelValue>();
	tgcalls::GroupInstanceDescriptor descriptor = {
		.config = tgcalls::GroupConfig{
		},
		.networkStateUpdated = [=](bool connected) {
			crl::on_main(weak, [=] { setInstanceConnected(connected); });
		},
		.audioLevelsUpdated = [=](const tgcalls::GroupLevelsUpdate &data) {
			const auto &updates = data.updates;
			if (updates.empty()) {
				return;
			} else if (updates.size() == 1 && !updates.front().ssrc) {
				const auto &value = updates.front().value;
				// Don't send many 0 while we're muted.
				if (myLevel->level == value.level
					&& myLevel->voice == value.voice) {
					return;
				}
				*myLevel = updates.front().value;
			}
			crl::on_main(weak, [=] { audioLevelsUpdated(data); });
		},
		.initialInputDeviceId = _audioInputId.toStdString(),
		.initialOutputDeviceId = _audioOutputId.toStdString(),
		.createAudioDeviceModule = Webrtc::AudioDeviceModuleCreator(
			settings.callAudioBackend()),
	};
	if (Logs::DebugEnabled()) {
		auto callLogFolder = cWorkingDir() + qsl("DebugLogs");
		auto callLogPath = callLogFolder + qsl("/last_group_call_log.txt");
		auto callLogNative = QDir::toNativeSeparators(callLogPath);
#ifdef Q_OS_WIN
		descriptor.config.logPath.data = callLogNative.toStdWString();
#else // Q_OS_WIN
		const auto callLogUtf = QFile::encodeName(callLogNative);
		descriptor.config.logPath.data.resize(callLogUtf.size());
		ranges::copy(callLogUtf, descriptor.config.logPath.data.begin());
#endif // Q_OS_WIN
		QFile(callLogPath).remove();
		QDir().mkpath(callLogFolder);
	}

	LOG(("Call Info: Creating group instance"));
	_instance = std::make_unique<tgcalls::GroupInstanceImpl>(
		std::move(descriptor));

	updateInstanceMuteState();
	updateInstanceVolumes();

	//raw->setAudioOutputDuckingEnabled(settings.callAudioDuckingEnabled());
}

void GroupCall::updateInstanceMuteState() {
	Expects(_instance != nullptr);

	const auto state = muted();
	_instance->setIsMuted(state != MuteState::Active
		&& state != MuteState::PushToTalk);
}

void GroupCall::updateInstanceVolumes() {
	const auto real = _peer->groupCall();
	if (!real || real->id() != _id) {
		return;
	}

	const auto &participants = real->participants();
	for (const auto &participant : participants) {
		const auto setVolume = participant.mutedByMe
			|| (participant.volume != Group::kDefaultVolume);
		if (setVolume && participant.ssrc) {
			_instance->setVolume(
				participant.ssrc,
				(participant.mutedByMe
					? 0.
					: (participant.volume / float64(Group::kDefaultVolume))));
		}
	}
}

void GroupCall::audioLevelsUpdated(const tgcalls::GroupLevelsUpdate &data) {
	Expects(!data.updates.empty());

	auto check = false;
	auto checkNow = false;
	const auto now = crl::now();
	for (const auto &[ssrcOrZero, value] : data.updates) {
		const auto ssrc = ssrcOrZero ? ssrcOrZero : _mySsrc;
		const auto level = value.level;
		const auto voice = value.voice;
		const auto self = (ssrc == _mySsrc);
		_levelUpdates.fire(LevelUpdate{
			.ssrc = ssrc,
			.value = level,
			.voice = voice,
			.self = self
		});
		if (level <= kSpeakLevelThreshold) {
			continue;
		}
		if (self
			&& voice
			&& (!_lastSendProgressUpdate
				|| _lastSendProgressUpdate + kUpdateSendActionEach < now)) {
			_lastSendProgressUpdate = now;
			_peer->session().sendProgressManager().update(
				_history,
				Api::SendProgressType::Speaking);
		}

		check = true;
		const auto i = _lastSpoke.find(ssrc);
		if (i == _lastSpoke.end()) {
			_lastSpoke.emplace(ssrc, Data::LastSpokeTimes{
				.anything = now,
				.voice = voice ? now : 0,
			});
			checkNow = true;
		} else {
			if ((i->second.anything + kCheckLastSpokeInterval / 3 <= now)
				|| (voice
					&& i->second.voice + kCheckLastSpokeInterval / 3 <= now)) {
				checkNow = true;
			}
			i->second.anything = now;
			if (voice) {
				i->second.voice = now;
			}
		}
	}
	if (checkNow) {
		checkLastSpoke();
	} else if (check && !_lastSpokeCheckTimer.isActive()) {
		_lastSpokeCheckTimer.callEach(kCheckLastSpokeInterval / 2);
	}
}

void GroupCall::checkLastSpoke() {
	const auto real = _peer->groupCall();
	if (!real || real->id() != _id) {
		return;
	}

	auto hasRecent = false;
	const auto now = crl::now();
	auto list = base::take(_lastSpoke);
	for (auto i = list.begin(); i != list.end();) {
		const auto [ssrc, when] = *i;
		if (when.anything + kCheckLastSpokeInterval >= now) {
			hasRecent = true;
			++i;
		} else {
			i = list.erase(i);
		}
		real->applyLastSpoke(ssrc, when, now);
	}
	_lastSpoke = std::move(list);

	if (!hasRecent) {
		_lastSpokeCheckTimer.cancel();
	} else if (!_lastSpokeCheckTimer.isActive()) {
		_lastSpokeCheckTimer.callEach(kCheckLastSpokeInterval / 3);
	}
}

void GroupCall::checkJoined() {
	if (state() != State::Connecting || !_id || !_mySsrc) {
		return;
	}
	_api.request(MTPphone_CheckGroupCall(
		inputCall(),
		MTP_int(_mySsrc)
	)).done([=](const MTPBool &result) {
		if (!mtpIsTrue(result)) {
			LOG(("Call Info: Rejoin after FALSE in checkGroupCall."));
			rejoin();
		} else if (state() == State::Connecting) {
			_checkJoinedTimer.callOnce(kCheckJoinedTimeout);
		}
	}).fail([=](const RPCError &error) {
		LOG(("Call Info: Rejoin after error '%1' in checkGroupCall."
			).arg(error.type()));
		rejoin();
	}).send();
}

void GroupCall::setInstanceConnected(bool connected) {
	if (_instanceConnected == connected) {
		return;
	}
	_instanceConnected = connected;
	if (state() == State::Connecting && connected) {
		setState(State::Joined);
	} else if (state() == State::Joined && !connected) {
		setState(State::Connecting);
	}
}

void GroupCall::maybeSendMutedUpdate(MuteState previous) {
	// Send only Active <-> !Active changes.
	const auto now = muted();
	const auto wasActive = (previous == MuteState::Active);
	const auto nowActive = (now == MuteState::Active);
	if (now == MuteState::ForceMuted
		|| previous == MuteState::ForceMuted
		|| (nowActive == wasActive)) {
		return;
	}
	sendMutedUpdate();
}

void GroupCall::sendMutedUpdate() {
	_api.request(_updateMuteRequestId).cancel();
	_updateMuteRequestId = _api.request(MTPphone_EditGroupCallMember(
		MTP_flags((muted() != MuteState::Active)
			? MTPphone_EditGroupCallMember::Flag::f_muted
			: MTPphone_EditGroupCallMember::Flag(0)),
		inputCall(),
		MTP_inputUserSelf(),
		MTP_int(100000) // volume
	)).done([=](const MTPUpdates &result) {
		_updateMuteRequestId = 0;
		_peer->session().api().applyUpdates(result);
	}).fail([=](const RPCError &error) {
		_updateMuteRequestId = 0;
		if (error.type() == u"GROUPCALL_FORBIDDEN"_q) {
			LOG(("Call Info: Rejoin after error '%1' in editGroupCallMember."
				).arg(error.type()));
			rejoin();
		}
	}).send();
}

rpl::producer<bool> GroupCall::connectingValue() const {
	using namespace rpl::mappers;
	return _state.value() | rpl::map(
		_1 == State::Creating
		|| _1 == State::Joining
		|| _1 == State::Connecting
	) | rpl::distinct_until_changed();
}

void GroupCall::setCurrentAudioDevice(bool input, const QString &deviceId) {
	if (input) {
		_mediaDevices->switchToAudioInput(deviceId);
	} else {
		_mediaDevices->switchToAudioOutput(deviceId);
	}
}

void GroupCall::toggleMute(const Group::MuteRequest &data) {
	if (data.locallyOnly) {
		applyParticipantLocally(data.user, data.mute, std::nullopt);
	} else {
		editParticipant(data.user, data.mute, std::nullopt);
	}
}

void GroupCall::changeVolume(const Group::VolumeRequest &data) {
	if (data.locallyOnly) {
		applyParticipantLocally(data.user, false, data.volume);
	} else {
		editParticipant(data.user, false, data.volume);
	}
}

void GroupCall::editParticipant(
		not_null<UserData*> user,
		bool mute,
		std::optional<int> volume) {
	const auto participant = LookupParticipant(_peer, _id, user);
	if (!participant) {
		return;
	}
	applyParticipantLocally(user, mute, volume);

	using Flag = MTPphone_EditGroupCallMember::Flag;
	const auto flags = (mute ? Flag::f_muted : Flag(0))
		| (volume.has_value() ? Flag::f_volume : Flag(0));
	_api.request(MTPphone_EditGroupCallMember(
		MTP_flags(flags),
		inputCall(),
		user->inputUser,
		MTP_int(std::clamp(volume.value_or(0), 1, Group::kMaxVolume))
	)).done([=](const MTPUpdates &result) {
		_peer->session().api().applyUpdates(result);
	}).fail([=](const RPCError &error) {
		if (error.type() == u"GROUPCALL_FORBIDDEN"_q) {
			LOG(("Call Info: Rejoin after error '%1' in editGroupCallMember."
				).arg(error.type()));
			rejoin();
		}
	}).send();
}

std::variant<int, not_null<UserData*>> GroupCall::inviteUsers(
		const std::vector<not_null<UserData*>> &users) {
	const auto real = _peer->groupCall();
	if (!real || real->id() != _id) {
		return 0;
	}
	const auto owner = &_peer->owner();
	const auto &invited = owner->invitedToCallUsers(_id);
	const auto &participants = real->participants();
	auto &&toInvite = users | ranges::view::filter([&](
			not_null<UserData*> user) {
		return !invited.contains(user) && !ranges::contains(
			participants,
			user,
			&Data::GroupCall::Participant::user);
	});

	auto count = 0;
	auto slice = QVector<MTPInputUser>();
	auto result = std::variant<int, not_null<UserData*>>(0);
	slice.reserve(kMaxInvitePerSlice);
	const auto sendSlice = [&] {
		count += slice.size();
		_api.request(MTPphone_InviteToGroupCall(
			inputCall(),
			MTP_vector<MTPInputUser>(slice)
		)).done([=](const MTPUpdates &result) {
			_peer->session().api().applyUpdates(result);
		}).send();
		slice.clear();
	};
	for (const auto user : users) {
		if (!count && slice.empty()) {
			result = user;
		}
		owner->registerInvitedToCallUser(_id, _peer, user);
		slice.push_back(user->inputUser);
		if (slice.size() == kMaxInvitePerSlice) {
			sendSlice();
		}
	}
	if (count != 0 || slice.size() != 1) {
		result = int(count + slice.size());
	}
	if (!slice.empty()) {
		sendSlice();
	}
	return result;
}

auto GroupCall::ensureGlobalShortcutManager()
-> std::shared_ptr<GlobalShortcutManager> {
	if (!_shortcutManager) {
		_shortcutManager = base::CreateGlobalShortcutManager();
	}
	return _shortcutManager;
}

void GroupCall::applyGlobalShortcutChanges() {
	auto &settings = Core::App().settings();
	if (!settings.groupCallPushToTalk()
		|| settings.groupCallPushToTalkShortcut().isEmpty()
		|| !base::GlobalShortcutsAvailable()
		|| !base::GlobalShortcutsAllowed()) {
		_shortcutManager = nullptr;
		_pushToTalk = nullptr;
		return;
	}
	ensureGlobalShortcutManager();
	const auto shortcut = _shortcutManager->shortcutFromSerialized(
		settings.groupCallPushToTalkShortcut());
	if (!shortcut) {
		settings.setGroupCallPushToTalkShortcut(QByteArray());
		settings.setGroupCallPushToTalk(false);
		Core::App().saveSettingsDelayed();
		_shortcutManager = nullptr;
		_pushToTalk = nullptr;
		return;
	}
	if (_pushToTalk) {
		if (shortcut->serialize() == _pushToTalk->serialize()) {
			return;
		}
		_shortcutManager->stopWatching(_pushToTalk);
	}
	_pushToTalk = shortcut;
	_shortcutManager->startWatching(_pushToTalk, [=](bool pressed) {
		pushToTalk(
			pressed,
			Core::App().settings().groupCallPushToTalkDelay());
	});
}

void GroupCall::pushToTalk(bool pressed, crl::time delay) {
	if (muted() == MuteState::ForceMuted
		|| muted() == MuteState::Active) {
		return;
	} else if (pressed) {
		_pushToTalkCancelTimer.cancel();
		setMuted(MuteState::PushToTalk);
	} else if (delay) {
		_pushToTalkCancelTimer.callOnce(delay);
	} else {
		pushToTalkCancel();
	}
}

void GroupCall::pushToTalkCancel() {
	_pushToTalkCancelTimer.cancel();
	if (muted() == MuteState::PushToTalk) {
		setMuted(MuteState::Muted);
	}
}

auto GroupCall::otherParticipantStateValue() const
-> rpl::producer<Group::ParticipantState> {
	return _otherParticipantStateValue.events();
}

//void GroupCall::setAudioVolume(bool input, float level) {
//	if (_instance) {
//		if (input) {
//			_instance->setInputVolume(level);
//		} else {
//			_instance->setOutputVolume(level);
//		}
//	}
//}

void GroupCall::setAudioDuckingEnabled(bool enabled) {
	if (_instance) {
		//_instance->setAudioOutputDuckingEnabled(enabled);
	}
}

void GroupCall::handleRequestError(const RPCError &error) {
	//if (error.type() == qstr("USER_PRIVACY_RESTRICTED")) {
	//	Ui::show(Box<InformBox>(tr::lng_call_error_not_available(tr::now, lt_user, _user->name)));
	//} else if (error.type() == qstr("PARTICIPANT_VERSION_OUTDATED")) {
	//	Ui::show(Box<InformBox>(tr::lng_call_error_outdated(tr::now, lt_user, _user->name)));
	//} else if (error.type() == qstr("CALL_PROTOCOL_LAYER_INVALID")) {
	//	Ui::show(Box<InformBox>(Lang::Hard::CallErrorIncompatible().replace("{user}", _user->name)));
	//}
	//finish(FinishType::Failed);
}

void GroupCall::handleControllerError(const QString &error) {
	if (error == u"ERROR_INCOMPATIBLE"_q) {
		//Ui::show(Box<InformBox>(
		//	Lang::Hard::CallErrorIncompatible().replace(
		//		"{user}",
		//		_user->name)));
	} else if (error == u"ERROR_AUDIO_IO"_q) {
		//Ui::show(Box<InformBox>(tr::lng_call_error_audio_io(tr::now)));
	}
	//finish(FinishType::Failed);
}

MTPInputGroupCall GroupCall::inputCall() const {
	Expects(_id != 0);

	return MTP_inputGroupCall(
		MTP_long(_id),
		MTP_long(_accessHash));
}

void GroupCall::destroyController() {
	if (_instance) {
		//_instance->stop([](tgcalls::FinalState) {
		//});

		DEBUG_LOG(("Call Info: Destroying call controller.."));
		_instance.reset();
		DEBUG_LOG(("Call Info: Call controller destroyed."));
	}
}

} // namespace Calls
