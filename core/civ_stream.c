// SPDX-License-Identifier: GPL-3.0-or-later
//
// CI-V UDP stream: open/close, CI-V data in both directions, watchdog.
//
// Port of icomUdpCivData from wfview main src/radio/icomudpcivdata.cpp,
// wfview is copyright 2017-2026 Elliott H. Liggett (W6EL) and Phil Taylor (M0VSE), GPLv3.
//
// The optional "split waterfall" re-framing of wfview (waterfallFormat == 2)
// is not done here; scope frames are passed on as received (wfview default).
//

#include "core/civ_stream.h"

#include <string.h>

#define OFF_OC_DATA		0x10
#define OFF_OC_SENDSEQ		0x13
#define OFF_OC_MAGIC		0x15
#define OFF_CIV_REPLY		0x10
#define OFF_CIV_DATALEN		0x11
#define OFF_CIV_SENDSEQ		0x13

#define OC_OPEN			0x04
#define OC_CLOSE		0x00

//
// Open/close packet (0x16), tracked
//

static void send_open_close(struct civ_stream *c, bool close_it, int64_t now_ms)
{
	uint8_t p[PROTO_OPENCLOSE_SIZE];

	memset(p, 0, sizeof(p));
	proto_put_le32(p + PROTO_OFF_LEN, PROTO_OPENCLOSE_SIZE);
	proto_put_le32(p + PROTO_OFF_SENTID, c->s.my_id);
	proto_put_le32(p + PROTO_OFF_RCVDID, c->s.remote_id);
	proto_put_le16(p + OFF_OC_DATA, 0x01c0);
	proto_put_be16(p + OFF_OC_SENDSEQ, c->send_seq_b);
	p[OFF_OC_MAGIC] = close_it ? OC_CLOSE : OC_OPEN;
	c->send_seq_b++;

	udp_stream_send_tracked(&c->s, p, sizeof(p), now_ms);
}

void civ_stream_init(struct civ_stream *c, const struct udp_io *io, uint32_t my_id,
		void (*on_data)(void *ctx, const uint8_t *civ, size_t len), void *ctx, int64_t now_ms)
{
	memset(c, 0, sizeof(*c));
	c->on_data = on_data;
	c->ctx = ctx;
	c->last_data = now_ms;
	udp_stream_init(&c->s, UDP_STREAM_CIV, io, my_id, now_ms);
	c->t_watchdog = now_ms + PROTO_WATCHDOG_PERIOD;
}

//
// Close: open/close with magic 0x00, then the stream disconnect
//

void civ_stream_close(struct civ_stream *c, int64_t now_ms)
{
	send_open_close(c, true, now_ms);
	c->t_open = 0;
	c->t_watchdog = 0;
	udp_stream_close(&c->s);
}

//
// Send CI-V bytes (one or more frames)
//

int civ_stream_send(struct civ_stream *c, const uint8_t *civ, size_t len, int64_t now_ms)
{
	uint8_t p[PROTO_MAX_PACKET];

	if (len > sizeof(p) - CIV_DATA_HEADER)
		return -1;

	memset(p, 0, CIV_DATA_HEADER);
	proto_put_le32(p + PROTO_OFF_LEN, (uint32_t)(CIV_DATA_HEADER + len));
	proto_put_le32(p + PROTO_OFF_SENTID, c->s.my_id);
	proto_put_le32(p + PROTO_OFF_RCVDID, c->s.remote_id);
	p[OFF_CIV_REPLY] = 0xc1;
	proto_put_le16(p + OFF_CIV_DATALEN, (uint16_t)len);
	proto_put_be16(p + OFF_CIV_SENDSEQ, c->send_seq_b);
	memcpy(p + CIV_DATA_HEADER, civ, len);

	c->send_seq_b++;
	return udp_stream_send_tracked(&c->s, p, CIV_DATA_HEADER + len, now_ms);
}

void civ_stream_receive(struct civ_stream *c, const uint8_t *buf, size_t len, int64_t now_ms)
{
	if (len == PROTO_CONTROL_SIZE) {
		const uint16_t type = proto_get_le16(buf + PROTO_OFF_TYPE);

		if (type == PROTO_TYPE_READY) {
			c->s.remote_id = proto_get_le32(buf + PROTO_OFF_SENTID);
			send_open_close(c, false, now_ms);
			c->t_open = now_ms + CIV_OPEN_REPEAT_MS;
		}
	} else if (len > 21) {
		if (proto_get_le16(buf + PROTO_OFF_TYPE) != 0x01) {
			c->t_open = 0;
			c->last_data = now_ms;

			if ((uint16_t)(proto_get_le16(buf + OFF_CIV_DATALEN) + CIV_DATA_HEADER) ==
					(uint16_t)proto_get_le32(buf + PROTO_OFF_LEN)) {
				c->frames_rx++;
				if (c->on_data != NULL)
					c->on_data(c->ctx, buf + CIV_DATA_HEADER, len - CIV_DATA_HEADER);
			}
		}
	}

	udp_stream_receive(&c->s, buf, len, now_ms);
}

void civ_stream_tick(struct civ_stream *c, int64_t now_ms)
{
	if (c->t_open != 0 && now_ms >= c->t_open) {
		send_open_close(c, false, now_ms);
		c->t_open = now_ms + CIV_OPEN_REPEAT_MS;
	}

	if (c->t_watchdog != 0 && now_ms >= c->t_watchdog) {
		if (now_ms - c->last_data > CIV_WATCHDOG_MS) {
			if (!c->alerted) {
				c->t_open = now_ms + CIV_OPEN_REPEAT_MS;
				c->alerted = true;
			}
		} else {
			c->alerted = false;
		}
		c->t_watchdog = now_ms + PROTO_WATCHDOG_PERIOD;
	}

	udp_stream_tick(&c->s, now_ms);
}

int64_t civ_stream_next_deadline(const struct civ_stream *c)
{
	const int64_t t[3] = { udp_stream_next_deadline(&c->s), c->t_open, c->t_watchdog };
	int64_t best = 0;
	size_t i;

	for (i = 0; i < 3; i++) {
		if (t[i] != 0 && (best == 0 || t[i] < best))
			best = t[i];
	}

	return best;
}
