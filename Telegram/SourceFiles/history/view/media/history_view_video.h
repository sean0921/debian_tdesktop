/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "history/view/media/history_view_file.h"

namespace HistoryView {

class Video : public File {
public:
	Video(
		not_null<Element*> parent,
		not_null<HistoryItem*> realParent,
		not_null<DocumentData*> document);

	void draw(Painter &p, const QRect &r, TextSelection selection, crl::time ms) const override;
	TextState textState(QPoint point, StateRequest request) const override;

	[[nodiscard]] TextSelection adjustSelection(
			TextSelection selection,
			TextSelectType type) const override {
		return _caption.adjustSelection(selection, type);
	}
	uint16 fullSelectionLength() const override {
		return _caption.length();
	}
	bool hasTextForCopy() const override {
		return !_caption.isEmpty();
	}

	TextForMimeData selectedText(TextSelection selection) const override;

	DocumentData *getDocument() const override {
		return _data;
	}

	QSize sizeForGrouping() const override;
	void drawGrouped(
		Painter &p,
		const QRect &clip,
		TextSelection selection,
		crl::time ms,
		const QRect &geometry,
		RectParts corners,
		not_null<uint64*> cacheKey,
		not_null<QPixmap*> cache) const override;
	TextState getStateGrouped(
		const QRect &geometry,
		QPoint point,
		StateRequest request) const override;

	bool uploading() const override;

	TextWithEntities getCaption() const override {
		return _caption.toTextWithEntities();
	}
	bool needsBubble() const override;
	bool customInfoLayout() const override {
		return _caption.isEmpty();
	}
	bool skipBubbleTail() const override {
		return isBubbleBottom() && _caption.isEmpty();
	}

	void parentTextUpdated() override;

protected:
	float64 dataProgress() const override;
	bool dataFinished() const override;
	bool dataLoaded() const override;

private:
	[[nodiscard]] QSize countOptimalSize() override;
	[[nodiscard]] QSize countCurrentSize(int newWidth) override;
	[[nodiscard]] QSize countOptimalDimensions() const;
	[[nodiscard]] bool downloadInCorner() const;

	void drawCornerStatus(Painter &p, bool selected) const;
	[[nodiscard]] TextState cornerStatusTextState(
		QPoint point,
		StateRequest request) const;

	void validateGroupedCache(
		const QRect &geometry,
		RectParts corners,
		not_null<uint64*> cacheKey,
		not_null<QPixmap*> cache) const;
	void setStatusSize(int newSize) const;
	void updateStatusText() const;
	QSize sizeForAspectRatio() const;

	not_null<DocumentData*> _data;
	int _thumbw = 1;
	int _thumbh = 1;
	Ui::Text::String _caption;

	QString _downloadSize;

};

} // namespace HistoryView
