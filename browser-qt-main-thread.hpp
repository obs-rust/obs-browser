/******************************************************************************
 Copyright (C) 2026 by Volker Pacher <volker.pacher@gmail.com>

 This program is free software: you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation, either version 2 of the License, or
 (at your option) any later version.

 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with this program.  If not, see <http://www.gnu.org/licenses/>.
 ******************************************************************************/

#pragma once

#include <QCoreApplication>
#include <QMetaObject>
#include <QObject>
#include <QThread>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>

/* Runs func on the Qt main thread and waits for it: directly on the main
 * thread or without a Qt application, else through the main loop
 * (obs-rust/obs-studio#2387). Once the main loop has stopped (OBS quits),
 * nothing would run it there, so it returns false without running instead
 * of waiting forever (obs-rust/obs-studio#2391). Returns whether func ran. */

inline std::atomic<bool> &QtMainLoopStopped()
{
	static std::atomic<bool> stopped{false};
	return stopped;
}

/* Call once on the main thread while the application exists (module load):
 * Qt emits aboutToQuit when the main loop is about to stop. */
inline void WatchQtMainLoop()
{
	if (QCoreApplication *qt = QCoreApplication::instance()) {
		QObject::connect(qt, &QCoreApplication::aboutToQuit, [] { QtMainLoopStopped() = true; });
	}
}

inline bool RunOnQtMainThread(const std::function<void()> &func)
{
	QCoreApplication *qt = QCoreApplication::instance();
	if (!qt || QThread::currentThread() == qt->thread()) {
		func();
		return true;
	}
	auto stopped = [] {
		return QtMainLoopStopped().load() || QCoreApplication::closingDown();
	};
	if (stopped()) {
		return false;
	}

	struct Handover {
		std::mutex mutex;
		std::condition_variable done_cv;
		enum class State { Queued, Running, Done, Cancelled } state = State::Queued;
	};
	auto handover = std::make_shared<Handover>();
	QMetaObject::invokeMethod(
		qt,
		[handover, func] {
			{
				std::lock_guard<std::mutex> lock(handover->mutex);
				if (handover->state == Handover::State::Cancelled) {
					return;
				}
				handover->state = Handover::State::Running;
			}
			func();
			{
				std::lock_guard<std::mutex> lock(handover->mutex);
				handover->state = Handover::State::Done;
			}
			handover->done_cv.notify_all();
		},
		Qt::QueuedConnection);

	/* Wait, but give up if the loop stops before func has started. */
	std::unique_lock<std::mutex> lock(handover->mutex);
	while (handover->state != Handover::State::Done) {
		if (handover->state == Handover::State::Queued && stopped()) {
			handover->state = Handover::State::Cancelled;
			return false;
		}
		handover->done_cv.wait_for(lock, std::chrono::milliseconds(50));
	}
	return true;
}
