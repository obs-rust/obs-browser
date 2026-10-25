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

#if defined(__linux__) || defined(__APPLE__)
#include <sys/stat.h>
#endif
#ifdef __linux__
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sched.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

SandboxDecision DecideSandbox(const SandboxProbe &probe)
{
	if (!probe.supported) {
		return {SandboxMode::Off, "no sandbox probe on this platform"};
	}

	/* macOS: each helper sandboxes itself with the framework's
	 * libcef_sandbox.dylib; without it a sandboxed helper exits. */
	if (probe.macos) {
		if (probe.sandbox_library) {
			return {SandboxMode::Seatbelt, "libcef_sandbox.dylib is in the CEF framework"};
		}
		return {SandboxMode::Off, "libcef_sandbox.dylib is missing from the CEF framework"};
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

bool SandboxFallback(const SandboxProbe &probe, const SandboxDecision &decision)
{
	return probe.supported && decision.mode == SandboxMode::Off;
}

bool SandboxWarningFor(const SandboxProbe &probe, const SandboxDecision &decision, SandboxWarning *warning)
{
	if (!SandboxFallback(probe, decision)) {
		return false;
	}
	/* macOS: the sandbox library is missing. Linux: as root Chromium never
	 * sandboxes; otherwise there were no user namespaces and no usable
	 * chrome-sandbox. */
	const char *message = probe.macos  ? "SandboxFallback.Message.NoSandboxLibrary"
			      : probe.root ? "SandboxFallback.Message.Root"
					   : "SandboxFallback.Message.NoNamespaces";
	*warning = {"browser.sandbox", "SandboxFallback.Title", message, false};
	return true;
}

#ifdef __linux__
namespace {

/* Only async-signal-safe calls: the child of fork in a threaded process. */
bool WriteAll(const char *path, const char *text)
{
	int fd = open(path, O_WRONLY | O_CLOEXEC);
	if (fd < 0) {
		return false;
	}
	size_t len = strlen(text);
	bool ok = write(fd, text, len) == static_cast<ssize_t>(len);
	close(fd);
	return ok;
}

/* "0 <id> 1" into buf, without snprintf (not async-signal-safe). */
void FormatMap(char *buf, unsigned id)
{
	char digits[16];
	int n = 0;
	do {
		digits[n++] = static_cast<char>('0' + id % 10);
		id /= 10;
	} while (id);
	char *p = buf;
	*p++ = '0';
	*p++ = ' ';
	while (n) {
		*p++ = digits[--n];
	}
	*p++ = ' ';
	*p++ = '1';
	*p = 0;
}

/* What Chromium's namespace sandbox does: new user, PID and network
 * namespaces, then a map of its own uid and gid. A sysctl alone misses the
 * AppArmor restriction (Ubuntu 24.04+) and seccomp filters (systemd
 * RestrictNamespaces); this runs the operations themselves, in a child. */
bool ProbeUserNamespaces()
{
	unsigned uid = geteuid();
	unsigned gid = getegid();
	char uid_map[32];
	char gid_map[32];
	FormatMap(uid_map, uid);
	FormatMap(gid_map, gid);

	pid_t pid = fork();
	if (pid < 0) {
		return false;
	}
	if (pid == 0) {
		if (unshare(CLONE_NEWUSER | CLONE_NEWPID | CLONE_NEWNET) != 0) {
			_exit(1);
		}
		if (!WriteAll("/proc/self/setgroups", "deny")) {
			_exit(2);
		}
		if (!WriteAll("/proc/self/uid_map", uid_map)) {
			_exit(3);
		}
		if (!WriteAll("/proc/self/gid_map", gid_map)) {
			_exit(4);
		}
		_exit(0);
	}

	int status = 0;
	while (waitpid(pid, &status, 0) < 0) {
		if (errno != EINTR) {
			return false;
		}
	}
	return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

/* Chromium looks for chrome-sandbox next to libcef, or at
 * CHROME_DEVEL_SANDBOX. The kernel ignores the setuid bit on a nosuid
 * mount. */
SandboxHelper ProbeHelper(const std::string &cef_dir)
{
	const char *devel = getenv("CHROME_DEVEL_SANDBOX");
	std::string path = devel && *devel ? devel : cef_dir + "/chrome-sandbox";

	struct stat st;
	if (stat(path.c_str(), &st) != 0) {
		return SandboxHelper::Absent;
	}

	struct statvfs vfs;
	bool nosuid = statvfs(path.c_str(), &vfs) == 0 && (vfs.f_flag & ST_NOSUID);
	if (S_ISREG(st.st_mode) && st.st_uid == 0 && (st.st_mode & S_ISUID) && (st.st_mode & S_IXOTH) && !nosuid) {
		return SandboxHelper::SetuidRoot;
	}
	return SandboxHelper::NotSetuid;
}

} // namespace

SandboxProbe ProbeSandbox(const std::string &cef_dir)
{
	SandboxProbe probe;
	probe.supported = true;
	probe.root = geteuid() == 0;
	/* As root the decision is off anyway; do not fork for nothing. */
	probe.user_namespaces = !probe.root && ProbeUserNamespaces();
	probe.helper = ProbeHelper(cef_dir);
	return probe;
}
#elif defined(__APPLE__)
SandboxProbe ProbeSandbox(const std::string &cef_dir)
{
	/* cef_dir: the CEF framework next to the helpers. Each helper loads
	 * Libraries/libcef_sandbox.dylib from it. */
	SandboxProbe probe;
	probe.supported = true;
	probe.macos = true;
	struct stat st;
	probe.sandbox_library = !cef_dir.empty() &&
				stat((cef_dir + "/Libraries/libcef_sandbox.dylib").c_str(), &st) == 0 &&
				S_ISREG(st.st_mode);
	return probe;
}
#else
SandboxProbe ProbeSandbox(const std::string &)
{
	return {};
}
#endif
