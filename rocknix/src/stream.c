// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
//
// SDL2 streaming client for embedded Linux (ROCKNIX / Wayland).
// Session wiring follows switch/src/host.cpp, audio follows switch/src/io.cpp,
// controller mapping follows gui/src/controllermanager.cpp.
//
// Resolution/codec are fixed at session negotiation by the Remote Play
// protocol, so the in-stream toggle chords (R1+L3 resolution, L1+R3 codec)
// persist the config and restart the session in place — the window stays up
// and the stream returns on the new profile after a few seconds.

#include "rocknix.h"

#include <chiaki/base64.h>
#include <chiaki/discovery.h>
#include <chiaki/ffmpegdecoder.h>
#include <chiaki/opusdecoder.h>

#include <SDL2/SDL.h>

#include <libavutil/pixfmt.h>

#include <argp.h>
#include <netdb.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char doc[] = "Stream from the registered console (pair with `chiaki regist` first).";

#define ARG_KEY_CONFIG 'c'
#define ARG_KEY_DECODER 'd'
#define ARG_KEY_RESOLUTION 'r'
#define ARG_KEY_FPS 'f'
#define ARG_KEY_FULLSCREEN 'F'
#define ARG_KEY_LOGIN_PIN 'l'
#define ARG_KEY_NO_WAKEUP 'W'

static struct argp_option options[] = {
	{ "config", ARG_KEY_CONFIG, "Path", 0, "Config file", 0 },
	{ "decoder", ARG_KEY_DECODER, "Name", 0, "Video decoder: software (default), an ffmpeg hwaccel (e.g. vaapi) or decoder name (e.g. h264_v4l2m2m)", 0 },
	{ "resolution", ARG_KEY_RESOLUTION, "Res", 0, "360p, 540p, 720p or 1080p (overrides config)", 0 },
	{ "fps", ARG_KEY_FPS, "FPS", 0, "30 or 60 (overrides config)", 0 },
	{ "fullscreen", ARG_KEY_FULLSCREEN, NULL, 0, "Fullscreen-desktop window (launcher normally handles this via sway)", 0 },
	{ "login-pin", ARG_KEY_LOGIN_PIN, "PIN", 0, "Console login passcode, if one is set", 0 },
	{ "no-wakeup", ARG_KEY_NO_WAKEUP, NULL, 0, "Do not try to wake the console from rest mode first", 0 },
	{ 0 }
};

typedef struct arguments
{
	const char *config_path;
	const char *decoder;
	const char *resolution;
	int fps;
	bool fullscreen;
	const char *login_pin;
	bool no_wakeup;
} Arguments;

static int parse_opt(int key, char *arg, struct argp_state *state)
{
	Arguments *a = state->input;
	switch(key)
	{
		case ARG_KEY_CONFIG: a->config_path = arg; break;
		case ARG_KEY_DECODER: a->decoder = arg; break;
		case ARG_KEY_RESOLUTION: a->resolution = arg; break;
		case ARG_KEY_FPS: a->fps = atoi(arg); break;
		case ARG_KEY_FULLSCREEN: a->fullscreen = true; break;
		case ARG_KEY_LOGIN_PIN: a->login_pin = arg; break;
		case ARG_KEY_NO_WAKEUP: a->no_wakeup = true; break;
		case ARGP_KEY_ARG: argp_usage(state); break;
		default: return ARGP_ERR_UNKNOWN;
	}
	return 0;
}

static struct argp argp = { options, parse_opt, 0, doc, 0, 0, 0 };

// ---- signals ----------------------------------------------------------------

// SIGINT/SIGTERM must end the session cleanly (chiaki_session_stop says
// goodbye to the console) — a hard kill leaves the console thinking Remote
// Play is still in use and the next connect fails with 0x80108b10.
// SIGUSR1/SIGUSR2 mirror the resolution/codec toggle chords for external
// triggers (ssh testing, future etk integration).
static volatile sig_atomic_t g_signal_quit = 0;
static volatile sig_atomic_t g_signal_toggle_res = 0;
static volatile sig_atomic_t g_signal_toggle_codec = 0;

static void signal_handler(int sig)
{
	switch(sig)
	{
		case SIGUSR1: g_signal_toggle_res = 1; break;
		case SIGUSR2: g_signal_toggle_codec = 1; break;
		default: g_signal_quit = 1; break;
	}
}

// ---- custom SDL events -----------------------------------------------------

enum
{
	RKNX_EVENT_FRAME = 0,
	RKNX_EVENT_QUIT = 1,
	RKNX_EVENT_RUMBLE = 2,
};

typedef struct stream_ctx
{
	ChiakiLog *log;
	ChiakiSession session;
	ChiakiOpusDecoder opus_decoder;
	ChiakiFfmpegDecoder video_decoder;
	Uint32 sdl_event_base;
	SDL_AudioDeviceID audio_device;
	double audio_boost;
	const char *login_pin;
	atomic_bool session_quit;
	atomic_bool frame_pending;
	atomic_int quit_reason;
} StreamCtx;

