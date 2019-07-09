/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "data/data_types.h"

enum class ImageRoundRadius;
class MainWindow;
class MainWidget;
class HistoryItem;
class History;
namespace HistoryView {
class Element;
} // namespace HistoryView

using HistoryItemsMap = base::flat_set<not_null<HistoryItem*>>;
using GifItems = QHash<Media::Clip::Reader*, HistoryItem*>;

enum RoundCorners {
	SmallMaskCorners = 0x00, // for images
	LargeMaskCorners,

	BoxCorners,
	MenuCorners,
	BotKbOverCorners,
	StickerCorners,
	StickerSelectedCorners,
	SelectedOverlaySmallCorners,
	SelectedOverlayLargeCorners,
	DateCorners,
	DateSelectedCorners,
	OverviewVideoCorners,
	OverviewVideoSelectedCorners,
	ForwardCorners,
	MediaviewSaveCorners,
	EmojiHoverCorners,
	StickerHoverCorners,
	BotKeyboardCorners,
	PhotoSelectOverlayCorners,

	Doc1Corners,
	Doc2Corners,
	Doc3Corners,
	Doc4Corners,

	InShadowCorners, // for photos without bg
	InSelectedShadowCorners,

	MessageInCorners, // with shadow
	MessageInSelectedCorners,
	MessageOutCorners,
	MessageOutSelectedCorners,

	RoundCornersCount
};

namespace App {
	MainWindow *wnd();
	MainWidget *main();

	QString formatPhone(QString phone);

	void addSavedGif(DocumentData *doc);
	void checkSavedGif(HistoryItem *item);
	[[nodiscard]] QString peerName(const PeerData *peer, bool forDialogs = false);

	void hoveredItem(HistoryView::Element *item);
	HistoryView::Element *hoveredItem();
	void pressedItem(HistoryView::Element *item);
	HistoryView::Element *pressedItem();
	void hoveredLinkItem(HistoryView::Element *item);
	HistoryView::Element *hoveredLinkItem();
	void pressedLinkItem(HistoryView::Element *item);
	HistoryView::Element *pressedLinkItem();
	void mousedItem(HistoryView::Element *item);
	HistoryView::Element *mousedItem();
	void clearMousedItems();

	const style::font &monofont();

	void initMedia();
	void deinitMedia();

	enum LaunchState {
		Launched = 0,
		QuitRequested = 1,
		QuitProcessed = 2,
	};
	void quit();
	bool quitting();
	LaunchState launchState();
	void setLaunchState(LaunchState state);
	void restart();

	constexpr auto kFileSizeLimit = 1500 * 1024 * 1024; // Load files up to 1500mb
	constexpr auto kImageSizeLimit = 64 * 1024 * 1024; // Open images up to 64mb jpg/png/gif
	QImage readImage(QByteArray data, QByteArray *format = nullptr, bool opaque = true, bool *animated = nullptr);
	QImage readImage(const QString &file, QByteArray *format = nullptr, bool opaque = true, bool *animated = nullptr, QByteArray *content = 0);
	QPixmap pixmapFromImageInPlace(QImage &&image);

	void complexOverlayRect(Painter &p, QRect rect, ImageRoundRadius radius, RectParts corners);
	void complexLocationRect(Painter &p, QRect rect, ImageRoundRadius radius, RectParts corners);

	QImage *cornersMask(ImageRoundRadius radius);
	void roundRect(Painter &p, int32 x, int32 y, int32 w, int32 h, style::color bg, RoundCorners index, const style::color *shadow = nullptr, RectParts parts = RectPart::Full);
	inline void roundRect(Painter &p, const QRect &rect, style::color bg, RoundCorners index, const style::color *shadow = nullptr, RectParts parts = RectPart::Full) {
		return roundRect(p, rect.x(), rect.y(), rect.width(), rect.height(), bg, index, shadow, parts);
	}
	void roundShadow(Painter &p, int32 x, int32 y, int32 w, int32 h, style::color shadow, RoundCorners index, RectParts parts = RectPart::Full);
	inline void roundShadow(Painter &p, const QRect &rect, style::color shadow, RoundCorners index, RectParts parts = RectPart::Full) {
		return roundShadow(p, rect.x(), rect.y(), rect.width(), rect.height(), shadow, index, parts);
	}
	void roundRect(Painter &p, int32 x, int32 y, int32 w, int32 h, style::color bg, ImageRoundRadius radius, RectParts parts = RectPart::Full);
	inline void roundRect(Painter &p, const QRect &rect, style::color bg, ImageRoundRadius radius, RectParts parts = RectPart::Full) {
		return roundRect(p, rect.x(), rect.y(), rect.width(), rect.height(), bg, radius, parts);
	}

};
