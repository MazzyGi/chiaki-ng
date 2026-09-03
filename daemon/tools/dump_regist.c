// dump the exact bytes chiaki would send for a regist request
#include <chiaki/regist.h>
#include <chiaki/rpcrypt.h>
#include <chiaki/base64.h>
#include <chiaki/session.h>
#include <chiaki/log.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

// mirror of lib/src/regist.c statics
static const char *const request_head_fmt =
	"POST %s HTTP/1.1\r\n HTTP/1.1\r\n"
	"HOST: %s\r\n"
	"User-Agent: remoteplay Windows\r\n"
	"Connection: close\r\n"
	"Content-Length: %llu\r\n";
static const char *request_path_ps5 = "/sie/ps5/rp/sess/rgst";
static const char *request_rp_version_fmt = "RP-Version: %s\r\n";
static const char *request_tail = "\r\n";
static const char *const request_inner_account_id_fmt =
	"Client-Type: %s\r\n"
	"Np-AccountId: %s\r\n";
const char *chiaki_client_type = "dabfa2ec873de5839bee8d3f4c0239c4282c07c25c6077a2931afcf0adc0d34f";

int main(int argc, char **argv)
{
	const char *account_id_b64 = "pxYafcW4PiM=";
	uint32_t pin = argc > 1 ? strtoul(argv[1], NULL, 10) : 70002262;

	ChiakiLog log;
	chiaki_log_init(&log, CHIAKI_LOG_ALL, NULL, NULL);

	uint8_t psn_account_id[CHIAKI_PSN_ACCOUNT_ID_SIZE];
	size_t decoded = sizeof(psn_account_id);
	chiaki_base64_decode(account_id_b64, strlen(account_id_b64), psn_account_id, &decoded);

	uint8_t ambassador[CHIAKI_RPCRYPT_KEY_SIZE];
	// deterministic ambassador so both sides produce identical bytes
	memset(ambassador, 0x42, sizeof(ambassador));

	uint8_t payload[0x400];
	size_t payload_size = sizeof(payload);

	// exactly what chiaki_regist_request_payload_format does for PS5_1
	const size_t inner_header_off = 0x1e0;
	memset(payload, 'A', inner_header_off);
	size_t key_0_off = payload[0x18D] & 0x1F;
	size_t key_1_off = payload[0] >> 3;
	printf("key_0_off=%zu key_1_off=%zu\n", key_0_off, key_1_off);

	ChiakiRPCrypt crypt;
	chiaki_rpcrypt_init_regist(&crypt, CHIAKI_TARGET_PS5_1, ambassador, key_0_off, pin);
	uint8_t aeropause[0x10];
	chiaki_rpcrypt_aeropause(CHIAKI_TARGET_PS5_1, key_1_off, aeropause, ambassador);
	memcpy(payload + 0xc7, aeropause + 8, 8);
	memcpy(payload + 0x191, aeropause, 8);

	char account_b64[CHIAKI_PSN_ACCOUNT_ID_SIZE * 2];
	chiaki_base64_encode(psn_account_id, CHIAKI_PSN_ACCOUNT_ID_SIZE, account_b64, sizeof(account_b64));
	int inner = snprintf((char *)payload + inner_header_off, sizeof(payload) - inner_header_off,
		request_inner_account_id_fmt, chiaki_client_type, account_b64);
	chiaki_rpcrypt_encrypt(&crypt, 0, payload + inner_header_off, payload + inner_header_off, inner);
	payload_size = inner_header_off + inner;

	printf("payload_size=%zu\n", payload_size);
	printf("payload[0x1e0..0x1ff] (encrypted inner): ");
	for (size_t i = 0x1e0; i < 0x1e0 + 16 && i < payload_size; i++)
		printf("%02x", payload[i]);
	printf("\n");

	char header[0x100];
	int cur = snprintf(header, sizeof(header), request_head_fmt, request_path_ps5, "192.168.8.243", (unsigned long long)payload_size);
	cur += snprintf(header + cur, sizeof(header) - cur, request_rp_version_fmt, chiaki_rp_version_string(CHIAKI_TARGET_PS5_1));
	size_t tail = strlen(request_tail) + 1;
	memcpy(header + cur, request_tail, tail);
	cur += (int)tail - 1;
	printf("header (%d bytes):\n%s\n", cur, header);

	FILE *f = fopen("/tmp/opencode/chiaki_payload.bin", "wb");
	fwrite(payload, 1, payload_size, f);
	fclose(f);
	f = fopen("/tmp/opencode/chiaki_header.bin", "wb");
	fwrite(header, 1, cur, f);
	fclose(f);
	printf("saved to /tmp/opencode/chiaki_payload.bin + chiaki_header.bin\n");
	return 0;
}