static void frame_available_cb(ChiakiFfmpegDecoder *decoder, void *user)
{
	StreamCtx *ctx = user;
	// coalesce: at most one FRAME event in flight — pull_frame always drains
	// to the latest frame, so queueing more would only back up the loop
	if(atomic_exchange(&ctx->frame_pending, true))
		return;
	SDL_Event ev = { 0 };
	ev.type = ctx->sdl_event_base;
	ev.user.code = RKNX_EVENT_FRAME;
	SDL_PushEvent(&ev);
}

static void session_event_cb(ChiakiEvent *event, void *user)
{
	StreamCtx *ctx = user;
	switch(event->type)
	{
		case CHIAKI_EVENT_CONNECTED:
			CHIAKI_LOGI(ctx->log, "Session connected");
			break;
		case CHIAKI_EVENT_LOGIN_PIN_REQUEST:
			if(ctx->login_pin)
			{
				CHIAKI_LOGI(ctx->log, "Console requests login PIN, sending");
				chiaki_session_set_login_pin(&ctx->session, (const uint8_t *)ctx->login_pin, strlen(ctx->login_pin));
			}
			else
			{
				CHIAKI_LOGE(ctx->log, "Console requests a login PIN, restart with --login-pin");
				SDL_Event ev = { 0 };
				ev.type = ctx->sdl_event_base;
				ev.user.code = RKNX_EVENT_QUIT;
				SDL_PushEvent(&ev);
			}
			break;
		case CHIAKI_EVENT_RUMBLE:
		{
			SDL_Event ev = { 0 };
			ev.type = ctx->sdl_event_base;
			ev.user.code = RKNX_EVENT_RUMBLE;
			ev.user.data1 = (void *)(uintptr_t)event->rumble.left;
			ev.user.data2 = (void *)(uintptr_t)event->rumble.right;
			SDL_PushEvent(&ev);
			break;
		}
		case CHIAKI_EVENT_QUIT:
		{
			CHIAKI_LOGI(ctx->log, "Session quit: %s (%s)",
				chiaki_quit_reason_string(event->quit.reason),
				event->quit.reason_str ? event->quit.reason_str : "-");
			atomic_store(&ctx->quit_reason, (int)event->quit.reason);
			atomic_store(&ctx->session_quit, true);
			SDL_Event ev = { 0 };
			ev.type = ctx->sdl_event_base;
			ev.user.code = RKNX_EVENT_QUIT;
			SDL_PushEvent(&ev);
			break;
		}
		default:
			break;
	}
}

// ---- audio (SDL queued audio, cf. switch/src/io.cpp) -----------------------

static void audio_settings_cb(uint32_t channels, uint32_t rate, void *user)
{
	StreamCtx *ctx = user;
	if(ctx->audio_device > 0)
		return;
	SDL_AudioSpec want;
	SDL_memset(&want, 0, sizeof(want));
	want.freq = (int)rate;
	want.format = AUDIO_S16SYS;
	want.channels = (Uint8)channels;
	want.samples = 1024;
	want.callback = NULL;
	ctx->audio_device = SDL_OpenAudioDevice(NULL, 0, &want, NULL, 0);
	if(ctx->audio_device <= 0)
		CHIAKI_LOGE(ctx->log, "SDL_OpenAudioDevice failed: %s", SDL_GetError());
	else
		SDL_PauseAudioDevice(ctx->audio_device, 0);
}

static void audio_frame_cb(int16_t *buf, size_t samples_count, void *user)
{
	StreamCtx *ctx = user;
	if(ctx->audio_device <= 0)
		return;

	if(ctx->audio_boost != 1.0)
	{
		for(size_t i = 0; i < samples_count * 2; i++)
		{
			int sample = (int)(buf[i] * ctx->audio_boost);
			if(sample > INT16_MAX)
				sample = INT16_MAX;
			else if(sample < INT16_MIN)
				sample = INT16_MIN;
			buf[i] = (int16_t)sample;
		}
	}

	// keep latency bounded: drop backlog beyond ~80ms (values from the switch port)
	if(SDL_GetQueuedAudioSize(ctx->audio_device) > 16000)
		SDL_ClearQueuedAudio(ctx->audio_device);

	SDL_QueueAudio(ctx->audio_device, buf, (Uint32)(sizeof(int16_t) * samples_count * 2));
}

// ---- discovery / wakeup ----------------------------------------------------

static atomic_int g_discovered_state; // mirrors ChiakiDiscoveryHostState

static void discovery_cb(ChiakiDiscoveryHost *host, void *user)
{
	(void)user;
	atomic_store(&g_discovered_state, (int)host->state);
}

