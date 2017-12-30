/*
This file is part of Telegram Desktop,
the official desktop version of Telegram messaging app, see https://telegram.org

Telegram Desktop is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

It is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
GNU General Public License for more details.

In addition, as a special exception, the copyright holders give permission
to link the code of portions of this program with the OpenSSL library.

Full license: https://github.com/telegramdesktop/tdesktop/blob/master/LICENSE
Copyright (c) 2014-2017 John Preston, https://desktop.telegram.org
*/
#pragma once

#include <crl/common/crl_common_config.h>

#if defined CRL_USE_COMMON_QUEUE || !defined CRL_USE_DISPATCH

#include <crl/common/crl_common_list.h>
#include <crl/common/crl_common_utils.h>
#include <atomic>

namespace crl {
namespace details {
class main_queue_pointer;
} // namespace details

class queue {
public:
	queue();
	queue(const queue &other) = delete;
	queue &operator=(const queue &other) = delete;

	template <typename Callable>
	void async(Callable &&callable) {
		if (_list.push_is_first(std::forward<Callable>(callable))) {
			wake_async();
		}
	}

	template <typename Callable>
	void sync(Callable &&callable) {
		semaphore waiter;
		async([&] {
			const auto guard = details::finally([&] { waiter.release(); });
			callable();
		});
		waiter.acquire();
	}

	~queue();

private:
	friend class details::main_queue_pointer;

	static void ProcessCallback(void *that);

	queue(queue_processor processor);

	void wake_async();
	void process();

	queue_processor _main_processor = nullptr;
	semaphore _sentinel_semaphore;
	details::list _list;
	std::atomic<bool> _queued = false;

};

} // namespace crl

#endif // CRL_USE_COMMON_QUEUE || !CRL_USE_DISPATCH
