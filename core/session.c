// SPDX-License-Identifier: GPL-3.0-or-later
//
// One connection to one rig behind a wfview server.
//
// Network thread: poll() on the three sockets and a wake pipe, dispatch
// datagrams, run timers. The rest of the app talks to it through
// session_get_info() (snapshot under a mutex), the CI-V out queue, and hooks.
//
// Deviation from wfview (phase 1 spec section 13): link loss. Once the server
// has answered, nothing received on any stream for 15 s ends the session
// as SESSION_LOST.
//

#include "core/session.h"

#include <errno.h>
#include <poll.h>
#include <stdatomic.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "core/audio_stream.h"
#include "core/civ_stream.h"
#include "core/udp_socket.h"

#define C_POLL_MAX_MS		500
#define C_LINK_LOSS_MS		(PROTO_STALE_CONNECTION_S * 1000)
#define C_CIV_QUEUE		8192

struct session {

	//
	// Configuration and hooks
	//
	struct profile prof;
	struct session_hooks hooks;
	uint32_t peer_ip;

	//
	// Thread
	//
	pthread_t thread;
	pthread_mutex_t lock;
	int wake[2];
	atomic_int stop;

	//
	// Sockets
	//
	struct udp_socket u_ctl;
	struct udp_socket u_civ;
	struct udp_socket u_audio;

	//
	// Streams
	//
	struct control ctl;
	struct civ_stream civ;
	struct audio_stream audio;
	bool streams_open;

	//
	// Link supervision
	//
	bool heard;
	int64_t last_rx;
	unsigned int rand_state;

	//
	// CI-V out queue (under lock): length-prefixed messages
	//
	size_t civq_len;
	uint8_t civq[C_CIV_QUEUE];

	//
	// Snapshot (under lock)
	//
	struct session_info info;
};

const char *session_state_name(enum session_state st)
{
	switch (st) {
	case SESSION_IDLE:		return "idle";
	case SESSION_RESOLVING:		return "resolving";
	case SESSION_CONNECTING:	return "connecting";
	case SESSION_CONNECTED:		return "connected";
	case SESSION_RADIO_GONE:	return "radio disconnected";
	case SESSION_LOST:		return "link lost";
	case SESSION_FAILED:		return "failed";
	case SESSION_CLOSED:		return "closed";
	}

	return "?";
}

const char *session_reason_name(enum session_reason r)
{
	switch (r) {
	case SESSION_REASON_NONE:		return "none";
	case SESSION_REASON_RESOLVE:		return "host not found";
	case SESSION_REASON_SOCKET:		return "socket error";
	case SESSION_REASON_AUTH:		return "invalid username/password";
	case SESSION_REASON_CONN_FAILED:	return "connection failed";
	case SESSION_REASON_IN_USE:		return "radio in use";
	case SESSION_REASON_LINK_LOST:		return "link lost";
	}

	return "?";
}

const char *control_state_name(enum control_state st)
{
	switch (st) {
	case CTL_STATE_AYT:		return "are-you-there";
	case CTL_STATE_READY_WAIT:	return "ready-wait";
	case CTL_STATE_LOGIN:		return "login";
	case CTL_STATE_AUTHENTICATED:	return "authenticated";
	case CTL_STATE_STREAM_REQ:	return "stream-request";
	case CTL_STATE_STREAMS_OPEN:	return "streams-open";
	case CTL_STATE_FAILED:		return "failed";
	case CTL_STATE_CLOSED:		return "closed";
	}

	return "?";
}

//
// Snapshot helpers (network thread)
//

static void notify(struct session *s)
{
	if (s->hooks.notify != NULL)
		s->hooks.notify(s->hooks.ctx);
}

