#include "session.h"
#include "proto.h"
#include <chiaki/regist.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <json-c/json.h>

static volatile sig_atomic_t g_quit = 0;
extern relay_session_t *g_session;
static relay_session_t rs;
static void on_sigint(int sig) { (void)sig; g_quit = 1; }

bool relay_net_start(relay_session_t *rs);
void relay_net_stop(void);

// ---- register mode: exchange PIN for credentials, save to json ----------

static const char *b64_tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static void b64_encode(const uint8_t *in, size_t n, char *out, size_t out_max)
{
	size_t o = 0;
	for (size_t i = 0; i < n && o + 4 < out_max; i += 3)
	{
		uint32_t v = (uint32_t)in[i] << 16;
		if (i + 1 < n) v |= (uint32_t)in[i+1] << 8;
		if (i + 2 < n) v |= in[i+2];
		out[o++] = b64_tbl[(v >> 18) & 63];
		out[o++] = b64_tbl[(v >> 12) & 63];
		out[o++] = (i + 1 < n) ? b64_tbl[(v >> 6) & 63] : '=';
		out[o++] = (i + 2 < n) ? b64_tbl[v & 63] : '=';
	}
	out[o] = 0;
}

static int b64_decode_str(const char *in, uint8_t *out, size_t out_max)
{
	int vals[256]; for (int i = 0; i < 256; i++) vals[i] = -1;
	for (int i = 0; i < 64; i++) vals[(uint8_t)b64_tbl[i]] = i;
	size_t o = 0; int acc = 0, nbits = 0;
	for (const char *p = in; *p && o < out_max; p++)
	{
		int v = vals[(uint8_t)*p];
		if (v < 0) continue;
		acc = (acc << 6) | v; nbits += 6;
		if (nbits >= 8) { nbits -= 8; out[o++] = (acc >> nbits) & 0xFF; }
	}
	return (int)o;
}

typedef struct regist_ctx_t
{
	int done; // 0 = running, 1 = success, -1 = failed
	ChiakiRegisteredHost host;
} regist_ctx_t;

static void regist_cb(ChiakiRegistEvent *event, void *user)
{
	regist_ctx_t *ctx = user;
	if (event->type == CHIAKI_REGIST_EVENT_TYPE_FINISHED_SUCCESS && event->registered_host)
	{
		ctx->host = *event->registered_host;
		ctx->done = 1;
	}
	else
		ctx->done = -1;
}

static int do_register(const char *host, const char *psn_account_id_b64,
	const char *pin_str, const char *console_pin_str, const char *save_path)
{
	ChiakiLog log;
	chiaki_log_init(&log, CHIAKI_LOG_INFO, NULL, NULL);

	uint8_t account_id[CHIAKI_PSN_ACCOUNT_ID_SIZE];
	if (b64_decode_str(psn_account_id_b64, account_id, sizeof(account_id)) != CHIAKI_PSN_ACCOUNT_ID_SIZE)
	{
		fprintf(stderr, "invalid account id b64 (must be 8 bytes)\n");
		return 1;
	}

	ChiakiRegistInfo info = {};
	info.host = host;
	info.target = CHIAKI_TARGET_PS5_1;
	info.broadcast = true;
	info.psn_online_id = NULL;
	memcpy(info.psn_account_id, account_id, sizeof(account_id));
	info.pin = (uint32_t)strtoul(pin_str, NULL, 10);
	info.console_pin = console_pin_str ? (uint32_t)strtoul(console_pin_str, NULL, 10) : info.pin;
	info.holepunch_info = NULL;
	info.rudp = NULL;

	regist_ctx_t ctx = {};
	ChiakiRegist regist;
	ChiakiErrorCode err = chiaki_regist_start(&regist, &log, &info, regist_cb, &ctx);
	if (err != CHIAKI_ERR_SUCCESS)
	{
		fprintf(stderr, "regist start failed: %s\n", chiaki_error_string(err));
		return 1;
	}
	while (!ctx.done && !g_quit)
		usleep(50000);
	chiaki_regist_fini(&regist);
	if (ctx.done != 1)
	{
		fprintf(stderr, "registration failed\n");
		return 1;
	}

	char regist_key_b64[64], rp_key_b64[64];
	b64_encode((const uint8_t *)ctx.host.rp_regist_key, CHIAKI_SESSION_AUTH_SIZE, regist_key_b64, sizeof(regist_key_b64));
	b64_encode(ctx.host.rp_key, 0x10, rp_key_b64, sizeof(rp_key_b64));

	json_object *root = json_object_new_object();
	json_object_object_add(root, "host", json_object_new_string(host));
	json_object_object_add(root, "ps5", json_object_new_boolean(true));
	json_object_object_add(root, "nickname", json_object_new_string(ctx.host.server_nickname));
	json_object_object_add(root, "regist_key", json_object_new_string(regist_key_b64));
	json_object_object_add(root, "rp_key", json_object_new_string(rp_key_b64));
	json_object_object_add(root, "rp_key_type", json_object_new_int((int)ctx.host.rp_key_type));
	if (json_object_to_file_ext(save_path, root, JSON_C_TO_STRING_PRETTY) < 0)
	{
		fprintf(stderr, "failed to write %s\n", save_path);
		return 1;
	}
	printf("registered: %s\n  regist_key: %s\n  rp_key: %s\n  key_type: %u\nsaved to %s\n",
		ctx.host.server_nickname, regist_key_b64, rp_key_b64, ctx.host.rp_key_type, save_path);
	json_object_put(root);
	return 0;
}

