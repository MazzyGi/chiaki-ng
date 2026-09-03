#include "proto.h"
#include "session.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <json-c/json.h>

static int tcp_srv = -1;
static int tcp_client = -1;
static int video_sock = -1;
static int audio_sock = -1;
static struct sockaddr_in client_video_addr, client_audio_addr;
static int client_video_addr_len = 0, client_audio_addr_len = 0;
static pthread_mutex_t net_mutex = PTHREAD_MUTEX_INITIALIZER;
static uint32_t video_counter = 0, audio_counter = 0;
relay_session_t *g_session = NULL; // owned by main.c
static volatile int running = 0;
static uint8_t video_seq = 0;

// ---- outbound UDP ----------------------------------------------------------

static void udp_send(int sock, const struct sockaddr_in *addr, int addr_len,
	const uint8_t *buf, size_t size)
{
	if (sock < 0 || !addr_len || size == 0)
		return;
	// TODO: fragment >1400-byte frames; PS5 H.265 access units can exceed MTU
	sendto(sock, buf, size, 0, (const struct sockaddr *)addr, addr_len);
}

// fragmentation header: [u8 magic][u32 BE counter][u8 frag_id][u8 frag_count][u16 BE size]
#define RELAY_MAX_PAYLOAD 1300

typedef struct __attribute__((packed)) frag_header_t
{
	uint8_t magic;
	uint32_t counter;
	uint8_t frag_id;
	uint8_t frag_count;
	uint16_t size;
} frag_header_t;
#define RELAY_FRAG_HEADER sizeof(frag_header_t) // == 9

static void udp_send_fragmented(int sock, struct sockaddr_in *addr, int addr_len,
	uint8_t magic, uint32_t *counter, const uint8_t *buf, size_t size)
{
	if (sock < 0 || !addr_len || size == 0)
		return;
	uint32_t cnt;
	pthread_mutex_lock(&net_mutex);
	cnt = (*counter)++;
	pthread_mutex_unlock(&net_mutex);

	int frag_count = (int)((size + RELAY_MAX_PAYLOAD - 1) / RELAY_MAX_PAYLOAD);
	if (frag_count > 255)
		return; // drop absurdly large frames
	for (int i = 0; i < frag_count; i++)
	{
		size_t off = (size_t)i * RELAY_MAX_PAYLOAD;
		size_t chunk = size - off < RELAY_MAX_PAYLOAD ? size - off : RELAY_MAX_PAYLOAD;
		uint8_t pkt[RELAY_FRAG_HEADER + RELAY_MAX_PAYLOAD];
		frag_header_t *h = (frag_header_t *)pkt;
		h->magic = magic;
		h->counter = htonl(cnt);
		h->frag_id = (uint8_t)i;
		h->frag_count = (uint8_t)frag_count;
		h->size = htons((uint16_t)chunk);
		memcpy(pkt + RELAY_FRAG_HEADER, buf + off, chunk);
		sendto(sock, pkt, RELAY_FRAG_HEADER + chunk, 0,
			(const struct sockaddr *)addr, addr_len);
	}
}

void relay_net_send_video(const uint8_t *buf, size_t size)
{
	udp_send_fragmented(video_sock, &client_video_addr, client_video_addr_len,
		RELAY_VIDEO_MAGIC, &video_counter, buf, size);
}

void relay_net_send_audio(const uint8_t *buf, size_t size)
{
	// opus frames are small - single packet
	if (audio_sock < 0 || !client_audio_addr_len || size == 0 || size > 1400)
		return;
	uint32_t cnt;
	pthread_mutex_lock(&net_mutex);
	cnt = audio_counter++;
	pthread_mutex_unlock(&net_mutex);
	uint8_t pkt[3 + 4 + 1400];
	pkt[0] = RELAY_AUDIO_MAGIC;
	uint32_t n = htonl(cnt);
	memcpy(pkt + 1, &n, 4);
	uint16_t sz = htons((uint16_t)size);
	memcpy(pkt + 5, &sz, 2);
	memcpy(pkt + 7, buf, size);
	sendto(audio_sock, pkt, 7 + size, 0,
		(const struct sockaddr *)&client_audio_addr, client_audio_addr_len);
}

void relay_net_set_audio_info(unsigned channels, unsigned rate)
{
	pthread_mutex_lock(&net_mutex);
	g_session ? (g_session->audio_channels = channels, g_session->audio_rate = rate, 0) : 0;
	pthread_mutex_unlock(&net_mutex);
}

