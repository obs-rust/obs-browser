#include <cstdio>
#include <functional>

bool QueueCEFTask(std::function<void()> task);

int main()
{
	if (QueueCEFTask([] {})) {
		std::fputs("task accepted before CEF initialization\n", stderr);
		return 1;
	}
	std::puts("task rejected safely before CEF initialization");
	return 0;
}
