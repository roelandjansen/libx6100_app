// SPDX-License-Identifier: GPL-3.0-or-later
//
// Native (SDL) entry point.
//
// Test mode:
//	radio <host> <control-port> <username> <password> [client-name] [rx-codec] [rx-rate] [latency-ms]
// connects, prints state changes and counters once a second, plus
// the decoded radio state (rig file from rigs/ in the current directory,
// picked by the announced model), the scope as a text line and the first
// CI-V frames. RX audio plays on the default
// SDL device. Ctrl-C disconnects cleanly.
//

#include <signal.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <poll.h>
#include <unistd.h>

#include <SDL.h>

#include "core/session.h"
#include "platform/native/audio_sdl.h"

#define C_CIV_PRINT_MAX	5

static volatile sig_atomic_t G_STOP;
static atomic_int G_CIV_PRINTED;

static void on_signal(int sig)
{
	(void)sig;
	G_STOP = 1;
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
// One line of decoded radio state plus a coarse text rendering of the scope row
//

static void print_radio(struct session *s)
{
	static const char C_SHADES[] = " .:-=+*#%@";
	struct radio_state st;
	struct radio_scope_row row;
	char spark[65];
	const struct radio_rx *rx;

	session_get_state(s, &st);
	if (!st.rig_valid) {
		printf("  rig: waiting for Transceiver ID\n");
		return;
	}

	rx = &st.rx[0];
	printf("  %s (0x%02x)  A %.6f MHz %s %s  B %.6f MHz  S %d (%.1f)  agc %d  pre %d  att %d  rfg %d  span %d  smode %d  tx %d  step %u%s  decoded %u  unmatched %u  rows %u\n",
			st.model, st.model_id,
			rx->freq_valid[0] ? (double)rx->freq_hz[0] / 1e6 : 0.0,
			rx->mode[0].valid ? rx->mode[0].name : "-",
			rx->mode[0].valid ? rx->mode[0].filter_name : "-",
			rx->freq_valid[1] ? (double)rx->freq_hz[1] / 1e6 : 0.0,
			st.meter[RADIO_METER_S].valid ? st.meter[RADIO_METER_S].raw : -1,
			st.meter[RADIO_METER_S].valid ? st.meter[RADIO_METER_S].cal : 0.0,
			st.agc.valid ? st.agc.value : -1,
			st.preamp.valid ? st.preamp.value : -1,
			st.attenuator.valid ? st.attenuator.value : -1,
			st.rf_gain.valid ? st.rf_gain.value : -1,
			rx->scope.span_hz.valid ? rx->scope.span_hz.value : -1,
			rx->scope.mode.valid ? rx->scope.mode.value : -1,
			st.transmit.valid ? st.transmit.value : -1,
			session_get_step(s), session_get_freq_lock(s) ? " LOCK" : "",
			st.frames_decoded, st.frames_unmatched, st.scope_rows);

	if (session_get_scope_row(s, 0, &row) && row.len > 0 && st.amp_max > 0) {
		size_t i;

		for (i = 0; i < 64; i++) {
			const size_t a = i * row.len / 64;
			const size_t b = (i + 1) * row.len / 64;
			unsigned int mx = 0;
			size_t k;

			for (k = a; k < b; k++)
				if (row.bins[k] > mx)
					mx = row.bins[k];
			spark[i] = C_SHADES[mx * 9u / (unsigned int)st.amp_max > 9u ? 9u : mx * 9u / (unsigned int)st.amp_max];
		}
		spark[64] = '\0';
		printf("  scope %.3f-%.3f MHz |%s|\n", (double)row.low_hz / 1e6, (double)row.high_hz / 1e6, spark);
	}
}

//
// Interactive controls on stdin, one command per line
//

static void print_help(void)
{
	printf("commands:\n"
		"  f <hz> | fb <hz>        set VFO A / B frequency\n"
		"  u [n] | d [n]           tune VFO A up / down n steps\n"
		"  step <hz>               tuning step\n"
		"  mode <num> [filter]     rig file mode number (X6100: LSB 0 USB 1 AM 2 CW 3 FM 5 CW-R 6), filter 1-3\n"
		"  agc <1-3>  pre <0|1>  att <0|20>  rfg <0-255>\n"
		"  span <hz>               half span, e.g. 24000\n"
		"  smode <0-3>             scope mode (0 center, 1 fixed)\n"
		"  set <value> <name...>   any rig file command by name\n"
		"  get <name...>           read a rig file command once\n"
		"  lock | unlock           frequency lock\n"
		"  help\n");
	fflush(stdout);
}

static void report(const char *what, int rc)
{
	if (rc != 0)
		printf("%s: rejected (%d)\n", what, rc);
	else
		printf("%s: queued\n", what);
	fflush(stdout);
}

static void handle_command(struct session *s, char *line)
{
	char cmd[32] = "";
	char rest[160] = "";
	long a = 0;
	long b = -1;
	int n;

	line[strcspn(line, "\r\n")] = '\0';
	n = sscanf(line, "%31s %159[^\n]", cmd, rest);
	if (n < 1)
		return;

	if (strcmp(cmd, "help") == 0) {
		print_help();
	} else if (strcmp(cmd, "f") == 0 || strcmp(cmd, "fb") == 0) {
		report(cmd, session_set_freq(s, cmd[1] == 'b' ? 1 : 0, strtoull(rest, NULL, 10)));
	} else if (strcmp(cmd, "u") == 0 || strcmp(cmd, "d") == 0) {
		a = n > 1 ? strtol(rest, NULL, 10) : 1;
		report(cmd, session_tune(s, 0, (int)(cmd[0] == 'u' ? a : -a)));
	} else if (strcmp(cmd, "step") == 0) {
		session_set_step(s, (uint32_t)strtoul(rest, NULL, 10));
		printf("step: %u Hz\n", session_get_step(s));
	} else if (strcmp(cmd, "mode") == 0) {
		if (sscanf(rest, "%ld %ld", &a, &b) < 1)
			return;
		if (b < 0) {
			struct radio_state st;

			session_get_state(s, &st);
			b = st.rx[0].mode[0].valid ? st.rx[0].mode[0].filter : 1;
		}
		report(cmd, session_set_mode(s, 0, (int)a, 0, (int)b));
	} else if (strcmp(cmd, "agc") == 0) {
		report(cmd, session_set_value(s, "AGC Status", 0, (int)strtol(rest, NULL, 10)));
	} else if (strcmp(cmd, "pre") == 0) {
		report(cmd, session_set_value(s, "Preamp Status", 0, (int)strtol(rest, NULL, 10)));
	} else if (strcmp(cmd, "att") == 0) {
		report(cmd, session_set_value(s, "Attenuator Status", 0, (int)strtol(rest, NULL, 10)));
	} else if (strcmp(cmd, "rfg") == 0) {
		report(cmd, session_set_value(s, "RF Gain", 0, (int)strtol(rest, NULL, 10)));
	} else if (strcmp(cmd, "span") == 0) {
		report(cmd, session_set_span(s, 0, (uint32_t)strtoul(rest, NULL, 10)));
	} else if (strcmp(cmd, "smode") == 0) {
		report(cmd, session_set_value(s, "Scope Mode", 0, (int)strtol(rest, NULL, 10)));
	} else if (strcmp(cmd, "set") == 0) {
		char *name = NULL;

		a = strtol(rest, &name, 10);
		while (name != NULL && *name == ' ')
			name++;
		report(cmd, session_set_value(s, name != NULL ? name : "", 0, (int)a));
	} else if (strcmp(cmd, "get") == 0) {
		report(cmd, session_get_value(s, rest, 0));
	} else if (strcmp(cmd, "lock") == 0 || strcmp(cmd, "unlock") == 0) {
		session_set_freq_lock(s, cmd[0] == 'l');
		printf("frequency lock: %s\n", cmd[0] == 'l' ? "on" : "off");
	} else {
		printf("unknown command, try help\n");
	}
	fflush(stdout);
}

//
// Wait up to ms for stdin lines, handling each as a command
//

static void wait_commands(struct session *s, int ms)
{
	static bool eof;
	struct pollfd pfd = { STDIN_FILENO, POLLIN, 0 };
	char line[256];

	if (eof) {
		poll(NULL, 0, ms);
		return;
	}

	// First wait up to ms, then take whatever else is already waiting
	while (poll(&pfd, 1, ms) > 0 && (pfd.revents & (POLLIN | POLLHUP))) {
		if (fgets(line, sizeof(line), stdin) == NULL) {
			eof = true;
			poll(NULL, 0, ms);
			return;
		}
		handle_command(s, line);
		ms = 0;
	}
}

//
// Connect test against a wfview server
//

static int run_test(int argc, char *argv[])
{
	const struct session_hooks hooks = { NULL, on_civ, NULL, NULL, NULL, NULL };
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
	p.rx_codec = (uint8_t)(argc > 6 ? strtol(argv[6], NULL, 0) : 0x04);
	p.rx_sample = (uint32_t)(argc > 7 ? strtol(argv[7], NULL, 10) : 48000);
	p.rx_latency = (uint16_t)(argc > 8 ? strtol(argv[8], NULL, 10) : 150);
	p.tx_codec = 0x04;
	p.tx_sample = 48000;
	p.tx_latency = 150;
	snprintf(p.rig_dir, sizeof(p.rig_dir), "%s", "rigs");

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

	print_help();

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

			printf("civ %u  audio %u  late %u (%d ms)  codec 0x%02x  buf %d ms  underruns %u  overrun %u  decode-err %u  peak %.2f  rexmit %u  req %u\n",
					now.civ_frames, now.audio_packets, now.audio_dropped_late, now.audio_lateness_ms,
					now.rx_codec, now.audio_latency_ms, now.audio_underruns, now.audio_overrun_frames,
					now.audio_decode_errors, (double)now.audio_peak, now.retransmitted, now.requests_sent);
			fflush(stdout);
			print_radio(s);
		}

		if (now.state == SESSION_FAILED || now.state == SESSION_LOST || now.state == SESSION_CLOSED) {
			rc = EXIT_FAILURE;
			break;
		}

		last = now;
		wait_commands(s, 1000);
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
