/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

class ApiWrap;

namespace Api {

struct InviteLink {
	QString link;
	not_null<UserData*> admin;
	TimeId date = 0;
	TimeId startDate = 0;
	TimeId expireDate = 0;
	int usageLimit = 0;
	int usage = 0;
	bool permanent = false;
	bool revoked = false;
};

struct PeerInviteLinks {
	std::vector<InviteLink> links;
	int count = 0;
};

struct JoinedByLinkUser {
	not_null<UserData*> user;
	TimeId date = 0;
};

struct JoinedByLinkSlice {
	std::vector<JoinedByLinkUser> users;
	int count = 0;
};

struct InviteLinkUpdate {
	not_null<PeerData*> peer;
	QString was;
	std::optional<InviteLink> now;
};
// #TODO links
//[[nodiscard]] JoinedByLinkSlice ParseJoinedByLinkSlice(
//	not_null<PeerData*> peer,
//	const MTPmessages_ChatInviteImporters &slice);

class InviteLinks final {
public:
	explicit InviteLinks(not_null<ApiWrap*> api);

	using Link = InviteLink;
	using Links = PeerInviteLinks;
	using Update = InviteLinkUpdate;

	void create(
		not_null<PeerData*> peer,
		Fn<void(Link)> done = nullptr,
		TimeId expireDate = 0,
		int usageLimit = 0);
	void edit(
		not_null<PeerData*> peer,
		const QString &link,
		TimeId expireDate,
		int usageLimit,
		Fn<void(Link)> done = nullptr);
	void revoke(
		not_null<PeerData*> peer,
		const QString &link,
		Fn<void(Link)> done = nullptr);
	void revokePermanent(
		not_null<PeerData*> peer,
		Fn<void(Link)> done = nullptr);
	void destroy(
		not_null<PeerData*> peer,
		const QString &link,
		Fn<void()> done = nullptr);
	void destroyAllRevoked(
		not_null<PeerData*> peer,
		Fn<void()> done = nullptr);

	void setPermanent(
		not_null<PeerData*> peer,
		const MTPExportedChatInvite &invite);
	void clearPermanent(not_null<PeerData*> peer);

	void requestLinks(not_null<PeerData*> peer);
	[[nodiscard]] const Links &links(not_null<PeerData*> peer) const;

	[[nodiscard]] rpl::producer<JoinedByLinkSlice> joinedFirstSliceValue(
		not_null<PeerData*> peer,
		const QString &link,
		int fullCount);
	[[nodiscard]] std::optional<JoinedByLinkSlice> joinedFirstSliceLoaded(
		not_null<PeerData*> peer,
		const QString &link) const;
	[[nodiscard]] rpl::producer<Update> updates(
		not_null<PeerData*> peer) const;
	[[nodiscard]] rpl::producer<> allRevokedDestroyed(
		not_null<PeerData*> peer) const;

	void requestMoreLinks(
		not_null<PeerData*> peer,
		TimeId lastDate,
		const QString &lastLink,
		bool revoked,
		Fn<void(Links)> done);

private:
	struct LinkKey {
		not_null<PeerData*> peer;
		QString link;

		friend inline bool operator<(const LinkKey &a, const LinkKey &b) {
			return (a.peer == b.peer)
				? (a.link < b.link)
				: (a.peer < b.peer);
		}
		friend inline bool operator==(const LinkKey &a, const LinkKey &b) {
			return (a.peer == b.peer) && (a.link == b.link);
		}
	};

	// #TODO links
	//[[nodiscard]] Links parseSlice(
	//	not_null<PeerData*> peer,
	//	const MTPmessages_ExportedChatInvites &slice) const;
	[[nodiscard]] Link parse(
		not_null<PeerData*> peer,
		const MTPExportedChatInvite &invite) const;
	[[nodiscard]] Link *lookupPermanent(not_null<PeerData*> peer);
	[[nodiscard]] Link *lookupPermanent(Links &links);
	[[nodiscard]] const Link *lookupPermanent(const Links &links) const;
	Link prepend(
		not_null<PeerData*> peer,
		const MTPExportedChatInvite &invite);
	void notify(not_null<PeerData*> peer);

	void editPermanentLink(
		not_null<PeerData*> peer,
		const QString &link);

	void performEdit(
		not_null<PeerData*> peer,
		const QString &link,
		Fn<void(Link)> done,
		bool revoke,
		TimeId expireDate = 0,
		int usageLimit = 0);
	void performCreate(
		not_null<PeerData*> peer,
		Fn<void(Link)> done,
		bool revokeLegacyPermanent,
		TimeId expireDate = 0,
		int usageLimit = 0);

	void requestJoinedFirstSlice(LinkKey key);
	[[nodiscard]] std::optional<JoinedByLinkSlice> lookupJoinedFirstSlice(
		LinkKey key) const;

	const not_null<ApiWrap*> _api;

	base::flat_map<not_null<PeerData*>, Links> _firstSlices;
	base::flat_map<not_null<PeerData*>, mtpRequestId> _firstSliceRequests;

	base::flat_map<LinkKey, JoinedByLinkSlice> _firstJoined;
	base::flat_map<LinkKey, mtpRequestId> _firstJoinedRequests;
	rpl::event_stream<LinkKey> _joinedFirstSliceLoaded;

	base::flat_map<
		not_null<PeerData*>,
		std::vector<Fn<void(Link)>>> _createCallbacks;
	base::flat_map<LinkKey, std::vector<Fn<void(Link)>>> _editCallbacks;
	base::flat_map<LinkKey, std::vector<Fn<void()>>> _deleteCallbacks;
	base::flat_map<
		not_null<PeerData*>,
		std::vector<Fn<void()>>> _deleteRevokedCallbacks;

	rpl::event_stream<Update> _updates;
	rpl::event_stream<not_null<PeerData*>> _allRevokedDestroyed;

};

} // namespace Api
