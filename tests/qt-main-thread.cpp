/* RunOnQtMainThread() (browser-qt-main-thread.hpp, obs-rust/obs-studio#2391):
 * while Qt's main loop runs, a worker thread's call runs on the main thread;
 * once the loop has finished, the call returns at once without running, so
 * a first browser source created off the main thread during shutdown cannot
 * hang OBS on quit. */
#include "browser-qt-main-thread.hpp"

#include <QCoreApplication>
#include <QTimer>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <future>
#include <thread>
#include <unistd.h>

namespace {
int failures = 0;

void Check(bool ok, const char *name)
{
	if (ok) {
		std::printf("ok   %s\n", name);
	} else {
		std::fprintf(stderr, "FAIL %s\n", name);
		failures++;
	}
}
} // namespace

int main(int argc, char **argv)
{
	QCoreApplication app(argc, argv);
	WatchQtMainLoop();

	/* The loop runs: a worker thread hands the function to the main thread. */
	std::atomic<bool> ran{false}, on_main{false};
	bool returned = false;
	std::thread worker;
	QTimer::singleShot(0, &app, [&] {
		worker = std::thread([&] {
			returned = RunOnQtMainThread([&] {
				ran = true;
				on_main = QThread::currentThread() == app.thread();
			});
			QMetaObject::invokeMethod(&app, [] { QCoreApplication::quit(); }, Qt::QueuedConnection);
		});
	});
	QTimer::singleShot(5000, &app, [] { QCoreApplication::quit(); });
	app.exec();
	worker.join();
	Check(returned && ran && on_main, "while the loop runs: runs on the main thread");

	/* The loop has finished; the application still exists (OBS quitting). */
	std::atomic<bool> ran_after{false};
	auto call = std::async(std::launch::async, [&] { return RunOnQtMainThread([&] { ran_after = true; }); });
	if (call.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
		std::fprintf(stderr,
			     "FAIL after the loop finished: still waiting after 2 s (OBS would hang on quit)\n");
		std::fflush(stdout);
		std::fflush(stderr);
		_exit(1);
	}
	Check(!call.get() && !ran_after, "after the loop finished: returns at once without running");

	if (failures) {
		std::fprintf(stderr, "%d case(s) failed\n", failures);
		return 1;
	}
	return 0;
}