static void update_info(struct session *s)
{
	struct session_info *i = &s->info;

	pthread_mutex_lock(&s->lock);
	i->control_state = s->ctl.state;
	snprintf(i->status, sizeof(i->status), "%s", s->ctl.status);
	memcpy(i->radio_name, s->ctl.dev_name, sizeof(i->radio_name));
	i->civ_addr = s->ctl.civ_id;
	i->baudrate = s->ctl.baudrate;
	memcpy(i->connection, s->ctl.connection, sizeof(i->connection));
	i->civ_port = s->ctl.civ_port;
	i->audio_port = s->ctl.audio_port;
	i->ayt_count = s->ctl.s.ayt_count;
	if (s->streams_open) {
		i->civ_frames = s->civ.frames_rx;
		i->audio_packets = s->audio.packets_rx;
		i->audio_dropped_late = s->audio.dropped_late;
	}
	i->retransmitted = s->ctl.s.stats.retransmitted;
	i->requests_sent = s->ctl.s.stats.requests_sent;
	if (s->streams_open) {
		i->retransmitted += s->civ.s.stats.retransmitted + s->audio.s.stats.retransmitted;
		i->requests_sent += s->civ.s.stats.requests_sent + s->audio.s.stats.requests_sent;
	}
	pthread_mutex_unlock(&s->lock);
}

static void set_state(struct session *s, enum session_state st, enum session_reason r)
{
	pthread_mutex_lock(&s->lock);
	s->info.state = st;
	if (r != SESSION_REASON_NONE)
		s->info.reason = r;
	pthread_mutex_unlock(&s->lock);
	update_info(s);
	notify(s);
}

//
// Control stream events
//

static int ev_alloc_ports(void *ctx, uint16_t *civ_local, uint16_t *audio_local)
{
	struct session *s = ctx;

	if (s->u_civ.fd < 0 && udp_socket_bind(&s->u_civ, 0) != 0)
		return -1;
	if (s->u_audio.fd < 0 && udp_socket_bind(&s->u_audio, 0) != 0)
		return -1;

	*civ_local = s->u_civ.local_port;
	*audio_local = s->u_audio.local_port;
	return 0;
}

static void on_civ_data(void *ctx, const uint8_t *civ, size_t len)
{
	struct session *s = ctx;

	if (s->hooks.civ_data != NULL)
		s->hooks.civ_data(s->hooks.ctx, civ, len);
}

static void ev_open_streams(void *ctx, uint16_t civ_port, uint16_t audio_port)
{
	struct session *s = ctx;
	const int64_t now = clock_mono_ms();
	struct udp_io io_civ = { udp_socket_send, clock_day_ms, &s->u_civ };
	struct udp_io io_audio = { udp_socket_send, clock_day_ms, &s->u_audio };
	struct audio_rx_events aev = { s->hooks.audio_start, s->hooks.audio_stop, s->hooks.audio_data, s->hooks.ctx };

	if (udp_socket_connect(&s->u_civ, s->peer_ip, civ_port) != 0 ||
			udp_socket_connect(&s->u_audio, s->peer_ip, audio_port) != 0) {
		set_state(s, SESSION_FAILED, SESSION_REASON_SOCKET);
		s->stop = 1;
		return;
	}

	civ_stream_init(&s->civ, &io_civ, udp_stream_endpoint_id(s->u_civ.local_ip, s->u_civ.local_port),
			on_civ_data, s, now);
	audio_stream_init(&s->audio, &io_audio,
			udp_stream_endpoint_id(s->u_audio.local_ip, s->u_audio.local_port),
			&aev, s->prof.rx_latency, now);
	s->streams_open = true;
	set_state(s, SESSION_CONNECTED, SESSION_REASON_NONE);
}

static void ev_close_streams(void *ctx)
{
	struct session *s = ctx;
	const int64_t now = clock_mono_ms();

	if (!s->streams_open)
		return;

	audio_stream_close(&s->audio);
	civ_stream_close(&s->civ, now);
	s->streams_open = false;
	update_info(s);
}

static void ev_status(void *ctx, const char *msg)
{
	struct session *s = ctx;

	(void)msg;
	update_info(s);
	notify(s);
}

static void ev_error(void *ctx, enum control_reason reason, const char *msg)
{
	struct session *s = ctx;

	// Into the control status, which update_info() copies to the snapshot
	snprintf(s->ctl.status, sizeof(s->ctl.status), "%s", msg);

	switch (reason) {
	case CTL_REASON_AUTH:
		set_state(s, SESSION_FAILED, SESSION_REASON_AUTH);
		s->stop = 1;
		break;
	case CTL_REASON_CONN_FAILED:
		set_state(s, SESSION_FAILED, SESSION_REASON_CONN_FAILED);
		break;
	case CTL_REASON_RADIO_GONE:
		set_state(s, SESSION_RADIO_GONE, SESSION_REASON_NONE);
		break;
	case CTL_REASON_IN_USE:
	case CTL_REASON_NONE:
		notify(s);
		break;
	}
}

