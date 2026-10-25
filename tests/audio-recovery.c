/* Play a local WebAudio page through real browser sources with "Control
 * audio via OBS" and report how many audio packets each source delivers.
 * Prints "COUNTS <n0> <n1> ..." (cumulative) every 100 ms and forwards the
 * OBS log (including the page's console.error lines) as "LOG <line>".
 * Runs until stdin is closed or a line is read. */
#include <obs.h>
#include <obs-nix-platform.h>
#include <util/platform.h>
#include <X11/Xlib.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define MAX_SOURCES 4

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static long packets[MAX_SOURCES];

static void count_audio(void *param, obs_source_t *source, const struct audio_data *audio, bool muted)
{
	(void)source;
	(void)audio;
	(void)muted;
	pthread_mutex_lock(&lock);
	packets[(size_t)param]++;
	pthread_mutex_unlock(&lock);
}

static void forward_log(int level, const char *format, va_list args, void *param)
{
	(void)level;
	(void)param;
	char line[4096];
	vsnprintf(line, sizeof(line), format, args);
	pthread_mutex_lock(&lock);
	printf("LOG %s\n", line);
	fflush(stdout);
	pthread_mutex_unlock(&lock);
}

int main(int argc, char **argv)
{
	if (argc < 7) {
		fprintf(stderr, "usage: %s MODULE DATA LIBOBS_DATA CONFIG PAGE SOURCES\n", argv[0]);
		return 2;
	}
	int count = atoi(argv[6]);
	if (count < 1 || count > MAX_SOURCES)
		return 2;
	base_set_log_handler(forward_log, NULL);

	Display *display = XOpenDisplay(NULL);
	if (!display)
		return 3;
	obs_set_nix_platform(OBS_NIX_PLATFORM_X11_EGL);
	obs_set_nix_platform_display(display);
	if (!obs_startup("en-US", argv[4], NULL))
		return 4;
	/* A staged tree outside the install prefix needs its libobs effects. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
	obs_add_data_path(argv[3]);
#pragma GCC diagnostic pop

	struct obs_audio_info audio = {.samples_per_sec = 48000, .speakers = SPEAKERS_STEREO};
	if (!obs_reset_audio(&audio))
		return 5;
	struct obs_video_info video = {
		.graphics_module = "libobs-opengl",
		.fps_num = 30,
		.fps_den = 1,
		.base_width = 320,
		.base_height = 180,
		.output_width = 320,
		.output_height = 180,
		.output_format = VIDEO_FORMAT_NV12,
		.gpu_conversion = true,
		.colorspace = VIDEO_CS_709,
		.range = VIDEO_RANGE_PARTIAL,
		.scale_type = OBS_SCALE_BICUBIC,
	};
	if (obs_reset_video(&video) != OBS_VIDEO_SUCCESS)
		return 6;

	obs_module_t *module = NULL;
	if (obs_open_module(&module, argv[1], argv[2]) != MODULE_SUCCESS || !obs_init_module(module))
		return 7;
	obs_post_load_modules();

	obs_source_t *sources[MAX_SOURCES] = {0};
	for (int i = 0; i < count; i++) {
		char name[32];
		snprintf(name, sizeof(name), "Audio page %d", i);
		obs_data_t *settings = obs_data_create();
		obs_data_set_bool(settings, "is_local_file", true);
		obs_data_set_string(settings, "local_file", argv[5]);
		obs_data_set_bool(settings, "reroute_audio", true);
		obs_data_set_int(settings, "width", 160);
		obs_data_set_int(settings, "height", 90);
		sources[i] = obs_source_create("browser_source", name, settings, NULL);
		obs_data_release(settings);
		if (!sources[i])
			return 8;
		obs_source_add_audio_capture_callback(sources[i], count_audio, (void *)(size_t)i);
		obs_set_output_source((uint32_t)i, sources[i]);
	}

	struct pollfd input = {.fd = STDIN_FILENO, .events = POLLIN};
	while (poll(&input, 1, 100) == 0) {
		pthread_mutex_lock(&lock);
		printf("COUNTS");
		for (int i = 0; i < count; i++)
			printf(" %ld", packets[i]);
		printf("\n");
		fflush(stdout);
		pthread_mutex_unlock(&lock);
	}

	for (int i = 0; i < count; i++) {
		obs_set_output_source((uint32_t)i, NULL);
		obs_source_remove_audio_capture_callback(sources[i], count_audio, (void *)(size_t)i);
		obs_source_release(sources[i]);
	}
	obs_shutdown();
	XCloseDisplay(display);
	puts("DONE");
	return 0;
}
