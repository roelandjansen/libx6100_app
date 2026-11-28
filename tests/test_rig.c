// SPDX-License-Identifier: GPL-3.0-or-later
//
// Host test: rig file reader on the real X6100.rig, model detection,
// queue scheduling, CI-V decoding (frequency, mode, meter) and scope rows.
// Run from the repository root (reads rigs/).
//

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/rig.h"
#include "core/rigfile.h"

static int G_FAILS;

#define CHECK(cond) do { \
	if (!(cond)) { \
		fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
		G_FAILS++; \
	} \
} while (0)

#define C_MAX_OUT 512

static struct {
	size_t n;
	size_t len[C_MAX_OUT];
	uint8_t f[C_MAX_OUT][64];
	uint32_t groups;
	char err[256];
} G;

static struct rig G_RIG;
static struct rig_def G_DEF;

static int ev_send(void *ctx, const uint8_t *civ, size_t len)
{
	(void)ctx;
	if (G.n < C_MAX_OUT && len <= 64) {
		memcpy(G.f[G.n], civ, len);
		G.len[G.n] = len;
	}
	G.n++;
	return 0;
}

static void ev_changed(void *ctx, uint32_t groups)
{
	(void)ctx;
	G.groups |= groups;
}

static void ev_error(void *ctx, const char *msg)
{
	(void)ctx;
	snprintf(G.err, sizeof(G.err), "%s", msg);
}

static const struct rig_events C_EV = { ev_send, ev_changed, ev_error, NULL };

static int sent(const uint8_t *frame, size_t len)
{
	size_t i;

	for (i = 0; i < G.n && i < C_MAX_OUT; i++) {
		if (G.len[i] == len && memcmp(G.f[i], frame, len) == 0)
			return 1;
	}
	return 0;
}

static void feed(const uint8_t *f, size_t len)
{
	rig_input(&G_RIG, f, len, 1000);
}

static void test_rigfile(void)
{
	char err[256];
	const struct rig_cmd *c;

	CHECK(rigfile_load(&G_DEF, "rigs/X6100.rig", err, sizeof(err)) == 0);
	CHECK(strcmp(G_DEF.model, "X6100") == 0);
	CHECK(strcmp(G_DEF.version, "2.10") == 0);
	CHECK(G_DEF.civ_address == 97);
	CHECK(G_DEF.spect_seq_max == 11);
	CHECK(G_DEF.spect_amp_max == 160);
	CHECK(G_DEF.spect_len_max == 475);
	CHECK(G_DEF.has_spectrum && G_DEF.has_transmit && G_DEF.has_fd_comms && !G_DEF.has_command29);
	CHECK(G_DEF.n_cmds == 100);
	CHECK(G_DEF.n_periodic == 32);
	CHECK(G_DEF.n_modes == 6);
	CHECK(G_DEF.n_filters == 3);
	CHECK(G_DEF.n_spans == 6);
	CHECK(G_DEF.n_meters == 42);
	CHECK(strcmp(G_DEF.spans[4].name, "\xc2\xb1" "24 kHz") == 0);
	CHECK(G_DEF.spans[4].freq == 24000);

	c = rigfile_cmd(&G_DEF, "S Meter");
	CHECK(c != NULL && c->n == 2 && c->bytes[0] == 0x15 && c->bytes[1] == 0x02 && c->max == 255);
	c = rigfile_cmd(&G_DEF, "Selected Freq");
	CHECK(c != NULL && c->n == 2 && c->bytes[0] == 0x25 && c->bytes[1] == 0x00 && c->get && c->set);
	c = rigfile_cmd(&G_DEF, "Freq (TRX)");
	CHECK(c != NULL && c->n == 1 && c->bytes[0] == 0x00);

	// The dual-receiver Icoms load too, whatever their file version
	CHECK(rigfile_load(&G_DEF, "rigs/IC-785x.rig", err, sizeof(err)) == 0);
	CHECK(G_DEF.civ_address == 0x8e && G_DEF.receivers == 2);
	CHECK(rigfile_load(&G_DEF, "rigs/IC-7610.rig", err, sizeof(err)) == 0);
	CHECK(G_DEF.receivers == 2);
	CHECK(rigfile_load(&G_DEF, "rigs/IC-7760.rig", err, sizeof(err)) == 0);
	CHECK(G_DEF.receivers == 2);

	// Directory scan by CI-V address
	CHECK(rigfile_find(&G_DEF, "rigs", 0x61, err, sizeof(err)) == 0);
	CHECK(strcmp(G_DEF.model, "X6100") == 0);
	CHECK(rigfile_find(&G_DEF, "rigs", 0x55, err, sizeof(err)) != 0);
	CHECK(strcmp(err, "unknown model 0x55: no rig file") == 0);
}

static void tick_n(int n, int64_t *t)
{
	int i;

	for (i = 0; i < n; i++) {
		*t += G_RIG.interval_ms;
		rig_tick(&G_RIG, *t);
	}
}

