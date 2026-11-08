// SPDX-License-Identifier: GPL-3.0-or-later
//
// Control stream: login, token, radio capabilities, stream request, status.
//
// Port of icomUdpHandler from wfview v3-initial src/radio/icomudphandler.cpp,
// wfview is copyright 2017-2026 Elliott H. Liggett (W6EL) and Phil Taylor (M0VSE), GPLv3.
//

#include "core/control.h"

#include <stdio.h>
#include <string.h>

//
// Offsets shared by login, token, status, conninfo (after the 0x10 header)
//

#define OFF_PAYLOADSIZE		0x10
#define OFF_REQUESTREPLY	0x14
#define OFF_REQUESTTYPE		0x15
#define OFF_INNERSEQ		0x16
#define OFF_TOKREQUEST		0x1a
#define OFF_TOKEN		0x1c
#define OFF_GUID		0x20
#define OFF_COMMONCAP		0x27
#define OFF_MAC			0x2a

//
// Packet specific offsets
//

#define OFF_TOKEN_RESETCAP	0x24
#define OFF_TOKEN_RESPONSE	0x30
#define OFF_STATUS_ERROR	0x30
#define OFF_STATUS_DISC		0x40
#define OFF_STATUS_CIVPORT	0x42
#define OFF_STATUS_AUDIOPORT	0x46
#define OFF_LOGINR_ERROR	0x30
#define OFF_LOGINR_CONNECTION	0x40
#define OFF_LOGIN_USERNAME	0x40
#define OFF_LOGIN_PASSWORD	0x50
#define OFF_LOGIN_NAME		0x60
#define OFF_CONN_NAME		0x40
#define OFF_CONN_BUSY		0x60
#define OFF_CONN_COMPUTER	0x64
#define OFF_CONN_IPADDRESS	0x84
#define OFF_CONN_USERNAME	0x60
#define OFF_CONN_RXENABLE	0x70
#define OFF_CONN_TXENABLE	0x71
#define OFF_CONN_RXCODEC	0x72
#define OFF_CONN_TXCODEC	0x73
#define OFF_CONN_RXSAMPLE	0x74
#define OFF_CONN_TXSAMPLE	0x78
#define OFF_CONN_CIVPORT	0x7c
#define OFF_CONN_AUDIOPORT	0x80
#define OFF_CONN_TXBUFFER	0x84
#define OFF_CONN_CONVERT	0x88
#define OFF_CAPS_NUMRADIOS	0x40

//
// Radio capability entry offsets (0x66 each)
//

#define OFF_RCAP_COMMONCAP	0x07
#define OFF_RCAP_MAC		0x0a
#define OFF_RCAP_NAME		0x10
#define OFF_RCAP_AUDIO		0x30
#define OFF_RCAP_CONNTYPE	0x50
#define OFF_RCAP_CIV		0x52
#define OFF_RCAP_RXSAMPLE	0x53
#define OFF_RCAP_TXSAMPLE	0x55
#define OFF_RCAP_BAUDRATE	0x5a
#define OFF_RCAP_CAPF		0x5e
#define OFF_RCAP_CAPG		0x61

#define TOKEN_CONFIRM		0x02
#define TOKEN_REMOVE		0x01
#define TOKEN_RENEW		0x05

#define COMMONCAP_MAC		0x8010
#define AUTH_SEQ_START		0x30

//
// Status and error reporting
//

static void set_status(struct control *c, const char *msg)
{
	snprintf(c->status, sizeof(c->status), "%s", msg);
	if (c->ev.status != NULL)
		c->ev.status(c->ev.ctx, c->status);
}

static void report_error(struct control *c, enum control_reason reason, const char *msg)
{
	c->reason = reason;
	if (c->ev.error != NULL)
		c->ev.error(c->ev.ctx, reason, msg);
}

//
// Copy a fixed-size, possibly unterminated string field
//

static void copy_field(char *dst, size_t dst_size, const uint8_t *src, size_t src_len)
{
	size_t n = 0;

	while (n < src_len && n + 1 < dst_size && src[n] != '\0') {
		dst[n] = (char)src[n];
		n++;
	}
	dst[n] = '\0';
}