// Sends a unicast SRCH and waits briefly; returns last seen state (UNKNOWN if silent)
static ChiakiDiscoveryHostState probe_console(ChiakiLog *log, const char *host, int wait_ms)
{
	atomic_store(&g_discovered_state, (int)CHIAKI_DISCOVERY_HOST_STATE_UNKNOWN);

	ChiakiDiscovery discovery;
	if(chiaki_discovery_init(&discovery, log, AF_INET) != CHIAKI_ERR_SUCCESS)
		return CHIAKI_DISCOVERY_HOST_STATE_UNKNOWN;
	ChiakiDiscoveryThread thread;
	if(chiaki_discovery_thread_start(&thread, &discovery, discovery_cb, NULL) != CHIAKI_ERR_SUCCESS)
	{
		chiaki_discovery_fini(&discovery);
		return CHIAKI_DISCOVERY_HOST_STATE_UNKNOWN;
	}

	struct addrinfo *addrinfos;
	if(getaddrinfo(host, NULL, NULL, &addrinfos) == 0)
	{
		for(struct addrinfo *ai = addrinfos; ai; ai = ai->ai_next)
		{
			if(ai->ai_family != AF_INET)
				continue;
			ChiakiDiscoveryPacket packet;
			memset(&packet, 0, sizeof(packet));
			packet.cmd = CHIAKI_DISCOVERY_CMD_SRCH;
			struct sockaddr_in addr = *(struct sockaddr_in *)ai->ai_addr;
			packet.protocol_version = CHIAKI_DISCOVERY_PROTOCOL_VERSION_PS4;
			addr.sin_port = htons(CHIAKI_DISCOVERY_PORT_PS4);
			chiaki_discovery_send(&discovery, &packet, (struct sockaddr *)&addr, sizeof(addr));
			packet.protocol_version = CHIAKI_DISCOVERY_PROTOCOL_VERSION_PS5;
			addr.sin_port = htons(CHIAKI_DISCOVERY_PORT_PS5);
			chiaki_discovery_send(&discovery, &packet, (struct sockaddr *)&addr, sizeof(addr));
			break;
		}
		freeaddrinfo(addrinfos);
	}

	for(int waited = 0; waited < wait_ms; waited += 100)
	{
		if(atomic_load(&g_discovered_state) != (int)CHIAKI_DISCOVERY_HOST_STATE_UNKNOWN)
			break;
		usleep(100 * 1000);
	}

	chiaki_discovery_thread_stop(&thread);
	chiaki_discovery_fini(&discovery);
	return (ChiakiDiscoveryHostState)atomic_load(&g_discovered_state);
}

static void ensure_awake(ChiakiLog *log, const RknxConfig *cfg)
{
	ChiakiDiscoveryHostState state = probe_console(log, cfg->host_addr, 2000);
	if(state == CHIAKI_DISCOVERY_HOST_STATE_READY)
	{
		CHIAKI_LOGI(log, "Console is awake");
		return;
	}
	if(state == CHIAKI_DISCOVERY_HOST_STATE_UNKNOWN)
	{
		CHIAKI_LOGW(log, "Console did not answer discovery, attempting session anyway");
		return;
	}

	CHIAKI_LOGI(log, "Console is in rest mode, sending wakeup");
	// the regist key bytes are an ASCII hex string, interpreted as the wakeup credential
	uint64_t credential = (uint64_t)strtoull((const char *)cfg->rp_regist_key, NULL, 16);
	chiaki_discovery_wakeup(log, NULL, cfg->host_addr, credential, chiaki_target_is_ps5((ChiakiTarget)cfg->target));

	for(int waited = 0; waited < 40000; waited += 2000)
	{
		state = probe_console(log, cfg->host_addr, 2000);
		if(state == CHIAKI_DISCOVERY_HOST_STATE_READY)
		{
			CHIAKI_LOGI(log, "Console is awake");
			// give services a moment to come up after standby
			usleep(1000 * 1000);
			return;
		}
	}
	CHIAKI_LOGW(log, "Console did not reach READY within 40s, attempting session anyway");
}

// ---- controller ------------------------------------------------------------

#define GUIDE_QUIT_HOLD_MS 1500
#define GUIDE_TAP_MS 400
#define PS_PULSE_MS 120
#define CHORD_HOLD_MS 600

typedef enum pad_action
{
	PAD_ACTION_NONE = 0,
	PAD_ACTION_QUIT,
	PAD_ACTION_TOGGLE_RES,
	PAD_ACTION_TOGGLE_CODEC,
} PadAction;

typedef struct pad_state
{
	SDL_GameController *controller;
	Uint32 guide_down_at; // 0 = not held
	Uint32 combo_down_at; // Select+Start held together; 0 = not held
	Uint32 ps_pulse_until;
	Uint32 r1l3_down_at;  // R1+L3 resolution chord
	Uint32 l1r3_down_at;  // L1+R3 codec chord
	bool r1l3_armed;      // require full release between chord fires
	bool l1r3_armed;
} PadState;

static void pad_open_first(PadState *pad, ChiakiLog *log)
{
	if(pad->controller)
		return;
	for(int i = 0; i < SDL_NumJoysticks(); i++)
	{
		if(!SDL_IsGameController(i))
			continue;
		pad->controller = SDL_GameControllerOpen(i);
		if(pad->controller)
		{
			CHIAKI_LOGI(log, "Using controller: %s", SDL_GameControllerName(pad->controller));
			pad->r1l3_armed = true;
			pad->l1r3_armed = true;
			return;
		}
	}
}

