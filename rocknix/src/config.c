// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
//
// Flat key=value config, one host per file. Written atomically (tmp+rename)
// because the file carries the Remote Play secrets and a torn write would
// force the user through console re-registration.

#include "rocknix.h"

#include <chiaki/base64.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

void rknx_config_defaults(RknxConfig *cfg)
{
	memset(cfg, 0, sizeof(*cfg));
	cfg->target = CHIAKI_TARGET_PS5_1;
	// 720p60 is the conservative first-run default (10 Mbit/s); raise to 1080p in the config once the link is proven.
	snprintf(cfg->resolution, sizeof(cfg->resolution), "720p");
	cfg->fps = 60;
	snprintf(cfg->codec, sizeof(cfg->codec), "h264");
	cfg->decoder[0] = '\0';
	cfg->audio_boost = 1.0;
	snprintf(cfg->haptics, sizeof(cfg->haptics), "normal");
	cfg->trigger_deadzone = 0.10;
	snprintf(cfg->bitrate, sizeof(cfg->bitrate), "auto");
}

void rknx_config_default_path(char *buf, size_t buf_size)
{
	const char *env = getenv("CHIAKI_CONFIG");
	if(env && *env)
	{
		snprintf(buf, buf_size, "%s", env);
		return;
	}
	struct stat st;
	if(stat("/storage/.config", &st) == 0 && S_ISDIR(st.st_mode))
	{
		snprintf(buf, buf_size, "/storage/.config/chiaki/chiaki.conf");
		return;
	}
	const char *home = getenv("HOME");
	snprintf(buf, buf_size, "%s/.config/chiaki/chiaki.conf", home ? home : ".");
}

static void strip(char *s)
{
	char *start = s;
	while(*start == ' ' || *start == '\t')
		start++;
	size_t len = strlen(start);
	while(len && (start[len - 1] == '\n' || start[len - 1] == '\r' || start[len - 1] == ' ' || start[len - 1] == '\t'))
		start[--len] = '\0';
	memmove(s, start, len + 1);
}

static int decode_b64_fixed(const char *val, uint8_t *out, size_t out_size)
{
	uint8_t tmp[64];
	size_t sz = sizeof(tmp);
	if(chiaki_base64_decode(val, strlen(val), tmp, &sz) != CHIAKI_ERR_SUCCESS)
		return -1;
	if(sz > out_size)
		return -1;
	memset(out, 0, out_size);
	memcpy(out, tmp, sz);
	return 0;
}

int rknx_config_load(RknxConfig *cfg, const char *path, ChiakiLog *log)
{
	rknx_config_defaults(cfg);
	FILE *f = fopen(path, "r");
	if(!f)
	{
		CHIAKI_LOGE(log, "Failed to open config %s: %s", path, strerror(errno));
		return -1;
	}
	char line[512];
	while(fgets(line, sizeof(line), f))
	{
		strip(line);
		if(line[0] == '\0' || line[0] == '#' || line[0] == '[')
			continue;
		char *eq = strchr(line, '=');
		if(!eq)
			continue;
		*eq = '\0';
		char *key = line;
		char *val = eq + 1;
		strip(key);
		strip(val);

		if(!strcmp(key, "host_addr"))
			snprintf(cfg->host_addr, sizeof(cfg->host_addr), "%s", val);
		else if(!strcmp(key, "nickname"))
			snprintf(cfg->nickname, sizeof(cfg->nickname), "%s", val);
		else if(!strcmp(key, "target"))
			cfg->target = atoi(val);
		else if(!strcmp(key, "psn_account_id"))
			snprintf(cfg->psn_account_id_b64, sizeof(cfg->psn_account_id_b64), "%s", val);
		else if(!strcmp(key, "rp_regist_key"))
			cfg->have_regist_key = decode_b64_fixed(val, cfg->rp_regist_key, sizeof(cfg->rp_regist_key)) == 0;
		else if(!strcmp(key, "rp_key"))
			cfg->have_rp_key = decode_b64_fixed(val, cfg->rp_key, sizeof(cfg->rp_key)) == 0;
		else if(!strcmp(key, "video_resolution"))
			snprintf(cfg->resolution, sizeof(cfg->resolution), "%s", val);
		else if(!strcmp(key, "video_fps"))
			cfg->fps = atoi(val);
		else if(!strcmp(key, "codec"))
			snprintf(cfg->codec, sizeof(cfg->codec), "%s", val);
		else if(!strcmp(key, "decoder"))
			snprintf(cfg->decoder, sizeof(cfg->decoder), "%s", val);
		else if(!strcmp(key, "audio_boost"))
			cfg->audio_boost = atof(val);
		else if(!strcmp(key, "haptics"))
			snprintf(cfg->haptics, sizeof(cfg->haptics), "%s", val);
		else if(!strcmp(key, "trigger_deadzone"))
		{
			cfg->trigger_deadzone = atof(val);
			if(cfg->trigger_deadzone < 0.0 || cfg->trigger_deadzone > 0.4)
				cfg->trigger_deadzone = 0.10;
		}
		else if(!strcmp(key, "bitrate"))
			snprintf(cfg->bitrate, sizeof(cfg->bitrate), "%s", val);
		else
			CHIAKI_LOGW(log, "Unknown config key \"%s\"", key);
	}
	fclose(f);
	return 0;
}

