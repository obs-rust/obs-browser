#include <cstdio>

#include "browser-sandbox.hpp"

/* Prints what ProbeSandbox finds here and what DecideSandbox makes of it.
 * Argument: the directory to look for chrome-sandbox in. */
int main(int argc, char **argv)
{
	if (argc != 2) {
		std::fputs("usage: sandbox-probe <cef dir>\n", stderr);
		return 2;
	}

	static const char *helpers[] = {"absent", "setuid-root", "not-setuid"};
	static const char *modes[] = {"off", "namespaces", "setuid-helper"};

	SandboxProbe probe = ProbeSandbox(argv[1]);
	SandboxDecision decision = DecideSandbox(probe);
	std::printf("supported=%d root=%d user_namespaces=%d helper=%s -> %s (%s)\n", probe.supported, probe.root,
		    probe.user_namespaces, helpers[static_cast<int>(probe.helper)],
		    modes[static_cast<int>(decision.mode)], decision.reason);
	return 0;
}
