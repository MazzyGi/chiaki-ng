// standalone direct-TCP regist: skips the UDP SRC3 search entirely.
// etaHEN PS5 does not answer the UDP search but the TCP regist port works.
#include <chiaki/regist.h>
#include <chiaki/rpcrypt.h>
#include <chiaki/base64.h>
#include <chiaki/session.h>
#include <chiaki/log.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>

static const char *request_path_ps5 = "/sie/ps5/rp/sess/rgst";

static const char *request_head_fmt =
	"POST %s HTTP/1.1\r\n HTTP/1.1\r\n"
	"HOST: %s\r\n"
	"User-Agent: remoteplay Windows\r\n"
	"Connection: close\r\n"
	"Content-Length: %llu\r\n";

static const char *request_rp_version_fmt = "RP-Version: %s\r\n";
static const char *request_tail = "\r\n";

static const char *client_type = "dabfa2ec873de5839bee8d3f4c0239c0239c4282c07c25c6077a2931afcf0adc0d34f";

static const char *request_inner_account_id_fmt =
	"Client-Type: %s\r\n"
	"Np-AccountId: %s\r\n";

static void b64(const uint8_t *in, size_t n, char *out, size_t out_max)
{
	static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	size_t o = 0;
	for (size_t i = 0; i < n && o + 4 < out_max; i += 3)
	{
		uint32_t v = (uint32_t)in[i] << 16;
		if (i + 1 < n) v |= (uint32_t)in[i+1] << 8;
		if (i + 2 < n) v |= in[i+2];
		out[o++] = tbl[(v >> 18) & 63];
		out[o++] = tbl[(v >> 12) & 63];
		out[o++] = (i + 1 < n) ? tbl[(v >> 6) & 63] : '=';
		out[o++] = (i + 2 < n) ? tbl[v & 63] : '=';
	}
	out[o] = 0;
}

int main(int argc, char **argv)
{
	const char *host = "192.168.8.166";
	const char *account_id_b64 = "pxYafcW4PiM=";
	uint32_t pin = 70002262;
	const char *local_addr = "192.168.8.243";
	if (argc > 1) host = argv[1];
	if (argc > 2) account_id_b64 = argv[2];
	if (argc > 3) pin = (uint32_t)strtoul(argv[3], NULL, 10);
	if (argc > 4) local_addr = argv[4];

	ChiakiLog log;
	chiaki_log_init(&log, CHIAKI_LOG_ALL, NULL, NULL);

	uint8_t psn_account_id[CHIAKI_PSN_ACCOUNT_ID_SIZE];
	size_t decoded = sizeof(psn_account_id);
	if (chiaki_base64_decode(account_id_b64, strlen(account_id_b64), psn_account_id, &decoded) != CHIAKI_ERR_SUCCESS
		|| decoded != CHIAKI_PSN_ACCOUNT_ID_SIZE)
	{ fprintf(stderr, "bad account id\n"); return 1; }

	uint8_t ambassador[CHIAKI_RPCRYPT_KEY_SIZE];
	chiaki_random_bytes_crypt(ambassador, sizeof(ambassador));

	uint8_t payload[0x400];
	size_t payload_size = sizeof(payload);
	const size_t inner_header_off = 0x1e0;
	memset(payload, 'A', inner_header_off);

	ChiakiRPCrypt crypt;
	size_t key_0_off = payload[0x18D] & 0x1F;
	size_t key_1_off = payload[0] >> 3;
	uint8_t aeropause[0x10];
	ChiakiErrorCode err = chiaki_rpcrypt_init_regist(&crypt, CHIAKI_TARGET_PS5_1, ambassador, key_0_off, pin);
	if (err != CHIAKI_ERR_SUCCESS) { fprintf(stderr, "rpcrypt init failed\n"); return 1; }
	err = chiaki_rpcrypt_aeropause(CHIAKI_TARGET_PS5_1, key_1_off, aeropause, ambassador);
	if (err != CHIAKI_ERR_SUCCESS) { fprintf(stderr, "aeropause failed\n"); return 1; }
	memcpy(payload + 0xc7, aeropause + 8, 8);
	memcpy(payload + 0x191, aeropause, 8);

	char account_b64[CHIAKI_PSN_ACCOUNT_ID_SIZE * 2];
	b64(psn_account_id, CHIAKI_PSN_ACCOUNT_ID_SIZE, account_b64, sizeof(account_b64));
	static const char *real_client_type = "dabfa2ec873de5839bee8d3f4c0239c4282c07c25c6077a2931afcf0adc0d34f";
	int inner = snprintf((char *)payload + inner_header_off, sizeof(payload) - inner_header_off,
		request_inner_account_id_fmt, real_client_type, account_b64);
	err = chiaki_rpcrypt_encrypt(&crypt, 0, payload + inner_header_off, payload + inner_header_off, inner);
	if (err != CHIAKI_ERR_SUCCESS) { fprintf(stderr, "encrypt failed\n"); return 1; }
	payload_size = inner_header_off + inner;

	char header[0x100];
	int cur = snprintf(header, sizeof(header), request_head_fmt, request_path_ps5, local_addr, (unsigned long long)payload_size);
	const char *rp_ver = chiaki_rp_version_string(CHIAKI_TARGET_PS5_1);
	cur += snprintf(header + cur, sizeof(header) - cur, request_rp_version_fmt, rp_ver);
	size_t tail = strlen(request_tail) + 1;
	memcpy(header + cur, request_tail, tail);
	cur += (int)tail - 1;

	int sock = socket(AF_INET, SOCK_STREAM, 0);
	struct sockaddr_in addr = {0};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(9295);
	inet_pton(AF_INET, host, &addr.sin_addr);
	if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0)
	{ perror("connect"); return 1; }
	printf("connected to %s:9295, sending %d header + %zu payload bytes (rp-ver %s)\n",
		host, cur, payload_size, rp_ver);
	send(sock, header, cur, 0);
	send(sock, payload, payload_size, 0);

	uint8_t resp[4096];
	ssize_t total = 0;
	while (total < (ssize_t)sizeof(resp) - 1)
	{
		ssize_t n = recv(sock, resp + total, sizeof(resp) - 1 - total, 0);
		if (n <= 0) break;
		total += n;
	}
	resp[total] = 0;
	printf("---- response (%zd bytes) ----\n", total);
	fwrite(resp, 1, total < 400 ? total : 400, stdout);
	printf("\n----------------\n");
	close(sock);
	return total > 0 ? 0 : 1;
}
