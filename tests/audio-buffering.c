/* Play a local WebAudio page through a real browser source with "Control
 * audio via OBS" and report, on stdout:
 *   PKT <arrival ns> <timestamp ns> <frames>  each packet the source outputs
 *   SRC_ONSET <ns>  a tone starting in the source's audio (its timestamps)
 *   MIX_ONSET <ns>  a tone starting in the mix (the output timeline)
 *   LOG <line>      the OBS log, including the audio buffering lines
 * All times are on os_gettime_ns()'s clock, which video frames use too.
 * Runs until stdin is closed or a line is read.
 *
 * STALLS ("alert:delay_s:length_s,...", optional) stalls CEF's audio
 * delivery as load does: delay_s into the given alert (counted from 1), the
 * thread that delivers the packets is blocked for length_s, and the packets
 * captured meanwhile then arrive at once with their capture timestamps. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <obs.h>
#include <obs-nix-platform.h>
#include <util/platform.h>
#include <X11/Xlib.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define SAMPLE_RATE 48000
#define LOUD 0.05f
/* A tone starts after at least this much silence. */
#define QUIET_NS 200000000ULL

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

#define MAX_STALLS 8
static int stall_alert[MAX_STALLS];
static uint64_t stall_delay[MAX_STALLS], stall_length[MAX_STALLS];
static int stalls, next_stall, alerts;
static uint64_t alert_start, alert_last_loud;
static obs_source_t *browser;

static void parse_stalls(const char *list)
{
	while (list && *list && stalls < MAX_STALLS) {
		double delay, length;
		if (sscanf(list, "%d:%lf:%lf", &stall_alert[stalls], &delay, &length) != 3)
			break;
		stall_delay[stalls] = (uint64_t)(delay * 1e9);
		stall_length[stalls++] = (uint64_t)(length * 1e9);
		list = strchr(list, ',');
		if (list)
			list++;
	}
}

/* The module calls this one: the executable is linked with -rdynamic, so it
 * comes before libobs's. Blocking here, before libobs, holds no libobs lock,
 * as a stall in CEF's delivery does not. */
void obs_source_output_audio(obs_source_t *source, const struct obs_source_audio *audio)
{
	static void (*output)(obs_source_t *, const struct obs_source_audio *);
	if (!output)
		output = (void (*)(obs_source_t *, const struct obs_source_audio *))dlsym(RTLD_NEXT,
											  "obs_source_output_audio");
	if (source == browser && audio->data[0]) {
		const float *samples = (const float *)audio->data[0];
		for (uint32_t i = 0; i < audio->frames; i++) {
			if (fabsf(samples[i]) < LOUD)
				continue;
			if (!alert_last_loud || audio->timestamp > alert_last_loud + 1000000000ULL) {
				alerts++;
				alert_start = audio->timestamp;
			}
			alert_last_loud = audio->timestamp;
			break;
		}
		uint64_t now = os_gettime_ns();
		if (next_stall < stalls && alerts == stall_alert[next_stall] &&
		    now >= alert_start + stall_delay[next_stall]) {
			blog(LOG_INFO, "TEST stalling audio delivery for %.1f s in alert %d",
			     (double)stall_length[next_stall] / 1e9, alerts);
			os_sleepto_ns(now + stall_length[next_stall]);
			next_stall++;
			/* The module saw this packet before the stall: drop it,
			 * whatever the build. The ones captured during the
			 * stall reach the module after it. */
			return;
		}
	}
	output(source, audio);
}

struct onset {
	const char *name;
	uint64_t last_loud;
};

static struct onset source_onset = {"SRC_ONSET", 0};
static struct onset mix_onset = {"MIX_ONSET", 0};

/* Called with the lock held. */
static void find_onset(struct onset *onset, const float *samples, uint32_t frames, uint64_t timestamp)
{
	if (!samples)
		return;
	for (uint32_t i = 0; i < frames; i++) {
		if (fabsf(samples[i]) < LOUD)
			continue;
		uint64_t when = timestamp + (uint64_t)i * 1000000000ULL / SAMPLE_RATE;
		if (!onset->last_loud || when > onset->last_loud + QUIET_NS)
			printf("%s %llu\n", onset->name, (unsigned long long)when);
		onset->last_loud = when;
	}
}

static void source_audio(void *param, obs_source_t *source, const struct audio_data *audio, bool muted)
{
	(void)param;
	(void)source;
	(void)muted;
	uint64_t arrival = os_gettime_ns();
	pthread_mutex_lock(&lock);
	printf("PKT %llu %llu %u\n", (unsigned long long)arrival, (unsigned long long)audio->timestamp, audio->frames);
	find_onset(&source_onset, (const float *)audio->data[0], audio->frames, audio->timestamp);
	fflush(stdout);
	pthread_mutex_unlock(&lock);
}

static void mix_audio(void *param, size_t mix_idx, struct audio_data *audio)
{
	(void)param;
	(void)mix_idx;
	pthread_mutex_lock(&lock);
	find_onset(&mix_onset, (const float *)audio->data[0], audio->frames, audio->timestamp);
	fflush(stdout);
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
	if (argc < 6) {
		fprintf(stderr, "usage: %s MODULE DATA LIBOBS_DATA CONFIG PAGE\n", argv[0]);
		return 2;
	}
	base_set_log_handler(forward_log, NULL);
	parse_stalls(getenv("STALLS"));

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

	struct obs_audio_info audio = {.samples_per_sec = SAMPLE_RATE, .speakers = SPEAKERS_STEREO};
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

	obs_data_t *settings = obs_data_create();
	obs_data_set_bool(settings, "is_local_file", true);
	obs_data_set_string(settings, "local_file", argv[5]);
	obs_data_set_bool(settings, "reroute_audio", true);
	obs_data_set_int(settings, "width", 160);
	obs_data_set_int(settings, "height", 90);
	obs_source_t *source = obs_source_create("browser_source", "Alert page", settings, NULL);
	browser = source;
	obs_data_release(settings);
	if (!source)
		return 8;
	obs_source_add_audio_capture_callback(source, source_audio, NULL);
	obs_add_raw_audio_callback(0, NULL, mix_audio, NULL);
	obs_set_output_source(0, source);

	struct pollfd input = {.fd = STDIN_FILENO, .events = POLLIN};
	while (poll(&input, 1, 100) == 0)
		;

	obs_set_output_source(0, NULL);
	obs_remove_raw_audio_callback(0, mix_audio, NULL);
	obs_source_remove_audio_capture_callback(source, source_audio, NULL);
	obs_source_release(source);
	obs_shutdown();
	XCloseDisplay(display);
	puts("DONE");
	return 0;
}