//
// Common header for login / token / conninfo
//

static void fill_auth_header(struct control *c, uint8_t *p, size_t size, uint8_t requesttype)
{
	memset(p, 0, size);
	proto_put_le32(p + PROTO_OFF_LEN, (uint32_t)size);
	proto_put_le32(p + PROTO_OFF_SENTID, c->s.my_id);
	proto_put_le32(p + PROTO_OFF_RCVDID, c->s.remote_id);
	proto_put_be32(p + OFF_PAYLOADSIZE, (uint32_t)(size - 0x10));
	p[OFF_REQUESTREPLY] = 0x01;
	p[OFF_REQUESTTYPE] = requesttype;
	proto_put_be16(p + OFF_INNERSEQ, c->auth_seq);
	c->auth_seq++;
	proto_put_le16(p + OFF_TOKREQUEST, c->tok_request);
}

//
// Login packet (0x80)
//

static void send_login(struct control *c, int64_t now_ms)
{
	uint8_t p[PROTO_LOGIN_SIZE];
	uint8_t enc[PASSCODE_MAX];
	char name[PASSCODE_MAX + 1];
	size_t n;

	c->tok_request = c->ev.rand16(c->ev.ctx);

	fill_auth_header(c, p, sizeof(p), 0x00);

	n = passcode_encode(c->cfg.username, enc);
	memcpy(p + OFF_LOGIN_USERNAME, enc, n);
	n = passcode_encode(c->cfg.password, enc);
	memcpy(p + OFF_LOGIN_PASSWORD, enc, n);

	// compName = clientName.mid(0, 8) + "-wfview", truncated to the 16-byte field
	snprintf(name, sizeof(name), "%.8s-wfview", c->cfg.client_name);
	memcpy(p + OFF_LOGIN_NAME, name, strlen(name));

	c->state = CTL_STATE_LOGIN;
	udp_stream_send_tracked(&c->s, p, sizeof(p), now_ms);
}

//
// Token packet (0x40)
//

static void send_token(struct control *c, uint8_t magic, int64_t now_ms)
{
	uint8_t p[PROTO_TOKEN_SIZE];

	fill_auth_header(c, p, sizeof(p), magic);
	proto_put_be16(p + OFF_TOKEN_RESETCAP, 0x0798);
	proto_put_le32(p + OFF_TOKEN, c->token);

	udp_stream_send_tracked(&c->s, p, sizeof(p), now_ms);
}

//
// Stream request (conninfo 0x90)
//

static void send_request_stream(struct control *c, int64_t now_ms)
{
	uint8_t p[PROTO_CONNINFO_SIZE];
	uint8_t enc[PASSCODE_MAX];
	size_t n;

	fill_auth_header(c, p, sizeof(p), 0x03);
	proto_put_le32(p + OFF_TOKEN, c->token);

	if (!c->use_guid) {
		proto_put_le16(p + OFF_COMMONCAP, COMMONCAP_MAC);
		memcpy(p + OFF_MAC, c->mac, 6);
	} else {
		memcpy(p + OFF_GUID, c->guid, 16);
	}

	memcpy(p + OFF_CONN_NAME, c->dev_name, strlen(c->dev_name));

	p[OFF_CONN_RXENABLE] = 1;
	if (c->tx_sample_rates > 1) {
		p[OFF_CONN_TXENABLE] = 1;
		p[OFF_CONN_TXCODEC] = c->cfg.tx_codec;
	}
	p[OFF_CONN_RXCODEC] = c->cfg.rx_codec;

	n = passcode_encode(c->cfg.username, enc);
	memcpy(p + OFF_CONN_USERNAME, enc, n);

	proto_put_be32(p + OFF_CONN_RXSAMPLE, c->cfg.rx_sample);
	proto_put_be32(p + OFF_CONN_TXSAMPLE, c->cfg.tx_sample);
	proto_put_be32(p + OFF_CONN_CIVPORT, c->civ_local);
	proto_put_be32(p + OFF_CONN_AUDIOPORT, c->audio_local);
	proto_put_be32(p + OFF_CONN_TXBUFFER, c->cfg.tx_latency);
	p[OFF_CONN_CONVERT] = 1;

	c->state = CTL_STATE_STREAM_REQ;
	udp_stream_send_tracked(&c->s, p, sizeof(p), now_ms);
}