static void test_detect_and_poll(void)
{
	static const uint8_t C_ID_QUERY[] = { 0xfe, 0xfe, 0x00, 0xe1, 0x19, 0x00, 0xfd };
	static const uint8_t C_ID_REPLY[] = { 0xfe, 0xfe, 0xe1, 0x61, 0x19, 0x00, 0x61, 0xfd };
	static const uint8_t C_SMETER[] = { 0xfe, 0xfe, 0x61, 0xe1, 0x15, 0x02, 0xfd };
	static const uint8_t C_FREQ[] = { 0xfe, 0xfe, 0x61, 0xe1, 0x25, 0x00, 0xfd };
	static const uint8_t C_SCOPE_ON[] = { 0xfe, 0xfe, 0x61, 0xe1, 0x27, 0x10, 0x01, 0xfd };
	static const uint8_t C_SCOPE_OUT[] = { 0xfe, 0xfe, 0x61, 0xe1, 0x27, 0x11, 0x01, 0xfd };
	static const uint8_t C_SCOPE_HOLD[] = { 0xfe, 0xfe, 0x61, 0xe1, 0x27, 0x17, 0x00, 0xfd };
	int64_t t = 0;
	const struct rig_cmd *hold;

	memset(&G, 0, sizeof(G));
	rig_init(&G_RIG, "rigs", 115200, 0, &C_EV, 0);
	CHECK(G_RIG.interval_ms == 250);

	// Counter 1: nothing, 2: Highest (empty), 3: High -> Transceiver ID broadcast
	tick_n(3, &t);
	CHECK(G.n == 1);
	CHECK(sent(C_ID_QUERY, sizeof(C_ID_QUERY)));

	// Our own broadcast echoed (to 00 from E1) is ignored
	feed(C_ID_QUERY, sizeof(C_ID_QUERY));
	CHECK(!G_RIG.have_caps);

	// Reply: rig file loaded, interval from 115200 baud with FD comms = 25 ms
	feed(C_ID_REPLY, sizeof(C_ID_REPLY));
	CHECK(G_RIG.have_caps);
	CHECK(G_RIG.civ_addr == 0x61);
	CHECK(G_RIG.st.rig_valid && G_RIG.st.model_id == 0x61);
	CHECK(strcmp(G_RIG.st.model, "X6100") == 0);
	CHECK(G_RIG.interval_ms == 25);
	CHECK((G.groups & RADIO_CHG_RIG) != 0);

	// Immediate reads of all periodic commands drain first, then the scheduler
	G.n = 0;
	tick_n(200, &t);
	CHECK(sent(C_SMETER, sizeof(C_SMETER)));
	CHECK(sent(C_FREQ, sizeof(C_FREQ)));
	CHECK(sent(C_SCOPE_ON, sizeof(C_SCOPE_ON)));
	CHECK(sent(C_SCOPE_OUT, sizeof(C_SCOPE_OUT)));

	// Scope commands get the receiver byte appended
	hold = rigfile_cmd(&G_RIG.def, "Scope Hold");
	CHECK(hold != NULL && hold->bytes[0] == 0x27 && hold->bytes[1] == 0x17);
	CHECK(sent(C_SCOPE_HOLD, sizeof(C_SCOPE_HOLD)));

	// Highest (S meter) is polled every second tick once the immediates are done
	{
		size_t k;
		int smeter = 0;

		G.n = 0;
		tick_n(100, &t);
		for (k = 0; k < G.n && k < C_MAX_OUT; k++) {
			if (G.len[k] == sizeof(C_SMETER) && memcmp(G.f[k], C_SMETER, sizeof(C_SMETER)) == 0)
				smeter++;
		}
		CHECK(smeter >= 44 && smeter <= 52);
	}

	// TX meters (2026-11-17): the rig reports TX: Power and SWR meter are polled; back
	// on RX they are not any more
	{
		static const uint8_t C_TX_ON[] = { 0xfe, 0xfe, 0xe1, 0x61, 0x1c, 0x00, 0x01, 0xfd };
		static const uint8_t C_TX_OFF[] = { 0xfe, 0xfe, 0xe1, 0x61, 0x1c, 0x00, 0x00, 0xfd };
		static const uint8_t C_PO[] = { 0xfe, 0xfe, 0x61, 0xe1, 0x15, 0x11, 0xfd };
		static const uint8_t C_SWR[] = { 0xfe, 0xfe, 0x61, 0xe1, 0x15, 0x12, 0xfd };

		G.n = 0;
		tick_n(100, &t);
		CHECK(!sent(C_PO, sizeof(C_PO)));
		feed(C_TX_ON, sizeof(C_TX_ON));
		G.n = 0;
		tick_n(100, &t);
		CHECK(sent(C_PO, sizeof(C_PO)));
		CHECK(sent(C_SWR, sizeof(C_SWR)));
		feed(C_TX_OFF, sizeof(C_TX_OFF));
		G.n = 0;
		tick_n(100, &t);
		CHECK(!sent(C_PO, sizeof(C_PO)));
		CHECK(!sent(C_SWR, sizeof(C_SWR)));
	}
}