static void pad_rumble_ack(PadState *pad)
{
	if(pad->controller)
		SDL_GameControllerRumble(pad->controller, 0xFFFF, 0xFFFF, 150);
}

// Chord helper: while BOTH buttons are held their bits are stripped from the
// state (the console never sees the chord), and after CHORD_HOLD_MS the
// action fires once — re-armed only when the chord is fully released.
static bool chord_check(ChiakiControllerState *state, Uint32 now,
		bool a_down, bool b_down, uint32_t suppress_mask,
		Uint32 *down_at, bool *armed)
{
	if(a_down && b_down)
	{
		state->buttons &= ~suppress_mask;
		if(!*down_at)
			*down_at = now ? now : 1;
		else if(*armed && now - *down_at >= CHORD_HOLD_MS)
		{
			*armed = false;
			return true;
		}
	}
	else
	{
		*down_at = 0;
		if(!a_down && !b_down)
			*armed = true;
	}
	return false;
}

static PadAction pad_read(PadState *pad, ChiakiControllerState *state)
{
	chiaki_controller_state_set_idle(state);
	SDL_GameController *c = pad->controller;
	if(!c)
		return PAD_ACTION_NONE;

	state->buttons |= SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_A) ? CHIAKI_CONTROLLER_BUTTON_CROSS : 0;
	state->buttons |= SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_B) ? CHIAKI_CONTROLLER_BUTTON_MOON : 0;
	state->buttons |= SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_X) ? CHIAKI_CONTROLLER_BUTTON_BOX : 0;
	state->buttons |= SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_Y) ? CHIAKI_CONTROLLER_BUTTON_PYRAMID : 0;
	state->buttons |= SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_LEFT) ? CHIAKI_CONTROLLER_BUTTON_DPAD_LEFT : 0;
	state->buttons |= SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_RIGHT) ? CHIAKI_CONTROLLER_BUTTON_DPAD_RIGHT : 0;
	state->buttons |= SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_UP) ? CHIAKI_CONTROLLER_BUTTON_DPAD_UP : 0;
	state->buttons |= SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_DOWN) ? CHIAKI_CONTROLLER_BUTTON_DPAD_DOWN : 0;
	state->buttons |= SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_LEFTSHOULDER) ? CHIAKI_CONTROLLER_BUTTON_L1 : 0;
	state->buttons |= SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) ? CHIAKI_CONTROLLER_BUTTON_R1 : 0;
	state->buttons |= SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_LEFTSTICK) ? CHIAKI_CONTROLLER_BUTTON_L3 : 0;
	state->buttons |= SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_RIGHTSTICK) ? CHIAKI_CONTROLLER_BUTTON_R3 : 0;
	state->buttons |= SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_START) ? CHIAKI_CONTROLLER_BUTTON_OPTIONS : 0;
	// on a handheld the touchpad click is worth more than Share
	state->buttons |= SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_BACK) ? CHIAKI_CONTROLLER_BUTTON_TOUCHPAD : 0;
	state->l2_state = (uint8_t)(SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_TRIGGERLEFT) >> 7);
	state->r2_state = (uint8_t)(SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) >> 7);
	state->left_x = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTX);
	state->left_y = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTY);
	state->right_x = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_RIGHTX);
	state->right_y = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_RIGHTY);

	// Guide: tap = PS button, hold = quit the stream. The PS bit is only sent
	// on release so a quit-hold never opens the console's PS menu.
	Uint32 now = SDL_GetTicks();
	bool guide = SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_GUIDE);
	if(guide)
	{
		if(!pad->guide_down_at)
			pad->guide_down_at = now ? now : 1;
		else if(now - pad->guide_down_at >= GUIDE_QUIT_HOLD_MS)
			return PAD_ACTION_QUIT;
	}
	else if(pad->guide_down_at)
	{
		if(now - pad->guide_down_at < GUIDE_TAP_MS)
			pad->ps_pulse_until = now + PS_PULSE_MS;
		pad->guide_down_at = 0;
	}
	if(pad->ps_pulse_until && now < pad->ps_pulse_until)
		state->buttons |= CHIAKI_CONTROLLER_BUTTON_PS;
	else
		pad->ps_pulse_until = 0;

	// Fallback quit: hold Select+Start. Needed because ROCKNIX's InputPlumber
	// virtual pad may swallow Guide for system hotkeys before SDL sees it.
	// While the combo is held, its individual bits are suppressed so a quit
	// never pops the console's touchpad/options actions first.
	if(SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_BACK)
		&& SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_START))
	{
		state->buttons &= ~(uint32_t)(CHIAKI_CONTROLLER_BUTTON_TOUCHPAD | CHIAKI_CONTROLLER_BUTTON_OPTIONS);
		if(!pad->combo_down_at)
			pad->combo_down_at = now ? now : 1;
		else if(now - pad->combo_down_at >= GUIDE_QUIT_HOLD_MS)
			return PAD_ACTION_QUIT;
	}
	else
		pad->combo_down_at = 0;

	// In-stream setting chords (etk's input_d stands down while a stream is
	// active, so these are exclusively ours here):
	//   R1+L3 = toggle resolution, L1+R3 = toggle codec.
	if(chord_check(state, now,
			SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER),
			SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_LEFTSTICK),
			CHIAKI_CONTROLLER_BUTTON_R1 | CHIAKI_CONTROLLER_BUTTON_L3,
			&pad->r1l3_down_at, &pad->r1l3_armed))
		return PAD_ACTION_TOGGLE_RES;
	if(chord_check(state, now,
			SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_LEFTSHOULDER),
			SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_RIGHTSTICK),
			CHIAKI_CONTROLLER_BUTTON_L1 | CHIAKI_CONTROLLER_BUTTON_R3,
			&pad->l1r3_down_at, &pad->l1r3_armed))
		return PAD_ACTION_TOGGLE_CODEC;

	return PAD_ACTION_NONE;
}

