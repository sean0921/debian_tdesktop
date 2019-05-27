/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "data/data_shared_media.h"

class AudioMsgId;

namespace Media {
namespace Audio {
class Instance;
} // namespace Audio
} // namespace Media

namespace Media {
namespace View {
class PlaybackProgress;
} // namespace View
} // namespace Media

namespace Media {
namespace Streaming {
class Player;
class Loader;
struct PlaybackOptions;
struct Update;
enum class Error;
} // namespace Streaming
} // namespace Media

namespace Media {
namespace Player {

void start(not_null<Audio::Instance*> instance);
void finish(not_null<Audio::Instance*> instance);

class Instance;
Instance *instance();

struct TrackState;

class Instance : private base::Subscriber {
public:
	void play(AudioMsgId::Type type);
	void pause(AudioMsgId::Type type);
	void stop(AudioMsgId::Type type);
	void playPause(AudioMsgId::Type type);
	bool next(AudioMsgId::Type type);
	bool previous(AudioMsgId::Type type);

	AudioMsgId::Type getActiveType() const;

	void play() {
		play(getActiveType());
	}
	void pause() {
		pause(getActiveType());
	}
	void stop() {
		stop(getActiveType());
	}
	void playPause() {
		playPause(getActiveType());
	}
	bool next() {
		return next(getActiveType());
	}
	bool previous() {
		return previous(getActiveType());
	}

	void playPauseCancelClicked(AudioMsgId::Type type);

	void play(const AudioMsgId &audioId);
	void playPause(const AudioMsgId &audioId);
	[[nodiscard]] TrackState getState(AudioMsgId::Type type) const;

	[[nodiscard]] Streaming::Player *roundVideoPlayer(
		HistoryItem *item) const;
	[[nodiscard]] View::PlaybackProgress *roundVideoPlayback(
		HistoryItem *item) const;

	[[nodiscard]] AudioMsgId current(AudioMsgId::Type type) const {
		if (const auto data = getData(type)) {
			return data->current;
		}
		return AudioMsgId();
	}

	[[nodiscard]] bool repeatEnabled(AudioMsgId::Type type) const {
		if (const auto data = getData(type)) {
			return data->repeatEnabled;
		}
		return false;
	}
	void toggleRepeat(AudioMsgId::Type type) {
		if (const auto data = getData(type)) {
			data->repeatEnabled = !data->repeatEnabled;
			_repeatChangedNotifier.notify(type);
		}
	}

	[[nodiscard]] bool isSeeking(AudioMsgId::Type type) const {
		if (const auto data = getData(type)) {
			return (data->seeking == data->current);
		}
		return false;
	}
	void startSeeking(AudioMsgId::Type type);
	void finishSeeking(AudioMsgId::Type type, float64 progress);
	void cancelSeeking(AudioMsgId::Type type);

	void updateVoicePlaybackSpeed();

	[[nodiscard]] bool nextAvailable(AudioMsgId::Type type) const;
	[[nodiscard]] bool previousAvailable(AudioMsgId::Type type) const;

	struct Switch {
		AudioMsgId from;
		FullMsgId to;
	};

	base::Observable<Switch> &switchToNextNotifier() {
		return _switchToNextNotifier;
	}
	base::Observable<bool> &playerWidgetOver() {
		return _playerWidgetOver;
	}
	base::Observable<TrackState> &updatedNotifier() {
		return _updatedNotifier;
	}
	base::Observable<AudioMsgId::Type> &tracksFinishedNotifier() {
		return _tracksFinishedNotifier;
	}
	base::Observable<AudioMsgId::Type> &trackChangedNotifier() {
		return _trackChangedNotifier;
	}
	base::Observable<AudioMsgId::Type> &repeatChangedNotifier() {
		return _repeatChangedNotifier;
	}

	rpl::producer<> playlistChanges(AudioMsgId::Type type) const;

	void documentLoadProgress(DocumentData *document);

	void handleLogout();

private:
	using SharedMediaType = Storage::SharedMediaType;
	using SliceKey = SparseIdsMergedSlice::Key;
	struct Streamed;
	struct Data {
		Data(AudioMsgId::Type type, SharedMediaType overview);
		Data(Data &&other);
		Data &operator=(Data &&other);
		~Data();

		AudioMsgId::Type type;
		Storage::SharedMediaType overview;
		AudioMsgId current;
		AudioMsgId seeking;
		std::optional<SparseIdsMergedSlice> playlistSlice;
		std::optional<SliceKey> playlistSliceKey;
		std::optional<SliceKey> playlistRequestedKey;
		std::optional<int> playlistIndex;
		rpl::lifetime playlistLifetime;
		rpl::event_stream<> playlistChanges;
		History *history = nullptr;
		History *migrated = nullptr;
		bool repeatEnabled = false;
		bool isPlaying = false;
		bool resumeOnCallEnd = false;
		std::unique_ptr<Streamed> streamed;
	};

	Instance();
	~Instance();

	friend void start(not_null<Audio::Instance*> instance);
	friend void finish(not_null<Audio::Instance*> instance);

	void setupShortcuts();
	void playStreamed(
		const AudioMsgId &audioId,
		std::unique_ptr<Streaming::Loader> loader);
	Streaming::PlaybackOptions streamingOptions(
		const AudioMsgId &audioId,
		crl::time position = 0);

	// Observed notifications.
	void handleSongUpdate(const AudioMsgId &audioId);

	void pauseOnCall(AudioMsgId::Type type);
	void resumeOnCall(AudioMsgId::Type type);

	void setCurrent(const AudioMsgId &audioId);
	void refreshPlaylist(not_null<Data*> data);
	std::optional<SliceKey> playlistKey(not_null<Data*> data) const;
	bool validPlaylist(not_null<Data*> data);
	void validatePlaylist(not_null<Data*> data);
	void playlistUpdated(not_null<Data*> data);
	bool moveInPlaylist(not_null<Data*> data, int delta, bool autonext);
	HistoryItem *itemByIndex(not_null<Data*> data, int index);

	void handleStreamingUpdate(
		not_null<Data*> data,
		Streaming::Update &&update);
	void handleStreamingError(
		not_null<Data*> data,
		Streaming::Error &&error);

	void clearStreamed(not_null<Data *> data);
	void emitUpdate(AudioMsgId::Type type);
	template <typename CheckCallback>
	void emitUpdate(AudioMsgId::Type type, CheckCallback check);

	Data *getData(AudioMsgId::Type type) {
		if (type == AudioMsgId::Type::Song) {
			return &_songData;
		} else if (type == AudioMsgId::Type::Voice) {
			return &_voiceData;
		}
		return nullptr;
	}

	const Data *getData(AudioMsgId::Type type) const {
		if (type == AudioMsgId::Type::Song) {
			return &_songData;
		} else if (type == AudioMsgId::Type::Voice) {
			return &_voiceData;
		}
		return nullptr;
	}

	HistoryItem *roundVideoItem() const;
	void requestRoundVideoResize() const;
	void requestRoundVideoRepaint() const;

	Data _songData;
	Data _voiceData;

	base::Observable<Switch> _switchToNextNotifier;
	base::Observable<bool> _playerWidgetOver;
	base::Observable<TrackState> _updatedNotifier;
	base::Observable<AudioMsgId::Type> _tracksFinishedNotifier;
	base::Observable<AudioMsgId::Type> _trackChangedNotifier;
	base::Observable<AudioMsgId::Type> _repeatChangedNotifier;

	rpl::lifetime _lifetime;

};

} // namespace Player
} // namespace Media