static void test_decode(void)
{
	// 25 00: selected freq 1.395 MHz (BCD, least significant byte first)
	static const uint8_t C_F[] = { 0xfe, 0xfe, 0xe1, 0x61, 0x25, 0x00, 0x00, 0x50, 0x39, 0x01, 0x00, 0xfd };
	// 25 01: unselected freq 7.074 MHz
	static const uint8_t C_F2[] = { 0xfe, 0xfe, 0xe1, 0x61, 0x25, 0x01, 0x00, 0x40, 0x07, 0x07, 0x00, 0xfd };
	// 00: transceive freq broadcast to 00
	static const uint8_t C_TRX[] = { 0xfe, 0xfe, 0x00, 0x61, 0x00, 0x00, 0x60, 0x39, 0x01, 0x00, 0xfd };
	// 26 00: mode AM (reg 2), data 0, filter 1
	static const uint8_t C_M[] = { 0xfe, 0xfe, 0xe1, 0x61, 0x26, 0x00, 0x02, 0x00, 0x01, 0xfd };
	// 15 02: S meter 0120 BCD
	static const uint8_t C_S[] = { 0xfe, 0xfe, 0xe1, 0x61, 0x15, 0x02, 0x01, 0x20, 0xfd };
	// 16 12: AGC 2
	static const uint8_t C_AGC[] = { 0xfe, 0xfe, 0xe1, 0x61, 0x16, 0x12, 0x02, 0xfd };
	// Unknown command
	static const uint8_t C_UNK[] = { 0xfe, 0xfe, 0xe1, 0x61, 0x7e, 0x7e, 0xfd };
	// Frame split across two CI-V packets
	static const uint8_t C_A[] = { 0xfe, 0xfe, 0xe1, 0x61, 0x15 };
	static const uint8_t C_B[] = { 0x02, 0x02, 0x41, 0xfd };

	G.groups = 0;
	feed(C_F, sizeof(C_F));
	CHECK(G_RIG.st.rx[0].freq_valid[0]);
	CHECK(G_RIG.st.rx[0].freq_hz[0] == 1395000);
	CHECK((G.groups & RADIO_CHG_FREQ) != 0);

	feed(C_F2, sizeof(C_F2));
	CHECK(G_RIG.st.rx[0].freq_hz[1] == 7074000);

	feed(C_TRX, sizeof(C_TRX));
	CHECK(G_RIG.st.rx[0].freq_hz[0] == 1396000);

	feed(C_M, sizeof(C_M));
	CHECK(G_RIG.st.rx[0].mode[0].valid);
	CHECK(strcmp(G_RIG.st.rx[0].mode[0].name, "AM") == 0);
	CHECK(strcmp(G_RIG.st.rx[0].mode[0].filter_name, "FIL1") == 0);

	feed(C_S, sizeof(C_S));
	CHECK(G_RIG.st.meter[RADIO_METER_S].valid);
	CHECK(G_RIG.st.meter[RADIO_METER_S].raw == 120);

	feed(C_AGC, sizeof(C_AGC));
	CHECK(G_RIG.st.agc.valid && G_RIG.st.agc.value == 2);

	feed(C_UNK, sizeof(C_UNK));
	CHECK(G_RIG.st.frames_unmatched == 1);

	feed(C_A, sizeof(C_A));
	feed(C_B, sizeof(C_B));
	CHECK(G_RIG.st.meter[RADIO_METER_S].raw == 241);
}

static void test_meter_cal(void)
{
	size_t i;
	int prev_val = -1;
	double prev_act = 0.0;
	int found = 0;

	// Recompute an interpolated S-meter point from the rig file itself
	for (i = 0; i < G_RIG.def.n_meters; i++) {
		const struct rig_meter *m = &G_RIG.def.meters[i];

		if (strcmp(m->meter, "S-Meter") != 0)
			continue;
		if (prev_val >= 0 && m->rig_val - prev_val >= 2 && !found) {
			const int mid = (prev_val + m->rig_val) / 2;
			const double want = m->actual - ((double)(m->rig_val - mid) * (m->actual - prev_act)) /
				(double)(m->rig_val - prev_val);
			uint8_t f[] = { 0xfe, 0xfe, 0xe1, 0x61, 0x15, 0x02, 0x00, 0x00, 0xfd };

			f[6] = (uint8_t)(mid / 100);
			f[7] = (uint8_t)((((mid / 10) % 10) << 4) | (mid % 10));
			feed(f, sizeof(f));
			CHECK(G_RIG.st.meter[RADIO_METER_S].raw == mid);
			CHECK(G_RIG.st.meter[RADIO_METER_S].cal > want - 1e-9 && G_RIG.st.meter[RADIO_METER_S].cal < want + 1e-9);
			found = 1;
		}
		prev_val = m->rig_val;
		prev_act = m->actual;
	}
	CHECK(found);
}

//
// One Scope Wave Data frame: 27 00 <rx> <payload>
//

static void scope_frame(const uint8_t *payload, size_t n)
{
	uint8_t f[128];

	f[0] = 0xfe;
	f[1] = 0xfe;
	f[2] = 0xe1;
	f[3] = 0x61;
	f[4] = 0x27;
	f[5] = 0x00;
	f[6] = 0x00;
	memcpy(f + 7, payload, n);
	f[7 + n] = 0xfd;
	feed(f, 8 + n);
}

// Recurring queue entries of a command, over all priorities
static int recurring_count(const char *type)
{
	const int cmd = rig_cmd_index(&G_RIG, type);
	int n = 0;

	for (size_t p = 0; p < RIG_PRIO_COUNT; p++)
		for (size_t i = 0; i < G_RIG.q[p].n; i++)
			n += G_RIG.q[p].items[i].recurring && G_RIG.q[p].items[i].cmd == cmd;

	return n;
}

