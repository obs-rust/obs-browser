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
#include <QThread>

#include <functional>

/* Runs func on the Qt main thread and waits for it: directly on the main
 * thread or without a Qt application, else through the main loop
 * (obs-rust/obs-studio#2387). Returns whether func ran. */

/* Call once on the main thread while the application exists. */
inline void WatchQtMainLoop() {}

inline bool RunOnQtMainThread(const std::function<void()> &func)
{
	QCoreApplication *qt = QCoreApplication::instance();
	if (!qt || QThread::currentThread() == qt->thread()) {
		func();
		return true;
	}
	QMetaObject::invokeMethod(qt, func, Qt::BlockingQueuedConnection);
	return true;
}
