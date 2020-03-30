/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "dialogs/dialogs_indexed_list.h"
#include "dialogs/dialogs_pinned_list.h"

namespace Dialogs {

class MainList final {
public:
	MainList(FilterId filterId, rpl::producer<int> pinnedLimit);

	bool empty() const;
	bool loaded() const;
	void setLoaded(bool loaded = true);
	void setAllAreMuted(bool allAreMuted = true);
	void clear();

	RowsByLetter addEntry(const Key &key);
	void removeEntry(const Key &key);

	void unreadStateChanged(
		const UnreadState &wasState,
		const UnreadState &nowState);
	void unreadEntryChanged(
		const Dialogs::UnreadState &state,
		bool added);
	void updateCloudUnread(const MTPDdialogFolder &data);
	[[nodiscard]] bool cloudUnreadKnown() const;
	[[nodiscard]] UnreadState unreadState() const;
	[[nodiscard]] rpl::producer<UnreadState> unreadStateChanges() const;

	[[nodiscard]] not_null<IndexedList*> indexed();
	[[nodiscard]] not_null<const IndexedList*> indexed() const;
	[[nodiscard]] not_null<PinnedList*> pinned();
	[[nodiscard]] not_null<const PinnedList*> pinned() const;

	void setCloudListSize(int size);
	[[nodiscard]] const rpl::variable<int> &fullSize() const;

private:
	void finalizeCloudUnread();
	void recomputeFullListSize();

	auto unreadStateChangeNotifier(bool notify) {
		const auto wasState = notify ? unreadState() : UnreadState();
		return gsl::finally([=] {
			if (notify) {
				_unreadStateChanges.fire_copy(wasState);
			}
		});
	}

	FilterId _filterId = 0;
	IndexedList _all;
	PinnedList _pinned;
	UnreadState _unreadState;
	UnreadState _cloudUnreadState;
	rpl::event_stream<UnreadState> _unreadStateChanges;
	rpl::variable<int> _fullListSize = 0;
	int _cloudListSize = 0;

	bool _loaded = false;
	bool _allAreMuted = false;

	rpl::lifetime _lifetime;

};

} // namespace Dialogs
