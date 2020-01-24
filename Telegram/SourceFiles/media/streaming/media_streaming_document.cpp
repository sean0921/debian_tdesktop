/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "media/streaming/media_streaming_document.h"

#include "media/streaming/media_streaming_instance.h"
#include "data/data_session.h"
#include "data/data_document.h"
#include "data/data_file_origin.h"
#include "storage/file_download.h" // Storage::kMaxFileInMemory.
#include "styles/style_widgets.h"

#include <QtCore/QBuffer>

namespace Media {
namespace Streaming {
namespace {

constexpr auto kWaitingFastDuration = crl::time(200);
constexpr auto kWaitingShowDuration = crl::time(500);
constexpr auto kWaitingShowDelay = crl::time(500);
constexpr auto kGoodThumbnailQuality = 87;

} // namespace

Document::Document(
	not_null<DocumentData*> document,
	std::shared_ptr<Reader> reader)
: _player(&document->owner(), reader)
, _radial(
	[=] { waitingCallback(); },
	st::defaultInfiniteRadialAnimation)
, _document(document) {
	_player.updates(
	) | rpl::start_with_next_error([=](Update &&update) {
		handleUpdate(std::move(update));
	}, [=](Streaming::Error &&error) {
		handleError(std::move(error));
	}, _player.lifetime());

	_player.fullInCache(
	) | rpl::start_with_next([=](bool fullInCache) {
		_document->setLoadedInMediaCache(fullInCache);
	}, _player.lifetime());
}

Player &Document::player() {
	return _player;
}

const Player &Document::player() const {
	return _player;
}

const Information &Document::info() const {
	return _info;
}

void Document::play(const PlaybackOptions &options) {
	_player.play(options);
	_info.audio.state.position
		= _info.video.state.position
		= options.position;
	waitingChange(true);
}

void Document::saveFrameToCover() {
	auto request = Streaming::FrameRequest();
	//request.radius = (_doc && _doc->isVideoMessage())
	//	? ImageRoundRadius::Ellipse
	//	: ImageRoundRadius::None;
	_info.video.cover = _player.ready()
		? _player.frame(request)
		: _info.video.cover;
}

void Document::registerInstance(not_null<Instance*> instance) {
	_instances.emplace(instance);
}

void Document::unregisterInstance(not_null<Instance*> instance) {
	_instances.remove(instance);
	_player.unregisterInstance(instance);
	refreshPlayerPriority();
}

void Document::refreshPlayerPriority() {
	if (_instances.empty()) {
		return;
	}
	const auto max = ranges::max_element(
		_instances,
		ranges::less(),
		&Instance::priority);
	_player.setLoaderPriority((*max)->priority());
}

bool Document::waitingShown() const {
	if (!_fading.animating() && !_waiting) {
		_radial.stop(anim::type::instant);
		return false;
	}
	return _radial.animating();
}

float64 Document::waitingOpacity() const {
	return _fading.value(_waiting ? 1. : 0.);
}

Ui::RadialState Document::waitingState() const {
	return _radial.computeState();
}

void Document::handleUpdate(Update &&update) {
	update.data.match([&](Information &update) {
		ready(std::move(update));
	}, [&](const PreloadedVideo &update) {
		_info.video.state.receivedTill = update.till;
	}, [&](const UpdateVideo &update) {
		_info.video.state.position = update.position;
	}, [&](const PreloadedAudio &update) {
		_info.audio.state.receivedTill = update.till;
	}, [&](const UpdateAudio &update) {
		_info.audio.state.position = update.position;
	}, [&](const WaitingForData &update) {
		waitingChange(update.waiting);
	}, [&](MutedByOther) {
	}, [&](Finished) {
		const auto finishTrack = [](TrackState &state) {
			state.position = state.receivedTill = state.duration;
		};
		finishTrack(_info.audio.state);
		finishTrack(_info.video.state);
	});
}

void Document::handleError(Error &&error) {
	if (error == Error::NotStreamable) {
		_document->setNotSupportsStreaming();
	} else if (error == Error::OpenFailed) {
		_document->setInappPlaybackFailed();
	}
	waitingChange(false);
}

void Document::ready(Information &&info) {
	_info = std::move(info);
	validateGoodThumbnail();
	waitingChange(false);
}

void Document::waitingChange(bool waiting) {
	if (_waiting == waiting) {
		return;
	}
	_waiting = waiting;
	const auto fade = [=](crl::time duration) {
		if (!_radial.animating()) {
			_radial.start(
				st::defaultInfiniteRadialAnimation.sineDuration);
		}
		_fading.start(
			[=] { waitingCallback(); },
			_waiting ? 0. : 1.,
			_waiting ? 1. : 0.,
			duration);
	};
	if (waiting) {
		if (_radial.animating()) {
			_timer.cancel();
			fade(kWaitingFastDuration);
		} else {
			_timer.callOnce(kWaitingShowDelay);
			_timer.setCallback([=] {
				fade(kWaitingShowDuration);
			});
		}
	} else {
		_timer.cancel();
		if (_radial.animating()) {
			fade(kWaitingFastDuration);
		}
	}
}

void Document::validateGoodThumbnail() {
	const auto good = _document->goodThumbnail();
	if (_info.video.cover.isNull()
		|| (good && good->loaded())
		|| _document->uploading()) {
		return;
	}
	auto image = [&] {
		auto result = _info.video.cover;
		if (_info.video.rotation != 0) {
			auto transform = QTransform();
			transform.rotate(_info.video.rotation);
			result = result.transformed(transform);
		}
		if (result.size() != _info.video.size) {
			result = result.scaled(
				_info.video.size,
				Qt::IgnoreAspectRatio,
				Qt::SmoothTransformation);
		}
		return result;
	}();

	auto bytes = QByteArray();
	{
		auto buffer = QBuffer(&bytes);
		image.save(&buffer, "JPG", kGoodThumbnailQuality);
	}
	const auto length = bytes.size();
	if (!length || length > Storage::kMaxFileInMemory) {
		LOG(("App Error: Bad thumbnail data for saving to cache."));
	} else if (_document->uploading()) {
		_document->setGoodThumbnailOnUpload(
			std::move(image),
			std::move(bytes));
	} else {
		_document->owner().cache().putIfEmpty(
			_document->goodThumbnailCacheKey(),
			Storage::Cache::Database::TaggedValue(
				std::move(bytes),
				Data::kImageCacheTag));
		_document->refreshGoodThumbnail();
	}
}

void Document::waitingCallback() {
	for (const auto &instance : _instances) {
		instance->callWaitingCallback();
	}
}

} // namespace Streaming
} // namespace Media
