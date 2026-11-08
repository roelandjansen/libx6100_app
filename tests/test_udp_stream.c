// SPDX-License-Identifier: GPL-3.0-or-later
//
// Host test: common UDP stream logic (handshake, idle, tracked send,
// retransmit in both directions, ping reply)
//

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/proto.h"
#include "core/udp_stream.h"

static int G_FAILS;

#define CHECK(cond) do { \
	if (!(cond)) { \
		fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
		G_FAILS++; \
	} \
} while (0)

//
// Captured output
//

#define C_MAX_OUT 64

struct capture {
	size_t n;
	size_t len[C_MAX_OUT];
	uint8_t buf[C_MAX_OUT][PROTO_MAX_PACKET];
};

static struct capture G_CAP;
static struct udp_stream G_S;

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

static uint32_t fixed_day_ms(void *ctx)
{
	(void)ctx;
	return 12345678u;
}

static const struct udp_io C_IO = { cap_send, fixed_day_ms, &G_CAP };

static uint16_t out_type(size_t i)
{
	return proto_get_le16(G_CAP.buf[i] + PROTO_OFF_TYPE);
}

static uint16_t out_seq(size_t i)
{
	return proto_get_le16(G_CAP.buf[i] + PROTO_OFF_SEQ);
}

//
// Build an incoming packet
//

static void make_control(uint8_t *p, uint16_t type, uint16_t seq, uint32_t sentid)
{
	memset(p, 0, PROTO_CONTROL_SIZE);
	proto_put_le32(p + PROTO_OFF_LEN, PROTO_CONTROL_SIZE);
	proto_put_le16(p + PROTO_OFF_TYPE, type);
	proto_put_le16(p + PROTO_OFF_SEQ, seq);
	proto_put_le32(p + PROTO_OFF_SENTID, sentid);
}

static void rx_data(uint16_t seq, int64_t now)
{
	uint8_t p[PROTO_CONTROL_SIZE];

	make_control(p, PROTO_TYPE_IDLE, seq, 0x1234);
	udp_stream_receive(&G_S, p, sizeof(p), now);
}

static void test_endpoint_id(void)
{
	// 192.168.1.20 port 50001 -> 0x01 0x14 0xc351
	CHECK(udp_stream_endpoint_id(0xc0a80114u, 50001) == 0x0114c351u);
}

static void test_civ_handshake(void)
{
	uint8_t p[PROTO_CONTROL_SIZE];
	uint32_t ev;

	memset(&G_CAP, 0, sizeof(G_CAP));
	udp_stream_init(&G_S, UDP_STREAM_CIV, &C_IO, 0xaabbccddu, 1000);

	// CI-V sends are-you-there at once
	CHECK(G_CAP.n == 1);
	CHECK(G_CAP.len[0] == PROTO_CONTROL_SIZE);
	CHECK(out_type(0) == PROTO_TYPE_AREYOUTHERE);
	CHECK(out_seq(0) == 0);
	CHECK(proto_get_le32(G_CAP.buf[0] + PROTO_OFF_SENTID) == 0xaabbccddu);

	// I am here: remote id stored, are-you-ready sent with seq 1
	make_control(p, PROTO_TYPE_IAMHERE, 0, 0x11223344u);
	ev = udp_stream_receive(&G_S, p, sizeof(p), 1010);
	CHECK((ev & UDP_EV_IAMHERE) != 0);
	CHECK(G_S.remote_id == 0x11223344u);
	CHECK(G_CAP.n == 2);
	CHECK(out_type(1) == PROTO_TYPE_READY);
	CHECK(out_seq(1) == 1);
	CHECK(G_S.t_ayt == 0);

	make_control(p, PROTO_TYPE_READY, 1, 0x11223344u);
	ev = udp_stream_receive(&G_S, p, sizeof(p), 1020);
	CHECK((ev & UDP_EV_READY) != 0);
}

