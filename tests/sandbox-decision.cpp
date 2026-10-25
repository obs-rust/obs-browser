#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>

#include "browser-sandbox.hpp"

namespace {

int failures = 0;
std::string locale_text; /* data/locale/en-US.ini, from argv[1] */

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

void ExpectFallback(const char *name, const SandboxProbe &probe, bool fallback)
{
	bool got = SandboxFallback(probe, DecideSandbox(probe));
	if (got != fallback) {
		std::fprintf(stderr, "FAIL fallback %s: %d (expected %d)\n", name, got, fallback);
		failures++;
	} else {
		std::printf("ok   fallback %s\n", name);
	}
}

bool HasLocaleKey(const char *key)
{
	return key && locale_text.find("\n" + std::string(key) + "=") != std::string::npos;
}

/* message_key: nullptr when the case must not warn. */
void ExpectWarning(const char *name, const SandboxProbe &probe, const char *message_key)
{
	SandboxWarning warning{};
	bool warns = SandboxWarningFor(probe, DecideSandbox(probe), &warning);
	bool ok;
	if (!message_key) {
		ok = !warns;
	} else {
		ok = warns && warning.id && std::strcmp(warning.id, "browser.sandbox") == 0 && warning.message_key &&
		     std::strcmp(warning.message_key, message_key) == 0 && warning.title_key &&
		     HasLocaleKey(warning.title_key) && HasLocaleKey(warning.message_key) && !warning.can_cancel;
	}
	if (!ok) {
		std::fprintf(stderr,
			     "FAIL warning %s: warns %d (expected %d), id \"%s\", title \"%s\" (in en-US.ini: %d), "
			     "message \"%s\" (expected \"%s\", in en-US.ini: %d), can_cancel %d\n",
			     name, warns, message_key != nullptr, warning.id ? warning.id : "(null)",
			     warning.title_key ? warning.title_key : "(null)", HasLocaleKey(warning.title_key),
			     warning.message_key ? warning.message_key : "(null)", message_key ? message_key : "(none)",
			     HasLocaleKey(warning.message_key), warning.can_cancel);
		failures++;
	} else {
		std::printf("ok   warning %s\n", name);
	}
}

SandboxProbe MacProbe(bool sandbox_library)
{
	SandboxProbe probe;
	probe.supported = true;
	probe.macos = true;
	probe.sandbox_library = sandbox_library;
	return probe;
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

int main(int argc, char **argv)
{
	if (argc > 1) {
		std::ifstream in(argv[1]);
		locale_text = "\n" + std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
	}

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

	/* The fallback warns only where a probe said the sandbox cannot work. */
	ExpectFallback("unsupported platform", SandboxProbe{}, false);
	ExpectFallback("root", Probe(true, true, SandboxHelper::Absent), true);
	ExpectFallback("user, namespaces", Probe(false, true, SandboxHelper::Absent), false);
	ExpectFallback("user, no namespaces, setuid helper", Probe(false, false, SandboxHelper::SetuidRoot), false);
	ExpectFallback("user, no namespaces, helper not setuid", Probe(false, false, SandboxHelper::NotSetuid), true);
	ExpectFallback("user, no namespaces, no helper", Probe(false, false, SandboxHelper::Absent), true);

	/* macOS (obs-rust/obs-studio#1092): the helpers sandbox themselves when the
	 * framework has libcef_sandbox.dylib; without it a sandboxed helper exits. */
	Expect("macOS, sandbox library", MacProbe(true), SandboxMode::Seatbelt, "libcef_sandbox.dylib");
	Expect("macOS, no sandbox library", MacProbe(false), SandboxMode::Off, "libcef_sandbox.dylib is missing");
	ExpectFallback("macOS, sandbox library", MacProbe(true), false);
	ExpectFallback("macOS, no sandbox library", MacProbe(false), true);
	ExpectWarning("macOS, sandbox library", MacProbe(true), nullptr);
	ExpectWarning("macOS, no sandbox library", MacProbe(false), "SandboxFallback.Message.NoSandboxLibrary");

	/* The warning (obs-rust/obs-studio#2218): only for the fallback, with
	 * the message for the cause, and nothing to cancel. */
	ExpectWarning("unsupported platform", SandboxProbe{}, nullptr);
	ExpectWarning("root", Probe(true, true, SandboxHelper::Absent), "SandboxFallback.Message.Root");
	ExpectWarning("root, setuid helper", Probe(true, false, SandboxHelper::SetuidRoot),
		      "SandboxFallback.Message.Root");
	ExpectWarning("user, namespaces", Probe(false, true, SandboxHelper::Absent), nullptr);
	ExpectWarning("user, no namespaces, setuid helper", Probe(false, false, SandboxHelper::SetuidRoot), nullptr);
	ExpectWarning("user, no namespaces, helper not setuid", Probe(false, false, SandboxHelper::NotSetuid),
		      "SandboxFallback.Message.NoNamespaces");
	ExpectWarning("user, no namespaces, no helper", Probe(false, false, SandboxHelper::Absent),
		      "SandboxFallback.Message.NoNamespaces");

	if (failures) {
		std::fprintf(stderr, "%d case(s) failed\n", failures);
		return 1;
	}
	return 0;
}
