/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "boxes/peer_list_box.h"
#include "boxes/edit_privacy_box.h"
#include "history/view/history_view_element.h"
#include "mtproto/sender.h"

namespace Settings {

class BlockedBoxController : public PeerListController, private base::Subscriber, private MTP::Sender {
public:
	void prepare() override;
	void rowClicked(not_null<PeerListRow*> row) override;
	void rowActionClicked(not_null<PeerListRow*> row) override;
	void loadMoreRows() override;

	static void BlockNewUser();

private:
	void receivedUsers(const QVector<MTPContactBlocked> &result);
	void handleBlockedEvent(not_null<UserData*> user);

	bool appendRow(not_null<UserData*> user);
	bool prependRow(not_null<UserData*> user);
	std::unique_ptr<PeerListRow> createRow(not_null<UserData*> user) const;

	int _offset = 0;
	mtpRequestId _loadRequestId = 0;
	bool _allLoaded = false;

};

class PhoneNumberPrivacyController : public EditPrivacyController {
public:
	using Option = EditPrivacyBox::Option;
	using Exception = EditPrivacyBox::Exception;

	Key key() override;
	MTPInputPrivacyKey apiKey() override;

	rpl::producer<QString> title() override;
	rpl::producer<QString> optionsTitleKey() override;
	rpl::producer<QString> warning() override;
	rpl::producer<QString> exceptionButtonTextKey(
		Exception exception) override;
	rpl::producer<QString> exceptionBoxTitle(Exception exception) override;
	rpl::producer<QString> exceptionsDescription() override;

};

class LastSeenPrivacyController : public EditPrivacyController {
public:
	using Option = EditPrivacyBox::Option;
	using Exception = EditPrivacyBox::Exception;

	Key key() override;
	MTPInputPrivacyKey apiKey() override;

	rpl::producer<QString> title() override;
	rpl::producer<QString> optionsTitleKey() override;
	rpl::producer<QString> warning() override;
	rpl::producer<QString> exceptionButtonTextKey(
		Exception exception) override;
	rpl::producer<QString> exceptionBoxTitle(Exception exception) override;
	rpl::producer<QString> exceptionsDescription() override;

	void confirmSave(
		bool someAreDisallowed,
		FnMut<void()> saveCallback) override;

};

class GroupsInvitePrivacyController : public EditPrivacyController {
public:
	using Option = EditPrivacyBox::Option;
	using Exception = EditPrivacyBox::Exception;

	Key key() override;
	MTPInputPrivacyKey apiKey() override;

	rpl::producer<QString> title() override;
	bool hasOption(Option option) override;
	rpl::producer<QString> optionsTitleKey() override;
	rpl::producer<QString> exceptionButtonTextKey(
		Exception exception) override;
	rpl::producer<QString> exceptionBoxTitle(Exception exception) override;
	rpl::producer<QString> exceptionsDescription() override;

};

class CallsPrivacyController : public EditPrivacyController {
public:
	using Option = EditPrivacyBox::Option;
	using Exception = EditPrivacyBox::Exception;

	Key key() override;
	MTPInputPrivacyKey apiKey() override;

	rpl::producer<QString> title() override;
	rpl::producer<QString> optionsTitleKey() override;
	rpl::producer<QString> exceptionButtonTextKey(
		Exception exception) override;
	rpl::producer<QString> exceptionBoxTitle(Exception exception) override;
	rpl::producer<QString> exceptionsDescription() override;

	object_ptr<Ui::RpWidget> setupBelowWidget(
		not_null<QWidget*> parent) override;

};

class CallsPeer2PeerPrivacyController : public EditPrivacyController {
public:
	using Option = EditPrivacyBox::Option;
	using Exception = EditPrivacyBox::Exception;

	Key key() override;
	MTPInputPrivacyKey apiKey() override;

	rpl::producer<QString> title() override;
	rpl::producer<QString> optionsTitleKey() override;
	QString optionLabel(EditPrivacyBox::Option option) override;
	rpl::producer<QString> warning() override;
	rpl::producer<QString> exceptionButtonTextKey(
		Exception exception) override;
	rpl::producer<QString> exceptionBoxTitle(Exception exception) override;
	rpl::producer<QString> exceptionsDescription() override;

};

class ForwardsPrivacyController
	: public EditPrivacyController
	, private HistoryView::SimpleElementDelegate {
public:
	using Option = EditPrivacyBox::Option;
	using Exception = EditPrivacyBox::Exception;

	Key key() override;
	MTPInputPrivacyKey apiKey() override;

	rpl::producer<QString> title() override;
	rpl::producer<QString> optionsTitleKey() override;
	rpl::producer<QString> warning() override;
	rpl::producer<QString> exceptionButtonTextKey(
		Exception exception) override;
	rpl::producer<QString> exceptionBoxTitle(Exception exception) override;
	rpl::producer<QString> exceptionsDescription() override;

	object_ptr<Ui::RpWidget> setupAboveWidget(
		not_null<QWidget*> parent,
		rpl::producer<Option> optionValue) override;

private:
	using Element = HistoryView::Element;
	not_null<HistoryView::ElementDelegate*> delegate();
	HistoryView::Context elementContext() override;

	static void PaintForwardedTooltip(
		Painter &p,
		not_null<HistoryView::Element*> view,
		Option value);

};

class ProfilePhotoPrivacyController : public EditPrivacyController {
public:
	using Option = EditPrivacyBox::Option;
	using Exception = EditPrivacyBox::Exception;

	Key key() override;
	MTPInputPrivacyKey apiKey() override;

	rpl::producer<QString> title() override;
	bool hasOption(Option option) override;
	rpl::producer<QString> optionsTitleKey() override;
	rpl::producer<QString> exceptionButtonTextKey(
		Exception exception) override;
	rpl::producer<QString> exceptionBoxTitle(Exception exception) override;
	rpl::producer<QString> exceptionsDescription() override;

};

} // namespace Settings