void relay_net_session_ended(void)
{
	// notify client over tcp
	char msg[] = "{\"type\":\"session_ended\"}\n";
	pthread_mutex_lock(&net_mutex);
	if (tcp_client >= 0)
		write(tcp_client, msg, sizeof(msg) - 1);
	pthread_mutex_unlock(&net_mutex);
}

// ---- TCP control channel ----------------------------------------------------

static void tcp_send_line(const char *line)
{
	pthread_mutex_lock(&net_mutex);
	if (tcp_client >= 0)
	{
		write(tcp_client, line, strlen(line));
		write(tcp_client, "\n", 1);
	}
	pthread_mutex_unlock(&net_mutex);
}

static void send_welcome(void)
{
	char buf[512];
	snprintf(buf, sizeof(buf),
		"{\"type\":\"welcome\",\"video_w\":%u,\"video_h\":%u,\"video_fov\":%u,\"audio_channels\":%u,\"audio_rate\":%u}",
		g_session ? g_session->video_width : 0,
		g_session ? g_session->video_height : 0,
		g_session ? g_session->video_fov : 0,
		g_session ? g_session->audio_channels : 0,
		g_session ? g_session->audio_rate : 0);
	tcp_send_line(buf);
}

static uint8_t clamp_stick(int v)
{
	if (v < -128) v = -128;
	if (v > 127) v = 127;
	return (uint8_t)(int8_t)v;
}

static void handle_ctrl_msg(json_object *obj)
{
	if (!g_session)
		return;
	json_object *o;
	uint32_t buttons = 0;
	int lx = 0, ly = 0, rx = 0, ry = 0, l2 = 0, r2 = 0;
	if (json_object_object_get_ex(obj, "buttons", &o) && json_object_is_type(o, json_type_int))
		buttons = (uint32_t)json_object_get_int(o);
	if (json_object_object_get_ex(obj, "lx", &o) && json_object_is_type(o, json_type_int))
		lx = json_object_get_int(o);
	if (json_object_object_get_ex(obj, "ly", &o) && json_object_is_type(o, json_type_int))
		ly = json_object_get_int(o);
	if (json_object_object_get_ex(obj, "rx", &o) && json_object_is_type(o, json_type_int))
		rx = json_object_get_int(o);
	if (json_object_object_get_ex(obj, "ry", &o) && json_object_is_type(o, json_type_int))
		ry = json_object_get_int(o);
	if (json_object_object_get_ex(obj, "l2", &o) && json_object_is_type(o, json_type_int))
		l2 = json_object_get_int(o);
	if (json_object_object_get_ex(obj, "r2", &o) && json_object_is_type(o, json_type_int))
		r2 = json_object_get_int(o);
	relay_session_set_controller(g_session, buttons,
		(int8_t)clamp_stick(lx), (int8_t)clamp_stick(ly),
		(int8_t)clamp_stick(rx), (int8_t)clamp_stick(ry),
		(uint8_t)l2, (uint8_t)r2);
}

static void *client_thread(void *arg)
{
	int fd = (int)(intptr_t)arg;
	char buf[4096];
	size_t used = 0;
	while (running)
	{
		ssize_t n = read(fd, buf + used, sizeof(buf) - used - 1);
		if (n <= 0)
			break;
		used += (size_t)n;
		buf[used] = 0;
		char *nl;
		while ((nl = strchr(buf, '\n')) != NULL)
		{
			*nl = 0;
			json_object *obj = json_tokener_parse(buf);
			if (obj)
			{
				json_object *o = NULL;
				const char *type = json_object_object_get_ex(obj, "type", &o) && json_object_is_type(o, json_type_string)
					? json_object_get_string(o) : NULL;
				if (type && !strcmp(type, "ctrl"))
					handle_ctrl_msg(obj);
				else if (type && !strcmp(type, "ping"))
					tcp_send_line("{\"type\":\"pong\"}");
				json_object_put(obj);
			}
			size_t consumed = (size_t)(nl - buf) + 1;
			memmove(buf, nl + 1, used - consumed);
			used -= consumed;
		}
		if (used >= sizeof(buf) - 1)
			used = 0; // line too long - drop
	}
	CHIAKI_LOGI(NULL, "relay: control client disconnected");
	pthread_mutex_lock(&net_mutex);
	if (tcp_client == fd)
	{
		close(tcp_client);
		tcp_client = -1;
	}
	pthread_mutex_unlock(&net_mutex);
	close(fd);
	return NULL;
}

