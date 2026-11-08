// SPDX-License-Identifier: GPL-3.0-or-later
//
// Icom RS-BA1 / wfview UDP protocol constants and byte helpers.
//
// Values from wfview v3-initial include/packettypes.h,
// wfview is copyright 2017-2026 Elliott H. Liggett (W6EL) and Phil Taylor (M0VSE), GPLv3.
//

#pragma once

#include <stddef.h>
#include <stdint.h>

//
// Timing (ms)
//

#define PROTO_AREYOUTHERE_PERIOD	500
#define PROTO_PING_PERIOD		500
#define PROTO_IDLE_PERIOD		100
#define PROTO_RETRANSMIT_PERIOD		100
#define PROTO_WATCHDOG_PERIOD		500
#define PROTO_TOKEN_RENEWAL		60000
#define PROTO_AUDIO_PERIOD		20
#define PROTO_STALE_CONNECTION_S	15

//
// Buffer limits
//

#define PROTO_BUFSIZE			500
#define PROTO_MAX_MISSING		50
#define PROTO_MAX_RETRANSMIT_REQ	4

//
// Packet sizes
//

#define PROTO_CONTROL_SIZE		0x10
#define PROTO_WATCHDOG_SIZE		0x14
#define PROTO_PING_SIZE			0x15
#define PROTO_OPENCLOSE_SIZE		0x16
#define PROTO_RETRANSMIT_RANGE_SIZE	0x18
#define PROTO_TOKEN_SIZE		0x40
#define PROTO_STATUS_SIZE		0x50
#define PROTO_LOGIN_RESPONSE_SIZE	0x60
#define PROTO_LOGIN_SIZE		0x80
#define PROTO_CONNINFO_SIZE		0x90
#define PROTO_CAPABILITIES_SIZE		0x42
#define PROTO_RADIO_CAP_SIZE		0x66
#define PROTO_MAX_PACKET		1500

//
// Control packet types
//

#define PROTO_TYPE_IDLE			0x00
#define PROTO_TYPE_RETRANSMIT		0x01
#define PROTO_TYPE_AREYOUTHERE		0x03
#define PROTO_TYPE_IAMHERE		0x04
#define PROTO_TYPE_DISCONNECT		0x05
#define PROTO_TYPE_READY		0x06
#define PROTO_TYPE_PING			0x07

//
// Common header offsets (little endian fields)
//

#define PROTO_OFF_LEN			0x00
#define PROTO_OFF_TYPE			0x04
#define PROTO_OFF_SEQ			0x06
#define PROTO_OFF_SENTID		0x08
#define PROTO_OFF_RCVDID		0x0c

//
// Ping packet offsets
//

#define PROTO_OFF_PING_REPLY		0x10
#define PROTO_OFF_PING_TIME		0x11

//
// Little endian access
//

static inline uint16_t proto_get_le16(const uint8_t *p)
{
	return (uint16_t)(p[0] | (p[1] << 8));
}

static inline uint32_t proto_get_le32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline void proto_put_le16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)(v & 0xff);
	p[1] = (uint8_t)(v >> 8);
}

static inline void proto_put_le32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v & 0xff);
	p[1] = (uint8_t)((v >> 8) & 0xff);
	p[2] = (uint8_t)((v >> 16) & 0xff);
	p[3] = (uint8_t)(v >> 24);
}

//
// Big endian access
//

static inline uint16_t proto_get_be16(const uint8_t *p)
{
	return (uint16_t)((p[0] << 8) | p[1]);
}

static inline uint32_t proto_get_be32(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static inline void proto_put_be16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)(v >> 8);
	p[1] = (uint8_t)(v & 0xff);
}

static inline void proto_put_be32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24);
	p[1] = (uint8_t)((v >> 16) & 0xff);
	p[2] = (uint8_t)((v >> 8) & 0xff);
	p[3] = (uint8_t)(v & 0xff);
}
