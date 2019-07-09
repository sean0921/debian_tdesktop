/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/runtime_composer.h"
#include "base/flags.h"
#include "base/value_ordering.h"
#include "data/data_media_types.h"

enum class UnreadMentionType;
struct HistoryMessageReplyMarkup;
class ReplyKeyboard;
class HistoryMessage;
class HistoryMedia;

namespace base {
template <typename Enum>
class enum_mask;
} // namespace base

namespace Storage {
enum class SharedMediaType : signed char;
using SharedMediaTypesMask = base::enum_mask<SharedMediaType>;
} // namespace Storage

namespace Ui {
class RippleAnimation;
} // namespace Ui

namespace style {
struct BotKeyboardButton;
struct RippleAnimation;
} // namespace style

namespace Data {
struct MessagePosition;
class Media;
} // namespace Data

namespace Window {
class SessionController;
} // namespace Window

namespace HistoryView {
struct TextState;
struct StateRequest;
enum class CursorState : char;
enum class PointState : char;
enum class Context : char;
class ElementDelegate;
} // namespace HistoryView

struct HiddenSenderInfo;

class HistoryItem : public RuntimeComposer<HistoryItem> {
public:
	static not_null<HistoryItem*> Create(
		not_null<History*> history,
		const MTPMessage &message);

	struct Destroyer {
		void operator()(HistoryItem *value);

	};

	virtual void dependencyItemRemoved(HistoryItem *dependency) {
	}
	virtual bool updateDependencyItem() {
		return true;
	}
	virtual MsgId dependencyMsgId() const {
		return 0;
	}
	virtual bool notificationReady() const {
		return true;
	}
	virtual void applyGroupAdminChanges(
		const base::flat_map<UserId, bool> &changes) {
	}

	UserData *viaBot() const;
	UserData *getMessageBot() const;

	bool isLogEntry() const {
		return (id > ServerMaxMsgId);
	}
	void addLogEntryOriginal(
		WebPageId localId,
		const QString &label,
		const TextWithEntities &content);

	not_null<History*> history() const {
		return _history;
	}
	not_null<PeerData*> from() const {
		return _from;
	}
	HistoryView::Element *mainView() const {
		return _mainView;
	}
	void setMainView(not_null<HistoryView::Element*> view) {
		_mainView = view;
	}
	void refreshMainView();
	void clearMainView();
	void removeMainView();

	void destroy();
	[[nodiscard]] bool out() const {
		return _flags & MTPDmessage::Flag::f_out;
	}
	[[nodiscard]] bool unread() const;
	void markClientSideAsRead();
	[[nodiscard]] bool mentionsMe() const;
	[[nodiscard]] bool isUnreadMention() const;
	[[nodiscard]] bool isUnreadMedia() const;
	[[nodiscard]] bool hasUnreadMediaFlag() const;
	void markMediaRead();


	// For edit media in history_message.
	virtual void returnSavedMedia() {};
	void savePreviousMedia() {
		_savedMedia = _media->clone(this);
	}
	[[nodiscard]] bool isEditingMedia() const {
		return _savedMedia != nullptr;
	}
	void clearSavedMedia() {
		_savedMedia = nullptr;
	}

	// Zero result means this message is not self-destructing right now.
	virtual crl::time getSelfDestructIn(crl::time now) {
		return 0;
	}

	bool definesReplyKeyboard() const;
	MTPDreplyKeyboardMarkup::Flags replyKeyboardFlags() const;

	bool hasSwitchInlineButton() const {
		return _flags & MTPDmessage_ClientFlag::f_has_switch_inline_button;
	}
	bool hasTextLinks() const {
		return _flags & MTPDmessage_ClientFlag::f_has_text_links;
	}
	bool isGroupEssential() const {
		return _flags & MTPDmessage_ClientFlag::f_is_group_essential;
	}
	bool isLocalUpdateMedia() const {
		return _flags & MTPDmessage_ClientFlag::f_is_local_update_media;
	}
	void setIsLocalUpdateMedia(bool flag) {
		if (flag) {
			_flags |= MTPDmessage_ClientFlag::f_is_local_update_media;
		} else {
			_flags &= ~MTPDmessage_ClientFlag::f_is_local_update_media;
		}
	}
	bool isGroupMigrate() const {
		return isGroupEssential() && isEmpty();
	}
	bool hasViews() const {
		return _flags & MTPDmessage::Flag::f_views;
	}
	bool isPost() const {
		return _flags & MTPDmessage::Flag::f_post;
	}
	bool isSilent() const {
		return _flags & MTPDmessage::Flag::f_silent;
	}
	virtual int viewsCount() const {
		return hasViews() ? 1 : -1;
	}

	virtual bool needCheck() const;

	virtual bool serviceMsg() const {
		return false;
	}
	virtual void applyEdition(const MTPDmessage &message) {
	}
	virtual void applyEdition(const MTPDmessageService &message) {
	}
	void applyEditionToHistoryCleared();
	virtual void updateSentMedia(const MTPMessageMedia *media) {
	}
	virtual void updateReplyMarkup(const MTPReplyMarkup *markup) {
	}
	virtual void updateForwardedInfo(const MTPMessageFwdHeader *fwd) {
	}