static uint16_t ev_rand16(void *ctx)
{
	struct session *s = ctx;

	s->rand_state = s->rand_state * 1103515245u + 12345u;
	return (uint16_t)(s->rand_state >> 8);
}

//
// Datagram dispatch
//

static void drain(struct session *s, struct udp_socket *u, int which, int64_t now)
{
	uint8_t buf[PROTO_MAX_PACKET];
	ssize_t n;

	while ((n = udp_socket_recv(u, buf, sizeof(buf))) > 0) {
		s->last_rx = now;
		s->heard = true;

		if (which == 0)
			control_receive(&s->ctl, buf, (size_t)n, now);
		else if (which == 1 && s->streams_open)
			civ_stream_receive(&s->civ, buf, (size_t)n, now);
		else if (which == 2 && s->streams_open)
			audio_stream_receive(&s->audio, buf, (size_t)n, now);

		if (s->stop)
			break;
	}
}

//
// Send queued CI-V (network thread)
//

static void flush_civq(struct session *s, int64_t now)
{
	uint8_t local[C_CIV_QUEUE];
	size_t len;
	size_t off = 0;

	pthread_mutex_lock(&s->lock);
	len = s->civq_len;
	memcpy(local, s->civq, len);
	s->civq_len = 0;
	pthread_mutex_unlock(&s->lock);

	while (off + 2 <= len) {
		const size_t n = (size_t)local[off] | ((size_t)local[off + 1] << 8);

		off += 2;
		if (s->streams_open)
			civ_stream_send(&s->civ, local + off, n, now);
		off += n;
	}
}

static int64_t min_deadline(int64_t a, int64_t b)
{
	if (a == 0)
		return b;
	if (b == 0)
		return a;
	return a < b ? a : b;
}

static void *net_thread(void *arg)
{
	struct session *s = arg;
	const struct udp_io io_ctl = { udp_socket_send, clock_day_ms, &s->u_ctl };
	const struct control_events cev = {
		ev_alloc_ports, ev_open_streams, ev_close_streams, ev_status, ev_error, ev_rand16, s
	};
	struct control_cfg cfg;
	int64_t now;

	set_state(s, SESSION_RESOLVING, SESSION_REASON_NONE);
	if (udp_resolve_ipv4(s->prof.host, &s->peer_ip) != 0) {
		set_state(s, SESSION_FAILED, SESSION_REASON_RESOLVE);
		return NULL;
	}

	if (udp_socket_bind(&s->u_ctl, 0) != 0 || udp_socket_connect(&s->u_ctl, s->peer_ip, s->prof.port_control) != 0) {
		set_state(s, SESSION_FAILED, SESSION_REASON_SOCKET);
		return NULL;
	}

	memset(&cfg, 0, sizeof(cfg));
	memcpy(cfg.username, s->prof.username, sizeof(cfg.username));
	memcpy(cfg.password, s->prof.password, sizeof(cfg.password));
	memcpy(cfg.client_name, s->prof.client_name, sizeof(cfg.client_name));
	cfg.rx_codec = s->prof.rx_codec;
	cfg.tx_codec = s->prof.tx_codec;
	cfg.rx_sample = s->prof.rx_sample;
	cfg.tx_sample = s->prof.tx_sample;
	cfg.tx_latency = s->prof.tx_latency;

	now = clock_mono_ms();
	s->rand_state = (unsigned int)now ^ (unsigned int)getpid();
	control_init(&s->ctl, &cfg, &io_ctl, &cev,
			udp_stream_endpoint_id(s->u_ctl.local_ip, s->u_ctl.local_port), now);
	set_state(s, SESSION_CONNECTING, SESSION_REASON_NONE);

	while (!s->stop) {
		struct pollfd pfd[4];
		int64_t deadline;
		int timeout;
		nfds_t n = 0;

		now = clock_mono_ms();
		deadline = control_next_deadline(&s->ctl);
		if (s->streams_open) {
			deadline = min_deadline(deadline, civ_stream_next_deadline(&s->civ));
			deadline = min_deadline(deadline, audio_stream_next_deadline(&s->audio));
		}
		timeout = C_POLL_MAX_MS;
		if (deadline != 0) {
			const int64_t d = deadline - now;

			timeout = d < 0 ? 0 : (d > C_POLL_MAX_MS ? C_POLL_MAX_MS : (int)d);
		}

		pfd[n].fd = s->wake[0];
		pfd[n++].events = POLLIN;
		pfd[n].fd = s->u_ctl.fd;
		pfd[n++].events = POLLIN;
		if (s->streams_open) {
			pfd[n].fd = s->u_civ.fd;
			pfd[n++].events = POLLIN;
			pfd[n].fd = s->u_audio.fd;
			pfd[n++].events = POLLIN;
		}

		if (poll(pfd, n, timeout) < 0 && errno != EINTR)
			break;

		now = clock_mono_ms();

		if (pfd[0].revents & POLLIN) {
			uint8_t junk[64];

			if (read(s->wake[0], junk, sizeof(junk)) < 0 && errno != EAGAIN) {
				// Wake pipe broken: nothing sensible left to do
				break;
			}
		}

		drain(s, &s->u_ctl, 0, now);
		if (s->streams_open && !s->stop) {
			drain(s, &s->u_civ, 1, now);
			drain(s, &s->u_audio, 2, now);
		}
		if (s->stop)
			break;

		flush_civq(s, now);

		control_tick(&s->ctl, now);
		if (s->streams_open) {
			civ_stream_tick(&s->civ, now);
			audio_stream_tick(&s->audio, now);
		}

		if (s->heard && now - s->last_rx > C_LINK_LOSS_MS) {
			set_state(s, SESSION_LOST, SESSION_REASON_LINK_LOST);
			break;
		}

		update_info(s);
	}

	control_shutdown(&s->ctl, clock_mono_ms());
	udp_socket_close(&s->u_civ);
	udp_socket_close(&s->u_audio);
	udp_socket_close(&s->u_ctl);

	pthread_mutex_lock(&s->lock);
	if (s->info.state != SESSION_LOST && s->info.state != SESSION_FAILED)
		s->info.state = SESSION_CLOSED;
	pthread_mutex_unlock(&s->lock);
	notify(s);

	return NULL;
}