// ---- video -----------------------------------------------------------------

typedef struct video_out
{
	SDL_Renderer *renderer;
	SDL_Texture *texture;
	int tex_w, tex_h;
	Uint32 tex_format;
} VideoOut;

static bool video_present(VideoOut *vid, ChiakiFfmpegDecoder *decoder, ChiakiLog *log)
{
	AVFrame *frame = chiaki_ffmpeg_decoder_pull_frame(decoder);
	if(!frame)
		return false;

	Uint32 want_format;
	switch(frame->format)
	{
		case AV_PIX_FMT_YUV420P:
		case AV_PIX_FMT_YUVJ420P:
			want_format = SDL_PIXELFORMAT_IYUV;
			break;
		case AV_PIX_FMT_NV12:
			want_format = SDL_PIXELFORMAT_NV12;
			break;
		default:
		{
			static bool warned = false;
			if(!warned)
			{
				CHIAKI_LOGE(log, "Unsupported frame pixel format %d, dropping frames", frame->format);
				warned = true;
			}
			av_frame_free(&frame);
			return false;
		}
	}

	if(!vid->texture || vid->tex_w != frame->width || vid->tex_h != frame->height || vid->tex_format != want_format)
	{
		if(vid->texture)
			SDL_DestroyTexture(vid->texture);
		vid->texture = SDL_CreateTexture(vid->renderer, want_format, SDL_TEXTUREACCESS_STREAMING, frame->width, frame->height);
		if(!vid->texture)
		{
			CHIAKI_LOGE(log, "SDL_CreateTexture failed: %s", SDL_GetError());
			av_frame_free(&frame);
			return false;
		}
		vid->tex_w = frame->width;
		vid->tex_h = frame->height;
		vid->tex_format = want_format;
		SDL_RenderSetLogicalSize(vid->renderer, frame->width, frame->height);
		CHIAKI_LOGI(log, "Video stream %dx%d (%s)", frame->width, frame->height,
			want_format == SDL_PIXELFORMAT_NV12 ? "NV12" : "IYUV");
	}

	if(want_format == SDL_PIXELFORMAT_IYUV)
		SDL_UpdateYUVTexture(vid->texture, NULL,
			frame->data[0], frame->linesize[0],
			frame->data[1], frame->linesize[1],
			frame->data[2], frame->linesize[2]);
	else
		SDL_UpdateNVTexture(vid->texture, NULL,
			frame->data[0], frame->linesize[0],
			frame->data[1], frame->linesize[1]);
	av_frame_free(&frame);

	SDL_RenderClear(vid->renderer);
	SDL_RenderCopy(vid->renderer, vid->texture, NULL, NULL);
	SDL_RenderPresent(vid->renderer);
	return true;
}

// ---- main command ----------------------------------------------------------

static ChiakiVideoResolutionPreset parse_resolution(const char *s)
{
	if(!strcmp(s, "360p")) return CHIAKI_VIDEO_RESOLUTION_PRESET_360p;
	if(!strcmp(s, "540p")) return CHIAKI_VIDEO_RESOLUTION_PRESET_540p;
	if(!strcmp(s, "1080p")) return CHIAKI_VIDEO_RESOLUTION_PRESET_1080p;
	return CHIAKI_VIDEO_RESOLUTION_PRESET_720p;
}

