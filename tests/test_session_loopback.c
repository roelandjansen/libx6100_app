// SPDX-License-Identifier: GPL-3.0-or-later
//
// Host test: full session over loopback against a minimal fake server
// (control handshake, stream request, CI-V open + data, audio data,
// clean disconnect). Exercises sockets, the network thread and timers.
//

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "core/proto.h"
#include "core/session.h"
#include "core/udp_socket.h"

static int G_FAILS;

#define CHECK(cond) do { \
	if (!(cond)) { \
		fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
		G_FAILS++; \
	} \
} while (0)

#define SRV_ID 0x7a7a7a7au

struct fake_server {
	int fd[3];
	uint16_t port[3];
	struct sockaddr_in peer[3];
	int have_peer[3];
	uint16_t seq[3];
	int civ_open;
	int got_disconnect;
	int got_login;
	int got_request;
	int reject_login;
};

static int srv_socket(uint16_t *port)
{
	struct sockaddr_in sa;
	socklen_t sl = sizeof(sa);
	int fd = socket(AF_INET, SOCK_DGRAM, 0);

	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (fd < 0 || bind(fd, (struct sockaddr *)(void *)&sa, sizeof(sa)) != 0)
		return -1;
	getsockname(fd, (struct sockaddr *)(void *)&sa, &sl);
	*port = ntohs(sa.sin_port);
	return fd;
}

static void srv_send(struct fake_server *f, int i, uint8_t *p, size_t len, int tracked)
{
	if (!f->have_peer[i])
		return;
	if (tracked) {
		f->seq[i]++;
		proto_put_le16(p + PROTO_OFF_SEQ, f->seq[i]);
	}
	sendto(f->fd[i], p, len, 0, (struct sockaddr *)(void *)&f->peer[i], sizeof(f->peer[i]));
}

static void srv_hdr(uint8_t *p, size_t len, uint16_t type)
{
	memset(p, 0, len);
	proto_put_le32(p + PROTO_OFF_LEN, (uint32_t)len);
	proto_put_le16(p + PROTO_OFF_TYPE, type);
	proto_put_le32(p + PROTO_OFF_SENTID, SRV_ID);
}

static void srv_handle_control(struct fake_server *f, const uint8_t *b, size_t len)
{
	uint8_t p[PROTO_CAPABILITIES_SIZE + PROTO_RADIO_CAP_SIZE];
	const uint16_t type = proto_get_le16(b + PROTO_OFF_TYPE);

	if (len == PROTO_CONTROL_SIZE && type == PROTO_TYPE_AREYOUTHERE) {
		srv_hdr(p, PROTO_CONTROL_SIZE, PROTO_TYPE_IAMHERE);
		srv_send(f, 0, p, PROTO_CONTROL_SIZE, 0);
	} else if (len == PROTO_CONTROL_SIZE && type == PROTO_TYPE_READY) {
		srv_hdr(p, PROTO_CONTROL_SIZE, PROTO_TYPE_READY);
		srv_send(f, 0, p, PROTO_CONTROL_SIZE, 0);
	} else if (len == PROTO_CONTROL_SIZE && type == PROTO_TYPE_DISCONNECT) {
		f->got_disconnect++;
	} else if (len == PROTO_LOGIN_SIZE) {
		f->got_login++;
		srv_hdr(p, PROTO_LOGIN_RESPONSE_SIZE, 0);
		memcpy(p + 0x1a, b + 0x1a, 2);
		proto_put_le32(p + 0x1c, 0x12345678u);
		memcpy(p + 0x40, "WFVIEW", 6);
		if (f->reject_login)
			proto_put_le32(p + 0x30, 0xfeffffffu);
		srv_send(f, 0, p, PROTO_LOGIN_RESPONSE_SIZE, 1);
	} else if (len == PROTO_TOKEN_SIZE && b[0x15] == 0x02) {
		srv_hdr(p, sizeof(p), 0);
		proto_put_le16(p + 0x40, 1);
		proto_put_le16(p + PROTO_CAPABILITIES_SIZE + 0x07, 0x8010);
		memcpy(p + PROTO_CAPABILITIES_SIZE + 0x10, "X6100", 5);
		p[PROTO_CAPABILITIES_SIZE + 0x52] = 0x61;
		proto_put_le16(p + PROTO_CAPABILITIES_SIZE + 0x55, 0x8b);
		proto_put_be32(p + PROTO_CAPABILITIES_SIZE + 0x5a, 115200);
		srv_send(f, 0, p, sizeof(p), 1);

		srv_hdr(p, PROTO_CONNINFO_SIZE, 0);
		proto_put_le16(p + 0x27, 0x8010);
		memcpy(p + 0x40, "X6100", 5);
		srv_send(f, 0, p, PROTO_CONNINFO_SIZE, 1);
	} else if (len == PROTO_CONNINFO_SIZE && b[0x15] == 0x03) {
		f->got_request++;
		srv_hdr(p, PROTO_STATUS_SIZE, 0);
		proto_put_be16(p + 0x42, f->port[1]);
		proto_put_be16(p + 0x46, f->port[2]);
		srv_send(f, 0, p, PROTO_STATUS_SIZE, 1);
	}
}