static int mkdir_parents(const char *path)
{
	char tmp[512];
	snprintf(tmp, sizeof(tmp), "%s", path);
	char *slash = strrchr(tmp, '/');
	if(!slash)
		return 0;
	*slash = '\0';
	for(char *p = tmp + 1; *p; p++)
	{
		if(*p == '/')
		{
			*p = '\0';
			if(mkdir(tmp, 0700) < 0 && errno != EEXIST)
				return -1;
			*p = '/';
		}
	}
	if(mkdir(tmp, 0700) < 0 && errno != EEXIST)
		return -1;
	return 0;
}

int rknx_config_save(const RknxConfig *cfg, const char *path, ChiakiLog *log)
{
	if(mkdir_parents(path) < 0)
	{
		CHIAKI_LOGE(log, "Failed to create config dir for %s: %s", path, strerror(errno));
		return -1;
	}

	char regist_key_b64[64] = "";
	char rp_key_b64[64] = "";
	if(cfg->have_regist_key)
		chiaki_base64_encode(cfg->rp_regist_key, sizeof(cfg->rp_regist_key), regist_key_b64, sizeof(regist_key_b64));
	if(cfg->have_rp_key)
		chiaki_base64_encode(cfg->rp_key, sizeof(cfg->rp_key), rp_key_b64, sizeof(rp_key_b64));

	char tmp_path[512];
	snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);
	FILE *f = fopen(tmp_path, "w");
	if(!f)
	{
		CHIAKI_LOGE(log, "Failed to write %s: %s", tmp_path, strerror(errno));
		return -1;
	}
	fprintf(f,
		"# chiaki-rocknix config (contains Remote Play secrets -- do not share)\n"
		"host_addr = %s\n"
		"nickname = %s\n"
		"target = %d\n"
		"psn_account_id = %s\n"
		"rp_regist_key = %s\n"
		"rp_key = %s\n"
		"video_resolution = %s\n"
		"video_fps = %d\n"
		"codec = %s\n"
		"decoder = %s\n"
		"audio_boost = %.2f\n"
		"haptics = %s\n"
		"trigger_deadzone = %.2f\n"
		"bitrate = %s\n",
		cfg->host_addr, cfg->nickname, cfg->target, cfg->psn_account_id_b64,
		regist_key_b64, rp_key_b64,
		cfg->resolution, cfg->fps, cfg->codec, cfg->decoder, cfg->audio_boost,
		cfg->haptics, cfg->trigger_deadzone, cfg->bitrate);
	fclose(f);
	if(chmod(tmp_path, 0600) < 0 || rename(tmp_path, path) < 0)
	{
		CHIAKI_LOGE(log, "Failed to finalize %s: %s", path, strerror(errno));
		unlink(tmp_path);
		return -1;
	}
	CHIAKI_LOGI(log, "Config written to %s", path);
	return 0;
}
