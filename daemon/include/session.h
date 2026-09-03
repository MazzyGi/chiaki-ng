#ifndef RELAY_SESSION_H
#define RELAY_SESSION_H

#include <chiaki/session.h>
#include <chiaki/regist.h>
#include <chiaki/log.h>
#include <stdbool.h>
#include <stdint.h>
#include <pthread.h>

typedef struct relay_session_t
{
	ChiakiSession session;
	ChiakiLog log;

	// host identity (from gui settings json)
	char host[64];
	char ps5[8];
	char regist_key[CHIAKI_SESSION_AUTH_SIZE];
	uint8_t rp_key[0x10];
	uint32_t rp_key_type;

	// negotiated stream params forwarded to client
	uint32_t video_width, video_height, video_fov;
	unsigned int audio_channels, audio_rate;

	bool connected;
	bool quit_requested;
} relay_session_t;

bool relay_session_init(relay_session_t *rs, const char *host, bool is_ps5,
	const char *regist_key_b64, const char *rp_key_b64, uint32_t rp_key_type,
	int height, int fps, int bitrate);
bool relay_session_start(relay_session_t *rs);
void relay_session_stop(relay_session_t *rs);
void relay_session_fini(relay_session_t *rs);
void relay_session_set_controller(relay_session_t *rs, uint32_t buttons,
	int8_t lx, int8_t ly, int8_t rx, int8_t ry, uint8_t l2, uint8_t r2);

// implemented in net.c - called from chiaki callbacks (stream threads!)
void relay_net_send_video(const uint8_t *buf, size_t size);
void relay_net_send_audio(const uint8_t *buf, size_t size);
void relay_net_set_audio_info(unsigned channels, unsigned rate);
void relay_net_session_ended(void);

#endif
