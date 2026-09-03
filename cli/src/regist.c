// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include <chiaki/regist.h>
#include <chiaki/log.h>
#include <chiaki/base64.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>

static volatile sig_atomic_t g_done = 0;
static void on_sigint(int sig) { (void)sig; g_done = -1; }

typedef struct
{
	int result; // 1 success, -1 fail
	ChiakiRegisteredHost host;
} RegistCtx;

static void regist_cb(ChiakiRegistEvent *event, void *user)
{
	RegistCtx *ctx = user;
	if(event->type == CHIAKI_REGIST_EVENT_TYPE_FINISHED_SUCCESS && event->registered_host)
	{
		ctx->host = *event->registered_host;
		ctx->result = 1;
	}
	else
		ctx->result = -1;
}

int chiaki_cli_cmd_regist(ChiakiLog *log, int argc, char *argv[])
{
	if(argc != 5)
	{
		fprintf(stderr, "Usage: %s %s <host> <account-id> <pin>\n", argv[0], argv[1]);
		fprintf(stderr, "account-id: PSN Account ID (base64, 8 bytes)\n");
		return 1;
	}

	const char *host = argv[2];
	const char *account_id_b64 = argv[3];
	uint32_t pin = (uint32_t)strtoul(argv[4], NULL, 10);

	ChiakiRegistInfo info = {0};
	info.host = host;
	info.target = CHIAKI_TARGET_PS5_1;
	info.broadcast = false;
	info.psn_online_id = NULL;

	uint8_t account_id[CHIAKI_PSN_ACCOUNT_ID_SIZE];
	size_t decoded = sizeof(account_id);
	if(chiaki_base64_decode(account_id_b64, strlen(account_id_b64), account_id, &decoded) != CHIAKI_ERR_SUCCESS
		|| decoded != CHIAKI_PSN_ACCOUNT_ID_SIZE)
	{
		fprintf(stderr, "Invalid account id\n");
		return 1;
	}
	memcpy(info.psn_account_id, account_id, sizeof(account_id));
	info.pin = pin;
	info.console_pin = pin;
	info.holepunch_info = NULL;
	info.rudp = NULL;

	RegistCtx ctx = {0};
	ChiakiRegist regist;
	ChiakiErrorCode err = chiaki_regist_start(&regist, log, &info, regist_cb, &ctx);
	if(err != CHIAKI_ERR_SUCCESS)
	{
		fprintf(stderr, "regist start failed\n");
		return 1;
	}

	signal(SIGINT, on_sigint);
	while(!ctx.result && !g_done)
		usleep(100 * 1000);

	chiaki_regist_stop(&regist);
	chiaki_regist_fini(&regist);

	if(ctx.result != 1)
	{
		fprintf(stderr, "registration failed\n");
		return 1;
	}

	char regist_key_b64[256] = {0};
	char rp_key_b64[128] = {0};
	size_t rk_len = sizeof(regist_key_b64);
	size_t pk_len = sizeof(rp_key_b64);
	// raw bytes -> hex is more robust than the chiaki b64 helper quirks
	for(size_t i = 0; i < CHIAKI_SESSION_AUTH_SIZE; i++)
		sprintf(regist_key_b64 + i * 2, "%02x", (uint8_t)ctx.host.rp_regist_key[i]);
	for(size_t i = 0; i < 0x10; i++)
		sprintf(rp_key_b64 + i * 2, "%02x", ctx.host.rp_key[i]);
	(void)rk_len; (void)pk_len;

	printf("OK\n");
	printf("nickname: %s\n", ctx.host.server_nickname);
	printf("target: %d\n", (int)ctx.host.target);
	printf("regist_key(hex): %s\n", regist_key_b64);
	printf("rp_key(hex): %s\n", rp_key_b64);
	printf("rp_key_type: %u\n", ctx.host.rp_key_type);
	return 0;
}