// one full session: connect, stream until quit/toggle, tear down.
// Returns the PadAction that ended it (NONE = session ended on its own).
static PadAction run_session(StreamCtx *ctx, RknxConfig *cfg, VideoOut *vid,
		PadState *pad, ChiakiLog *log, bool *session_failed)
{
	*session_failed = false;
	bool is_ps5 = chiaki_target_is_ps5((ChiakiTarget)cfg->target);

	ChiakiConnectVideoProfile profile;
	chiaki_connect_video_profile_preset(&profile, parse_resolution(cfg->resolution),
		cfg->fps == 30 ? CHIAKI_VIDEO_FPS_PRESET_30 : CHIAKI_VIDEO_FPS_PRESET_60);
	if(!strcmp(cfg->codec, "h265"))
	{
		if(is_ps5)
			profile.codec = CHIAKI_CODEC_H265;
		else
			CHIAKI_LOGW(log, "h265 requested but console is a PS4, keeping h264");
	}
	CHIAKI_LOGI(log, "Session profile: %s@%d %s", cfg->resolution, cfg->fps,
		profile.codec == CHIAKI_CODEC_H265 ? "h265" : "h264");

	const char *decoder_name = cfg->decoder;
	if(!decoder_name[0] || !strcmp(decoder_name, "software") || !strcmp(decoder_name, "auto"))
		decoder_name = NULL;

	atomic_store(&ctx->session_quit, false);
	atomic_store(&ctx->frame_pending, false);
	atomic_store(&ctx->quit_reason, (int)CHIAKI_QUIT_REASON_NONE);

	// drain events left over from a previous session — its teardown-time
	// QUIT event would otherwise kill this session on the first tick
	SDL_PumpEvents();
	SDL_FlushEvent(ctx->sdl_event_base);

	ChiakiErrorCode err = chiaki_ffmpeg_decoder_init(&ctx->video_decoder, log,
		profile.codec, decoder_name, frame_available_cb, ctx);
	if(err != CHIAKI_ERR_SUCCESS)
	{
		CHIAKI_LOGE(log, "Failed to init video decoder (decoder=%s)", decoder_name ? decoder_name : "software");
		*session_failed = true;
		return PAD_ACTION_NONE;
	}

	chiaki_opus_decoder_init(&ctx->opus_decoder, log);
	ChiakiConnectInfo connect_info = { 0 };
	connect_info.host = cfg->host_addr;
	connect_info.ps5 = is_ps5;
	connect_info.video_profile = profile;
	connect_info.video_profile_auto_downgrade = true;
	connect_info.enable_keyboard = false;
	memcpy(connect_info.regist_key, cfg->rp_regist_key, sizeof(connect_info.regist_key));
	memcpy(connect_info.morning, cfg->rp_key, sizeof(connect_info.morning));

	err = chiaki_session_init(&ctx->session, &connect_info, log);
	if(err != CHIAKI_ERR_SUCCESS)
	{
		CHIAKI_LOGE(log, "chiaki_session_init failed: %s", chiaki_error_string(err));
		chiaki_opus_decoder_fini(&ctx->opus_decoder);
		chiaki_ffmpeg_decoder_fini(&ctx->video_decoder);
		*session_failed = true;
		return PAD_ACTION_NONE;
	}
	ChiakiAudioSink audio_sink;
	chiaki_opus_decoder_set_cb(&ctx->opus_decoder, audio_settings_cb, audio_frame_cb, ctx);
	chiaki_opus_decoder_get_sink(&ctx->opus_decoder, &audio_sink);
	chiaki_session_set_audio_sink(&ctx->session, &audio_sink);
	chiaki_session_set_video_sample_cb(&ctx->session, chiaki_ffmpeg_decoder_video_sample_cb, &ctx->video_decoder);
	chiaki_session_set_event_cb(&ctx->session, session_event_cb, ctx);

	err = chiaki_session_start(&ctx->session);
	if(err != CHIAKI_ERR_SUCCESS)
	{
		CHIAKI_LOGE(log, "chiaki_session_start failed: %s", chiaki_error_string(err));
		chiaki_session_fini(&ctx->session);
		chiaki_opus_decoder_fini(&ctx->opus_decoder);
		chiaki_ffmpeg_decoder_fini(&ctx->video_decoder);
		*session_failed = true;
		return PAD_ACTION_NONE;
	}

	ChiakiControllerState controller_state;
	chiaki_controller_state_set_idle(&controller_state);

	PadAction ending = PAD_ACTION_NONE;
	bool running = true;
	while(running)
	{
		SDL_Event event;
		// ~8ms tick keeps controller feedback at ~120Hz without spinning
		if(SDL_WaitEventTimeout(&event, 8))
		{
			do
			{
				if(event.type == SDL_QUIT)
				{
					ending = PAD_ACTION_QUIT;
					running = false;
				}
				else if(event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_ESCAPE)
				{
					ending = PAD_ACTION_QUIT;
					running = false;
				}
				else if(event.type == SDL_CONTROLLERDEVICEADDED)
					pad_open_first(pad, log);
				else if(event.type == SDL_CONTROLLERDEVICEREMOVED && pad->controller
					&& event.cdevice.which == SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(pad->controller)))
				{
					SDL_GameControllerClose(pad->controller);
					pad->controller = NULL;
					pad_open_first(pad, log);
				}
				else if(event.type == ctx->sdl_event_base)
				{
					switch(event.user.code)
					{
						case RKNX_EVENT_FRAME:
							// clear BEFORE pulling: a frame decoded during
							// present re-arms the event instead of being lost
							atomic_store(&ctx->frame_pending, false);
							video_present(vid, &ctx->video_decoder, log);
							break;
						case RKNX_EVENT_RUMBLE:
							if(pad->controller)
								SDL_GameControllerRumble(pad->controller,
									(Uint16)((uintptr_t)event.user.data1 << 8),
									(Uint16)((uintptr_t)event.user.data2 << 8), 5000);
							break;
						case RKNX_EVENT_QUIT:
							running = false;
							break;
					}
				}
			} while(SDL_PollEvent(&event));
		}

		if(g_signal_quit)
		{
			CHIAKI_LOGI(log, "Signal received, quitting cleanly");
			ending = PAD_ACTION_QUIT;
			running = false;
		}
		if(g_signal_toggle_res)
		{
			g_signal_toggle_res = 0;
			ending = PAD_ACTION_TOGGLE_RES;
			running = false;
		}
		if(g_signal_toggle_codec)
		{
			g_signal_toggle_codec = 0;
			ending = PAD_ACTION_TOGGLE_CODEC;
			running = false;
		}

		PadAction action = pad_read(pad, &controller_state);
		if(action != PAD_ACTION_NONE && running)
		{
			CHIAKI_LOGI(log, "Pad action %d", (int)action);
			ending = action;
			running = false;
		}
		chiaki_session_set_controller_state(&ctx->session, &controller_state);
	}

	CHIAKI_LOGI(log, "Shutting down session");
	if(!atomic_load(&ctx->session_quit))
		chiaki_session_stop(&ctx->session);
	chiaki_session_join(&ctx->session);
	chiaki_session_fini(&ctx->session);
	chiaki_opus_decoder_fini(&ctx->opus_decoder);
	chiaki_ffmpeg_decoder_fini(&ctx->video_decoder);

	// black frame between sessions so a toggle reads as an intentional switch
	SDL_RenderClear(vid->renderer);
	SDL_RenderPresent(vid->renderer);
	return ending;
}

