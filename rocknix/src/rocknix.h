// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#ifndef CHIAKI_ROCKNIX_H
#define CHIAKI_ROCKNIX_H

#include <chiaki/common.h>
#include <chiaki/log.h>
#include <chiaki/session.h>

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RKNX_MORNING_SIZE 0x10

typedef struct rknx_config_t
{
	char host_addr[128];
	char nickname[64];
	int target; // numeric ChiakiTarget
	char psn_account_id_b64[64];
	uint8_t rp_regist_key[CHIAKI_SESSION_AUTH_SIZE];
	uint8_t rp_key[RKNX_MORNING_SIZE];
	bool have_regist_key;
	bool have_rp_key;
	char resolution[8]; // 360p/540p/720p/1080p
	int fps;            // 30 or 60
	char codec[8];      // h264 or h265 (h265 only valid for PS5)
	char decoder[32];   // empty/"software", an AVHWDeviceType name, or a decoder name like h264_v4l2m2m
	double audio_boost; // 1.0 = passthrough
} RknxConfig;

void rknx_config_defaults(RknxConfig *cfg);
// Resolution: $CHIAKI_CONFIG > /storage/.config/chiaki/chiaki.conf (if /storage/.config exists) > ~/.config/chiaki/chiaki.conf
void rknx_config_default_path(char *buf, size_t buf_size);
int rknx_config_load(RknxConfig *cfg, const char *path, ChiakiLog *log);
int rknx_config_save(const RknxConfig *cfg, const char *path, ChiakiLog *log);

int rknx_cmd_regist(ChiakiLog *log, int argc, char *argv[]);
int rknx_cmd_stream(ChiakiLog *log, int argc, char *argv[]);
int rknx_cmd_list(ChiakiLog *log, int argc, char *argv[]);

#ifdef __cplusplus
}
#endif

#endif