//
// Select a radio from the capabilities list and request its streams
//

static void set_current_radio(struct control *c, size_t idx, int64_t now_ms)
{
	const struct radio_cap *r = &c->radios[idx];

	if (c->stream_opened && c->ev.close_streams != NULL)
		c->ev.close_streams(c->ev.ctx);
	c->stream_opened = false;

	if (c->civ_local == 0 || c->audio_local == 0) {
		if (c->ev.alloc_ports(c->ev.ctx, &c->civ_local, &c->audio_local) != 0)
			return;
	}

	c->baudrate = r->baudrate;

	if (r->commoncap == COMMONCAP_MAC) {
		memcpy(c->mac, r->mac, 6);
		c->use_guid = false;
	} else {
		c->use_guid = true;
		memcpy(c->guid, r->guid, 16);
	}

	memcpy(c->dev_name, r->name, sizeof(c->dev_name));
	c->civ_id = r->civ;
	c->rx_sample_rates = r->rxsample;
	c->tx_sample_rates = r->txsample;

	send_request_stream(c, now_ms);
}

//
// Packet handlers
//

static void on_token(struct control *c, const uint8_t *b, int64_t now_ms)
{
	uint32_t response;

	if (!(b[OFF_REQUESTTYPE] == TOKEN_RENEW && b[OFF_REQUESTREPLY] == 0x02 &&
			proto_get_le16(b + PROTO_OFF_TYPE) != 0x01))
		return;

	response = proto_get_le32(b + OFF_TOKEN_RESPONSE);

	if (response == 0x00000000) {
		c->t_token = now_ms + PROTO_TOKEN_RENEWAL;
		if (!c->stream_opened)
			send_request_stream(c, now_ms);
	} else if (response == 0xffffffffu) {
		c->s.remote_id = proto_get_le32(b + PROTO_OFF_SENTID);
		c->tok_request = proto_get_le16(b + OFF_TOKREQUEST);
		c->token = proto_get_le32(b + OFF_TOKEN);
		c->stream_opened = false;
		send_request_stream(c, now_ms);
	}
}

static void on_status(struct control *c, const uint8_t *b)
{
	const uint32_t error = proto_get_le32(b + OFF_STATUS_ERROR);

	if (proto_get_le16(b + PROTO_OFF_TYPE) == 0x01)
		return;

	if (error == 0xffffffffu && !c->stream_opened) {
		report_error(c, CTL_REASON_CONN_FAILED, "Connection failed\ntry rebooting the radio.");
		c->state = CTL_STATE_FAILED;
	} else if (error == 0x00000000 && b[OFF_STATUS_DISC] == 0x01) {
		report_error(c, CTL_REASON_RADIO_GONE, "Got radio disconnected.");
		if (c->stream_opened) {
			if (c->ev.close_streams != NULL)
				c->ev.close_streams(c->ev.ctx);
			c->stream_opened = false;
		}
	} else {
		c->civ_port = proto_get_be16(b + OFF_STATUS_CIVPORT);
		c->audio_port = proto_get_be16(b + OFF_STATUS_AUDIOPORT);
		if (!c->stream_opened) {
			if (c->tx_sample_rates < 2) {
				c->cfg.tx_sample = 0;
				c->cfg.tx_codec = 0;
			}
			c->stream_opened = true;
			c->state = CTL_STATE_STREAMS_OPEN;
			c->ev.open_streams(c->ev.ctx, c->civ_port, c->audio_port);
		}
	}
}

