#include "session.h"
#include "proto.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static void relay_video_cb(uint8_t *buf, size_t buf_size, int32_t frames_lost,
	bool frame_recovered, void *user)
{
	(void)frames_lost; (void)frame_recovered; (void)user;
	relay_net_send_video(buf, buf_size);
}

static void relay_audio_header_cb(ChiakiAudioHeader *header, void *user)
{
	(void)user;
	relay_net_set_audio_info(header->channels, header->rate);
}

static void relay_audio_frame_cb(uint8_t *buf, size_t buf_size, void *user)
{
	(void)user;
	relay_net_send_audio(buf, buf_size);
}

static bool relay_event_cb(ChiakiEvent *event, void *user)
{
	relay_session_t *rs = user;
	switch (event->type)
	{
	case CHIAKI_EVENT_CONNECTED:
		CHIAKI_LOGI(&rs->log, "chiaki session connected");
		rs->connected = true;
		break;
	case CHIAKI_EVENT_QUIT:
		CHIAKI_LOGI(&rs->log, "chiaki session quit: %s", event->quit.reason ? event->quit.reason : "?");
		rs->connected = false;
		relay_net_session_ended();
		break;
	default:
		break;
	}
	return true;
}

static int b64_decode(const char *in, uint8_t *out, size_t out_max)
{
	static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	int vals[256]; for (int i = 0; i < 256; i++) vals[i] = -1;
	for (int i = 0; i < 64; i++) vals[(uint8_t)tbl[i]] = i;
	size_t in_len = strlen(in);
	size_t o = 0; int acc = 0, nbits = 0;
	for (size_t i = 0; i < in_len && o < out_max; i++)
	{
		int v = vals[(uint8_t)in[i]];
		if (v < 0) continue;
		acc = (acc << 6) | v; nbits += 6;
		if (nbits >= 8) { nbits -= 8; out[o++] = (acc >> nbits) & 0xFF; }
	}
	return (int)o;
}

static ChiakiVideoResolutionPreset preset_for(int height)
{
	switch (height)
	{
	case 360: return CHIAKI_VIDEO_RESOLUTION_PRESET_360p;
	case 540: return CHIAKI_VIDEO_RESOLUTION_PRESET_540p;
	case 720: return CHIAKI_VIDEO_RESOLUTION_PRESET_720p;
	default: return CHIAKI_VIDEO_RESOLUTION_PRESET_1080p;
	}
}

bool relay_session_init(relay_session_t *rs, const char *host, bool is_ps5,
	const char *regist_key_b64, const char *rp_key_b64, uint32_t rp_key_type,
	int height, int fps, int bitrate)
{
	memset(rs, 0, sizeof(*rs));
	chiaki_log_init(&rs->log, CHIAKI_LOG_INFO, NULL, NULL);
	snprintf(rs->host, sizeof(rs->host), "%s", host);
	snprintf(rs->ps5, sizeof(rs->ps5), "%s", is_ps5 ? "true" : "false");

	if (b64_decode(regist_key_b64, (uint8_t *)rs->regist_key, sizeof(rs->regist_key) - 1) <= 0)
	{ CHIAKI_LOGE(&rs->log, "bad regist key b64"); return false; }
	if (b64_decode(rp_key_b64, rs->rp_key, sizeof(rs->rp_key)) != 0x10)
	{ CHIAKI_LOGE(&rs->log, "bad rp key b64 (must decode to 16 bytes)"); return false; }
	rs->rp_key_type = rp_key_type;

	ChiakiConnectInfo connect_info = {};
	connect_info.host = rs->host;
	connect_info.ps5 = is_ps5;
	connect_info.video_profile_auto_downgrade = true;
	memcpy(connect_info.regist_key, rs->regist_key, sizeof(connect_info.regist_key));
	memcpy(connect_info.morning, rs->rp_key, sizeof(connect_info.morning));

	chiaki_connect_video_profile_preset(&connect_info.video_profile,
		preset_for(height),
		fps >= 60 ? CHIAKI_VIDEO_FPS_PRESET_60 : CHIAKI_VIDEO_FPS_PRESET_30);
	connect_info.video_profile.bitrate = bitrate;

	rs->video_width = connect_info.video_profile.width;
	rs->video_height = connect_info.video_profile.height;
	rs->video_fov = connect_info.video_profile.max_fps;

	ChiakiErrorCode err = chiaki_session_init(&rs->session, &connect_info, &rs->log);
	if (err != CHIAKI_ERR_SUCCESS)
	{ CHIAKI_LOGE(&rs->log, "chiaki_session_init failed: %s", chiaki_error_string(err)); return false; }

	chiaki_session_set_event_cb(&rs->session, relay_event_cb, rs);
	chiaki_session_set_video_sample_cb(&rs->session, relay_video_cb, rs);

	ChiakiAudioSink audio_sink = {
		.user = rs,
		.header_cb = relay_audio_header_cb,
		.frame_cb = relay_audio_frame_cb,
	};
	chiaki_session_set_audio_sink(&rs->session, &audio_sink);
	return true;
}

bool relay_session_start(relay_session_t *rs)
{
	ChiakiErrorCode err = chiaki_session_start(&rs->session);
	if (err != CHIAKI_ERR_SUCCESS)
	{ CHIAKI_LOGE(&rs->log, "chiaki_session_start failed"); return false; }
	return true;
}

void relay_session_stop(relay_session_t *rs)
{
	chiaki_session_stop(&rs->session);
}

void relay_session_fini(relay_session_t *rs)
{
	chiaki_session_join(&rs->session);
	chiaki_session_fini(&rs->session);
}

void relay_session_set_controller(relay_session_t *rs, uint32_t buttons,
	int8_t lx, int8_t ly, int8_t rx, int8_t ry, uint8_t l2, uint8_t r2)
{
	if (!rs->connected)
		return;
	ChiakiControllerState state;
	chiaki_controller_state_set_idle(&state);
	state.buttons = buttons;
	state.left_x = lx * 128; state.left_y = ly * 128;
	state.right_x = rx * 128; state.right_y = ry * 128;
	state.l2_state = l2; state.r2_state = r2;
	chiaki_session_set_controller_state(&rs->session, &state);
}
