// SPDX-License-Identifier: GPL-3.0-or-later
//
// Native (SDL) entry point.
//
// Phase 1 test mode:
//	radio <host> <control-port> <username> <password> [client-name] [rx-codec] [rx-rate] [latency-ms]
// connects, prints state changes and counters once a second, sends a
// CI-V Transceiver ID query (FE FE 00 E1 19 00 FD) every 2 s once connected
// and prints the first CI-V frames received. RX audio plays on the default
// SDL device. Ctrl-C disconnects cleanly.
//

#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <SDL.h>

#include "core/session.h"
#include "platform/native/audio_sdl.h"

#define C_CIV_PRINT_MAX	20

static volatile sig_atomic_t G_STOP;
static atomic_int G_CIV_PRINTED;

static void on_signal(int sig)
{
	(void)sig;
	G_STOP = 1;
}

static void sleep_ms(long ms)
{
	struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };

	nanosleep(&ts, NULL);
}

//
// CI-V hook (network thread): print the first frames as hex
//

static void on_civ(void *ctx, const uint8_t *civ, size_t len)
{
	char line[3 * 64 + 1];
	size_t i;
	size_t n = 0;

	(void)ctx;
	if (atomic_fetch_add(&G_CIV_PRINTED, 1) >= C_CIV_PRINT_MAX)
		return;

	for (i = 0; i < len && i < 64; i++)
		n += (size_t)snprintf(line + n, sizeof(line) - n, "%02x ", civ[i]);
	printf("civ rx (%zu): %s%s\n", len, line, len > 64 ? "..." : "");
	fflush(stdout);
}

//
// Connect test against a wfview server
//

static int run_test(int argc, char *argv[])
{
	static const uint8_t C_RIG_ID[] = { 0xfe, 0xfe, 0x00, 0xe1, 0x19, 0x00, 0xfd };
	const struct session_hooks hooks = { NULL, on_civ, NULL, NULL, NULL, NULL };
	struct profile p;
	struct session *s = NULL;
	struct session_info last;
	struct session_info now;
	struct sigaction sa;
	long port;
	int tick = 0;
	int rc = EXIT_SUCCESS;

	memset(&p, 0, sizeof(p));
	snprintf(p.host, sizeof(p.host), "%s", argv[1]);
	port = strtol(argv[2], NULL, 10);
	if (port <= 0 || port > 65535) {
		fprintf(stderr, "error: bad port %s\n", argv[2]);
		return EXIT_FAILURE;
	}
	p.port_control = (uint16_t)port;
	snprintf(p.username, sizeof(p.username), "%s", argv[3]);
	snprintf(p.password, sizeof(p.password), "%s", argv[4]);
	snprintf(p.client_name, sizeof(p.client_name), "%s", argc > 5 ? argv[5] : "libx6100");
	p.rx_codec = (uint8_t)(argc > 6 ? strtol(argv[6], NULL, 0) : 0x04);
	p.rx_sample = (uint32_t)(argc > 7 ? strtol(argv[7], NULL, 10) : 48000);
	p.rx_latency = (uint16_t)(argc > 8 ? strtol(argv[8], NULL, 10) : 150);
	p.tx_codec = 0x04;
	p.tx_sample = 48000;
	p.tx_latency = 150;

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_signal;
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);

	if (audio_sdl_open(48000, &p.dev_rate) != 0) {
		fprintf(stderr, "warning: no audio device, continuing without playback\n");
		p.dev_rate = 0;
	} else {
		printf("audio device: %u Hz, rx codec request 0x%02x at %u Hz, latency %u ms\n",
				p.dev_rate, p.rx_codec, p.rx_sample, p.rx_latency);
	}

	if (session_connect(&s, &p, &hooks) != 0) {
		fprintf(stderr, "error: cannot start session\n");
		if (p.dev_rate != 0)
			audio_sdl_close();
		return EXIT_FAILURE;
	}

	if (p.dev_rate != 0)
		audio_sdl_attach(s);

	memset(&last, 0, sizeof(last));
	last.state = SESSION_IDLE;

	while (!G_STOP) {
		session_get_info(s, &now);

		if (now.state != last.state || strcmp(now.status, last.status) != 0) {
			printf("state: %s  control: %s  reason: %s  status: %s\n",
					session_state_name(now.state), control_state_name(now.control_state),
					session_reason_name(now.reason), now.status);
			if (now.state == SESSION_CONNECTED)
				printf("radio: %s  civ: 0x%02x  baud: %u  connection: %s  ports: civ %u audio %u\n",
						now.radio_name, now.civ_addr, now.baudrate, now.connection,
						now.civ_port, now.audio_port);
			fflush(stdout);
		}

		if (now.state == SESSION_CONNECTED) {
			if (tick % 2 == 0)
				session_civ_send(s, C_RIG_ID, sizeof(C_RIG_ID));

			printf("civ %u  audio %u  late %u (%d ms)  codec 0x%02x  buf %d ms  underruns %u  overrun %u  decode-err %u  peak %.2f  rexmit %u  req %u\n",
					now.civ_frames, now.audio_packets, now.audio_dropped_late, now.audio_lateness_ms,
					now.rx_codec, now.audio_latency_ms, now.audio_underruns, now.audio_overrun_frames,
					now.audio_decode_errors, (double)now.audio_peak, now.retransmitted, now.requests_sent);
			fflush(stdout);
			tick++;
		}

		if (now.state == SESSION_FAILED || now.state == SESSION_LOST || now.state == SESSION_CLOSED) {
			rc = EXIT_FAILURE;
			break;
		}

		last = now;
		sleep_ms(1000);
	}

	if (p.dev_rate != 0)
		audio_sdl_close();
	session_disconnect(s);
	printf("disconnected\n");
	return rc;
}

int main(int argc, char *argv[])
{
	SDL_version ver;

	if (argc >= 5)
		return run_test(argc, argv);

	SDL_GetVersion(&ver);
	printf("libx6100_app native, SDL %u.%u.%u\n", ver.major, ver.minor, ver.patch);
	printf("test mode: %s <host> <control-port> <username> <password> [client-name] [rx-codec] [rx-rate] [latency-ms]\n", argv[0]);
	printf("  rx-codec: 0x01 ulaw, 0x02 pcm8, 0x04 pcm16 (default), 0x40 opus, 0x80 adpcm (mono rigs)\n");

	return EXIT_SUCCESS;
}