static void on_login_response(struct control *c, const uint8_t *b, int64_t now_ms)
{
	if (proto_get_le16(b + PROTO_OFF_TYPE) == 0x01)
		return;

	copy_field(c->connection, sizeof(c->connection), b + OFF_LOGINR_CONNECTION, 16);
	if (strcmp(c->connection, "FTTH") == 0)
		c->high_bandwidth = true;

	if (strcmp(c->connection, "WFVIEW") != 0 && (c->cfg.rx_codec >= 0x40 || c->cfg.tx_codec >= 0x40)) {
		report_error(c, CTL_REASON_NONE, "Opus codec not supported, forcing LPCM16");
		if (c->cfg.rx_codec >= 0x40)
			c->cfg.rx_codec = 0x04;
		if (c->cfg.tx_codec >= 0x40)
			c->cfg.tx_codec = 0x04;
	}

	if (proto_get_le32(b + OFF_LOGINR_ERROR) == 0xfeffffffu) {
		report_error(c, CTL_REASON_AUTH, "Invalid Username/Password");
		c->state = CTL_STATE_FAILED;
	} else if (!c->authenticated) {
		if (proto_get_le16(b + OFF_TOKREQUEST) == c->tok_request) {
			set_status(c, "Radio Login OK!");
			c->token = proto_get_le32(b + OFF_TOKEN);
			send_token(c, TOKEN_CONFIRM, now_ms);
			c->t_token = now_ms + PROTO_TOKEN_RENEWAL;
			c->authenticated = true;
			c->state = CTL_STATE_AUTHENTICATED;
		}
	}
}

static bool radio_matches(const struct radio_cap *r, const uint8_t *b)
{
	if (r->commoncap == COMMONCAP_MAC && memcmp(r->mac, b + OFF_MAC, 6) == 0)
		return true;

	return memcmp(r->guid, b + OFF_GUID, 16) == 0;
}

static void on_conninfo(struct control *c, const uint8_t *b, int64_t now_ms)
{
	char computer[17];
	char comp_name[PASSCODE_MAX + 1];
	char msg[CONTROL_STATUS_LEN];
	const uint32_t busy = proto_get_le32(b + OFF_CONN_BUSY);
	const uint8_t *ip = b + OFF_CONN_IPADDRESS;
	size_t f;

	copy_field(computer, sizeof(computer), b + OFF_CONN_COMPUTER, 16);
	snprintf(comp_name, sizeof(comp_name), "%.8s-wfview", c->cfg.client_name);

	// Radio usage per matching radio: kept for a later radio list UI
	for (f = 0; f < c->n_radios; f++)
		(void)radio_matches(&c->radios[f], b);

	if (!c->stream_opened && c->n_radios == 1) {
		if (busy != 0) {
			if (proto_get_le32(ip) != 0 && strcmp(computer, comp_name) != 0) {
				snprintf(msg, sizeof(msg), "%s in use by: %s (%u.%u.%u.%u)",
						c->dev_name, computer, ip[0], ip[1], ip[2], ip[3]);
				set_status(c, msg);
				c->reason = CTL_REASON_IN_USE;
				udp_stream_send_control(&c->s, false, PROTO_TYPE_IDLE,
						proto_get_le16(b + PROTO_OFF_SEQ), now_ms);
			}
		} else {
			snprintf(msg, sizeof(msg), "%s available", c->dev_name);
			set_status(c, msg);
			set_current_radio(c, 0, now_ms);
		}
	}
}

static void on_capabilities(struct control *c, const uint8_t *b, size_t len)
{
	size_t f;

	if ((len - PROTO_CAPABILITIES_SIZE) % PROTO_RADIO_CAP_SIZE != 0)
		return;

	for (f = PROTO_CAPABILITIES_SIZE; f + PROTO_RADIO_CAP_SIZE <= len; f += PROTO_RADIO_CAP_SIZE) {
		const uint8_t *e = b + f;
		struct radio_cap *r;

		if (c->n_radios == CONTROL_MAX_RADIOS)
			break;

		r = &c->radios[c->n_radios++];
		memset(r, 0, sizeof(*r));
		memcpy(r->guid, e, 16);
		r->commoncap = proto_get_le16(e + OFF_RCAP_COMMONCAP);
		memcpy(r->mac, e + OFF_RCAP_MAC, 6);
		copy_field(r->name, sizeof(r->name), e + OFF_RCAP_NAME, 32);
		copy_field(r->audio, sizeof(r->audio), e + OFF_RCAP_AUDIO, 32);
		r->conntype = proto_get_le16(e + OFF_RCAP_CONNTYPE);
		r->civ = e[OFF_RCAP_CIV];
		r->rxsample = proto_get_le16(e + OFF_RCAP_RXSAMPLE);
		r->txsample = proto_get_le16(e + OFF_RCAP_TXSAMPLE);
		r->baudrate = proto_get_be32(e + OFF_RCAP_BAUDRATE);
		r->capf = proto_get_le16(e + OFF_RCAP_CAPF);
		r->capg = proto_get_le16(e + OFF_RCAP_CAPG);
	}
}