// ---- test mode: ffmpeg-generated h265 test pattern through the relay path --
static FILE *test_ff = NULL;

static bool relay_test_start(int width, int height, int fps, int bitrate_kbps)
{
	char cmd[512];
	snprintf(cmd, sizeof(cmd),
		"ffmpeg -loglevel error -f lavfi -i testsrc2=size=%dx%d:rate=%d "
		"-c:v libx265 -preset ultrafast -tune zerolatency -b:v %dk "
		"-x265-params \"repeat-headers=1:keyint=%d\" -f h264 pipe: 2>/dev/null",
		width, height, fps, bitrate_kbps, fps * 2);
	// note: -f h264 is annexb for h265 too (ffmpeg maps it)
	test_ff = popen(cmd, "r");
	return test_ff != NULL;
}

static void relay_test_feed(void)
{
	// read annexb access units (split on 00 00 00 01 start codes) and ship
	static uint8_t buf[512 * 1024];
	static size_t used = 0;
	if (!test_ff)
		return;
	size_t n = fread(buf + used, 1, 4096, test_ff);
	if (n == 0)
	{
		if (feof(test_ff))
		{ pclose(test_ff); test_ff = NULL; relay_net_session_ended(); }
		return;
	}
	used += n;
	// find last complete access unit boundary (start code that is not the first)
	size_t last_au = 0;
	for (size_t i = 4; i + 3 < used; i++)
	{
		if (buf[i] == 0 && buf[i+1] == 0 && ((buf[i+2] == 1) ||
			(buf[i+2] == 0 && buf[i+3] == 1)))
		{
			size_t sc = (buf[i+2] == 1) ? 3 : 4;
			uint8_t nut = (buf[i+sc] >> 1) & 0x3f;
			if (nut <= 31 || nut == 32 || nut == 33 || nut == 34 || nut == 39 || nut == 40)
			{
				// VCL (0-31) or VPS/SPS/PPS/SEI begins an AU
				last_au = i;
			}
		}
	}
	if (last_au > 0)
	{
		relay_net_send_video(buf, last_au);
		memmove(buf, buf + last_au, used - last_au);
		used -= last_au;
	}
	if (used > sizeof(buf) - 8192)
	{
		// overflow guard - ship what we have
		relay_net_send_video(buf, used);
		used = 0;
	}
}

static void print_usage(const char *argv0)
{
	fprintf(stderr,
		"chiaki-relay - headless chiaki relay daemon (OrangePi side)\n\n"
		"usage: %s --register --host <ip> --account-id <b64> --pin <code> [--console-pin <code>] [--save file]\n"
		"       %s --host <ip> [--creds file] [--regist-key <b64> --rp-key <b64> --key-type 3]\n"
		"          [--height 1080] [--fps 60] [--bitrate 40000]\n\n"
		"   register: exchange PIN (shown on PS5 screen) for stream credentials\n"
		"   account-id: PSN account id, base64 (8 bytes)\n"
		"   pin/console-pin: digits without spaces\n"
		"   creds: json saved by --register (default /etc/chiaki-relay.json)\n"
		"   height: 360/540/720/1080, fps: 30/60, bitrate: kbps\n",
		argv0, argv0);
}

