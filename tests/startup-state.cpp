#include "../browser-startup.hpp"

#include <cassert>
#include <cstdlib>
#include <future>

using namespace std::chrono_literals;

int main()
{
	for (bool success : {false, true}) {
		BrowserStartup startup;
		startup.Watch(1h, [] { std::abort(); });
		assert(startup.Complete([success](bool warned) {
			assert(!warned);
			return success;
		}) == success);
		assert(startup.WaitForShutdown(100ms));
	}

	// A startup warning does not abandon an initializer that is merely slow.
	{
		BrowserStartup startup;
		std::promise<void> warning;
		startup.Watch(0ms, [&warning] { warning.set_value(); });
		warning.get_future().get();
		assert(startup.Complete([](bool warned) {
			assert(warned);
			return true;
		}));
		assert(startup.WaitForShutdown(100ms));
	}

	// A timed-out initializer and its state live until process exit. If init
	// later returns, its completion callback must not touch destroyed OBS data.
	auto *startup = new BrowserStartup;
	startup->Watch(1h, [] { std::abort(); });
	assert(!startup->WaitForShutdown(1ms));
	assert(startup->IsAbandoned());
	std::promise<void> entered;
	std::promise<void> escaped;
	auto escaped_future = escaped.get_future();
	new std::thread([startup, &entered, &escaped] {
		entered.set_value();
		startup->Complete([&escaped](bool) {
			escaped.set_value();
			return true;
		});
	});
	entered.get_future().get();
	assert(escaped_future.wait_for(50ms) == std::future_status::timeout);
	// Avoid destructing promises while the intentionally parked thread lives.
	std::_Exit(0);
}
