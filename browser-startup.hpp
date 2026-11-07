#pragma once

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

/* The Linux manager and unload share this state. A timed-out initializer may
 * never be cancelled safely: it still owns CEF locks and calls into this
 * library. Keep this object, CEF, and its events alive until process exit.
 * Complete() also parks an initializer that returns after abandonment, before
 * it can restore signal handlers, log, or publish a result into a torn-down OBS.
 */
class BrowserStartup {

	std::mutex mutex;
	std::condition_variable changed;
	std::thread watchdog;
	bool complete = false;
	bool abandoned = false;
	bool stopping = false;
	bool warned = false;

public:
	template<typename Warning> void Watch(std::chrono::milliseconds timeout, Warning warning)
	{
		watchdog = std::thread([this, timeout, warning] {
			std::unique_lock lock(mutex);
			if (!changed.wait_for(lock, timeout, [this] { return complete || stopping; })) {
				warned = true;
				warning();
			}
		});
	}

	template<typename Finish> bool Complete(Finish finish)
	{
		std::unique_lock lock(mutex);
		// This cannot be a detach-and-return: OBS may already have destroyed
		// its module metadata, log handler, and global core by now.
		changed.wait(lock, [this] { return !abandoned; });
		bool result = finish(warned);
		complete = true;
		changed.notify_all();
		return result;
	}

	bool WaitForShutdown(std::chrono::milliseconds timeout)
	{
		std::unique_lock lock(mutex);
		if (!changed.wait_for(lock, timeout, [this] { return complete; })) {
			abandoned = true;
		}
		stopping = true;
		changed.notify_all();
		lock.unlock();
		if (watchdog.joinable()) {
			watchdog.join();
		}
		return !abandoned;
	}

	bool IsAbandoned()
	{
		std::lock_guard lock(mutex);
		return abandoned;
	}
};