static void test_scope(void)
{
	// seq 01/11, center mode, center 1.395 MHz, half span 24 kHz, in range
	static const uint8_t C_HDR[] = {
		0x01, 0x11, 0x00,
		0x00, 0x50, 0x39, 0x01, 0x00,
		0x00, 0x40, 0x02, 0x00, 0x00,
		0x00
	};
	uint8_t p[64];
	int s;
	int k;
	const struct radio_scope_row *row = &G_RIG.row[0];

	// Scope On/Off recurs until the first row (wfview deletes it then: sent on, it
	// put the rig back on its scope screen several times a second)
	CHECK(recurring_count("Scope On/Off") == 1);

	G.groups = 0;
	scope_frame(C_HDR, sizeof(C_HDR));
	CHECK(G_RIG.st.scope_rows == 0);
	CHECK(recurring_count("Scope On/Off") == 1);

	// Sequences 2..11: 9 x 50 + 25 = 475 bins (IC-705 layout)
	for (s = 2; s <= 11; s++) {
		const int n = s < 11 ? 50 : 25;

		p[0] = (uint8_t)(((s / 10) << 4) | (s % 10));
		p[1] = 0x11;
		for (k = 0; k < n; k++)
			p[2 + k] = (uint8_t)(s * 10 + k % 10);
		scope_frame(p, (size_t)(2 + n));
	}

	CHECK(G_RIG.st.scope_rows == 1);
	CHECK(recurring_count("Scope On/Off") == 0);	/* deleted with the first row */
	CHECK((G.groups & RADIO_CHG_SCOPE) != 0);
	CHECK(row->valid && !row->oor);
	CHECK(row->len == 475);
	CHECK(row->low_hz == 1371000);
	CHECK(row->high_hz == 1419000);
	CHECK(row->bins[0] == 20 && row->bins[474] == 110 + 4);

	// Out of order: seq 3 without seq 2 drops the row
	scope_frame(C_HDR, sizeof(C_HDR));
	p[0] = 0x03;
	p[1] = 0x11;
	scope_frame(p, 52);
	for (s = 4; s <= 11; s++) {
		p[0] = (uint8_t)(((s / 10) << 4) | (s % 10));
		scope_frame(p, s < 11 ? 52 : 27);
	}
	CHECK(G_RIG.st.scope_rows == 1);
}

static void test_unknown_model(void)
{
	static const uint8_t C_ID_REPLY[] = { 0xfe, 0xfe, 0xe1, 0x55, 0x19, 0x00, 0x55, 0xfd };

	memset(&G, 0, sizeof(G));
	rig_init(&G_RIG, "rigs", 115200, 0, &C_EV, 0);
	feed(C_ID_REPLY, sizeof(C_ID_REPLY));
	CHECK(G_RIG.failed);
	CHECK(strcmp(G.err, "unknown model 0x55: no rig file") == 0);
}

//
// Addition 8: the selected receiver polled on a two-receiver rig (IC-7610), not on a
// one-receiver rig (X6100); a change of it re-reads both receivers' frequency
//

static size_t count_sent(const uint8_t *frame, size_t len)
{
	size_t i;
	size_t n = 0;

	for (i = 0; i < G.n && i < C_MAX_OUT; i++) {
		if (G.len[i] == len && memcmp(G.f[i], frame, len) == 0)
			n++;
	}
	return n;
}

static void test_selected_receiver(void)
{
	static const uint8_t C_ID_7610[] = { 0xfe, 0xfe, 0xe1, 0x98, 0x19, 0x00, 0x98, 0xfd };
	static const uint8_t C_ASK_7610[] = { 0xfe, 0xfe, 0x98, 0xe1, 0x07, 0xd2, 0xfd };
	static const uint8_t C_SUB_7610[] = { 0xfe, 0xfe, 0xe1, 0x98, 0x07, 0xd2, 0x01, 0xfd };
	static const uint8_t C_RXF0_7610[] = { 0xfe, 0xfe, 0x98, 0xe1, 0x25, 0x00, 0xfd };
	static const uint8_t C_RXF1_7610[] = { 0xfe, 0xfe, 0x98, 0xe1, 0x25, 0x01, 0xfd };
	static const uint8_t C_ID_X6100[] = { 0xfe, 0xfe, 0xe1, 0x61, 0x19, 0x00, 0x61, 0xfd };
	static const uint8_t C_ASK_X6100[] = { 0xfe, 0xfe, 0x61, 0xe1, 0x07, 0xd2, 0xfd };
	int64_t t = 0;

	//
	// IC-7610: asked again and again, as a recurring poll
	//
	memset(&G, 0, sizeof(G));
	rig_init(&G_RIG, "rigs", 115200, 0, &C_EV, 0);
	tick_n(3, &t);
	feed(C_ID_7610, sizeof(C_ID_7610));
	CHECK(G_RIG.have_caps && G_RIG.st.receivers == 2);
	tick_n(300, &t);
	G.n = 0;
	tick_n(400, &t);
	CHECK(count_sent(C_ASK_7610, sizeof(C_ASK_7610)) >= 3);

	//
	// SUB pressed on the rig: rx_selected follows, both receivers are read again
	//
	CHECK(!G_RIG.st.rx_selected.valid || G_RIG.st.rx_selected.value == 0);
	G.n = 0;
	feed(C_SUB_7610, sizeof(C_SUB_7610));
	CHECK(G_RIG.st.rx_selected.valid && G_RIG.st.rx_selected.value == 1);
	tick_n(20, &t);
	CHECK(sent(C_RXF0_7610, sizeof(C_RXF0_7610)));
	CHECK(sent(C_RXF1_7610, sizeof(C_RXF1_7610)));

	//
	// X6100 (one receiver): never asked
	//
	memset(&G, 0, sizeof(G));
	t = 0;
	rig_init(&G_RIG, "rigs", 115200, 0, &C_EV, 0);
	tick_n(3, &t);
	feed(C_ID_X6100, sizeof(C_ID_X6100));
	CHECK(G_RIG.have_caps && G_RIG.st.receivers == 1);
	G.n = 0;
	tick_n(700, &t);
	CHECK(count_sent(C_ASK_X6100, sizeof(C_ASK_X6100)) == 0);
}

//
// Addition 9: one receiver set and tuned directly with RX Frequency and its
// receiver byte (IC-7610); a one-receiver rig has no receiver 1
//

static void drain(int64_t *t);