//
// Public API
//

int session_connect(struct session **out, const struct profile *p, const struct session_hooks *h)
{
	struct session *s = calloc(1, sizeof(*s));

	if (s == NULL)
		return -1;

	s->prof = *p;
	if (h != NULL)
		s->hooks = *h;
	s->u_ctl.fd = -1;
	s->u_civ.fd = -1;
	s->u_audio.fd = -1;

	if (pipe(s->wake) != 0) {
		free(s);
		return -1;
	}
	pthread_mutex_init(&s->lock, NULL);

	if (pthread_create(&s->thread, NULL, net_thread, s) != 0) {
		close(s->wake[0]);
		close(s->wake[1]);
		pthread_mutex_destroy(&s->lock);
		free(s);
		return -1;
	}

	*out = s;
	return 0;
}

void session_disconnect(struct session *s)
{
	const uint8_t b = 1;

	if (s == NULL)
		return;

	s->stop = 1;
	if (write(s->wake[1], &b, 1) < 0) {
		// Thread still sees stop on its next poll timeout (<= 500 ms)
	}
	pthread_join(s->thread, NULL);

	close(s->wake[0]);
	close(s->wake[1]);
	pthread_mutex_destroy(&s->lock);
	free(s);
}

void session_get_info(struct session *s, struct session_info *out)
{
	pthread_mutex_lock(&s->lock);
	*out = s->info;
	pthread_mutex_unlock(&s->lock);
}

int session_civ_send(struct session *s, const uint8_t *civ, size_t len)
{
	const uint8_t b = 1;
	int rc = -1;

	pthread_mutex_lock(&s->lock);
	if (len <= 0xffff && s->civq_len + 2 + len <= sizeof(s->civq)) {
		s->civq[s->civq_len] = (uint8_t)(len & 0xff);
		s->civq[s->civq_len + 1] = (uint8_t)(len >> 8);
		memcpy(s->civq + s->civq_len + 2, civ, len);
		s->civq_len += 2 + len;
		rc = 0;
	}
	pthread_mutex_unlock(&s->lock);

	if (rc == 0 && write(s->wake[1], &b, 1) < 0) {
		// Picked up on the next poll timeout (<= 500 ms)
	}

	return rc;
}
