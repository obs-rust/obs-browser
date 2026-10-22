#include <cstdio>
#include <cstring>

#include "browser-sandbox.hpp"

namespace {

int failures = 0;

void Expect(const char *name, const SandboxProbe &probe, SandboxMode mode, const char *reason_part)
{
	SandboxDecision decision = DecideSandbox(probe);
	bool reason_ok = decision.reason && std::strstr(decision.reason, reason_part);
	if (decision.mode != mode || !reason_ok) {
		std::fprintf(stderr, "FAIL %s: mode %d (expected %d), reason \"%s\" (expected to contain \"%s\")\n",
			     name, static_cast<int>(decision.mode), static_cast<int>(mode),
			     decision.reason ? decision.reason : "(null)", reason_part);
		failures++;
	} else {
		std::printf("ok   %s\n", name);
	}
}

SandboxProbe Probe(bool root, bool user_namespaces, SandboxHelper helper)
{
	SandboxProbe probe;
	probe.supported = true;
	probe.root = root;
	probe.user_namespaces = user_namespaces;
	probe.helper = helper;
	return probe;
}

} // namespace

int main()
{
	/* No probe (Windows, macOS for now): as today. */
	Expect("unsupported platform", SandboxProbe{}, SandboxMode::Off, "platform");

	/* Chromium exits the process when it runs sandboxed as root. */
	Expect("root, namespaces", Probe(true, true, SandboxHelper::Absent), SandboxMode::Off, "root");
	Expect("root, setuid helper", Probe(true, false, SandboxHelper::SetuidRoot), SandboxMode::Off, "root");

	/* User namespaces: no helper needed. A helper that is not setuid does
	 * not matter here (measured on CEF 7871: renders sandboxed). */
	Expect("user, namespaces", Probe(false, true, SandboxHelper::Absent), SandboxMode::Namespaces, "namespace");
	Expect("user, namespaces, helper not setuid", Probe(false, true, SandboxHelper::NotSetuid),
	       SandboxMode::Namespaces, "namespace");
	Expect("user, namespaces, setuid helper", Probe(false, true, SandboxHelper::SetuidRoot),
	       SandboxMode::Namespaces, "namespace");

	/* No user namespaces: only a setuid root helper works. Chromium aborts
	 * when it finds a helper that is not setuid. */
	Expect("user, no namespaces, setuid helper", Probe(false, false, SandboxHelper::SetuidRoot),
	       SandboxMode::SetuidHelper, "chrome-sandbox");
	Expect("user, no namespaces, helper not setuid", Probe(false, false, SandboxHelper::NotSetuid),
	       SandboxMode::Off, "not setuid");
	Expect("user, no namespaces, no helper", Probe(false, false, SandboxHelper::Absent), SandboxMode::Off,
	       "no user namespaces");

	if (failures) {
		std::fprintf(stderr, "%d case(s) failed\n", failures);
		return 1;
	}
	return 0;
}
