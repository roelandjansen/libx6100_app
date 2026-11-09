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

#include "core/audio_rx.h"
#include "core/audio_stream.h"
#include "core/civ_stream.h"
#include "core/rig.h"
#include "core/tx_audio.h"
#include "core/udp_socket.h"

#define C_POLL_MAX_MS		500
#define C_LINK_LOSS_MS		(PROTO_STALE_CONNECTION_S * 1000)
#define C_CIV_QUEUE		8192
#define C_CTL_QUEUE		64
#define C_TX_POLL_MS		10

struct ctl_cmd {
	enum session_ctl_op op;
	uint8_t vfo;
	uint64_t u;
	int a;
	int b;
	int c;
	char name[RIG_NAME_LEN];
};

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
	struct audio_rx *arx;
	struct rig rig;
	bool rig_active;
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
	struct radio_state state;
	struct radio_scope_row rows[RADIO_MAX_RX];

	//
	// Control queue (under lock) and client-side settings
	//
	size_t ctlq_n;
	struct ctl_cmd ctlq[C_CTL_QUEUE];
	atomic_uint step_hz;
	atomic_bool freq_lock;

	//
	// TX (network thread, settings atomic)
	//
	struct tx_audio *tx;
	bool tx_ready;
	bool ptt;
	bool rig_tx_prev;
	int64_t t_ptt_timeout;
	atomic_llong last_heartbeat;
	atomic_bool ptt_timeout_en;
	atomic_int ptt_timeout_s;
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
	case SESSION_REASON_UNKNOWN_MODEL:	return "unknown model";
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
		i->audio_lateness_ms = s->audio.s.ping_lateness_ms;
		if (s->arx != NULL) {
			struct audio_rx_stats st;

			audio_rx_get_stats(s->arx, &st);
			i->audio_latency_ms = st.latency_ms;
			i->audio_underruns = st.underruns;
			i->audio_overrun_frames = st.overrun_frames;
			i->audio_decode_errors = st.decode_errors;
			i->audio_peak = st.peak;
		}
		i->rx_codec = s->ctl.cfg.rx_codec;
	}
	i->tx_available = s->streams_open && s->tx_ready && s->rig_active && rig_has_ptt(&s->rig);
	i->ptt = s->ptt;
	i->ptt_remaining_s = 0;
	if (s->ptt && s->t_ptt_timeout != 0) {
		const int64_t left = s->t_ptt_timeout - clock_mono_ms();

		i->ptt_remaining_s = left > 0 ? (int)((left + 999) / 1000) : 0;
	}
	i->tx_codec = s->ctl.cfg.tx_codec;
	i->tx_sample = s->ctl.cfg.tx_sample;
	if (s->streams_open) {
		i->tx_packets = s->audio.packets_tx;
		i->tx_bytes = s->audio.bytes_tx;
	}
	if (s->tx != NULL) {
		struct tx_audio_stats ts;

		tx_audio_get_stats(s->tx, &ts);
		i->tx_blocks = ts.blocks;
		i->tx_encode_errors = ts.encode_errors;
		i->tx_overrun_frames = ts.overrun_frames;
		i->tx_peak = ts.peak;
		i->tx_rms = ts.rms;
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
// TX (network thread)
//

static void tx_notice(struct session *s, const char *msg)
{
	pthread_mutex_lock(&s->lock);
	snprintf(s->info.tx_notice, sizeof(s->info.tx_notice), "%s", msg);
	s->info.tx_notices++;
	pthread_mutex_unlock(&s->lock);
	notify(s);
}

//
// PTT on / off as MainController::setPTT(): Transceiver Status, the PTT
// timeout armed on PTT on (never extended), stopped on PTT off
//

static int do_ptt(struct session *s, bool on, int64_t now)
{
	int rc;

	if (on && (!s->tx_ready || !s->rig_active))
		return SESSION_ERR_NO_TX;
	if (!s->rig_active)
		return RIG_ERR_NO_CAPS;

	rc = rig_ptt(&s->rig, on);
	if (rc != 0)
		return rc;

	if (on) {
		if (!s->ptt) {
			tx_audio_discard(s->tx);
			atomic_store(&s->last_heartbeat, now);
		}
		s->ptt = true;
		if (atomic_load(&s->ptt_timeout_en) && s->t_ptt_timeout == 0)
			s->t_ptt_timeout = now + (int64_t)atomic_load(&s->ptt_timeout_s) * 1000;
	} else {
		s->ptt = false;
		s->t_ptt_timeout = 0;
	}

	return 0;
}

//
// Local PTT release by a guard, with a notice
//

static void ptt_guard(struct session *s, const char *why, int64_t now)
{
	if (!s->ptt)
		return;
	if (do_ptt(s, false, now) != 0) {
		s->ptt = false;
		s->t_ptt_timeout = 0;
	}
	tx_notice(s, why);
}

static void emit_tx(void *ctx, const uint8_t *data, size_t len, float peak, float rms)
{
	struct session *s = ctx;

	(void)peak;
	(void)rms;
	if (s->streams_open)
		audio_stream_send_tx(&s->audio, data, len, clock_mono_ms());
}

static void tx_run(struct session *s, int64_t now)
{
	const int hb = s->prof.ptt_heartbeat_ms;

	if (s->tx == NULL)
		return;

	if (!s->ptt || !s->tx_ready) {
		tx_audio_discard(s->tx);
		return;
	}

	// Timeout switched off while keyed: stop it (wfview ct_pttTimeout)
	if (!atomic_load(&s->ptt_timeout_en))
		s->t_ptt_timeout = 0;
	if (s->t_ptt_timeout != 0 && now >= s->t_ptt_timeout) {
		ptt_guard(s, "PTT timeout", now);
		return;
	}
	if (hb > 0 && now - atomic_load(&s->last_heartbeat) > hb) {
		ptt_guard(s, "PTT released: no heartbeat", now);
		return;
	}

	tx_audio_pump(s->tx, emit_tx, s);
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

//
// Rig layer events (network thread)
//

static int rig_send(void *ctx, const uint8_t *civ, size_t len)
{
	struct session *s = ctx;

	if (!s->streams_open)
		return -1;
	return civ_stream_send(&s->civ, civ, len, clock_mono_ms());
}

static void rig_changed(void *ctx, uint32_t groups)
{
	struct session *s = ctx;
	int i;

	// The radio has returned to receive, however that happened: stop the
	// PTT timeout (MainController funcTransceiverStatus)
	if (s->rig.st.transmit.valid) {
		const bool tx = s->rig.st.transmit.value != 0;

		if (s->rig_tx_prev && !tx)
			s->t_ptt_timeout = 0;
		s->rig_tx_prev = tx;
	}

	pthread_mutex_lock(&s->lock);
	s->state = s->rig.st;
	if (groups & RADIO_CHG_SCOPE) {
		for (i = 0; i < RADIO_MAX_RX; i++)
			s->rows[i] = s->rig.row[i];
	}
	pthread_mutex_unlock(&s->lock);

	if (s->hooks.notify != NULL)
		s->hooks.notify(s->hooks.ctx);
}

static void rig_error(void *ctx, const char *msg)
{
	struct session *s = ctx;

	// Hard error on an unknown model (deviation 3): no fallback rig file
	snprintf(s->ctl.status, sizeof(s->ctl.status), "%s", msg);
	set_state(s, SESSION_FAILED, SESSION_REASON_UNKNOWN_MODEL);
	s->stop = 1;
}

static void on_civ_data(void *ctx, const uint8_t *civ, size_t len)
{
	struct session *s = ctx;

	if (s->rig_active)
		rig_input(&s->rig, civ, len, clock_mono_ms());

	if (s->hooks.civ_data != NULL)
		s->hooks.civ_data(s->hooks.ctx, civ, len);
}

//
// Audio stream events (network thread): feed the RX pipeline, then the hooks
//

static void on_audio_start(void *ctx)
{
	struct session *s = ctx;

	if (s->hooks.audio_start != NULL)
		s->hooks.audio_start(s->hooks.ctx);
}

static void on_audio_stop(void *ctx)
{
	struct session *s = ctx;

	if (s->hooks.audio_stop != NULL)
		s->hooks.audio_stop(s->hooks.ctx);
}

static void on_audio_data(void *ctx, uint32_t seq, const uint8_t *payload, size_t len)
{
	struct session *s = ctx;
	const int64_t now = clock_mono_ms();

	if (s->arx != NULL)
		audio_rx_push(s->arx, payload, len, now, now);

	if (s->hooks.audio_data != NULL)
		s->hooks.audio_data(s->hooks.ctx, seq, payload, len);
}

static void ev_open_streams(void *ctx, uint16_t civ_port, uint16_t audio_port)
{
	struct session *s = ctx;
	const int64_t now = clock_mono_ms();
	struct udp_io io_civ = { udp_socket_send, clock_day_ms, &s->u_civ };
	struct udp_io io_audio = { udp_socket_send, clock_day_ms, &s->u_audio };
	struct audio_rx_events aev = { on_audio_start, on_audio_stop, on_audio_data, s };

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
	// Codec as finally negotiated (Opus may have been forced to LPCM16 at login)
	if (s->arx != NULL &&
			audio_rx_configure(s->arx, s->ctl.cfg.rx_codec, s->prof.rx_sample, s->prof.rx_latency) != 0) {
		snprintf(s->ctl.status, sizeof(s->ctl.status), "Unsupported RX codec 0x%02x", s->ctl.cfg.rx_codec);
	}

	// TX path: only when the radio announced TX (tx_sample forced to 0 at
	// login otherwise) and a capture device is configured
	s->tx_ready = false;
	if (s->tx != NULL && s->ctl.cfg.tx_sample != 0 && s->ctl.cfg.tx_codec != 0) {
		if (tx_audio_configure(s->tx, s->ctl.cfg.tx_codec, s->ctl.cfg.tx_sample) == 0)
			s->tx_ready = true;
		else
			snprintf(s->ctl.status, sizeof(s->ctl.status), "Unsupported TX codec 0x%02x at %u Hz",
					s->ctl.cfg.tx_codec, (unsigned int)s->ctl.cfg.tx_sample);
	}

	s->streams_open = true;

	// Rig layer: Transceiver ID broadcast first, rig file from the announced model
	{
		const struct rig_events rev = { rig_send, rig_changed, rig_error, s };

		rig_init(&s->rig, s->prof.rig_dir, s->ctl.baudrate, s->prof.poll_ms, &rev, now);
		s->rig_active = true;
	}
	set_state(s, SESSION_CONNECTED, SESSION_REASON_NONE);
}

static void ev_close_streams(void *ctx)
{
	struct session *s = ctx;
	const int64_t now = clock_mono_ms();

	if (!s->streams_open)
		return;

	if (s->ptt) {
		s->ptt = false;
		s->t_ptt_timeout = 0;
		tx_notice(s, "PTT released: streams closed");
	}
	s->tx_ready = false;
	if (s->tx != NULL)
		tx_audio_stop(s->tx);

	audio_stream_close(&s->audio);
	civ_stream_close(&s->civ, now);
	if (s->arx != NULL)
		audio_rx_stop(s->arx);
	s->rig_active = false;
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

//
// Run queued controls (network thread)
//

static void flush_ctl(struct session *s)
{
	struct ctl_cmd local[C_CTL_QUEUE];
	size_t n;
	size_t i;

	pthread_mutex_lock(&s->lock);
	n = s->ctlq_n;
	memcpy(local, s->ctlq, n * sizeof(local[0]));
	s->ctlq_n = 0;
	pthread_mutex_unlock(&s->lock);

	for (i = 0; i < n; i++) {
		const struct ctl_cmd *c = &local[i];
		int rc = RIG_ERR_NO_CAPS;

		if (c->op == SESSION_CTL_PTT) {
			rc = do_ptt(s, c->a != 0, clock_mono_ms());
		} else if (c->op == SESSION_CTL_CAPTURE_LOST) {
			ptt_guard(s, "PTT released: capture lost", clock_mono_ms());
			rc = 0;
		} else if (s->rig_active) {
			switch (c->op) {
			case SESSION_CTL_FREQ:
				rc = rig_set_freq(&s->rig, c->vfo, c->u);
				break;
			case SESSION_CTL_TUNE:
				rc = rig_tune(&s->rig, c->vfo, c->a, (uint32_t)c->u);
				break;
			case SESSION_CTL_MODE:
				rc = rig_set_mode(&s->rig, c->vfo, c->a, c->b, c->c);
				break;
			case SESSION_CTL_VALUE:
				rc = rig_set_value(&s->rig, c->name, c->vfo, c->a);
				break;
			case SESSION_CTL_SPAN:
				rc = rig_set_span(&s->rig, c->vfo, (uint32_t)c->u);
				break;
			case SESSION_CTL_GET:
				rc = rig_get(&s->rig, c->name, c->vfo);
				break;
			case SESSION_CTL_ATU_TUNE:
				// MainController::tuneNow(): Tuner/ATU Status 2, Immediate, unique
				rc = rig_set_value(&s->rig, "Tuner/ATU Status", 0, 2);
				break;
			case SESSION_CTL_PTT:
			case SESSION_CTL_CAPTURE_LOST:
				break;
			}
		}

		if (rc != 0) {
			pthread_mutex_lock(&s->lock);
			s->info.ctl_rejected++;
			s->info.ctl_last_error = rc;
			pthread_mutex_unlock(&s->lock);
		}
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
			if (s->rig_active)
				deadline = min_deadline(deadline, rig_next_deadline(&s->rig));
		}
		timeout = C_POLL_MAX_MS;
		if (deadline != 0) {
			const int64_t d = deadline - now;

			timeout = d < 0 ? 0 : (d > C_POLL_MAX_MS ? C_POLL_MAX_MS : (int)d);
		}
		// Keyed: pick up captured audio every 10 ms (20 ms blocks)
		if (s->ptt && timeout > C_TX_POLL_MS)
			timeout = C_TX_POLL_MS;

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
		flush_ctl(s);
		if (s->streams_open)
			tx_run(s, now);

		control_tick(&s->ctl, now);
		if (s->streams_open) {
			civ_stream_tick(&s->civ, now);
			audio_stream_tick(&s->audio, now);
			if (s->rig_active)
				rig_tick(&s->rig, now);
		}

		if (s->heard && now - s->last_rx > C_LINK_LOSS_MS) {
			// Link loss guard: PTT off locally (nothing reaches the server)
			if (s->ptt) {
				s->ptt = false;
				s->t_ptt_timeout = 0;
				tx_notice(s, "PTT released: link lost");
			}
			set_state(s, SESSION_LOST, SESSION_REASON_LINK_LOST);
			break;
		}

		update_info(s);
	}

	// Leaving while keyed (disconnect, failure): PTT off straight to the rig
	if (s->ptt && s->streams_open && s->rig_active) {
		if (rig_ptt_off_now(&s->rig) == 0)
			tx_notice(s, "PTT released: disconnect");
		s->ptt = false;
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
	atomic_init(&s->step_hz, 100u);
	atomic_init(&s->freq_lock, false);
	atomic_init(&s->last_heartbeat, 0);
	atomic_init(&s->ptt_timeout_en, p->ptt_timeout_enabled);
	atomic_init(&s->ptt_timeout_s, p->ptt_timeout_s < 30 ? 30 : (p->ptt_timeout_s > 999 ? 999 : p->ptt_timeout_s));

	if (p->cap_rate != 0) {
		s->tx = calloc(1, sizeof(*s->tx));
		if (s->tx == NULL) {
			free(s);
			return -1;
		}
		tx_audio_init(s->tx, p->cap_rate);
		tx_audio_set_gain(s->tx, p->mic_gain);
	}

	if (p->dev_rate != 0) {
		s->arx = calloc(1, sizeof(*s->arx));
		if (s->arx == NULL) {
			free(s->tx);
			free(s);
			return -1;
		}
		audio_rx_init(s->arx, p->dev_rate);
	}

	if (pipe(s->wake) != 0) {
		free(s->arx);
		free(s->tx);
		free(s);
		return -1;
	}
	pthread_mutex_init(&s->lock, NULL);

	if (pthread_create(&s->thread, NULL, net_thread, s) != 0) {
		close(s->wake[0]);
		close(s->wake[1]);
		pthread_mutex_destroy(&s->lock);
		free(s->arx);
		free(s->tx);
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
	if (s->arx != NULL) {
		audio_rx_stop(s->arx);
		free(s->arx);
	}
	if (s->tx != NULL) {
		tx_audio_stop(s->tx);
		free(s->tx);
	}
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

//
// Device callback: fill frames stereo floats (silence without a pipeline).
// The platform must stop its device before session_disconnect().
//

void session_audio_read(struct session *s, float *out, size_t frames)
{
	if (s == NULL || s->arx == NULL) {
		memset(out, 0, frames * 2 * sizeof(float));
		return;
	}

	audio_rx_read(s->arx, out, frames);
}

void session_set_volume(struct session *s, uint8_t volume)
{
	if (s->arx != NULL)
		audio_rx_set_volume(s->arx, volume);
}

void session_get_state(struct session *s, struct radio_state *out)
{
	pthread_mutex_lock(&s->lock);
	*out = s->state;
	pthread_mutex_unlock(&s->lock);
}

bool session_get_scope_row(struct session *s, int receiver, struct radio_scope_row *out)
{
	bool ok = false;

	if (receiver < 0 || receiver >= RADIO_MAX_RX)
		return false;

	pthread_mutex_lock(&s->lock);
	if (s->rows[receiver].valid) {
		*out = s->rows[receiver];
		ok = true;
	}
	pthread_mutex_unlock(&s->lock);
	return ok;
}

//
// Controls: push onto the queue and wake the network thread
//

static int ctl_push(struct session *s, const struct ctl_cmd *c)
{
	const uint8_t b = 1;
	int rc = 0;

	pthread_mutex_lock(&s->lock);
	if (s->ctlq_n == C_CTL_QUEUE) {
		rc = SESSION_ERR_FULL;
	} else {
		s->ctlq[s->ctlq_n++] = *c;
		s->info.ctl_queued++;
	}
	pthread_mutex_unlock(&s->lock);

	if (rc == 0 && write(s->wake[1], &b, 1) < 0) {
		// Picked up on the next poll timeout (<= 500 ms)
	}

	return rc;
}

int session_set_freq(struct session *s, uint8_t vfo, uint64_t hz)
{
	struct ctl_cmd c;

	if (atomic_load(&s->freq_lock))
		return SESSION_ERR_LOCKED;

	memset(&c, 0, sizeof(c));
	c.op = SESSION_CTL_FREQ;
	c.vfo = vfo;
	c.u = hz;
	return ctl_push(s, &c);
}

int session_tune(struct session *s, uint8_t vfo, int steps)
{
	struct ctl_cmd c;

	if (atomic_load(&s->freq_lock))
		return SESSION_ERR_LOCKED;

	memset(&c, 0, sizeof(c));
	c.op = SESSION_CTL_TUNE;
	c.vfo = vfo;
	c.a = steps;
	c.u = atomic_load(&s->step_hz);
	return ctl_push(s, &c);
}

int session_set_mode(struct session *s, uint8_t vfo, int mode_num, int data, int filter)
{
	struct ctl_cmd c;

	memset(&c, 0, sizeof(c));
	c.op = SESSION_CTL_MODE;
	c.vfo = vfo;
	c.a = mode_num;
	c.b = data;
	c.c = filter;
	return ctl_push(s, &c);
}

int session_set_value(struct session *s, const char *type, uint8_t receiver, int value)
{
	struct ctl_cmd c;

	memset(&c, 0, sizeof(c));
	c.op = SESSION_CTL_VALUE;
	c.vfo = receiver;
	c.a = value;
	snprintf(c.name, sizeof(c.name), "%s", type);
	return ctl_push(s, &c);
}

int session_set_span(struct session *s, uint8_t receiver, uint32_t half_span_hz)
{
	struct ctl_cmd c;

	memset(&c, 0, sizeof(c));
	c.op = SESSION_CTL_SPAN;
	c.vfo = receiver;
	c.u = half_span_hz;
	return ctl_push(s, &c);
}

int session_get_value(struct session *s, const char *type, uint8_t receiver)
{
	struct ctl_cmd c;

	memset(&c, 0, sizeof(c));
	c.op = SESSION_CTL_GET;
	c.vfo = receiver;
	snprintf(c.name, sizeof(c.name), "%s", type);
	return ctl_push(s, &c);
}

void session_set_step(struct session *s, uint32_t hz)
{
	atomic_store(&s->step_hz, hz == 0 ? 1u : hz);
}

uint32_t session_get_step(struct session *s)
{
	return atomic_load(&s->step_hz);
}

void session_set_freq_lock(struct session *s, bool locked)
{
	atomic_store(&s->freq_lock, locked);
}

bool session_get_freq_lock(struct session *s)
{
	return atomic_load(&s->freq_lock);
}

//
// TX (phase 5)
//

int tx_ptt(struct session *s, int on)
{
	struct ctl_cmd c;
	bool connected;
	bool available;

	pthread_mutex_lock(&s->lock);
	connected = s->info.state == SESSION_CONNECTED;
	available = s->info.tx_available;
	pthread_mutex_unlock(&s->lock);

	// PTT off is always passed on
	if (on) {
		if (!s->prof.enable_ptt)
			return SESSION_ERR_PTT_DISABLED;
		if (!connected)
			return SESSION_ERR_NOT_CONNECTED;
		if (!available)
			return SESSION_ERR_NO_TX;
		atomic_store(&s->last_heartbeat, clock_mono_ms());
	}

	memset(&c, 0, sizeof(c));
	c.op = SESSION_CTL_PTT;
	c.a = on != 0;
	return ctl_push(s, &c);
}

int tx_ptt_heartbeat(struct session *s)
{
	atomic_store(&s->last_heartbeat, clock_mono_ms());
	return 0;
}

int tx_tune(struct session *s)
{
	struct ctl_cmd c;
	bool connected;

	if (!s->prof.enable_ptt)
		return SESSION_ERR_PTT_DISABLED;

	pthread_mutex_lock(&s->lock);
	connected = s->info.state == SESSION_CONNECTED;
	pthread_mutex_unlock(&s->lock);
	if (!connected)
		return SESSION_ERR_NOT_CONNECTED;

	memset(&c, 0, sizeof(c));
	c.op = SESSION_CTL_ATU_TUNE;
	return ctl_push(s, &c);
}

void tx_set_mic_gain(struct session *s, int percent)
{
	if (s->tx != NULL)
		tx_audio_set_gain(s->tx, percent < 0 ? 0u : (unsigned int)percent);
}

void tx_set_ptt_timeout(struct session *s, int enabled, int seconds)
{
	// qBound(30, PTTTimeoutSeconds, 999) as wfview; applies from the next PTT on
	atomic_store(&s->ptt_timeout_s, seconds < 30 ? 30 : (seconds > 999 ? 999 : seconds));
	atomic_store(&s->ptt_timeout_en, enabled != 0);
}

void tx_capture_write(struct session *s, const float *in, size_t frames, int channels)
{
	if (s != NULL && s->tx != NULL)
		tx_audio_write(s->tx, in, frames, channels);
}

void tx_capture_lost(struct session *s)
{
	struct ctl_cmd c;

	memset(&c, 0, sizeof(c));
	c.op = SESSION_CTL_CAPTURE_LOST;
	if (ctl_push(s, &c) != 0) {
		// Queue full: the heartbeat guard (if set) or PTT timeout still applies
	}
}

const struct rig_def *session_rig_def(struct session *s)
{
	bool valid;

	pthread_mutex_lock(&s->lock);
	valid = s->state.rig_valid;
	pthread_mutex_unlock(&s->lock);
	return valid ? &s->rig.def : NULL;
}
