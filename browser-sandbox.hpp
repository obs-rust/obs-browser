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

#include <string>

/* Whether the CEF sandbox can work, decided before CefInitialize
 * (obs-rust/obs-studio#1092). With the sandbox on, Chromium exits the whole
 * process when it cannot sandbox: as root, and when it finds no user
 * namespaces and a chrome-sandbox helper that is not setuid root. */

enum class SandboxHelper {
	Absent,     /* no chrome-sandbox next to libcef */
	SetuidRoot, /* owned by root, setuid, executable */
	NotSetuid,  /* present, but Chromium cannot use it */
};

/* The facts the decision uses. ProbeSandbox fills them on Linux. */
struct SandboxProbe {
	bool supported = false;       /* a probe exists for this platform */
	bool root = false;            /* the effective user is root */
	bool user_namespaces = false; /* a child can create user, PID and network namespaces and map its uid */
	SandboxHelper helper = SandboxHelper::Absent;
};

enum class SandboxMode {
	Off,          /* settings.no_sandbox = true */
	Namespaces,   /* Chromium's user namespace sandbox */
	SetuidHelper, /* Chromium's setuid chrome-sandbox helper */
};

struct SandboxDecision {
	SandboxMode mode;
	const char *reason; /* for the log */
};

/* Pure: the same probe gives the same decision. */
SandboxDecision DecideSandbox(const SandboxProbe &probe);

/* The fallback (B in obs-rust/obs-studio#1092): a probe says the sandbox
 * cannot work, so the browser runs without it, and the user is warned.
 * Without a probe (Windows, macOS for now) the sandbox is off as before,
 * and only the log says so. */
bool SandboxFallback(const SandboxProbe &probe, const SandboxDecision &decision);

/* cef_dir: the directory of libcef (and of chrome-sandbox). */
SandboxProbe ProbeSandbox(const std::string &cef_dir);
