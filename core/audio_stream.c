// SPDX-License-Identifier: GPL-3.0-or-later
//
// Audio UDP stream: RX audio packets, late-packet drop, 30 s watchdog.
//
// Port of the stream part of icomUdpAudio from wfview v3-initial
// src/radio/icomudpaudio.cpp,
// wfview is copyright 2017-2026 Elliott H. Liggett (W6EL) and Phil Taylor (M0VSE), GPLv3.
//

#include "core/audio_stream.h"

#include <string.h>

#include "core/ping_delay.h"

//
// Start the player; as wfview startAudio() this also sends are-you-there
//

static void start_audio(struct audio_stream *a, int64_t now_ms, bool send_ayt)
{
	if (send_ayt)
		udp_stream_send_control(&a->s, false, PROTO_TYPE_AREYOUTHERE, 0, now_ms);

	a->playing = true;
	if (a->ev.start != NULL)
		a->ev.start(a->ev.ctx);
}

void audio_stream_init(struct audio_stream *a, const struct udp_io *io, uint32_t my_id,
		const struct audio_rx_events *ev, uint16_t rx_latency_ms, int64_t now_ms)
{
	memset(a, 0, sizeof(*a));
	a->ev = *ev;
	a->rx_latency_ms = rx_latency_ms;
	a->last_audio = now_ms;

	// udp_stream_init sends the first are-you-there for the audio kind
	udp_stream_init(&a->s, UDP_STREAM_AUDIO, io, my_id, now_ms);
	start_audio(a, now_ms, false);
	a->t_watchdog = now_ms + PROTO_WATCHDOG_PERIOD;
}

void audio_stream_close(struct audio_stream *a)
{
	if (a->playing && a->ev.stop != NULL)
		a->ev.stop(a->ev.ctx);
	a->playing = false;
	a->t_watchdog = 0;
	udp_stream_close(&a->s);
}

void audio_stream_receive(struct audio_stream *a, const uint8_t *buf, size_t len, int64_t now_ms)
{
	if (len != PROTO_CONTROL_SIZE &&
			proto_get_le16(buf + PROTO_OFF_TYPE) != 0x01 &&
			proto_get_le32(buf + PROTO_OFF_LEN) >= 0x20 &&
			len >= AUDIO_HEADER) {
		const uint16_t seq = proto_get_le16(buf + PROTO_OFF_SEQ);

		if (seq == 0)
			a->seq_prefix++;

		if (!a->playing)
			start_audio(a, now_ms, true);

		if (a->s.ping_lateness_ms > ping_delay_drop_threshold_ms(a->rx_latency_ms)) {
			a->dropped_late++;
		} else {
			a->packets_rx++;
			if (a->ev.data != NULL)
				a->ev.data(a->ev.ctx, ((uint32_t)a->seq_prefix << 16) | seq,
						buf + AUDIO_HEADER, len - AUDIO_HEADER);
			a->last_audio = now_ms;
		}
	}

	udp_stream_receive(&a->s, buf, len, now_ms);
}

void audio_stream_tick(struct audio_stream *a, int64_t now_ms)
{
	if (a->t_watchdog != 0 && now_ms >= a->t_watchdog) {
		if (now_ms - a->last_audio > AUDIO_WATCHDOG_MS) {
			if (!a->alerted) {
				a->alerted = true;
				if (a->playing && a->ev.stop != NULL)
					a->ev.stop(a->ev.ctx);
				a->playing = false;
			}
		} else {
			a->alerted = false;
		}
		a->t_watchdog = now_ms + PROTO_WATCHDOG_PERIOD;
	}

	udp_stream_tick(&a->s, now_ms);
}

int64_t audio_stream_next_deadline(const struct audio_stream *a)
{
	int64_t t = udp_stream_next_deadline(&a->s);

	if (a->t_watchdog != 0 && (t == 0 || a->t_watchdog < t))
		t = a->t_watchdog;

	return t;
}