	virtual void addToUnreadMentions(UnreadMentionType type);
	virtual void eraseFromUnreadMentions() {
	}
	virtual Storage::SharedMediaTypesMask sharedMediaTypes() const = 0;

	void indexAsNewItem();

	virtual QString notificationHeader() const {
		return QString();
	}
	virtual QString notificationText() const;

	enum class DrawInDialog {
		Normal,
		WithoutSender,
	};

	// Returns text with link-start and link-end commands for service-color highlighting.
	// Example: "[link1-start]You:[link1-end] [link1-start]Photo,[link1-end] caption text"
	virtual QString inDialogsText(DrawInDialog way) const;
	virtual QString inReplyText() const {
		return inDialogsText(DrawInDialog::WithoutSender);
	}
	virtual TextWithEntities originalText() const {
		return TextWithEntities();
	}
	virtual TextForMimeData clipboardText() const {
		return TextForMimeData();
	}

	virtual void setViewsCount(int32 count) {
	}
	virtual void setRealId(MsgId newId);

	void drawInDialog(
		Painter &p,
		const QRect &r,
		bool active,
		bool selected,
		DrawInDialog way,
		const HistoryItem *&cacheFor,
		Ui::Text::String &cache) const;

	bool emptyText() const {
		return _text.isEmpty();
	}

	bool isPinned() const;
	bool canPin() const;
	bool canStopPoll() const;
	virtual bool allowsForward() const;
	virtual bool allowsEdit(TimeId now) const;
	bool canDelete() const;
	bool canDeleteForEveryone(TimeId now) const;
	bool suggestReport() const;
	bool suggestBanReport() const;
	bool suggestDeleteAllReport() const;

	bool hasDirectLink() const;

	MsgId id;

	ChannelId channelId() const;
	FullMsgId fullId() const {
		return FullMsgId(channelId(), id);
	}
	Data::MessagePosition position() const;
	TimeId date() const;

	Data::Media *media() const {
		return _media.get();
	}
	virtual void setText(const TextWithEntities &textWithEntities) {
	}
	virtual bool textHasLinks() const {
		return false;
	}

	virtual HistoryMessage *toHistoryMessage() { // dynamic_cast optimize
		return nullptr;
	}
	virtual const HistoryMessage *toHistoryMessage() const { // dynamic_cast optimize
		return nullptr;
	}
	MsgId replyToId() const;

	not_null<PeerData*> author() const;

	TimeId dateOriginal() const;
	PeerData *senderOriginal() const;
	const HiddenSenderInfo *hiddenForwardedInfo() const;
	not_null<PeerData*> fromOriginal() const;
	QString authorOriginal() const;
	MsgId idOriginal() const;

	bool isEmpty() const;

	MessageGroupId groupId() const;

	const HistoryMessageReplyMarkup *inlineReplyMarkup() const {
		return const_cast<HistoryItem*>(this)->inlineReplyMarkup();
	}
	const ReplyKeyboard *inlineReplyKeyboard() const {
		return const_cast<HistoryItem*>(this)->inlineReplyKeyboard();
	}
	HistoryMessageReplyMarkup *inlineReplyMarkup();
	ReplyKeyboard *inlineReplyKeyboard();

	[[nodiscard]] ChannelData *discussionPostOriginalSender() const;
	[[nodiscard]] bool isDiscussionPost() const;
	[[nodiscard]] PeerData *displayFrom() const;

	virtual std::unique_ptr<HistoryView::Element> createView(
		not_null<HistoryView::ElementDelegate*> delegate) = 0;

	virtual ~HistoryItem();

protected:
	HistoryItem(
		not_null<History*> history,
		MsgId id,
		MTPDmessage::Flags flags,
		TimeId date,
		UserId from);

	virtual void markMediaAsReadHook() {
	}

	void finishEdition(int oldKeyboardTop);
	void finishEditionToEmpty();

	const not_null<History*> _history;
	not_null<PeerData*> _from;
	MTPDmessage::Flags _flags = 0;

	void invalidateChatListEntry();

	void setGroupId(MessageGroupId groupId);

	Ui::Text::String _text = { st::msgMinWidth };
	int _textWidth = -1;
	int _textHeight = 0;

	std::unique_ptr<Data::Media> _savedMedia;
	std::unique_ptr<Data::Media> _media;

private:
	TimeId _date = 0;

	HistoryView::Element *_mainView = nullptr;
	friend class HistoryView::Element;

	MessageGroupId _groupId = MessageGroupId();

};

QDateTime ItemDateTime(not_null<const HistoryItem*> item);

ClickHandlerPtr goToMessageClickHandler(
	not_null<PeerData*> peer,
	MsgId msgId,
	FullMsgId returnToId = FullMsgId());
ClickHandlerPtr goToMessageClickHandler(
	not_null<HistoryItem*> item,
	FullMsgId returnToId = FullMsgId());
