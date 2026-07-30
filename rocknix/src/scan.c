// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
//
// Broadcast console discovery with machine-parseable output, for the
// on-device menu. One tab-separated line per console:
//   addr\tstate\tname\tps5
// state: ready|standby|unknown; ps5: 1|0

#include "rocknix.h"

#include <chiaki/discovery.h>
#include <chiaki/thread.h>

#include <argp.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char doc[] = "Scan the local network for consoles (broadcast discovery).";

#define ARG_KEY_TIMEOUT 't'

static struct argp_option options[] = {
	{ "timeout", ARG_KEY_TIMEOUT, "Seconds", 0, "How long to listen (default 3)", 0 },
	{ 0 }
};

typedef struct arguments
{
	int timeout;
} Arguments;

static int parse_opt(int key, char *arg, struct argp_state *state)
{
	Arguments *a = state->input;
	switch(key)
	{
		case ARG_KEY_TIMEOUT: a->timeout = atoi(arg); break;
		case ARGP_KEY_ARG: argp_usage(state); break;
		default: return ARGP_ERR_UNKNOWN;
	}
	return 0;
}

static struct argp argp = { options, parse_opt, 0, doc, 0, 0, 0 };

#define SCAN_MAX_HOSTS 8

typedef struct scan_state
{
	ChiakiMutex mutex;
	char seen[SCAN_MAX_HOSTS][64];
	size_t seen_count;
} ScanState;

static void scan_cb(ChiakiDiscoveryHost *host, void *user)
{
	ScanState *state = user;
	if(!host->host_addr)
		return;
	chiaki_mutex_lock(&state->mutex);
	for(size_t i = 0; i < state->seen_count; i++)
	{
		if(!strcmp(state->seen[i], host->host_addr))
		{
			chiaki_mutex_unlock(&state->mutex);
			return;
		}
	}
	if(state->seen_count < SCAN_MAX_HOSTS)
		snprintf(state->seen[state->seen_count++], sizeof(state->seen[0]), "%s", host->host_addr);
	chiaki_mutex_unlock(&state->mutex);

	const char *state_str = "unknown";
	if(host->state == CHIAKI_DISCOVERY_HOST_STATE_READY)
		state_str = "ready";
	else if(host->state == CHIAKI_DISCOVERY_HOST_STATE_STANDBY)
		state_str = "standby";
	printf("%s\t%s\t%s\t%d\n", host->host_addr, state_str,
		host->host_name ? host->host_name : "?",
		chiaki_discovery_host_is_ps5(host) ? 1 : 0);
	fflush(stdout);
}

int rknx_cmd_scan(ChiakiLog *log, int argc, char *argv[])
{
	Arguments arguments = { 0 };
	arguments.timeout = 3;
	if(argp_parse(&argp, argc, argv, ARGP_IN_ORDER, NULL, &arguments) != 0)
		return 1;
	if(arguments.timeout < 1)
		arguments.timeout = 1;
	if(arguments.timeout > 30)
		arguments.timeout = 30;

	ScanState state = { 0 };
	if(chiaki_mutex_init(&state.mutex, false) != CHIAKI_ERR_SUCCESS)
		return 1;

	ChiakiDiscovery discovery;
	if(chiaki_discovery_init(&discovery, log, AF_INET) != CHIAKI_ERR_SUCCESS)
	{
		CHIAKI_LOGE(log, "Discovery init failed");
		return 1;
	}
	ChiakiDiscoveryThread thread;
	if(chiaki_discovery_thread_start(&thread, &discovery, scan_cb, &state) != CHIAKI_ERR_SUCCESS)
	{
		CHIAKI_LOGE(log, "Discovery thread start failed");
		chiaki_discovery_fini(&discovery);
		return 1;
	}

	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = INADDR_BROADCAST; // SO_BROADCAST is set by discovery init

	// re-send each second: standby consoles can be slow to answer
	for(int t = 0; t < arguments.timeout; t++)
	{
		ChiakiDiscoveryPacket packet;
		memset(&packet, 0, sizeof(packet));
		packet.cmd = CHIAKI_DISCOVERY_CMD_SRCH;
		packet.protocol_version = CHIAKI_DISCOVERY_PROTOCOL_VERSION_PS4;
		addr.sin_port = htons(CHIAKI_DISCOVERY_PORT_PS4);
		chiaki_discovery_send(&discovery, &packet, (struct sockaddr *)&addr, sizeof(addr));
		packet.protocol_version = CHIAKI_DISCOVERY_PROTOCOL_VERSION_PS5;
		addr.sin_port = htons(CHIAKI_DISCOVERY_PORT_PS5);
		chiaki_discovery_send(&discovery, &packet, (struct sockaddr *)&addr, sizeof(addr));
		sleep(1);
	}

	chiaki_discovery_thread_stop(&thread);
	chiaki_discovery_fini(&discovery);
	chiaki_mutex_fini(&state.mutex);
	return 0;
}