static void test_rx_freq(void)
{
	static const uint8_t C_ID_7610[] = { 0xfe, 0xfe, 0xe1, 0x98, 0x19, 0x00, 0x98, 0xfd };
	static const uint8_t C_SUB_7076[] = { 0xfe, 0xfe, 0x98, 0xe1, 0x25, 0x01, 0x00, 0x60, 0x07, 0x07, 0x00, 0xfd };
	static const uint8_t C_SUB_ASK[] = { 0xfe, 0xfe, 0x98, 0xe1, 0x25, 0x01, 0xfd };
	static const uint8_t C_SUB_7074[] = { 0xfe, 0xfe, 0x98, 0xe1, 0x25, 0x01, 0x00, 0x40, 0x07, 0x07, 0x00, 0xfd };
	static const uint8_t C_MAIN_14074[] = { 0xfe, 0xfe, 0x98, 0xe1, 0x25, 0x00, 0x00, 0x40, 0x07, 0x14, 0x00, 0xfd };
	static const uint8_t C_ID_X6100[] = { 0xfe, 0xfe, 0xe1, 0x61, 0x19, 0x00, 0x61, 0xfd };
	int64_t t = 0;

	memset(&G, 0, sizeof(G));
	rig_init(&G_RIG, "rigs", 115200, 0, &C_EV, 0);
	tick_n(3, &t);
	feed(C_ID_7610, sizeof(C_ID_7610));
	CHECK(G_RIG.have_caps);
	drain(&t);

	//
	// The unselected receiver set: 25 01 + frequency; the model shows it at once and
	// the set is read back
	//
	G.n = 0;
	CHECK(rig_set_rx_freq(&G_RIG, 1, 7076000) == 0);
	CHECK(G_RIG.st.rx[1].freq_valid[0] && G_RIG.st.rx[1].freq_hz[0] == 7076000);
	drain(&t);
	CHECK(sent(C_SUB_7076, sizeof(C_SUB_7076)));
	CHECK(sent(C_SUB_ASK, sizeof(C_SUB_ASK)));

	//
	// Tuned two 1 kHz steps down from there; the selected receiver set too
	//
	G.n = 0;
	CHECK(rig_tune_rx(&G_RIG, 1, -2, 1000) == 0);
	drain(&t);
	CHECK(sent(C_SUB_7074, sizeof(C_SUB_7074)));
	G.n = 0;
	CHECK(rig_set_rx_freq(&G_RIG, 0, 14074000) == 0);
	drain(&t);
	CHECK(sent(C_MAIN_14074, sizeof(C_MAIN_14074)));
	CHECK(rig_set_rx_freq(&G_RIG, 2, 14074000) == RIG_ERR_RANGE);

	//
	// One receiver: no receiver 1
	//
	memset(&G, 0, sizeof(G));
	t = 0;
	rig_init(&G_RIG, "rigs", 115200, 0, &C_EV, 0);
	tick_n(3, &t);
	feed(C_ID_X6100, sizeof(C_ID_X6100));
	CHECK(G_RIG.have_caps && G_RIG.st.receivers == 1);
	CHECK(rig_set_rx_freq(&G_RIG, 1, 7076000) == RIG_ERR_RANGE);
	CHECK(rig_set_rx_freq(&G_RIG, 0, 7076000) == 0);
}

//
// Addition 10: scope controls an RS-BA1 style scope window needs (IC-7610): REF
// level in tenths of a dB both ways, VBW per receiver, single / dual scope
//