int rknx_cmd_stream(ChiakiLog *log, int argc, char *argv[])
{
	Arguments arguments = { 0 };
	if(argp_parse(&argp, argc, argv, ARGP_IN_ORDER, NULL, &arguments) != 0)
		return 1;

	char config_path[512];
	if(arguments.config_path)
		snprintf(config_path, sizeof(config_path), "%s", arguments.config_path);
	else
		rknx_config_default_path(config_path, sizeof(config_path));

	RknxConfig cfg;
	if(rknx_config_load(&cfg, config_path, log) != 0)
	{
		fprintf(stderr, "No config at %s -- pair first with: chiaki regist --host <console> --pin <pin> --account-id <b64>\n", config_path);
		return 1;
	}
	if(!cfg.host_addr[0] || !cfg.have_regist_key || !cfg.have_rp_key)
	{
		fprintf(stderr, "Config %s is incomplete (missing host or keys), re-run regist.\n", config_path);
		return 1;
	}
	if(arguments.resolution)
		snprintf(cfg.resolution, sizeof(cfg.resolution), "%s", arguments.resolution);
	if(arguments.fps == 30 || arguments.fps == 60)
		cfg.fps = arguments.fps;
	if(arguments.decoder)
		snprintf(cfg.decoder, sizeof(cfg.decoder), "%s", arguments.decoder);

	bool is_ps5 = chiaki_target_is_ps5((ChiakiTarget)cfg.target);
	CHIAKI_LOGI(log, "Streaming from %s (%s, %s)", cfg.host_addr,
		cfg.nickname[0] ? cfg.nickname : "?", is_ps5 ? "PS5" : "PS4");

	if(!arguments.no_wakeup)
		ensure_awake(log, &cfg);

	SDL_SetHint(SDL_HINT_APP_NAME, "chiaki");
	SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
	if(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS) != 0)
	{
		fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
		return 1;
	}

	StreamCtx ctx = { 0 };
	ctx.log = log;
	ctx.audio_boost = cfg.audio_boost > 0.0 ? cfg.audio_boost : 1.0;
	ctx.login_pin = arguments.login_pin;
	ctx.sdl_event_base = SDL_RegisterEvents(1);
	atomic_init(&ctx.session_quit, false);
	atomic_init(&ctx.frame_pending, false);
	atomic_init(&ctx.quit_reason, (int)CHIAKI_QUIT_REASON_NONE);

	SDL_Window *window = SDL_CreateWindow("Chiaki",
		SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
		1280, 720,
		SDL_WINDOW_RESIZABLE | (arguments.fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0));
	if(!window)
	{
		fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
		SDL_Quit();
		return 1;
	}
	VideoOut vid = { 0 };
	// no PRESENTVSYNC: on Wayland a vsynced present blocks on frame callbacks,
	// which the compositor withholds while the surface is occluded (e.g. ES
	// fullscreen on top) — that deadlocks the whole event loop and backs up
	// the decoder. Pacing comes from the 60fps stream itself.
	vid.renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
	if(!vid.renderer)
		vid.renderer = SDL_CreateRenderer(window, -1, 0);
	if(!vid.renderer)
	{
		fprintf(stderr, "SDL_CreateRenderer failed: %s\n", SDL_GetError());
		SDL_DestroyWindow(window);
		SDL_Quit();
		return 1;
	}
	SDL_SetRenderDrawColor(vid.renderer, 0, 0, 0, 255);
	SDL_RenderClear(vid.renderer);
	SDL_RenderPresent(vid.renderer);

	struct sigaction sa = { 0 };
	sa.sa_handler = signal_handler;
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGUSR1, &sa, NULL);
	sigaction(SIGUSR2, &sa, NULL);

	PadState pad = { 0 };
	pad_open_first(&pad, log);

	int exit_code = 0;
	int in_use_retries = 0;
	bool restart = true;
	while(restart && !g_signal_quit)
	{
		restart = false;
		bool session_failed = false;
		PadAction ending = run_session(&ctx, &cfg, &vid, &pad, log, &session_failed);
		if(session_failed)
		{
			exit_code = 1;
			break;
		}

		// The console keeps the Remote Play slot busy for a few seconds after
		// a disconnect (both toggle-reconnects and a previous hard-killed
		// client hit this). Self-heal instead of surfacing 0x80108b10.
		if(ending == PAD_ACTION_NONE
			&& (ChiakiQuitReason)atomic_load(&ctx.quit_reason) == CHIAKI_QUIT_REASON_SESSION_REQUEST_RP_IN_USE
			&& in_use_retries < 6 && !g_signal_quit)
		{
			in_use_retries++;
			CHIAKI_LOGW(log, "Console still holds a previous session, retrying (%d/6) in 2.5s", in_use_retries);
			usleep(2500 * 1000);
			restart = true;
			continue;
		}
		in_use_retries = 0;

		switch(ending)
		{
			case PAD_ACTION_TOGGLE_RES:
			{
				bool to_1080 = strcmp(cfg.resolution, "1080p") != 0;
				snprintf(cfg.resolution, sizeof(cfg.resolution), "%s", to_1080 ? "1080p" : "720p");
				CHIAKI_LOGI(log, "Toggling resolution -> %s, reconnecting", cfg.resolution);
				rknx_config_save(&cfg, config_path, log);
				pad_rumble_ack(&pad);
				restart = true;
				break;
			}
			case PAD_ACTION_TOGGLE_CODEC:
			{
				if(!chiaki_target_is_ps5((ChiakiTarget)cfg.target))
				{
					CHIAKI_LOGW(log, "Codec toggle ignored: PS4 streams are h264 only");
					restart = true; // keep streaming
					break;
				}
				bool to_h265 = strcmp(cfg.codec, "h265") != 0;
				snprintf(cfg.codec, sizeof(cfg.codec), "%s", to_h265 ? "h265" : "h264");
				CHIAKI_LOGI(log, "Toggling codec -> %s, reconnecting", cfg.codec);
				rknx_config_save(&cfg, config_path, log);
				pad_rumble_ack(&pad);
				restart = true;
				break;
			}
			case PAD_ACTION_QUIT:
			default:
			{
				// honest exit status: launchers keep their terminal open on
				// failure so the quit reason (e.g. "Remote Play on Console is
				// already in use") is readable instead of flashing past
				ChiakiQuitReason reason = (ChiakiQuitReason)atomic_load(&ctx.quit_reason);
				bool session_ended_itself = atomic_load(&ctx.session_quit);
				if(ending == PAD_ACTION_NONE && session_ended_itself
					&& reason != CHIAKI_QUIT_REASON_NONE && reason != CHIAKI_QUIT_REASON_STOPPED)
					exit_code = 2;
				break;
			}
		}
	}

	if(pad.controller)
		SDL_GameControllerClose(pad.controller);
	if(ctx.audio_device > 0)
		SDL_CloseAudioDevice(ctx.audio_device);
	if(vid.texture)
		SDL_DestroyTexture(vid.texture);
	SDL_DestroyRenderer(vid.renderer);
	SDL_DestroyWindow(window);
	SDL_Quit();
	return exit_code;
}

int rknx_cmd_list(ChiakiLog *log, int argc, char *argv[])
{
	(void)argc;
	(void)argv;
	char config_path[512];
	rknx_config_default_path(config_path, sizeof(config_path));
	RknxConfig cfg;
	if(rknx_config_load(&cfg, config_path, log) != 0)
	{
		printf("No config at %s\n", config_path);
		return 1;
	}
	printf("config:     %s\n", config_path);
	printf("host:       %s\n", cfg.host_addr);
	printf("nickname:   %s\n", cfg.nickname);
	printf("target:     %d (%s)\n", cfg.target, chiaki_target_is_ps5((ChiakiTarget)cfg.target) ? "PS5" : "PS4");
	printf("registered: %s\n", (cfg.have_regist_key && cfg.have_rp_key) ? "yes" : "no");
	printf("video:      %s@%d %s decoder=%s\n", cfg.resolution, cfg.fps, cfg.codec,
		cfg.decoder[0] ? cfg.decoder : "software");
	return 0;
}