static void test_control_timers(void)
{
	uint8_t p[PROTO_CONTROL_SIZE];

	memset(&G_CAP, 0, sizeof(G_CAP));
	udp_stream_init(&G_S, UDP_STREAM_CONTROL, &C_IO, 1, 0);

	// Control: nothing at once, are-you-there after 500 ms, no ping or idle yet
	CHECK(G_CAP.n == 0);
	CHECK(G_S.t_ping == 0);
	CHECK(G_S.t_idle == 0);
	udp_stream_tick(&G_S, 499);
	CHECK(G_CAP.n == 0);
	udp_stream_tick(&G_S, 500);
	CHECK(G_CAP.n == 1);
	CHECK(out_type(0) == PROTO_TYPE_AREYOUTHERE);

	// I am here starts ping and idle
	make_control(p, PROTO_TYPE_IAMHERE, 0, 9);
	udp_stream_receive(&G_S, p, sizeof(p), 600);
	CHECK(G_S.t_ping == 1100);
	CHECK(G_S.t_idle == 700);

	// Idle is tracked: seq 0, then 1
	G_CAP.n = 0;
	udp_stream_tick(&G_S, 700);
	CHECK(G_CAP.n == 1);
	CHECK(out_type(0) == PROTO_TYPE_IDLE);
	CHECK(out_seq(0) == 0);
	CHECK(G_S.tx_seq == 1);
	CHECK(G_S.t_idle == 800);

	// A tracked send pushes the idle timer out
	{
		uint8_t d[PROTO_CONTROL_SIZE];

		make_control(d, PROTO_TYPE_IDLE, 0, 0);
		udp_stream_send_tracked(&G_S, d, sizeof(d), 750);
		CHECK(G_S.t_idle == 850);
		CHECK(proto_get_le16(d + PROTO_OFF_SEQ) == 1);
	}
}

static void test_tx_retransmit(void)
{
	uint8_t p[PROTO_CONTROL_SIZE + 8];
	uint8_t d[0x20];
	size_t i;

	memset(&G_CAP, 0, sizeof(G_CAP));
	udp_stream_init(&G_S, UDP_STREAM_AUDIO, &C_IO, 1, 0);

	// Send three tracked packets, seq 0..2
	for (i = 0; i < 3; i++) {
		memset(d, 0, sizeof(d));
		proto_put_le32(d + PROTO_OFF_LEN, sizeof(d));
		d[0x1f] = (uint8_t)(0xa0 + i);
		udp_stream_send_tracked(&G_S, d, sizeof(d), 10);
	}

	// Single request for seq 1: resend identical bytes
	G_CAP.n = 0;
	make_control(p, PROTO_TYPE_RETRANSMIT, 1, 9);
	udp_stream_receive(&G_S, p, PROTO_CONTROL_SIZE, 20);
	CHECK(G_CAP.n == 1);
	CHECK(G_CAP.len[0] == sizeof(d));
	CHECK(G_CAP.buf[0][0x1f] == 0xa1);
	CHECK(out_seq(0) == 1);

	// Multi request for seq 2 (held) and 7 (not held): resend 2, idle with seq 7
	G_CAP.n = 0;
	make_control(p, PROTO_TYPE_RETRANSMIT, 0, 9);
	proto_put_le32(p + PROTO_OFF_LEN, PROTO_CONTROL_SIZE + 4);
	proto_put_le16(p + 0x10, 2);
	proto_put_le16(p + 0x12, 7);
	udp_stream_receive(&G_S, p, PROTO_CONTROL_SIZE + 4, 30);
	CHECK(G_CAP.n == 2);
	CHECK(G_CAP.buf[0][0x1f] == 0xa2);
	CHECK(G_CAP.len[1] == PROTO_CONTROL_SIZE);
	CHECK(out_type(1) == PROTO_TYPE_IDLE);
	CHECK(out_seq(1) == 7);
}

static void test_txbuf_limit(void)
{
	uint8_t d[PROTO_CONTROL_SIZE];
	uint8_t p[PROTO_CONTROL_SIZE];
	int i;

	memset(&G_CAP, 0, sizeof(G_CAP));
	udp_stream_init(&G_S, UDP_STREAM_AUDIO, &C_IO, 1, 0);

	// 600 packets: buffer keeps the last 501 (seq 99..599)
	for (i = 0; i < 600; i++) {
		make_control(d, PROTO_TYPE_IDLE, 0, 0);
		udp_stream_send_tracked(&G_S, d, sizeof(d), 0);
	}
	CHECK(G_S.tx_count == PROTO_BUFSIZE + 1);
	CHECK(G_S.tx_first == 99);

	G_CAP.n = 0;
	make_control(p, PROTO_TYPE_RETRANSMIT, 98, 9);
	udp_stream_receive(&G_S, p, sizeof(p), 0);
	CHECK(G_CAP.n == 0);

	make_control(p, PROTO_TYPE_RETRANSMIT, 99, 9);
	udp_stream_receive(&G_S, p, sizeof(p), 0);
	CHECK(G_CAP.n == 1);
	CHECK(out_seq(0) == 99);
}