static void test_scope_controls(void)
{
	static const uint8_t C_ID_7610[] = { 0xfe, 0xfe, 0xe1, 0x98, 0x19, 0x00, 0x98, 0xfd };
	static const uint8_t C_REF_SUB[] = { 0xfe, 0xfe, 0xe1, 0x98, 0x27, 0x19, 0x01, 0x12, 0x50, 0x01, 0xfd };
	static const uint8_t C_VBW_MAIN[] = { 0xfe, 0xfe, 0xe1, 0x98, 0x27, 0x1d, 0x00, 0x01, 0xfd };
	static const uint8_t C_DUAL[] = { 0xfe, 0xfe, 0xe1, 0x98, 0x27, 0x13, 0x01, 0xfd };
	static const uint8_t C_OUTPUT[] = { 0xfe, 0xfe, 0xe1, 0x98, 0x27, 0x11, 0x01, 0xfd };
	static const uint8_t C_SET_REF[] = { 0xfe, 0xfe, 0x98, 0xe1, 0x27, 0x19, 0x00, 0x12, 0x50, 0x01, 0xfd };
	static const uint8_t C_SET_REF_POS[] = { 0xfe, 0xfe, 0x98, 0xe1, 0x27, 0x19, 0x01, 0x00, 0x50, 0x00, 0xfd };
	static const uint8_t C_SET_VBW[] = { 0xfe, 0xfe, 0x98, 0xe1, 0x27, 0x1d, 0x01, 0x01, 0xfd };
	static const uint8_t C_SET_DUAL[] = { 0xfe, 0xfe, 0x98, 0xe1, 0x27, 0x13, 0x01, 0xfd };
	static const uint8_t C_GET_REF_MAIN[] = { 0xfe, 0xfe, 0x98, 0xe1, 0x27, 0x19, 0x00, 0xfd };
	static const uint8_t C_GET_REF_SUB[] = { 0xfe, 0xfe, 0x98, 0xe1, 0x27, 0x19, 0x01, 0xfd };
	static const uint8_t C_GET_VBW_MAIN[] = { 0xfe, 0xfe, 0x98, 0xe1, 0x27, 0x1d, 0x00, 0xfd };
	static const uint8_t C_GET_VBW_SUB[] = { 0xfe, 0xfe, 0x98, 0xe1, 0x27, 0x1d, 0x01, 0xfd };
	int64_t t = 0;
	int ticks;

	memset(&G, 0, sizeof(G));
	rig_init(&G_RIG, "rigs", 115200, 0, &C_EV, 0);
	tick_n(3, &t);
	feed(C_ID_7610, sizeof(C_ID_7610));
	CHECK(G_RIG.have_caps);
	drain(&t);

	//
	// REF and VBW of both receivers read at start-up (addition 11): queued at
	// Immediate after the first reads of the polled commands: some 2.5 s in on an
	// IC-7610 at 25 ms a tick (within 200 ticks here, with the drain above), not
	// after the Medium High rotation (some 23 ticks a step)
	//
	for (ticks = 0; ticks < 300 && !sent(C_GET_VBW_SUB, sizeof(C_GET_VBW_SUB)); ticks++)
		tick_n(1, &t);
	CHECK(sent(C_GET_REF_MAIN, sizeof(C_GET_REF_MAIN)));
	CHECK(sent(C_GET_REF_SUB, sizeof(C_GET_REF_SUB)));
	CHECK(sent(C_GET_VBW_MAIN, sizeof(C_GET_VBW_MAIN)));
	CHECK(sent(C_GET_VBW_SUB, sizeof(C_GET_VBW_SUB)));
	CHECK(ticks < 140);

	//
	// Decoding
	//
	feed(C_REF_SUB, sizeof(C_REF_SUB));
	CHECK(G_RIG.st.rx[1].scope.ref.valid && G_RIG.st.rx[1].scope.ref.value == -125);
	feed(C_VBW_MAIN, sizeof(C_VBW_MAIN));
	CHECK(G_RIG.st.rx[0].scope.vbw.valid && G_RIG.st.rx[0].scope.vbw.value == 1);
	CHECK(!G_RIG.st.rx[1].scope.vbw.valid);
	feed(C_DUAL, sizeof(C_DUAL));
	CHECK(G_RIG.st.scope_dual.valid && G_RIG.st.scope_dual.value == 1);
	feed(C_OUTPUT, sizeof(C_OUTPUT));
	CHECK(G_RIG.st.scope_output.valid && G_RIG.st.scope_output.value == 1);

	//
	// REF level: -12.5 dB on MAIN, +0.5 dB on SUB; off the 0.5 dB grid or past 30 dB refused
	//
	G.n = 0;
	CHECK(rig_set_value(&G_RIG, "Scope Ref", 0, -125) == 0);
	CHECK(rig_set_value(&G_RIG, "Scope Ref", 1, 5) == 0);
	CHECK(rig_set_value(&G_RIG, "Scope Ref", 0, 3) == RIG_ERR_RANGE);
	CHECK(rig_set_value(&G_RIG, "Scope Ref", 0, -305) == RIG_ERR_RANGE);
	drain(&t);
	CHECK(sent(C_SET_REF, sizeof(C_SET_REF)));
	CHECK(sent(C_SET_REF_POS, sizeof(C_SET_REF_POS)));

	//
	// VBW on SUB: sent with the receiver byte, shown at once
	//
	G.n = 0;
	CHECK(rig_set_value(&G_RIG, "Scope VBW", 1, 1) == 0);
	CHECK(G_RIG.st.rx[1].scope.vbw.valid && G_RIG.st.rx[1].scope.vbw.value == 1);
	drain(&t);
	CHECK(sent(C_SET_VBW, sizeof(C_SET_VBW)));

	//
	// Both receivers' scopes
	//
	G.n = 0;
	CHECK(rig_set_value(&G_RIG, "Scope Single/Dual", 0, 1) == 0);
	drain(&t);
	CHECK(sent(C_SET_DUAL, sizeof(C_SET_DUAL)));
}


//
// Phase 4: run the scheduler until the queue is idle, collecting frames
//

static void drain(int64_t *t)
{
	int i;

	for (i = 0; i < 60; i++) {
		*t += G_RIG.interval_ms;
		rig_tick(&G_RIG, *t);
	}
}

static int sent_bytes(const uint8_t *b, size_t n)
{
	return sent(b, n);
}

//
// Addition 12: sets at Immediate go ahead of the start-up reads and keep their
// order; the scope is switched on and its data asked for first
//

static int sent_at(const uint8_t *frame, size_t len)
{
	size_t i;

	for (i = 0; i < G.n && i < C_MAX_OUT; i++) {
		if (G.len[i] == len && memcmp(G.f[i], frame, len) == 0)
			return (int)i;
	}
	return -1;
}

static void test_set_order(void)
{
	static const uint8_t C_ID_7610[] = { 0xfe, 0xfe, 0xe1, 0x98, 0x19, 0x00, 0x98, 0xfd };
	static const uint8_t C_SCOPE_ON[] = { 0xfe, 0xfe, 0x98, 0xe1, 0x27, 0x10, 0x01, 0xfd };
	static const uint8_t C_SCOPE_OUT[] = { 0xfe, 0xfe, 0x98, 0xe1, 0x27, 0x11, 0x01, 0xfd };
	static const uint8_t C_HOLD_ON[] = { 0xfe, 0xfe, 0x98, 0xe1, 0x27, 0x17, 0x00, 0x01, 0xfd };
	static const uint8_t C_SLOW[] = { 0xfe, 0xfe, 0x98, 0xe1, 0x27, 0x1a, 0x00, 0x02, 0xfd };
	int64_t t = 0;
	int ticks;

	memset(&G, 0, sizeof(G));
	rig_init(&G_RIG, "rigs", 115200, 0, &C_EV, 0);
	tick_n(3, &t);
	feed(C_ID_7610, sizeof(C_ID_7610));
	CHECK(G_RIG.have_caps);

	// The first frames once the rig is known: VFO Main/Sub Band (addition 8, its first
	// read queued last), then the scope switched on
	G.n = 0;
	for (ticks = 0; ticks < 100 && G.n < 3; ticks++)
		tick_n(1, &t);
	CHECK(sent_at(C_SCOPE_ON, sizeof(C_SCOPE_ON)) == 1);
	CHECK(sent_at(C_SCOPE_OUT, sizeof(C_SCOPE_OUT)) == 2);

	// Two sets while the start-up reads wait: both out at once, in order
	G.n = 0;
	CHECK(rig_set_value(&G_RIG, "Scope Hold", 0, 1) == 0);
	CHECK(rig_set_value(&G_RIG, "Scope Speed", 0, 2) == 0);
	for (ticks = 0; ticks < 100 && G.n < 3; ticks++)
		tick_n(1, &t);
	CHECK(sent_at(C_HOLD_ON, sizeof(C_HOLD_ON)) == 0);
	CHECK(sent_at(C_SLOW, sizeof(C_SLOW)) == 1);
}

