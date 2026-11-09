// SPDX-License-Identifier: GPL-3.0-or-later
//
// Host test: TX audio pipeline (ring, gain, resample, every TX codec with a
// decode round trip) and TX audio packets on the audio stream.
//

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/audio_decode.h"
#include "core/audio_stream.h"
#include "core/proto.h"
#include "core/tx_audio.h"

static int G_FAILS;

#define CHECK(cond) do { \
	if (!(cond)) { \
		fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
		G_FAILS++; \
	} \
} while (0)

#define C_PI 3.14159265358979323846

static struct tx_audio G_TX;
static size_t G_PHASE;

//
// Collected encoded blocks
//

static struct {
	size_t n;
	size_t len[64];
	uint8_t data[64][TX_AUDIO_MAX_ENC];
	float peak;
} G_OUT;

static void on_emit(void *ctx, const uint8_t *data, size_t len, float peak, float rms)
{
	(void)ctx;
	(void)rms;
	if (G_OUT.n < 64 && len <= TX_AUDIO_MAX_ENC) {
		memcpy(G_OUT.data[G_OUT.n], data, len);
		G_OUT.len[G_OUT.n] = len;
	}
	G_OUT.n++;
	G_OUT.peak = peak;
}

static void write_sine(struct tx_audio *t, uint32_t rate, double hz, double amp, size_t frames, int ch)
{
	static float buf[48000 * 2];
	size_t phase = G_PHASE;
	size_t i;
	int c;

	for (i = 0; i < frames; i++) {
		const float v = (float)(amp * sin(2.0 * C_PI * hz * (double)(phase + i) / (double)rate));

		for (c = 0; c < ch; c++)
			buf[i * (size_t)ch + (size_t)c] = c == 0 ? v : 0.0f;
	}
	G_PHASE += frames;
	tx_audio_write(t, buf, frames, ch);
}

//
// Decode all collected blocks with the RX decoder, return RMS of the tail
//

static double decoded_rms(uint8_t codec, uint32_t rate, size_t *frames_out)
{
	static float out[AUDIO_DECODE_MAX_FRAMES * 2];
	struct audio_decoder d;
	double sq = 0.0;
	size_t total = 0;
	size_t counted = 0;
	size_t i;

	CHECK(audio_decoder_init(&d, codec, rate) == 0);
	for (i = 0; i < G_OUT.n && i < 64; i++) {
		const int n = audio_decode(&d, G_OUT.data[i], G_OUT.len[i], out, AUDIO_DECODE_MAX_FRAMES);
		int k;

		CHECK(n > 0);
		// skip the first blocks (resampler / codec start-up)
		for (k = 0; i >= 3 && k < n; k++) {
			sq += (double)out[k] * out[k];
			counted++;
		}
		if (n > 0)
			total += (size_t)n;
	}
	audio_decoder_free(&d);
	*frames_out = total;
	return counted ? sqrt(sq / (double)counted) : 0.0;
}

static void run_codec(uint8_t codec, uint32_t cap, uint32_t txr, size_t expect_len)
{
	size_t frames = 0;
	double rms;

	memset(&G_OUT, 0, sizeof(G_OUT));
	tx_audio_init(&G_TX, cap);
	CHECK(tx_audio_configure(&G_TX, codec, txr) == 0);

	// 200 ms of 1 kHz at 0.5: ten 20 ms blocks
	write_sine(&G_TX, cap, 1000.0, 0.5, cap / 5, 1);
	CHECK(tx_audio_pump(&G_TX, on_emit, NULL) >= 9);
	CHECK(G_OUT.n >= 9 && G_OUT.n <= 10);
	if (expect_len != 0)
		CHECK(G_OUT.len[2] == expect_len);

	rms = decoded_rms(codec, txr, &frames);
	CHECK(frames >= (size_t)txr / 50 * 9);
	// sine at 0.5 has RMS 0.354
	if (fabs(rms - 0.354) > 0.06)
		fprintf(stderr, "codec 0x%02x %u->%u rms %.3f\n", codec, cap, txr, rms);
	CHECK(fabs(rms - 0.354) <= 0.06);
	tx_audio_stop(&G_TX);
}

