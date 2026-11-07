/* Real module lifecycle with the frontend's final process-shutdown handshake.
 * The optional hooks let the same test reproduce the original unbounded wait. */
#include <obs.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>

static void unsafe_destructor(void)
{
	fputs("TEST: static destructors ran with stalled CEF\n", stderr);
	abort();
}

int main(int argc, char **argv)
{
	if (argc < 5)
		return 2;
	obs_set_cmdline_args(argc, (const char *const *)argv);
	if (!obs_startup("en-US", argv[3], NULL))
		return 3;
	obs_module_t *module = NULL;
	if (obs_open_module(&module, argv[1], argv[2]) != MODULE_SUCCESS || !obs_init_module(module))
		return 4;
	void *library = obs_get_module_lib(module);
	void (*prepare)(void) = dlsym(library, "obs_browser_prepare_shutdown");
	void (*finish)(void) = dlsym(library, "obs_browser_finish_shutdown");
	if (prepare && finish)
		prepare();
	void (*initialize)(void) = dlsym(library, "obs_browser_initialize");
	if (!initialize)
		return 6;
	if (getenv("TEST_EXPECT_ABANDON"))
		atexit(unsafe_destructor);
	initialize();
	puts("holding browser startup until stdin is released");
	fflush(stdout);
	getchar();
	obs_shutdown();
	puts("browser startup and immediate shutdown completed");
	fflush(stdout);
	if (finish)
		finish();
	return 0;
}