static void test_controls(void)
{
	static const uint8_t C_F7076[] = { 0xfe, 0xfe, 0x61, 0xe1, 0x25, 0x00, 0x00, 0x60, 0x07, 0x07, 0x00, 0xfd };
	static const uint8_t C_F7074[] = { 0xfe, 0xfe, 0x61, 0xe1, 0x25, 0x00, 0x00, 0x40, 0x07, 0x07, 0x00, 0xfd };
	static const uint8_t C_FGET[] = { 0xfe, 0xfe, 0x61, 0xe1, 0x25, 0x00, 0xfd };
	static const uint8_t C_USB[] = { 0xfe, 0xfe, 0x61, 0xe1, 0x26, 0x00, 0x01, 0x00, 0x02, 0xfd };
	static const uint8_t C_AGC2[] = { 0xfe, 0xfe, 0x61, 0xe1, 0x16, 0x12, 0x02, 0xfd };
	static const uint8_t C_RFG[] = { 0xfe, 0xfe, 0x61, 0xe1, 0x14, 0x02, 0x01, 0x28, 0xfd };
	static const uint8_t C_SPAN[] = { 0xfe, 0xfe, 0x61, 0xe1, 0x27, 0x15, 0x00, 0x00, 0x40, 0x02, 0x00, 0x00, 0xfd };
	static const uint8_t C_SMODE[] = { 0xfe, 0xfe, 0x61, 0xe1, 0x27, 0x14, 0x00, 0x01, 0xfd };
	static const uint8_t C_PRE[] = { 0xfe, 0xfe, 0x61, 0xe1, 0x16, 0x02, 0x01, 0xfd };
	static const uint8_t C_AGC_GET[] = { 0xfe, 0xfe, 0x61, 0xe1, 0x16, 0x12, 0xfd };
	static const uint8_t C_SPAN_GET[] = { 0xfe, 0xfe, 0x61, 0xe1, 0x27, 0x15, 0x00, 0xfd };
	static const uint8_t C_PTT_ON[] = { 0xfe, 0xfe, 0x61, 0xe1, 0x1c, 0x00, 0x01, 0xfd };
	static const uint8_t C_PTT_OFF[] = { 0xfe, 0xfe, 0x61, 0xe1, 0x1c, 0x00, 0x00, 0xfd };
	static const uint8_t C_PTT_GET[] = { 0xfe, 0xfe, 0x61, 0xe1, 0x1c, 0x00, 0xfd };
	static const uint8_t C_TUNE[] = { 0xfe, 0xfe, 0x61, 0xe1, 0x1c, 0x01, 0x02, 0xfd };
	static const uint8_t C_ID_REPLY[] = { 0xfe, 0xfe, 0xe1, 0x61, 0x19, 0x00, 0x61, 0xfd };
	int64_t t = 0;

	memset(&G, 0, sizeof(G));
	rig_init(&G_RIG, "rigs", 115200, 0, &C_EV, 0);
	feed(C_ID_REPLY, sizeof(C_ID_REPLY));
	CHECK(G_RIG.have_caps);

	// Start-up reads for AGC and scope span (additions 5 and 6)
	drain(&t);
	CHECK(sent_bytes(C_AGC_GET, sizeof(C_AGC_GET)));
	CHECK(sent_bytes(C_SPAN_GET, sizeof(C_SPAN_GET)));

	// Three quick sets before a tick: only the last is sent (addUnique); model updated at once
	G.n = 0;
	CHECK(rig_set_freq(&G_RIG, 0, 7074000) == 0);
	CHECK(rig_set_freq(&G_RIG, 0, 7075000) == 0);
	CHECK(rig_set_freq(&G_RIG, 0, 7076000) == 0);
	CHECK(G_RIG.st.rx[0].freq_hz[0] == 7076000);
	drain(&t);
	CHECK(sent_bytes(C_F7076, sizeof(C_F7076)));
	CHECK(!sent_bytes(C_F7074, sizeof(C_F7074)));
	CHECK(sent_bytes(C_FGET, sizeof(C_FGET)));

	// Tuning with floor zeros (wfview niceTS default)
	CHECK(rig_round_frequency(7076000, 3, 1000) == 7079000);
	CHECK(rig_round_frequency(7076500, -1, 1000) == 7076000);
	CHECK(rig_round_frequency(7076500, -2, 1000) == 7075000);
	CHECK(rig_round_frequency(7076500, 1, 1000) == 7077000);
	CHECK(rig_round_frequency(500, -3, 1000) == 0);
	CHECK(rig_tune(&G_RIG, 0, -2, 1000) == 0);
	CHECK(G_RIG.st.rx[0].freq_hz[0] == 7074000);

	// Mode USB (num 1 -> reg 1), data 0, filter 2
	G.n = 0;
	CHECK(rig_set_mode(&G_RIG, 0, 1, 0, 2) == 0);
	CHECK(rig_set_mode(&G_RIG, 0, 42, 0, 2) == RIG_ERR_RANGE);
	drain(&t);
	CHECK(sent_bytes(C_USB, sizeof(C_USB)));

	// AGC (bcd uchar, range 1..3), RF gain (bcdEncodeInt), preamp
	G.n = 0;
	CHECK(rig_set_value(&G_RIG, "AGC Status", 0, 2) == 0);
	CHECK(rig_set_value(&G_RIG, "AGC Status", 0, 5) == RIG_ERR_RANGE);
	CHECK(rig_set_value(&G_RIG, "RF Gain", 0, 128) == 0);
	CHECK(rig_set_value(&G_RIG, "Preamp Status", 0, 1) == 0);
	CHECK(rig_set_value(&G_RIG, "No Such Command", 0, 1) == RIG_ERR_UNSUPPORTED);
	CHECK(rig_set_value(&G_RIG, "Attenuator Status", 0, 1) == RIG_ERR_RANGE);
	CHECK(rig_set_value(&G_RIG, "Attenuator Status", 0, 20) == 0);
	CHECK(rig_set_value(&G_RIG, "Attenuator Status", 0, 0) == 0);
	CHECK(rig_set_value(&G_RIG, "Preamp Status", 0, 2) == RIG_ERR_RANGE);
	drain(&t);
	CHECK(sent_bytes(C_AGC2, sizeof(C_AGC2)));
	CHECK(sent_bytes(C_RFG, sizeof(C_RFG)));
	CHECK(sent_bytes(C_PRE, sizeof(C_PRE)));
	CHECK(sent_bytes(C_AGC_GET, sizeof(C_AGC_GET)));

	// Scope: span from the rig file list only, mode fixed
	G.n = 0;
	CHECK(rig_set_span(&G_RIG, 0, 24000) == 0);
	CHECK(rig_set_span(&G_RIG, 0, 7000) == RIG_ERR_RANGE);
	CHECK(rig_set_value(&G_RIG, "Scope Mode", 0, 1) == 0);
	drain(&t);
	CHECK(sent_bytes(C_SPAN, sizeof(C_SPAN)));
	CHECK(sent_bytes(C_SMODE, sizeof(C_SMODE)));
	CHECK(sent_bytes(C_SPAN_GET, sizeof(C_SPAN_GET)));

	// Filter width (2026-11-18): Hz to the rig's index for the mode: 2400 Hz in USB is 28,
	// 300 Hz is 5; in AM 6000 Hz is 29
	{
		static const uint8_t C_FW2400[] = { 0xfe, 0xfe, 0x61, 0xe1, 0x1a, 0x03, 0x28, 0xfd };
		static const uint8_t C_FW300[] = { 0xfe, 0xfe, 0x61, 0xe1, 0x1a, 0x03, 0x05, 0xfd };
		static const uint8_t C_FW6000[] = { 0xfe, 0xfe, 0x61, 0xe1, 0x1a, 0x03, 0x29, 0xfd };

		G.n = 0;
		G_RIG.st.rx[0].mode[0].valid = true;
		snprintf(G_RIG.st.rx[0].mode[0].name, sizeof(G_RIG.st.rx[0].mode[0].name), "%s", "USB");
		CHECK(rig_set_value(&G_RIG, "Filter Width", 0, 2400) == 0);
		drain(&t);	/* a second set of the same command would replace the first */
		CHECK(rig_set_value(&G_RIG, "Filter Width", 0, 300) == 0);
		drain(&t);
		CHECK(sent_bytes(C_FW2400, sizeof(C_FW2400)));
		CHECK(sent_bytes(C_FW300, sizeof(C_FW300)));
		G.n = 0;
		snprintf(G_RIG.st.rx[0].mode[0].name, sizeof(G_RIG.st.rx[0].mode[0].name), "%s", "AM");
		CHECK(rig_set_value(&G_RIG, "Filter Width", 0, 6000) == 0);
		drain(&t);
		CHECK(sent_bytes(C_FW6000, sizeof(C_FW6000)));
	}

	// PTT (1C 00, Immediate, not unique) and ATU tune (1C 01 02)
	G.n = 0;
	CHECK(rig_has_ptt(&G_RIG));
	CHECK(rig_ptt(&G_RIG, true) == 0);
	CHECK(rig_set_value(&G_RIG, "Tuner/ATU Status", 0, 2) == 0);
	CHECK(rig_set_value(&G_RIG, "Tuner/ATU Status", 0, 3) == RIG_ERR_RANGE);
	drain(&t);
	CHECK(sent_bytes(C_PTT_ON, sizeof(C_PTT_ON)));
	CHECK(sent_bytes(C_TUNE, sizeof(C_TUNE)));
	CHECK(sent_bytes(C_PTT_GET, sizeof(C_PTT_GET)));

	// PTT off straight out, without a tick
	G.n = 0;
	CHECK(rig_ptt_off_now(&G_RIG) == 0);
	CHECK(G.n == 1 && sent_bytes(C_PTT_OFF, sizeof(C_PTT_OFF)));
}

int main(void)
{
	test_rigfile();
	test_detect_and_poll();
	test_decode();
	test_meter_cal();
	test_scope();
	test_controls();
	test_unknown_model();
	test_selected_receiver();
	test_rx_freq();
	test_scope_controls();
	test_set_order();

	if (G_FAILS != 0) {
		fprintf(stderr, "test_rig: %d failure(s)\n", G_FAILS);
		return EXIT_FAILURE;
	}

	return EXIT_SUCCESS;
}