static void test_codecs(void)
{
	// 20 ms blocks: PCM16 at 8 kHz = 160 samples = 320 bytes, u-law 160, PCM8 160
	run_codec(AUDIO_CODEC_PCM16_1, 48000, 48000, 1920);
	run_codec(AUDIO_CODEC_PCM16_1, 48000, 8000, 320);
	run_codec(AUDIO_CODEC_PCM16_1, 44100, 16000, 640);
	run_codec(AUDIO_CODEC_ULAW_1, 48000, 8000, 160);
	run_codec(AUDIO_CODEC_PCM8_1, 48000, 8000, 160);
	run_codec(AUDIO_CODEC_OPUS_1, 48000, 48000, 0);
	run_codec(AUDIO_CODEC_ADPCM_1, 48000, 8000, 0);
}

static void test_refused(void)
{
	tx_audio_init(&G_TX, 48000);
	CHECK(tx_audio_configure(&G_TX, AUDIO_CODEC_PCM16_2, 48000) == TX_AUDIO_ERR_CODEC);
	CHECK(tx_audio_configure(&G_TX, AUDIO_CODEC_OPUS_2, 48000) == TX_AUDIO_ERR_CODEC);
	CHECK(tx_audio_configure(&G_TX, AUDIO_CODEC_OPUS_1, 44100) == TX_AUDIO_ERR_RATE);
	CHECK(tx_audio_pump(&G_TX, on_emit, NULL) == 0);

	tx_audio_init(&G_TX, 0);
	CHECK(tx_audio_configure(&G_TX, AUDIO_CODEC_PCM16_1, 48000) == TX_AUDIO_ERR_RATE);
}

static void test_ring_gain_stereo(void)
{
	struct tx_audio_stats st;
	int16_t v;

	// Stereo capture: first channel taken; gain 50 % halves the peak
	G_PHASE = 0;
	memset(&G_OUT, 0, sizeof(G_OUT));
	tx_audio_init(&G_TX, 48000);
	CHECK(tx_audio_configure(&G_TX, AUDIO_CODEC_PCM16_1, 48000) == 0);
	tx_audio_set_gain(&G_TX, 50);
	write_sine(&G_TX, 48000, 1000.0, 0.8, 960, 2);
	CHECK(tx_audio_pump(&G_TX, on_emit, NULL) == 1);
	CHECK(fabsf(G_OUT.peak - 0.4f) < 0.01f);
	v = (int16_t)(G_OUT.data[0][12 * 2] | (G_OUT.data[0][12 * 2 + 1] << 8));
	CHECK(abs(v - (int)lrint(0.4 * 32767.0 * sin(2.0 * C_PI * 12.0 / 48.0))) <= 2);

	// Partial block stays in the ring
	memset(&G_OUT, 0, sizeof(G_OUT));
	write_sine(&G_TX, 48000, 1000.0, 0.8, 500, 1);
	CHECK(tx_audio_pump(&G_TX, on_emit, NULL) == 0);
	write_sine(&G_TX, 48000, 1000.0, 0.8, 460, 1);
	CHECK(tx_audio_pump(&G_TX, on_emit, NULL) == 1);

	// Discard empties it
	write_sine(&G_TX, 48000, 1000.0, 0.8, 2000, 1);
	tx_audio_discard(&G_TX);
	CHECK(tx_audio_pump(&G_TX, on_emit, NULL) == 0);

	// Overrun: more than the ring holds is dropped and counted
	write_sine(&G_TX, 48000, 1000.0, 0.8, 40000, 1);
	tx_audio_get_stats(&G_TX, &st);
	CHECK(st.overrun_frames == 40000 - TX_AUDIO_RING_FRAMES);
	tx_audio_stop(&G_TX);
}

