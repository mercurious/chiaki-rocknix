// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
//
// One-shot console pairing: runs chiaki_regist and persists the returned
// secrets to the config file, so streaming never needs an interactive UI.

#include "rocknix.h"

#include <chiaki/base64.h>
#include <chiaki/regist.h>

#include <argp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char doc[] =
	"Register (pair) with a console and write the config file.\n"
	"The PIN is shown on the console under Settings -> Remote Play -> Link Device.\n"
	"Get your PSN account id (base64) with scripts/psn-account-id.py.";

#define ARG_KEY_HOST 'h'
#define ARG_KEY_PIN 'p'
#define ARG_KEY_ACCOUNT_ID 'a'
#define ARG_KEY_ONLINE_ID 'o'
#define ARG_KEY_PS4 '4'
#define ARG_KEY_PS5 '5'
#define ARG_KEY_CONFIG 'c'

static struct argp_option options[] = {
	{ "host", ARG_KEY_HOST, "Host", 0, "Console address (required)", 0 },
	{ "pin", ARG_KEY_PIN, "PIN", 0, "8-digit PIN from the console (required)", 0 },
	{ "account-id", ARG_KEY_ACCOUNT_ID, "ID", 0, "PSN account id, base64 (required for PS5 / PS4 >= 7.0)", 0 },
	{ "online-id", ARG_KEY_ONLINE_ID, "ID", 0, "PSN online id (only for PS4 < 7.0)", 0 },
	{ "ps4", ARG_KEY_PS4, NULL, 0, "Console is a PlayStation 4", 0 },
	{ "ps5", ARG_KEY_PS5, NULL, 0, "Console is a PlayStation 5 (default)", 0 },
	{ "config", ARG_KEY_CONFIG, "Path", 0, "Config file to write", 0 },
	{ 0 }
};

typedef struct arguments
{
	const char *host;
	const char *pin;
	const char *account_id;
	const char *online_id;
	const char *config_path;
	bool ps5;
} Arguments;

static int parse_opt(int key, char *arg, struct argp_state *state)
{
	Arguments *arguments = state->input;
	switch(key)
	{
		case ARG_KEY_HOST:
			arguments->host = arg;
			break;
		case ARG_KEY_PIN:
			arguments->pin = arg;
			break;
		case ARG_KEY_ACCOUNT_ID:
			arguments->account_id = arg;
			break;
		case ARG_KEY_ONLINE_ID:
			arguments->online_id = arg;
			break;
		case ARG_KEY_PS4:
			arguments->ps5 = false;
			break;
		case ARG_KEY_PS5:
			arguments->ps5 = true;
			break;
		case ARG_KEY_CONFIG:
			arguments->config_path = arg;
			break;
		case ARGP_KEY_ARG:
			argp_usage(state);
			break;
		default:
			return ARGP_ERR_UNKNOWN;
	}
	return 0;
}

static struct argp argp = { options, parse_opt, 0, doc, 0, 0, 0 };

typedef struct regist_state
{
	volatile int finished; // 0 = running, 1 = success, -1 = failed/canceled
	ChiakiRegisteredHost host;
} RegistState;

static void regist_cb(ChiakiRegistEvent *event, void *user)
{
	RegistState *state = user;
	switch(event->type)
	{
		case CHIAKI_REGIST_EVENT_TYPE_FINISHED_SUCCESS:
			state->host = *event->registered_host;
			state->finished = 1;
			break;
		default:
			state->finished = -1;
			break;
	}
}

int rknx_cmd_regist(ChiakiLog *log, int argc, char *argv[])
{
	Arguments arguments = { 0 };
	arguments.ps5 = true;
	if(argp_parse(&argp, argc, argv, ARGP_IN_ORDER, NULL, &arguments) != 0)
		return 1;

	if(!arguments.host || !arguments.pin)
	{
		fprintf(stderr, "regist: --host and --pin are required, see --help.\n");
		return 1;
	}

	ChiakiRegistInfo info = { 0 };
	info.host = arguments.host;
	info.broadcast = false;
	info.pin = (uint32_t)strtoul(arguments.pin, NULL, 10);
	info.target = arguments.ps5 ? CHIAKI_TARGET_PS5_1 : CHIAKI_TARGET_PS4_10;

	if(arguments.account_id)
	{
		size_t account_id_size = sizeof(info.psn_account_id);
		if(chiaki_base64_decode(arguments.account_id, strlen(arguments.account_id),
				info.psn_account_id, &account_id_size) != CHIAKI_ERR_SUCCESS
			|| account_id_size != CHIAKI_PSN_ACCOUNT_ID_SIZE)
		{
			fprintf(stderr, "regist: --account-id is not valid base64 of %d bytes.\n", CHIAKI_PSN_ACCOUNT_ID_SIZE);
			return 1;
		}
		info.psn_online_id = NULL;
	}
	else if(arguments.online_id && !arguments.ps5)
	{
		info.psn_online_id = arguments.online_id;
	}
	else
	{
		fprintf(stderr, "regist: --account-id is required (or --online-id for PS4 < 7.0), see --help.\n");
		return 1;
	}

	RegistState state = { 0 };
	ChiakiRegist regist;
	ChiakiErrorCode err = chiaki_regist_start(&regist, log, &info, regist_cb, &state);
	if(err != CHIAKI_ERR_SUCCESS)
	{
		CHIAKI_LOGE(log, "Failed to start registration: %s", chiaki_error_string(err));
		return 1;
	}

	// regist runs on its own thread and always fires exactly one FINISHED event
	for(int waited = 0; !state.finished && waited < 60000; waited += 100)
		usleep(100 * 1000);
	if(!state.finished)
	{
		CHIAKI_LOGE(log, "Registration timed out");
		chiaki_regist_stop(&regist);
	}
	chiaki_regist_fini(&regist);

	if(state.finished != 1)
	{
		CHIAKI_LOGE(log, "Registration failed (check PIN, account id and that the console is fully awake)");
		return 1;
	}

	CHIAKI_LOGI(log, "Registered with %s (nickname \"%s\")", arguments.host, state.host.server_nickname);

	char config_path[512];
	if(arguments.config_path)
		snprintf(config_path, sizeof(config_path), "%s", arguments.config_path);
	else
		rknx_config_default_path(config_path, sizeof(config_path));

	// Start from the existing config if present so stream settings survive re-pairing
	RknxConfig cfg;
	if(rknx_config_load(&cfg, config_path, log) != 0)
		rknx_config_defaults(&cfg);

	snprintf(cfg.host_addr, sizeof(cfg.host_addr), "%s", arguments.host);
	snprintf(cfg.nickname, sizeof(cfg.nickname), "%s", state.host.server_nickname);
	cfg.target = (int)state.host.target;
	if(arguments.account_id)
		snprintf(cfg.psn_account_id_b64, sizeof(cfg.psn_account_id_b64), "%s", arguments.account_id);
	memcpy(cfg.rp_regist_key, state.host.rp_regist_key, sizeof(cfg.rp_regist_key));
	memcpy(cfg.rp_key, state.host.rp_key, sizeof(cfg.rp_key));
	cfg.have_regist_key = true;
	cfg.have_rp_key = true;

	if(rknx_config_save(&cfg, config_path, log) != 0)
		return 1;

	printf("Registration successful. You can now run: chiaki stream\n");
	return 0;
}