static void test_rx_missing(void)
{
	int k;

	memset(&G_CAP, 0, sizeof(G_CAP));
	udp_stream_init(&G_S, UDP_STREAM_AUDIO, &C_IO, 1, 0);
	G_S.t_ayt = 0;
	G_S.t_ping = 0;
	G_CAP.n = 0;

	// seq 1, then 4: 2 and 3 missing
	rx_data(1, 0);
	rx_data(4, 0);
	CHECK(G_S.missing_n == 2);

	// Scan: one multi request with 2,2,3,3
	udp_stream_tick(&G_S, 100);
	CHECK(G_CAP.n == 1);
	CHECK(G_CAP.len[0] == PROTO_CONTROL_SIZE + 8);
	CHECK(out_type(0) == PROTO_TYPE_RETRANSMIT);
	CHECK(proto_get_le32(G_CAP.buf[0] + PROTO_OFF_LEN) == PROTO_CONTROL_SIZE + 8);
	CHECK(proto_get_le16(G_CAP.buf[0] + 0x10) == 2);
	CHECK(proto_get_le16(G_CAP.buf[0] + 0x12) == 2);
	CHECK(proto_get_le16(G_CAP.buf[0] + 0x14) == 3);
	CHECK(proto_get_le16(G_CAP.buf[0] + 0x16) == 3);

	// 2 arrives late: next scan is a single request for 3
	rx_data(2, 150);
	CHECK(G_S.missing_n == 1);
	G_CAP.n = 0;
	udp_stream_tick(&G_S, 200);
	CHECK(G_CAP.n == 1);
	CHECK(G_CAP.len[0] == PROTO_CONTROL_SIZE);
	CHECK(out_type(0) == PROTO_TYPE_RETRANSMIT);
	CHECK(out_seq(0) == 3);

	// 3 was requested twice; after 4 requests in total it is dropped
	for (k = 0; k < 2; k++)
		udp_stream_tick(&G_S, 300 + k * 100);
	CHECK(G_S.missing_n == 1);
	udp_stream_tick(&G_S, 500);
	CHECK(G_S.missing_n == 0);
	CHECK(G_S.stats.missing_dropped == 1);

	// Large gap (> MAX_MISSING): reset, nothing missing
	rx_data(100, 700);
	CHECK(G_S.missing_n == 0);
	CHECK(G_S.rx_first == 100);
	CHECK(G_S.rx_last == 100);

	// Lower seq than tracked: reset
	rx_data(50, 710);
	CHECK(G_S.rx_first == 50);
}

static void test_ping_reply(void)
{
	uint8_t p[PROTO_PING_SIZE];
	uint32_t ev;

	memset(&G_CAP, 0, sizeof(G_CAP));
	udp_stream_init(&G_S, UDP_STREAM_AUDIO, &C_IO, 0x55u, 0);
	G_S.remote_id = 0x66u;
	G_S.t_ayt = 0;
	G_CAP.n = 0;

	memset(p, 0, sizeof(p));
	proto_put_le32(p + PROTO_OFF_LEN, PROTO_PING_SIZE);
	proto_put_le16(p + PROTO_OFF_TYPE, PROTO_TYPE_PING);
	proto_put_le16(p + PROTO_OFF_SEQ, 7);
	p[PROTO_OFF_PING_REPLY] = 0x00;
	proto_put_le32(p + PROTO_OFF_PING_TIME, 424242u);

	ev = udp_stream_receive(&G_S, p, sizeof(p), 5);
	CHECK((ev & UDP_EV_PING) != 0);
	CHECK(G_CAP.n == 1);
	CHECK(G_CAP.len[0] == PROTO_PING_SIZE);
	CHECK(out_seq(0) == 7);
	CHECK(G_CAP.buf[0][PROTO_OFF_PING_REPLY] == 0x01);
	CHECK(proto_get_le32(G_CAP.buf[0] + PROTO_OFF_PING_TIME) == 424242u);
	CHECK(proto_get_le32(G_CAP.buf[0] + PROTO_OFF_SENTID) == 0x55u);
	CHECK(proto_get_le32(G_CAP.buf[0] + PROTO_OFF_RCVDID) == 0x66u);

	// Our ping: stamp from day_ms; reply with matching seq advances ping_seq
	G_CAP.n = 0;
	udp_stream_tick(&G_S, PROTO_PING_PERIOD);
	CHECK(G_CAP.n == 1);
	CHECK(proto_get_le32(G_CAP.buf[0] + PROTO_OFF_PING_TIME) == 12345678u);
	p[PROTO_OFF_PING_REPLY] = 0x01;
	proto_put_le16(p + PROTO_OFF_SEQ, 0);
	udp_stream_receive(&G_S, p, sizeof(p), PROTO_PING_PERIOD + 5);
	CHECK(G_S.ping_seq == 1);
}

int main(void)
{
	test_endpoint_id();
	test_civ_handshake();
	test_control_timers();
	test_tx_retransmit();
	test_txbuf_limit();
	test_rx_missing();
	test_ping_reply();

	if (G_FAILS != 0) {
		fprintf(stderr, "test_udp_stream: %d failure(s)\n", G_FAILS);
		return EXIT_FAILURE;
	}

	return EXIT_SUCCESS;
}
