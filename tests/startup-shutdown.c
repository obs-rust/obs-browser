/* Exercise the real module's startup/unload boundary, including a source
 * released before CEF is ready. Run each mode in a fresh process/profile. */
#include <obs.h>
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
	if (argc != 5) {
		fprintf(stderr, "usage: %s MODULE DATA CONFIG initialize|source\n", argv[0]);
		return 2;
	}
	obs_set_cmdline_args(argc, (const char *const *)argv);
	if (!obs_startup("en-US", argv[3], NULL))
		return 3;
	obs_module_t *module = NULL;
	if (obs_open_module(&module, argv[1], argv[2]) != MODULE_SUCCESS || !obs_init_module(module))
		return 4;
	if (strcmp(argv[4], "source") == 0) {
		obs_source_t *source = obs_source_create("browser_source", "Early browser", NULL, NULL);
		if (!source)
			return 5;
		obs_source_release(source);
	} else {
		void (*initialize)(void) = dlsym(obs_get_module_lib(module), "obs_browser_initialize");
		if (!initialize)
			return 6;
		initialize();
	}
	obs_shutdown();
	puts("browser startup and immediate shutdown completed");
	return 0;
}