int main(int argc, char **argv)
{
	const char *host = NULL, *regist_key = NULL, *rp_key = NULL;
	const char *cred_file = NULL;
	bool is_ps5 = true;
	uint32_t key_type = 3;
	int height = 1080, fps = 60, bitrate = 40000;

	// test mode: no PS5 needed - ffmpeg h265 pattern through the relay path
	if (argc > 1 && !strcmp(argv[1], "--test"))
	{
		signal(SIGINT, on_sigint);
		signal(SIGTERM, on_sigint);
		static relay_session_t rs_dummy;
		memset(&rs_dummy, 0, sizeof(rs_dummy));
		rs_dummy.video_width = 1280;
		rs_dummy.video_height = 720;
		rs_dummy.video_fov = 60;
		rs_dummy.audio_channels = 2;
		rs_dummy.audio_rate = 48000;
		g_session = &rs_dummy;
		if (!relay_net_start(&rs_dummy))
			return 1;
		if (!relay_test_start(1280, 720, 60, 8000))
		{ fprintf(stderr, "failed to start ffmpeg test source\n"); return 1; }
		CHIAKI_LOGI(NULL, "relay: TEST MODE - h265 test pattern on udp:%d", RELAY_VIDEO_PORT);
		while (!g_quit)
		{
			relay_test_feed();
			usleep(500);
		}
		relay_net_stop();
		return 0;
	}

	// register mode
	if (argc > 1 && !strcmp(argv[1], "--register"))
	{
		const char *acct = NULL, *pin = NULL, *cpin = NULL, *save = "/etc/chiaki-relay.json";
		for (int i = 2; i < argc; i++)
		{
			if (!strcmp(argv[i], "--host") && i + 1 < argc) host = argv[++i];
			else if (!strcmp(argv[i], "--account-id") && i + 1 < argc) acct = argv[++i];
			else if (!strcmp(argv[i], "--pin") && i + 1 < argc) pin = argv[++i];
			else if (!strcmp(argv[i], "--console-pin") && i + 1 < argc) cpin = argv[++i];
			else if (!strcmp(argv[i], "--save") && i + 1 < argc) save = argv[++i];
			else { print_usage(argv[0]); return 1; }
		}
		if (!host || !acct || !pin)
		{ print_usage(argv[0]); return 1; }
		signal(SIGINT, on_sigint);
		return do_register(host, acct, pin, cpin, save);
	}

	for (int i = 1; i < argc; i++)
	{
		if (!strcmp(argv[i], "--host") && i + 1 < argc) host = argv[++i];
		else if (!strcmp(argv[i], "--ps5")) is_ps5 = true;
		else if (!strcmp(argv[i], "--ps4")) is_ps5 = false;
		else if (!strcmp(argv[i], "--regist-key") && i + 1 < argc) regist_key = argv[++i];
		else if (!strcmp(argv[i], "--rp-key") && i + 1 < argc) rp_key = argv[++i];
		else if (!strcmp(argv[i], "--key-type") && i + 1 < argc) key_type = (uint32_t)atoi(argv[++i]);
		else if (!strcmp(argv[i], "--height") && i + 1 < argc) height = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--fps") && i + 1 < argc) fps = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--bitrate") && i + 1 < argc) bitrate = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--creds") && i + 1 < argc) cred_file = argv[++i];
		else { print_usage(argv[0]); return 1; }
	}

	char regist_key_buf[128], rp_key_buf[128];
	if (cred_file)
	{
		json_object *root = json_object_from_file(cred_file);
		if (!root)
		{ fprintf(stderr, "cannot read creds %s\n", cred_file); return 1; }
		json_object *o;
		if (json_object_object_get_ex(root, "regist_key", &o)) regist_key = strncpy(regist_key_buf, json_object_get_string(o), sizeof(regist_key_buf) - 1);
		if (json_object_object_get_ex(root, "rp_key", &o)) rp_key = strncpy(rp_key_buf, json_object_get_string(o), sizeof(rp_key_buf) - 1);
		if (json_object_object_get_ex(root, "rp_key_type", &o)) key_type = (uint32_t)json_object_get_int(o);
		if (!host && json_object_object_get_ex(root, "host", &o)) host = json_object_get_string(o);
		json_object_put(root);
	}
	if (!host || !regist_key || !rp_key)
	{ print_usage(argv[0]); return 1; }

	signal(SIGINT, on_sigint);
	signal(SIGTERM, on_sigint);

	if (!relay_session_init(&rs, host, is_ps5, regist_key, rp_key, key_type, height, fps, bitrate))
		return 1;
	if (!relay_net_start(&rs))
		return 1;

	CHIAKI_LOGI(&rs.log, "relay: connecting to %s ...", host);
	if (!relay_session_start(&rs))
	{
		relay_net_stop();
		return 1;
	}

	// main loop: wait for ctrl-c; keep relaying
	while (!g_quit)
		sleep(1);

	CHIAKI_LOGI(&rs.log, "relay: shutting down");
	relay_session_stop(&rs);
	relay_session_fini(&rs);
	relay_net_stop();
	return 0;
}
