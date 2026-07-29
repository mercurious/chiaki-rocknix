// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
//
// Subcommand dispatch, modeled on cli/src/main.c. discover/wakeup come from
// chiaki-cli-lib; stream/regist/list are the ROCKNIX frontend.

#include "rocknix.h"

#include <chiaki-cli.h>

#include <argp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char doc[] =
	"Chiaki (PlayStation Remote Play) for ROCKNIX"
	"\v"
	"Supported commands are:\n"
	"  stream      Stream from the registered console (default).\n"
	"  regist      Pair with a console using the Remote Play PIN.\n"
	"  discover    Discover consoles.\n"
	"  wakeup      Send a wakeup packet.\n"
	"  list        Show the current configuration.\n";

#define ARG_KEY_VERBOSE 'v'

static struct argp_option options[] = {
	{ "verbose", ARG_KEY_VERBOSE, NULL, 0, "Verbose logging", 0 },
	{ 0 }
};

typedef struct context
{
	ChiakiLog log;
} Context;

static int call_subcmd(struct argp_state *state, const char *name, int (*subcmd)(ChiakiLog *log, int argc, char *argv[]))
{
	if(state->next < 1 || state->argc < state->next)
		return 1;

	int argc = state->argc - state->next + 1;
	char **argv = &state->argv[state->next - 1];

	size_t l = strlen(state->name) + strlen(name) + 2;
	argv[0] = malloc(l);
	if(!argv[0])
		return 1;
	snprintf(argv[0], l, "%s %s", state->name, name);

	Context *ctx = state->input;
	int r = subcmd(&ctx->log, argc, argv);

	free(argv[0]);
	return r;
}

static int parse_opt(int key, char *arg, struct argp_state *state)
{
	Context *ctx = state->input;

	switch(key)
	{
		case ARG_KEY_VERBOSE:
			ctx->log.level_mask = CHIAKI_LOG_ALL;
			break;
		case ARGP_KEY_ARG:
			if(strcmp(arg, "stream") == 0)
				exit(call_subcmd(state, "stream", rknx_cmd_stream));
			else if(strcmp(arg, "regist") == 0)
				exit(call_subcmd(state, "regist", rknx_cmd_regist));
			else if(strcmp(arg, "discover") == 0)
				exit(call_subcmd(state, "discover", chiaki_cli_cmd_discover));
			else if(strcmp(arg, "wakeup") == 0)
				exit(call_subcmd(state, "wakeup", chiaki_cli_cmd_wakeup));
			else if(strcmp(arg, "list") == 0)
				exit(call_subcmd(state, "list", rknx_cmd_list));
			// fallthrough
		case ARGP_KEY_END:
			argp_usage(state);
			break;
		default:
			return ARGP_ERR_UNKNOWN;
	}
	return 0;
}

static struct argp argp = { options, parse_opt, "<cmd> [CMD-ARGS...]", doc, 0, 0, 0 };

int main(int argc, char *argv[])
{
	Context ctx;
	chiaki_log_init(&ctx.log, CHIAKI_LOG_ALL & ~(CHIAKI_LOG_VERBOSE | CHIAKI_LOG_DEBUG), chiaki_log_cb_print, NULL);

	argp_parse(&argp, argc, argv, ARGP_IN_ORDER, NULL, &ctx);

	return 0;
}
