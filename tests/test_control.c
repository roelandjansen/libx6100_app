// SPDX-License-Identifier: GPL-3.0-or-later
//
// Host test: control stream against a simulated server
// (I am here, ready, login, token, capabilities, conninfo, status)
//

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/control.h"
#include "core/passcode.h"
#include "core/proto.h"

static int G_FAILS;

#define CHECK(cond) do { \
	if (!(cond)) { \
		fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
		G_FAILS++; \
	} \
} while (0)

#define C_MAX_OUT 32

struct capture {
	size_t n;
	size_t len[C_MAX_OUT];
	uint8_t buf[C_MAX_OUT][PROTO_MAX_PACKET];
};

struct evlog {
	int alloc;
	int open;
	int close;
	uint16_t civ_port;
	uint16_t audio_port;
	enum control_reason last_error;
	char status[CONTROL_STATUS_LEN];
};

static struct capture G_CAP;
static struct evlog G_EV;
static struct control G_C;

static int cap_send(void *ctx, const uint8_t *buf, size_t len)
{
	struct capture *c = ctx;

	if (c->n < C_MAX_OUT) {
		memcpy(c->buf[c->n], buf, len);
		c->len[c->n] = len;
	}
	c->n++;
	return 0;
}

static uint32_t day_ms(void *ctx)
{
	(void)ctx;
	return 1000u;
}

static int ev_alloc(void *ctx, uint16_t *civ, uint16_t *audio)
{
	(void)ctx;
	G_EV.alloc++;
	*civ = 40001;
	*audio = 40002;
	return 0;
}

static void ev_open(void *ctx, uint16_t civ, uint16_t audio)
{
	(void)ctx;
	G_EV.open++;
	G_EV.civ_port = civ;
	G_EV.audio_port = audio;
}

static void ev_close(void *ctx)
{
	(void)ctx;
	G_EV.close++;
}

static void ev_status(void *ctx, const char *msg)
{
	(void)ctx;
	snprintf(G_EV.status, sizeof(G_EV.status), "%s", msg);
}

static void ev_error(void *ctx, enum control_reason r, const char *msg)
{
	(void)ctx;
	(void)msg;
	G_EV.last_error = r;
}

static uint16_t ev_rand(void *ctx)
{
	(void)ctx;
	return 0xbeef;
}

static const struct udp_io C_IO = { cap_send, day_ms, &G_CAP };
static const struct control_events C_EV = { ev_alloc, ev_open, ev_close, ev_status, ev_error, ev_rand, NULL };

static size_t last(void)
{
	return G_CAP.n - 1;
}

static void hdr(uint8_t *p, size_t len, uint16_t type, uint16_t seq)
{
	memset(p, 0, len);
	proto_put_le32(p + PROTO_OFF_LEN, (uint32_t)len);
	proto_put_le16(p + PROTO_OFF_TYPE, type);
	proto_put_le16(p + PROTO_OFF_SEQ, seq);
	proto_put_le32(p + PROTO_OFF_SENTID, 0x5e5e5e5eu);
	proto_put_le32(p + PROTO_OFF_RCVDID, 0x01020304u);
}

static void start(void)
{
	struct control_cfg cfg;
	uint8_t p[PROTO_CONTROL_SIZE];

	memset(&G_CAP, 0, sizeof(G_CAP));
	memset(&G_EV, 0, sizeof(G_EV));
	memset(&cfg, 0, sizeof(cfg));
	snprintf(cfg.username, sizeof(cfg.username), "%s", "ab");
	snprintf(cfg.password, sizeof(cfg.password), "%s", "~~");
	snprintf(cfg.client_name, sizeof(cfg.client_name), "%s", "zeetak");
	cfg.rx_codec = 0x40;
	cfg.tx_codec = 0x04;
	cfg.rx_sample = 48000;
	cfg.tx_sample = 48000;
	cfg.tx_latency = 150;

	control_init(&G_C, &cfg, &C_IO, &C_EV, 0x01020304u, 0);

	// are-you-there after 500 ms
	control_tick(&G_C, 500);
	CHECK(G_CAP.n == 1);

	// I am here -> are-you-ready
	hdr(p, sizeof(p), PROTO_TYPE_IAMHERE, 0);
	control_receive(&G_C, p, sizeof(p), 510);
	CHECK(G_C.state == CTL_STATE_READY_WAIT);
	CHECK(G_C.s.remote_id == 0x5e5e5e5eu);

	// I am ready -> login
	hdr(p, sizeof(p), PROTO_TYPE_READY, 1);
	control_receive(&G_C, p, sizeof(p), 520);
	CHECK(G_C.state == CTL_STATE_LOGIN);
}

