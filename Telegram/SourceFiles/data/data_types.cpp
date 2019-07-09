/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "data/data_types.h"

#include "data/data_document.h"
#include "data/data_file_origin.h"
#include "data/data_session.h"
#include "ui/image/image_source.h"
#include "ui/widgets/input_fields.h"
#include "storage/cache/storage_cache_types.h"
#include "base/openssl_help.h"
#include "auth_session.h"

namespace Data {
namespace {

constexpr auto kDocumentCacheTag = 0x0000000000000100ULL;
constexpr auto kDocumentCacheMask = 0x00000000000000FFULL;
constexpr auto kDocumentThumbCacheTag = 0x0000000000000200ULL;
constexpr auto kDocumentThumbCacheMask = 0x00000000000000FFULL;
constexpr auto kStorageCacheTag = 0x0000010000000000ULL;
constexpr auto kStorageCacheMask = 0x000000FFFFFFFFFFULL;
constexpr auto kWebDocumentCacheTag = 0x0000020000000000ULL;
constexpr auto kWebDocumentCacheMask = 0x000000FFFFFFFFFFULL;
constexpr auto kUrlCacheTag = 0x0000030000000000ULL;
constexpr auto kUrlCacheMask = 0x000000FFFFFFFFFFULL;
constexpr auto kGeoPointCacheTag = 0x0000040000000000ULL;
constexpr auto kGeoPointCacheMask = 0x000000FFFFFFFFFFULL;

} // namespace

struct ReplyPreview::Data {
	Data(std::unique_ptr<Images::Source> &&source, bool good);

	Image image;
	bool good = false;
};

ReplyPreview::Data::Data(
	std::unique_ptr<Images::Source> &&source,
	bool good)
: image(std::move(source))
, good(good) {
}

Storage::Cache::Key DocumentCacheKey(int32 dcId, uint64 id) {
	return Storage::Cache::Key{
		Data::kDocumentCacheTag | (uint64(dcId) & Data::kDocumentCacheMask),
		id
	};
}

Storage::Cache::Key DocumentThumbCacheKey(int32 dcId, uint64 id) {
	const auto part = (uint64(dcId) & Data::kDocumentThumbCacheMask);
	return Storage::Cache::Key{
		Data::kDocumentThumbCacheTag | part,
		id
	};
}

Storage::Cache::Key WebDocumentCacheKey(const WebFileLocation &location) {
	const auto CacheDcId = cTestMode() ? 2 : 4;
	const auto dcId = uint64(CacheDcId) & 0xFFULL;
	const auto &url = location.url();
	const auto hash = openssl::Sha256(bytes::make_span(url));
	const auto bytes = bytes::make_span(hash);
	const auto bytes1 = bytes.subspan(0, sizeof(uint32));
	const auto bytes2 = bytes.subspan(sizeof(uint32), sizeof(uint64));
	const auto part1 = *reinterpret_cast<const uint32*>(bytes1.data());
	const auto part2 = *reinterpret_cast<const uint64*>(bytes2.data());
	return Storage::Cache::Key{
		Data::kWebDocumentCacheTag | (dcId << 32) | part1,
		part2
	};
}

Storage::Cache::Key UrlCacheKey(const QString &location) {
	const auto url = location.toUtf8();
	const auto hash = openssl::Sha256(bytes::make_span(url));
	const auto bytes = bytes::make_span(hash);
	const auto bytes1 = bytes.subspan(0, sizeof(uint32));
	const auto bytes2 = bytes.subspan(sizeof(uint32), sizeof(uint64));
	const auto bytes3 = bytes.subspan(
		sizeof(uint32) + sizeof(uint64),
		sizeof(uint16));
	const auto part1 = *reinterpret_cast<const uint32*>(bytes1.data());
	const auto part2 = *reinterpret_cast<const uint64*>(bytes2.data());
	const auto part3 = *reinterpret_cast<const uint16*>(bytes3.data());
	return Storage::Cache::Key{
		Data::kUrlCacheTag | (uint64(part3) << 32) | part1,
		part2
	};
}

Storage::Cache::Key GeoPointCacheKey(const GeoPointLocation &location) {
	const auto zoomscale = ((uint32(location.zoom) & 0x0FU) << 8)
		| (uint32(location.scale) & 0x0FU);
	const auto widthheight = ((uint32(location.width) & 0xFFFFU) << 16)
		| (uint32(location.height) & 0xFFFFU);
	return Storage::Cache::Key{
		Data::kGeoPointCacheTag | (uint64(zoomscale) << 32) | widthheight,
		(uint64(std::round(std::abs(location.lat + 360.) * 1000000)) << 32)
		| uint64(std::round(std::abs(location.lon + 360.) * 1000000))
	};
}

ReplyPreview::ReplyPreview() = default;

ReplyPreview::ReplyPreview(ReplyPreview &&other) = default;

ReplyPreview &ReplyPreview::operator=(ReplyPreview &&other) = default;

ReplyPreview::~ReplyPreview() = default;

void ReplyPreview::prepare(
		not_null<Image*> image,
		FileOrigin origin,
		Images::Options options) {
	int w = image->width(), h = image->height();
	if (w <= 0) w = 1;
	if (h <= 0) h = 1;
	auto thumbSize = (w > h)
		? QSize(
			w * st::msgReplyBarSize.height() / h,
			st::msgReplyBarSize.height())
		: QSize(
			st::msgReplyBarSize.height(),
			h * st::msgReplyBarSize.height() / w);
	thumbSize *= cIntRetinaFactor();
	const auto prepareOptions = Images::Option::Smooth
		| Images::Option::TransparentBackground
		| options;
	auto outerSize = st::msgReplyBarSize.height();
	auto bitmap = image->pixNoCache(
		origin,
		thumbSize.width(),
		thumbSize.height(),
		prepareOptions,
		outerSize,
		outerSize);
	_data = std::make_unique<ReplyPreview::Data>(
		std::make_unique<Images::ImageSource>(
			bitmap.toImage(),
			"PNG"),
		((options & Images::Option::Blurred) == 0));
}

void ReplyPreview::clear() {
	_data = nullptr;
}

Image *ReplyPreview::image() const {
	return _data ? &_data->image : nullptr;
}

bool ReplyPreview::good() const {
	return !empty() && _data->good;
}

bool ReplyPreview::empty() const {
	return !_data;
}

} // namespace Data