//
// Start the control stream
//

void control_init(struct control *c, const struct control_cfg *cfg, const struct udp_io *io,
		const struct control_events *ev, uint32_t my_id, int64_t now_ms)
{
	memset(c, 0, sizeof(*c));
	c->cfg = *cfg;
	c->ev = *ev;
	c->auth_seq = AUTH_SEQ_START;
	c->state = CTL_STATE_AYT;
	udp_stream_init(&c->s, UDP_STREAM_CONTROL, io, my_id, now_ms);
}

//
// Process one datagram
//

void control_receive(struct control *c, const uint8_t *buf, size_t len, int64_t now_ms)
{
	uint32_t ev;

	switch (len) {
	case PROTO_CONTROL_SIZE:
		if (proto_get_le16(buf + PROTO_OFF_TYPE) == PROTO_TYPE_IAMHERE && c->state == CTL_STATE_AYT)
			c->state = CTL_STATE_READY_WAIT;
		break;
	case PROTO_PING_SIZE:
		break;
	case PROTO_TOKEN_SIZE:
		on_token(c, buf, now_ms);
		break;
	case PROTO_STATUS_SIZE:
		on_status(c, buf);
		break;
	case PROTO_LOGIN_RESPONSE_SIZE:
		on_login_response(c, buf, now_ms);
		break;
	case PROTO_CONNINFO_SIZE:
		on_conninfo(c, buf, now_ms);
		break;
	default:
		if (len > PROTO_CAPABILITIES_SIZE)
			on_capabilities(c, buf, len);
		break;
	}

	ev = udp_stream_receive(&c->s, buf, len, now_ms);

	// "I am ready" in response to "are you ready": log in
	if ((ev & UDP_EV_READY) != 0)
		send_login(c, now_ms);
}

//
// Timers
//

void control_tick(struct control *c, int64_t now_ms)
{
	if (c->t_token != 0 && now_ms >= c->t_token) {
		send_token(c, TOKEN_RENEW, now_ms);
		// Rearmed when the renewal reply arrives; retry after the same period otherwise
		c->t_token = now_ms + PROTO_TOKEN_RENEWAL;
	}

	udp_stream_tick(&c->s, now_ms);

	// Deviation 4 (UI only): wfview has this message in sendAreYouThere(), but its
	// timer calls sendControl() directly, so it never shows. Here it does.
	if (c->state == CTL_STATE_AYT && !c->ayt_warned && c->s.ayt_count >= CONTROL_AYT_WARN) {
		set_status(c, "Radio not responding!");
		c->ayt_warned = true;
	}
}

int64_t control_next_deadline(const struct control *c)
{
	int64_t t = udp_stream_next_deadline(&c->s);

	if (c->t_token != 0 && (t == 0 || c->t_token < t))
		t = c->t_token;

	return t;
}

//
// Shutdown, as wfview icomUdpHandler::shutdown()
//

void control_shutdown(struct control *c, int64_t now_ms)
{
	c->t_token = 0;
	c->s.t_ayt = 0;
	c->s.t_ping = 0;
	c->s.t_idle = 0;
	c->s.t_retransmit = 0;

	if (c->stream_opened) {
		if (c->ev.close_streams != NULL)
			c->ev.close_streams(c->ev.ctx);
		send_token(c, TOKEN_REMOVE, now_ms);
		c->stream_opened = false;
	}

	udp_stream_send_control(&c->s, false, PROTO_TYPE_DISCONNECT, 0, now_ms);
	c->state = CTL_STATE_CLOSED;
}