static void test_login_packet(void)
{
	const uint8_t *l;

	start();
	l = G_CAP.buf[last()];

	CHECK(G_CAP.len[last()] == PROTO_LOGIN_SIZE);
	CHECK(proto_get_le32(l + PROTO_OFF_LEN) == PROTO_LOGIN_SIZE);
	CHECK(proto_get_le16(l + PROTO_OFF_TYPE) == 0);
	CHECK(proto_get_be32(l + 0x10) == PROTO_LOGIN_SIZE - 0x10);
	CHECK(l[0x14] == 0x01);
	CHECK(l[0x15] == 0x00);
	CHECK(proto_get_be16(l + 0x16) == 0x30);
	CHECK(proto_get_le16(l + 0x1a) == 0xbeef);
	CHECK(l[0x40] == 0x38 && l[0x41] == 0x2e && l[0x42] == 0);
	CHECK(l[0x50] == 0x52 && l[0x51] == 0x47);
	CHECK(memcmp(l + 0x60, "zeetak-wfview", 13) == 0);
	CHECK(proto_get_le32(l + PROTO_OFF_SENTID) == 0x01020304u);
	CHECK(proto_get_le32(l + PROTO_OFF_RCVDID) == 0x5e5e5e5eu);
}

static void login_response(uint32_t error, uint16_t tokreq, const char *conn)
{
	uint8_t p[PROTO_LOGIN_RESPONSE_SIZE];

	hdr(p, sizeof(p), 0, 5);
	proto_put_le16(p + 0x1a, tokreq);
	proto_put_le32(p + 0x1c, 0xcafef00du);
	proto_put_le32(p + 0x30, error);
	memcpy(p + 0x40, conn, strlen(conn));
	control_receive(&G_C, p, sizeof(p), 600);
}

static void test_bad_password(void)
{
	start();
	login_response(0xfeffffffu, 0xbeef, "WFVIEW");
	CHECK(G_EV.last_error == CTL_REASON_AUTH);
	CHECK(G_C.state == CTL_STATE_FAILED);
}

static void test_full_connect(void)
{
	uint8_t caps[PROTO_CAPABILITIES_SIZE + PROTO_RADIO_CAP_SIZE];
	uint8_t ci[PROTO_CONNINFO_SIZE];
	uint8_t st[PROTO_STATUS_SIZE];
	uint8_t *r;
	const uint8_t *q;
	size_t n_before;

	start();

	// Non-wfview connection forces Opus to LPCM16
	login_response(0, 0xbeef, "FTTH");
	CHECK(G_C.cfg.rx_codec == 0x04);
	CHECK(G_C.high_bandwidth);
	CHECK(G_C.authenticated);
	CHECK(G_C.token == 0xcafef00du);
	CHECK(strcmp(G_EV.status, "Radio Login OK!") == 0);

	// Token confirm
	q = G_CAP.buf[last()];
	CHECK(G_CAP.len[last()] == PROTO_TOKEN_SIZE);
	CHECK(q[0x15] == 0x02);
	CHECK(proto_get_be16(q + 0x16) == 0x31);
	CHECK(proto_get_le32(q + 0x1c) == 0xcafef00du);
	CHECK(proto_get_be16(q + 0x24) == 0x0798);
	CHECK(G_C.t_token == 600 + PROTO_TOKEN_RENEWAL);

	// Capabilities, one radio identified by MAC, CI-V 0x61, TX capable
	hdr(caps, sizeof(caps), 0, 6);
	proto_put_le16(caps + 0x40, 1);
	r = caps + PROTO_CAPABILITIES_SIZE;
	proto_put_le16(r + 0x07, 0x8010);
	memcpy(r + 0x0a, "\x11\x22\x33\x44\x55\x66", 6);
	memcpy(r + 0x10, "X6100", 5);
	r[0x52] = 0x61;
	proto_put_le16(r + 0x55, 0x8b);
	proto_put_be32(r + 0x5a, 115200);
	control_receive(&G_C, caps, sizeof(caps), 610);
	CHECK(G_C.n_radios == 1);
	CHECK(G_C.radios[0].civ == 0x61);
	CHECK(strcmp(G_C.radios[0].name, "X6100") == 0);

	// Conninfo, not busy -> ports allocated, stream request
	hdr(ci, sizeof(ci), 0, 7);
	proto_put_le16(ci + 0x27, 0x8010);
	memcpy(ci + 0x2a, "\x11\x22\x33\x44\x55\x66", 6);
	memcpy(ci + 0x40, "X6100", 5);
	n_before = G_CAP.n;
	control_receive(&G_C, ci, sizeof(ci), 620);
	CHECK(G_EV.alloc == 1);
	CHECK(G_CAP.n == n_before + 1);
	CHECK(G_C.state == CTL_STATE_STREAM_REQ);
	CHECK(G_C.baudrate == 115200);

	q = G_CAP.buf[last()];
	CHECK(G_CAP.len[last()] == PROTO_CONNINFO_SIZE);
	CHECK(q[0x15] == 0x03);
	CHECK(proto_get_le16(q + 0x27) == 0x8010);
	CHECK(memcmp(q + 0x2a, "\x11\x22\x33\x44\x55\x66", 6) == 0);
	CHECK(memcmp(q + 0x40, "X6100", 5) == 0);
	CHECK(q[0x60] == 0x38);
	CHECK(q[0x70] == 1);
	CHECK(q[0x71] == 1);
	CHECK(q[0x72] == 0x04);
	CHECK(q[0x73] == 0x04);
	CHECK(proto_get_be32(q + 0x74) == 48000);
	CHECK(proto_get_be32(q + 0x7c) == 40001);
	CHECK(proto_get_be32(q + 0x80) == 40002);
	CHECK(proto_get_be32(q + 0x84) == 150);
	CHECK(q[0x88] == 1);

	// Status with the server ports -> open streams
	hdr(st, sizeof(st), 0, 8);
	proto_put_be16(st + 0x42, 50092);
	proto_put_be16(st + 0x46, 50093);
	control_receive(&G_C, st, sizeof(st), 630);
	CHECK(G_EV.open == 1);
	CHECK(G_EV.civ_port == 50092);
	CHECK(G_EV.audio_port == 50093);
	CHECK(G_C.state == CTL_STATE_STREAMS_OPEN);

	// Token renewal after 60 s
	n_before = G_CAP.n;
	control_tick(&G_C, 600 + PROTO_TOKEN_RENEWAL);
	CHECK(G_CAP.n > n_before);
	CHECK(G_CAP.buf[n_before][0x15] == 0x05);

	// Radio disconnected -> close streams
	hdr(st, sizeof(st), 0, 9);
	st[0x40] = 0x01;
	control_receive(&G_C, st, sizeof(st), 640);
	CHECK(G_EV.close == 1);
	CHECK(G_EV.last_error == CTL_REASON_RADIO_GONE);

	// Shutdown: disconnect sent last
	control_shutdown(&G_C, 650);
	CHECK(proto_get_le16(G_CAP.buf[last()] + PROTO_OFF_TYPE) == PROTO_TYPE_DISCONNECT);
	CHECK(G_C.state == CTL_STATE_CLOSED);
}