uint32 AudioMsgId::CreateExternalPlayId() {
	static auto Result = uint32(0);
	return ++Result ? Result : ++Result;
}

AudioMsgId AudioMsgId::ForVideo() {
	auto result = AudioMsgId();
	result._externalPlayId = CreateExternalPlayId();
	result._type = Type::Video;
	return result;
}

void AudioMsgId::setTypeFromAudio() {
	if (_audio->isVoiceMessage() || _audio->isVideoMessage()) {
		_type = Type::Voice;
	} else if (_audio->isVideoFile()) {
		_type = Type::Video;
	} else if (_audio->isAudioFile()) {
		_type = Type::Song;
	} else {
		_type = Type::Unknown;
	}
}

void MessageCursor::fillFrom(not_null<const Ui::InputField*> field) {
	const auto cursor = field->textCursor();
	position = cursor.position();
	anchor = cursor.anchor();
	const auto top = field->scrollTop().current();
	scroll = (top != field->scrollTopMax()) ? top : QFIXED_MAX;
}

void MessageCursor::applyTo(not_null<Ui::InputField*> field) {
	auto cursor = field->textCursor();
	cursor.setPosition(anchor, QTextCursor::MoveAnchor);
	cursor.setPosition(position, QTextCursor::KeepAnchor);
	field->setTextCursor(cursor);
	field->scrollTo(scroll);
}

HistoryItem *FileClickHandler::getActionItem() const {
	return Auth().data().message(context());
}

PeerId PeerFromMessage(const MTPmessage &message) {
	return message.match([](const MTPDmessageEmpty &) {
		return PeerId(0);
	}, [](const auto &message) {
		const auto fromId = message.vfrom_id();
		const auto toId = peerFromMTP(message.vto_id());
		const auto out = message.is_out();
		return (out || !fromId || !peerIsUser(toId))
			? toId
			: peerFromUser(*fromId);
	});
}

MTPDmessage::Flags FlagsFromMessage(const MTPmessage &message) {
	return message.match([](const MTPDmessageEmpty &) {
		return MTPDmessage::Flags(0);
	}, [](const MTPDmessage &message) {
		return message.vflags().v;
	}, [](const MTPDmessageService &message) {
		return mtpCastFlags(message.vflags().v);
	});
}

MsgId IdFromMessage(const MTPmessage &message) {
	return message.match([](const auto &message) {
		return message.vid().v;
	});
}

TimeId DateFromMessage(const MTPmessage &message) {
	return message.match([](const MTPDmessageEmpty &) {
		return TimeId(0);
	}, [](const auto &message) {
		return message.vdate().v;
	});
}
