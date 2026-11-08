// SPDX-License-Identifier: GPL-3.0-or-later
//
// Thin POSIX UDP socket wrapper, IPv4 only.
//

#pragma once

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

struct udp_socket {
	int fd;
	uint32_t peer_ip;	// host order
	uint16_t peer_port;
	uint32_t local_ip;	// host order, valid after connect
	uint16_t local_port;
};

//
// Name resolution
//

int udp_resolve_ipv4(const char *host, uint32_t *ip_out);

//
// Lifecycle
//

int udp_socket_bind(struct udp_socket *u, uint16_t local_port);
int udp_socket_connect(struct udp_socket *u, uint32_t peer_ip, uint16_t peer_port);
void udp_socket_close(struct udp_socket *u);

//
// Traffic (non-blocking)
//

int udp_socket_send(void *ctx, const uint8_t *buf, size_t len);
ssize_t udp_socket_recv(struct udp_socket *u, uint8_t *buf, size_t cap);

//
// Clocks
//

int64_t clock_mono_ms(void);
uint32_t clock_day_ms(void *ctx);
