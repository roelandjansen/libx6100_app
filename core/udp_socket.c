// SPDX-License-Identifier: GPL-3.0-or-later
//
// Thin POSIX UDP socket wrapper, IPv4 only.
//

#include "core/udp_socket.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

//
// Resolve a hostname or dotted IPv4 address to an IPv4 address (host order)
//

int udp_resolve_ipv4(const char *host, uint32_t *ip_out)
{
	struct addrinfo hints;
	struct addrinfo *res = NULL;
	struct in_addr a;
	int rc;

	if (inet_pton(AF_INET, host, &a) == 1) {
		*ip_out = ntohl(a.s_addr);
		return 0;
	}

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_DGRAM;

	rc = getaddrinfo(host, NULL, &hints, &res);
	if (rc != 0 || res == NULL)
		return -1;

	*ip_out = ntohl(((const struct sockaddr_in *)(const void *)res->ai_addr)->sin_addr.s_addr);
	freeaddrinfo(res);
	return 0;
}

//
// Create a non-blocking UDP socket bound to local_port (0 = any free port)
//

int udp_socket_bind(struct udp_socket *u, uint16_t local_port)
{
	struct sockaddr_in sa;
	socklen_t sl = sizeof(sa);
	int flags;

	memset(u, 0, sizeof(*u));
	u->fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (u->fd < 0)
		return -1;

	flags = fcntl(u->fd, F_GETFL, 0);
	if (flags < 0 || fcntl(u->fd, F_SETFL, flags | O_NONBLOCK) < 0)
		goto fail;

	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_addr.s_addr = htonl(INADDR_ANY);
	sa.sin_port = htons(local_port);
	if (bind(u->fd, (const struct sockaddr *)(const void *)&sa, sizeof(sa)) < 0)
		goto fail;

	if (getsockname(u->fd, (struct sockaddr *)(void *)&sa, &sl) < 0)
		goto fail;
	u->local_port = ntohs(sa.sin_port);
	return 0;

fail:
	close(u->fd);
	u->fd = -1;
	return -1;
}

//
// Fix the peer; afterwards only its datagrams are received and the
// local address used towards it is known (for the endpoint ID)
//

int udp_socket_connect(struct udp_socket *u, uint32_t peer_ip, uint16_t peer_port)
{
	struct sockaddr_in sa;
	socklen_t sl = sizeof(sa);

	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_addr.s_addr = htonl(peer_ip);
	sa.sin_port = htons(peer_port);
	if (connect(u->fd, (const struct sockaddr *)(const void *)&sa, sizeof(sa)) < 0)
		return -1;

	u->peer_ip = peer_ip;
	u->peer_port = peer_port;

	if (getsockname(u->fd, (struct sockaddr *)(void *)&sa, &sl) < 0)
		return -1;
	u->local_ip = ntohl(sa.sin_addr.s_addr);
	u->local_port = ntohs(sa.sin_port);
	return 0;
}

void udp_socket_close(struct udp_socket *u)
{
	if (u->fd >= 0)
		close(u->fd);
	u->fd = -1;
}

//
// udp_io send hook: ctx is the struct udp_socket
//

int udp_socket_send(void *ctx, const uint8_t *buf, size_t len)
{
	struct udp_socket *u = ctx;
	ssize_t n;

	if (u->fd < 0)
		return -1;

	n = send(u->fd, buf, len, 0);
	return (n == (ssize_t)len) ? 0 : -1;
}

//
// Receive one datagram; 0 when nothing pending, -1 on error
//

ssize_t udp_socket_recv(struct udp_socket *u, uint8_t *buf, size_t cap)
{
	ssize_t n = recv(u->fd, buf, cap, 0);

	if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR || errno == ECONNREFUSED))
		return 0;

	return n;
}

//
// Monotonic ms
//

int64_t clock_mono_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

//
// ms since local midnight, as wfview's QTime::currentTime().msecsSinceStartOfDay()
//

uint32_t clock_day_ms(void *ctx)
{
	struct timespec ts;
	struct tm tm;
	time_t t;

	(void)ctx;
	clock_gettime(CLOCK_REALTIME, &ts);
	t = ts.tv_sec;
	localtime_r(&t, &tm);

	return (uint32_t)(((tm.tm_hour * 60 + tm.tm_min) * 60 + tm.tm_sec) * 1000) + (uint32_t)(ts.tv_nsec / 1000000);
}