static void srv_handle_stream(struct fake_server *f, int i, const uint8_t *b, size_t len)
{
	uint8_t p[0x40];
	const uint16_t type = proto_get_le16(b + PROTO_OFF_TYPE);

	if (len == PROTO_CONTROL_SIZE && type == PROTO_TYPE_AREYOUTHERE) {
		srv_hdr(p, PROTO_CONTROL_SIZE, PROTO_TYPE_IAMHERE);
		srv_send(f, i, p, PROTO_CONTROL_SIZE, 0);
	} else if (len == PROTO_CONTROL_SIZE && type == PROTO_TYPE_READY) {
		srv_hdr(p, PROTO_CONTROL_SIZE, PROTO_TYPE_READY);
		srv_send(f, i, p, PROTO_CONTROL_SIZE, 0);
	} else if (i == 1 && len == PROTO_OPENCLOSE_SIZE && b[0x15] == 0x04) {
		f->civ_open = 1;
	}
}

static void srv_poll(struct fake_server *f, int ms)
{
	struct pollfd pfd[3];
	int i;

	for (i = 0; i < 3; i++) {
		pfd[i].fd = f->fd[i];
		pfd[i].events = POLLIN;
	}

	if (poll(pfd, 3, ms) <= 0)
		return;

	for (i = 0; i < 3; i++) {
		uint8_t b[PROTO_MAX_PACKET];
		struct sockaddr_in from;
		socklen_t fl = sizeof(from);
		ssize_t n;

		if (!(pfd[i].revents & POLLIN))
			continue;
		n = recvfrom(f->fd[i], b, sizeof(b), 0, (struct sockaddr *)(void *)&from, &fl);
		if (n < PROTO_CONTROL_SIZE)
			continue;
		f->peer[i] = from;
		f->have_peer[i] = 1;
		if (i == 0)
			srv_handle_control(f, b, (size_t)n);
		else
			srv_handle_stream(f, i, b, (size_t)n);
	}
}

static void srv_send_data(struct fake_server *f)
{
	uint8_t p[0x40];
	const uint8_t civ[] = { 0xfe, 0xfe, 0xe0, 0x61, 0x03, 0x00, 0x00, 0x10, 0x14, 0x00, 0xfd };

	if (f->civ_open) {
		srv_hdr(p, 0x15 + sizeof(civ), 0);
		p[0x10] = 0xc1;
		proto_put_le16(p + 0x11, sizeof(civ));
		memcpy(p + 0x15, civ, sizeof(civ));
		srv_send(f, 1, p, 0x15 + sizeof(civ), 1);
	}

	if (f->have_peer[2]) {
		srv_hdr(p, 0x18 + 0x20, 0);
		srv_send(f, 2, p, 0x18 + 0x20, 1);
	}
}

int main(void)
{
	struct fake_server f;
	struct profile p;
	struct session *s = NULL;
	struct session_info info;
	int64_t until;
	int i;

	memset(&f, 0, sizeof(f));
	for (i = 0; i < 3; i++) {
		f.fd[i] = srv_socket(&f.port[i]);
		CHECK(f.fd[i] >= 0);
	}

	memset(&p, 0, sizeof(p));
	snprintf(p.host, sizeof(p.host), "%s", "127.0.0.1");
	p.port_control = f.port[0];
	snprintf(p.username, sizeof(p.username), "%s", "user");
	snprintf(p.password, sizeof(p.password), "%s", "pass");
	snprintf(p.client_name, sizeof(p.client_name), "%s", "test");
	p.rx_codec = 0x04;
	p.rx_sample = 48000;
	p.rx_latency = 150;

	CHECK(session_connect(&s, &p, NULL) == 0);

	// Run the fake server for up to 3 s, sending data every 20 ms once open
	until = clock_mono_ms() + 3000;
	while (clock_mono_ms() < until) {
		srv_poll(&f, 20);
		srv_send_data(&f);
		session_get_info(s, &info);
		if (info.state == SESSION_CONNECTED && info.civ_frames > 5 && info.audio_packets > 5)
			break;
	}

	session_get_info(s, &info);
	CHECK(f.got_login == 1);
	CHECK(f.got_request == 1);
	CHECK(f.civ_open == 1);
	CHECK(info.state == SESSION_CONNECTED);
	CHECK(info.civ_addr == 0x61);
	CHECK(strcmp(info.radio_name, "X6100") == 0);
	CHECK(strcmp(info.connection, "WFVIEW") == 0);
	CHECK(info.civ_frames > 5);
	CHECK(info.audio_packets > 5);

	session_disconnect(s);

	// Collect the disconnect packets (control, CI-V and audio each send 0x05)
	until = clock_mono_ms() + 500;
	while (clock_mono_ms() < until)
		srv_poll(&f, 20);
	CHECK(f.got_disconnect >= 1);

	// Second run: server rejects the login; reason and wfview's text must reach the snapshot
	f.reject_login = 1;
	f.got_login = 0;
	CHECK(session_connect(&s, &p, NULL) == 0);
	until = clock_mono_ms() + 3000;
	while (clock_mono_ms() < until) {
		srv_poll(&f, 20);
		session_get_info(s, &info);
		if (info.state == SESSION_FAILED)
			break;
	}
	session_get_info(s, &info);
	CHECK(f.got_login == 1);
	CHECK(info.state == SESSION_FAILED);
	CHECK(info.reason == SESSION_REASON_AUTH);
	CHECK(strcmp(info.status, "Invalid Username/Password") == 0);
	session_disconnect(s);

	for (i = 0; i < 3; i++)
		close(f.fd[i]);

	if (G_FAILS != 0) {
		fprintf(stderr, "test_session_loopback: %d failure(s)\n", G_FAILS);
		return EXIT_FAILURE;
	}

	return EXIT_SUCCESS;
}
