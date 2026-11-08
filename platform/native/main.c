// SPDX-License-Identifier: GPL-3.0-or-later
//
// Native (SDL) entry point.
//
// Phase 1 test mode:
//	radio <host> <control-port> <username> <password> [client-name]
// connects, prints state changes and counters once a second, Ctrl-C to
// disconnect cleanly.
//

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <SDL.h>

#include "core/session.h"

static volatile sig_atomic_t G_STOP;

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
// Connect test against a wfview server
//

static int run_test(int argc, char *argv[])
{
	struct profile p;
	struct session *s = NULL;
	struct session_info last;
	struct session_info now;
	struct sigaction sa;
	long port;
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
	p.rx_codec = 0x04;
	p.rx_sample = 48000;
	p.rx_latency = 150;
	p.tx_codec = 0x04;
	p.tx_sample = 48000;
	p.tx_latency = 150;

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_signal;
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);

	if (session_connect(&s, &p, NULL) != 0) {
		fprintf(stderr, "error: cannot start session\n");
		return EXIT_FAILURE;
	}

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

		if (now.state == SESSION_CONNECTED &&
				(now.civ_frames != last.civ_frames || now.audio_packets != last.audio_packets)) {
			printf("civ frames: %u  audio packets: %u  dropped late: %u  retransmitted: %u  requests: %u\n",
					now.civ_frames, now.audio_packets, now.audio_dropped_late,
					now.retransmitted, now.requests_sent);
			fflush(stdout);
		}

		if (now.state == SESSION_FAILED || now.state == SESSION_LOST || now.state == SESSION_CLOSED) {
			rc = EXIT_FAILURE;
			break;
		}

		last = now;
		sleep_ms(1000);
	}

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
	printf("test mode: %s <host> <control-port> <username> <password> [client-name]\n", argv[0]);

	return EXIT_SUCCESS;
}