static void test_ulaw(void)
{
	// G.711 reference points
	CHECK(tx_audio_ulaw(0.0f) == 0xff);
	CHECK(tx_audio_ulaw(1.0f) == 0x80);
	CHECK(tx_audio_ulaw(-1.0f) == 0x00);
}

//
// TX audio packets: header as wfview audio_packet, 1364 byte chunks
//

static struct {
	size_t n;
	size_t len[8];
	uint8_t pkt[8][PROTO_MAX_PACKET];
} G_NET;

static int net_send(void *ctx, const uint8_t *buf, size_t len)
{
	(void)ctx;
	if (G_NET.n < 8) {
		memcpy(G_NET.pkt[G_NET.n], buf, len);
		G_NET.len[G_NET.n] = len;
	}
	G_NET.n++;
	return 0;
}

static uint32_t net_day_ms(void *ctx)
{
	(void)ctx;
	return 0;
}

static void ev_none(void *ctx)
{
	(void)ctx;
}

static void test_packets(void)
{
	static struct audio_stream a;
	static uint8_t data[3000];
	const struct udp_io io = { net_send, net_day_ms, NULL };
	const struct audio_rx_events ev = { ev_none, ev_none, NULL, NULL };
	size_t i;

	for (i = 0; i < sizeof(data); i++)
		data[i] = (uint8_t)i;

	audio_stream_init(&a, &io, 0x11223344u, &ev, 150, 0);
	a.s.remote_id = 0x55667788u;
	G_NET.n = 0;

	CHECK(audio_stream_send_tx(&a, data, 3000, 10) == 0);
	CHECK(G_NET.n == 3);
	CHECK(G_NET.len[0] == AUDIO_HEADER + 1364);
	CHECK(G_NET.len[2] == AUDIO_HEADER + 272);
	CHECK(proto_get_le32(G_NET.pkt[0] + PROTO_OFF_LEN) == AUDIO_HEADER + 1364);
	CHECK(proto_get_le16(G_NET.pkt[0] + PROTO_OFF_TYPE) == 0);
	CHECK(proto_get_le32(G_NET.pkt[0] + PROTO_OFF_SENTID) == 0x11223344u);
	CHECK(proto_get_le32(G_NET.pkt[0] + PROTO_OFF_RCVDID) == 0x55667788u);
	CHECK(proto_get_le16(G_NET.pkt[0] + AUDIO_OFF_IDENT) == 0x0080);
	CHECK(proto_get_be16(G_NET.pkt[0] + AUDIO_OFF_SENDSEQ) == 0);
	CHECK(proto_get_be16(G_NET.pkt[1] + AUDIO_OFF_SENDSEQ) == 1);
	CHECK(proto_get_be16(G_NET.pkt[2] + AUDIO_OFF_DATALEN) == 272);
	CHECK(G_NET.pkt[1][AUDIO_HEADER] == (uint8_t)1364);
	CHECK(proto_get_le16(G_NET.pkt[1] + PROTO_OFF_SEQ) == proto_get_le16(G_NET.pkt[0] + PROTO_OFF_SEQ) + 1);

	// 0xa0 byte chunk: ident 0x9781
	G_NET.n = 0;
	CHECK(audio_stream_send_tx(&a, data, 0xa0, 20) == 0);
	CHECK(G_NET.n == 1);
	CHECK(proto_get_le16(G_NET.pkt[0] + AUDIO_OFF_IDENT) == 0x9781);
	CHECK(proto_get_be16(G_NET.pkt[0] + AUDIO_OFF_SENDSEQ) == 3);
	CHECK(a.packets_tx == 4 && a.bytes_tx == 3000 + 0xa0);
	audio_stream_close(&a);
}

int main(void)
{
	test_codecs();
	test_refused();
	test_ring_gain_stereo();
	test_ulaw();
	test_packets();

	if (G_FAILS != 0) {
		fprintf(stderr, "test_tx: %d failure(s)\n", G_FAILS);
		return EXIT_FAILURE;
	}
	printf("test_tx: ok\n");
	return EXIT_SUCCESS;
}