static void test_busy(void)
{
	uint8_t caps[PROTO_CAPABILITIES_SIZE + PROTO_RADIO_CAP_SIZE];
	uint8_t ci[PROTO_CONNINFO_SIZE];

	start();
	login_response(0, 0xbeef, "WFVIEW");

	hdr(caps, sizeof(caps), 0, 6);
	memcpy(caps + PROTO_CAPABILITIES_SIZE + 0x10, "X6100", 5);
	control_receive(&G_C, caps, sizeof(caps), 610);

	hdr(ci, sizeof(ci), 0, 7);
	proto_put_le32(ci + 0x60, 1);
	memcpy(ci + 0x64, "shack-pc", 8);
	memcpy(ci + 0x84, "\xc0\xa8\x01\x14", 4);
	control_receive(&G_C, ci, sizeof(ci), 620);

	CHECK(G_EV.alloc == 0);
	CHECK(G_C.reason == CTL_REASON_IN_USE);
	CHECK(strstr(G_EV.status, "in use by: shack-pc (192.168.1.20)") != NULL);
	CHECK(proto_get_le16(G_CAP.buf[last()] + PROTO_OFF_TYPE) == PROTO_TYPE_IDLE);
	CHECK(proto_get_le16(G_CAP.buf[last()] + PROTO_OFF_SEQ) == 7);
	CHECK(G_C.cfg.rx_codec == 0x40);
}

static void test_not_responding(void)
{
	struct control_cfg cfg;
	int64_t t;

	memset(&G_CAP, 0, sizeof(G_CAP));
	memset(&G_EV, 0, sizeof(G_EV));
	memset(&cfg, 0, sizeof(cfg));
	control_init(&G_C, &cfg, &C_IO, &C_EV, 1, 0);

	// 19 are-you-there: no message yet
	for (t = 500; t <= 19 * 500; t += 500)
		control_tick(&G_C, t);
	CHECK(G_C.s.ayt_count == 19);
	CHECK(!G_C.ayt_warned);
	CHECK(G_EV.status[0] == '\0');

	// 20th: message once, and it keeps trying
	control_tick(&G_C, 20 * 500);
	CHECK(strcmp(G_EV.status, "Radio not responding!") == 0);
	G_EV.status[0] = '\0';
	for (t = 21 * 500; t <= 30 * 500; t += 500)
		control_tick(&G_C, t);
	CHECK(G_EV.status[0] == '\0');
	CHECK(G_C.state == CTL_STATE_AYT);
	CHECK(proto_get_le16(G_CAP.buf[last()] + PROTO_OFF_TYPE) == PROTO_TYPE_AREYOUTHERE);
}

int main(void)
{
	test_not_responding();
	test_login_packet();
	test_bad_password();
	test_full_connect();
	test_busy();

	if (G_FAILS != 0) {
		fprintf(stderr, "test_control: %d failure(s)\n", G_FAILS);
		return EXIT_FAILURE;
	}

	return EXIT_SUCCESS;
}
