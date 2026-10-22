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

#include "browser-sandbox.hpp"

SandboxDecision DecideSandbox(const SandboxProbe &probe)
{
	if (!probe.supported) {
		return {SandboxMode::Off, "no sandbox probe on this platform"};
	}

	/* Chromium does not run sandboxed as root: it exits the process. */
	if (probe.root) {
		return {SandboxMode::Off, "OBS runs as root"};
	}

	/* Chromium prefers the namespace sandbox and then ignores the helper. */
	if (probe.user_namespaces) {
		return {SandboxMode::Namespaces, "user namespaces are available"};
	}

	switch (probe.helper) {
	case SandboxHelper::SetuidRoot:
		return {SandboxMode::SetuidHelper, "no user namespaces; chrome-sandbox is setuid root"};
	case SandboxHelper::NotSetuid:
		/* Chromium aborts on a helper it cannot use, instead of
		 * running without the sandbox. */
		return {SandboxMode::Off, "no user namespaces, and chrome-sandbox is not setuid root"};
	case SandboxHelper::Absent:
		break;
	}
	return {SandboxMode::Off, "no user namespaces and no chrome-sandbox"};
}

SandboxProbe ProbeSandbox(const std::string &)
{
	return {};
}