static void *accept_thread(void *arg)
{
	(void)arg;
	while (running)
	{
		struct sockaddr_in caddr;
		socklen_t clen = sizeof(caddr);
		int fd = accept(tcp_srv, (struct sockaddr *)&caddr, &clen);
		if (fd < 0)
		{
			if (!running) break;
			continue;
		}
		pthread_mutex_lock(&net_mutex);
		if (tcp_client >= 0)
		{
			// only one client at a time
			pthread_mutex_unlock(&net_mutex);
			const char busy[] = "{\"type\":\"busy\"}\n";
			write(fd, busy, sizeof(busy) - 1);
			close(fd);
			continue;
		}
		tcp_client = fd;
		// video/audio UDP destinations are learned from client PROBE1 packets
		client_video_addr_len = 0;
		client_audio_addr_len = 0;
		pthread_mutex_unlock(&net_mutex);

		char ip[INET_ADDRSTRLEN];
		inet_ntop(AF_INET, &caddr.sin_addr, ip, sizeof(ip));
		CHIAKI_LOGI(NULL, "relay: control client connected from %s", ip);
		send_welcome();

		pthread_t t;
		pthread_create(&t, NULL, client_thread, (void *)(intptr_t)fd);
		pthread_detach(t);
	}
	return NULL;
}

// ---- UDP probe listener: client punches so we learn its address ------------
// probe payload: "PROBE1 <video_port> <audio_port>" from the client's source
// port; daemon then sends video/audio to that source ip:declared ports.

static void *video_probe_thread(void *arg)
{
	(void)arg;
	uint8_t buf[64];
	while (running)
	{
		struct sockaddr_in from;
		socklen_t flen = sizeof(from);
		ssize_t n = recvfrom(video_sock, buf, sizeof(buf) - 1, 0,
			(struct sockaddr *)&from, &flen);
		if (n <= 0) continue;
		buf[n] = 0;
		if (strncmp((char *)buf, "PROBE1", 6) == 0)
		{
			int vport = 0, aport = 0;
			char tmp[64];
			memcpy(tmp, buf, n < 63 ? n : 63); tmp[n < 63 ? n : 63] = 0;
			if (sscanf(tmp, "PROBE1 %d %d", &vport, &aport) == 2
				&& vport > 0 && vport < 65536 && aport > 0 && aport < 65536)
			{
				pthread_mutex_lock(&net_mutex);
				if (tcp_client >= 0)
				{
					client_video_addr = from;
					client_video_addr.sin_port = htons((uint16_t)vport);
					client_video_addr_len = sizeof(client_video_addr);
					client_audio_addr = from;
					client_audio_addr.sin_port = htons((uint16_t)aport);
					client_audio_addr_len = sizeof(client_audio_addr);
				}
				pthread_mutex_unlock(&net_mutex);
			}
		}
	}
	return NULL;
}

bool relay_net_start(relay_session_t *rs)
{
	g_session = rs;
	running = 1;

	tcp_srv = socket(AF_INET, SOCK_STREAM, 0);
	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_ANY);
	addr.sin_port = htons(RELAY_TCP_PORT);
	int yes = 1;
	setsockopt(tcp_srv, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
	if (bind(tcp_srv, (struct sockaddr *)&addr, sizeof(addr)) < 0 || listen(tcp_srv, 1) < 0)
	{
		perror("relay tcp bind/listen");
		return false;
	}

	video_sock = socket(AF_INET, SOCK_DGRAM, 0);
	addr.sin_port = htons(RELAY_VIDEO_PORT);
	if (bind(video_sock, (struct sockaddr *)&addr, sizeof(addr)) < 0)
	{ perror("relay video udp bind"); return false; }

	audio_sock = socket(AF_INET, SOCK_DGRAM, 0);
	addr.sin_port = htons(RELAY_AUDIO_PORT);
	if (bind(audio_sock, (struct sockaddr *)&addr, sizeof(addr)) < 0)
	{ perror("relay audio udp bind"); return false; }

	pthread_t t;
	pthread_create(&t, NULL, accept_thread, NULL); pthread_detach(t);
	pthread_create(&t, NULL, video_probe_thread, NULL); pthread_detach(t);

	CHIAKI_LOGI(NULL, "relay: listening tcp:%d udp:%d/%d", RELAY_TCP_PORT, RELAY_VIDEO_PORT, RELAY_AUDIO_PORT);
	return true;
}

void relay_net_stop(void)
{
	running = 0;
	if (tcp_srv >= 0) { shutdown(tcp_srv, SHUT_RDWR); close(tcp_srv); tcp_srv = -1; }
	if (tcp_client >= 0) { close(tcp_client); tcp_client = -1; }
	if (video_sock >= 0) { close(video_sock); video_sock = -1; }
	if (audio_sock >= 0) { close(audio_sock); audio_sock = -1; }
}
