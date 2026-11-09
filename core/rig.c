// SPDX-License-Identifier: GPL-3.0-or-later
//
// Rig layer: model detection, rig file, command queue, CI-V parsing.
//
// Ports of wfview v3-initial src/cachingqueue.cpp, src/radio/icomcommander.cpp,
// src/rigcommander.cpp and src/MainController.cpp,
// wfview is copyright 2017-2026 Elliott H. Liggett (W6EL) and Phil Taylor (M0VSE), GPLv3.
//
// Queue semantics (QMultiMap in wfview): per priority, a plain insert goes
// before existing items of the same priority, an insert with the cend()
// hint goes after them, and the scheduler takes the last one. Here each
// priority is an array: plain insert at the front, hinted insert at the
// back, take from the back.
//

#include "core/rig.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

//
// Decode kinds for rig file command names (wfview funcString names)
//

enum kind {
	K_NONE,
	K_FREQ_RX,		// RX Frequency: receiver byte, BCD
	K_FREQ,			// selected / get / TX freq: BCD, VFO 0
	K_FREQ_UNSEL,		// BCD, VFO 1
	K_MODE_RX,		// RX Mode: receiver byte, mode
	K_MODE,
	K_MODE_UNSEL,
	K_DATAMODE_FILTER,
	K_BCD1,
	K_BCD2,
	K_KEY_SPEED,
	K_CW_PITCH,
	K_METER,
	K_BOOL,
	K_RAW1,
	K_TRANSCEIVER_ID,
	K_FILTER_WIDTH,
	K_SCOPE_WAVE,
	K_SCOPE_MODE,
	K_SCOPE_SPAN,
	K_SCOPE_EDGE,
	K_SCOPE_HOLD,
	K_SCOPE_REF,
	K_SCOPE_SPEED,
	K_SCOPE_RX_OTHER,	// receiver byte, value ignored (VBW, RBW, center type)
	K_IGNORE
};

struct kind_entry {
	const char *name;
	enum kind kind;
	size_t field;		// offsetof radio_state radio_value, or meter index for K_METER
};

#define F(m) offsetof(struct radio_state, m)

static const struct kind_entry C_KINDS[] = {
	{ "RX Frequency", K_FREQ_RX, 0 },
	{ "Freq (TRX)", K_FREQ, 0 },
	{ "Freq Get", K_FREQ, 0 },
	{ "Selected Freq", K_FREQ, 0 },
	{ "TX Freq", K_FREQ, 0 },
	{ "Unselected Freq", K_FREQ_UNSEL, 0 },
	{ "RX Mode", K_MODE_RX, 0 },
	{ "Mode (TRX)", K_MODE, 0 },
	{ "Mode Get", K_MODE, 0 },
	{ "Selected Mode", K_MODE, 0 },
	{ "Unselected Mode", K_MODE_UNSEL, 0 },
	{ "Data Mode Filter", K_DATAMODE_FILTER, 0 },

	{ "Tuning Step", K_BCD1, F(tuning_step) },
	{ "Attenuator Status", K_BCD1, F(attenuator) },
	{ "AGC Status", K_BCD1, F(agc) },
	{ "Preamp Status", K_BCD1, F(preamp) },
	{ "Break-In Status", K_BCD1, F(break_in) },
	{ "Tuner/ATU Status", K_BCD1, F(tuner) },
	{ "Split/Duplex", K_RAW1, F(split) },

	{ "AF Gain", K_BCD2, F(af_gain) },
	{ "RF Gain", K_BCD2, F(rf_gain) },
	{ "Squelch", K_BCD2, F(squelch) },
	{ "NR Level", K_BCD2, F(nr_level) },
	{ "PBT Inner", K_BCD2, F(pbt_inner) },
	{ "PBT Outer", K_BCD2, F(pbt_outer) },
	{ "RF Power", K_BCD2, F(rf_power) },
	{ "Mic Gain", K_BCD2, F(mic_gain) },
	{ "NB Level", K_BCD2, F(nb_level) },
	{ "Monitor Gain", K_BCD2, F(monitor_gain) },
	{ "Key Speed", K_KEY_SPEED, F(key_speed) },
	{ "CW Pitch", K_CW_PITCH, F(cw_pitch) },

	{ "S Meter", K_METER, RADIO_METER_S },
	{ "Center Meter", K_METER, RADIO_METER_CENTER },
	{ "Power Meter", K_METER, RADIO_METER_POWER },
	{ "SWR Meter", K_METER, RADIO_METER_SWR },
	{ "ALC Meter", K_METER, RADIO_METER_ALC },
	{ "Comp Meter", K_METER, RADIO_METER_COMP },
	{ "Vd Meter", K_METER, RADIO_METER_VD },
	{ "Id Meter", K_METER, RADIO_METER_ID },

	{ "Noise Blanker", K_BOOL, F(noise_blanker) },
	{ "Noise Reduction", K_BOOL, F(noise_reduction) },
	{ "Auto Notch", K_BOOL, F(auto_notch) },
	{ "Compressor Status", K_BOOL, F(compressor) },
	{ "Monitor Status", K_BOOL, F(monitor) },
	{ "Vox Status", K_BOOL, F(vox) },
	{ "Dial Lock Status", K_BOOL, F(dial_lock) },
	{ "Overflow Status", K_BOOL, F(overflow) },
	{ "Transceiver Status", K_BOOL, F(transmit) },

	{ "Transceiver ID", K_TRANSCEIVER_ID, 0 },
	{ "Filter Width", K_FILTER_WIDTH, F(filter_width) },

	{ "Scope Wave Data", K_SCOPE_WAVE, 0 },
	{ "Scope Mode", K_SCOPE_MODE, 0 },
	{ "Scope Span", K_SCOPE_SPAN, 0 },
	{ "Scope Edge", K_SCOPE_EDGE, 0 },
	{ "Scope Hold", K_SCOPE_HOLD, 0 },
	{ "Scope Ref", K_SCOPE_REF, 0 },
	{ "Scope Speed", K_SCOPE_SPEED, 0 },
	{ "Scope VBW", K_SCOPE_RX_OTHER, 0 },
	{ "Scope RBW", K_SCOPE_RX_OTHER, 0 },
	{ "Scope Center Type", K_SCOPE_RX_OTHER, 0 },
};

static const char *const C_METER_NAMES[RADIO_METER_COUNT] = {
	"S-Meter", "Center", "Power", "SWR", "ALC", "Comp", "Voltage", "Current"
};

static const struct {
	const char *name;
	enum rig_prio prio;
} C_PRIO_NAMES[] = {
	{ "None", RIG_PRIO_NONE }, { "Immediate", RIG_PRIO_IMMEDIATE }, { "Highest", RIG_PRIO_HIGHEST },
	{ "High", RIG_PRIO_HIGH }, { "Medium High", RIG_PRIO_MEDIUM_HIGH }, { "Medium", RIG_PRIO_MEDIUM },
	{ "Medium Low", RIG_PRIO_MEDIUM_LOW }, { "Low", RIG_PRIO_LOW }, { "Lowest", RIG_PRIO_LOWEST },
};

static const struct kind_entry *kind_of(const char *name)
{
	size_t i;

	for (i = 0; i < sizeof(C_KINDS) / sizeof(C_KINDS[0]); i++) {
		if (strcmp(C_KINDS[i].name, name) == 0)
			return &C_KINDS[i];
	}

	return NULL;
}

//
// BCD helpers (icomcommander.cpp)
//

static int bcd1(uint8_t in)
{
	return (in & 0x0f) + ((in & 0xf0) >> 4) * 10;
}

static int bcd2(uint8_t hundreds, uint8_t tensunits)
{
	return (hundreds & 0x0f) * 100 + ((tensunits & 0xf0) >> 4) * 10 + (tensunits & 0x0f);
}

static int bcd_uint(uint8_t hundreds, uint8_t tensunits)
{
	return ((hundreds & 0xf0) >> 4) * 1000 + bcd2(hundreds, tensunits);
}

static uint64_t bcd_freq(const uint8_t *p, size_t n)
{
	uint64_t v = 0;
	uint64_t m = 1;
	size_t i;

	for (i = 0; i < n; i++) {
		v += (uint64_t)(p[i] & 0x0fu) * m;
		m *= 10;
		v += (uint64_t)((p[i] & 0xf0u) >> 4) * m;
		m *= 10;
	}

	return v;
}

//
// parseFrequency(data, lastPosition) for Scope Span: 3 = MHz byte index
//

static uint64_t parse_frequency(const uint8_t *d, size_t len, size_t last)
{
	uint64_t hz = 0;

	if (len <= last)
		return 0;
	if (len > last + 2) {
		hz += (uint64_t)(d[last + 2] & 0x0fu) * 1000000000ull;
		hz += (uint64_t)((d[last + 2] & 0xf0u) >> 4) * 10000000000ull;
	}
	if (len > last + 1) {
		hz += (uint64_t)(d[last + 1] & 0x0fu) * 100000000ull;
		hz += (uint64_t)((d[last + 1] & 0xf0u) >> 4) * 1000000000ull;
	}
	hz += (uint64_t)(d[last] & 0x0fu) * 1000000ull;
	hz += (uint64_t)((d[last] & 0xf0u) >> 4) * 10000000ull;
	hz += (uint64_t)(d[last - 1] & 0x0fu) * 10000ull;
	hz += (uint64_t)((d[last - 1] & 0xf0u) >> 4) * 100000ull;
	hz += (uint64_t)(d[last - 2] & 0x0fu) * 100ull;
	hz += (uint64_t)((d[last - 2] & 0xf0u) >> 4) * 1000ull;
	hz += (uint64_t)(d[last - 3] & 0x0fu);
	hz += (uint64_t)((d[last - 3] & 0xf0u) >> 4) * 10ull;
	return hz;
}

static uint8_t bcd_encode_char(int v)
{
	return (uint8_t)(((v / 10) % 10) << 4 | (v % 10));
}

//
// Meter calibration (rigcommander.cpp getMeterCal)
//

static double meter_cal(const struct rig_def *d, int meter, int value, bool *red)
{
	const char *name = C_METER_NAMES[meter];
	const struct rig_meter *prev = NULL;
	size_t i;

	*red = false;

	for (i = 0; i < d->n_meters; i++) {
		const struct rig_meter *m = &d->meters[i];

		if (strcmp(m->meter, name) != 0)
			continue;
		if (value <= m->rig_val) {
			*red = m->red_line;
			if (prev == NULL || value == m->rig_val)
				return m->actual;
			return m->actual - ((double)(m->rig_val - value) * (m->actual - prev->actual)) /
				(double)(m->rig_val - prev->rig_val);
		}
		prev = m;
	}

	if (prev != NULL) {
		*red = prev->red_line;
		return prev->actual;
	}

	return (double)value;
}

//
// Queue
//

static bool item_eq(const struct rig_item *a, const struct rig_item *b)
{
	return a->cmd == b->cmd && a->receiver == b->receiver && a->recurring == b->recurring &&
		a->has_value == b->has_value;
}

static enum rig_prio recurring_prio(const struct rig *r, int cmd, uint8_t receiver)
{
	size_t p;
	size_t i;

	for (p = 0; p < RIG_PRIO_COUNT; p++) {
		for (i = 0; i < r->q[p].n; i++) {
			const struct rig_item *it = &r->q[p].items[i];

			if (it->cmd == cmd && it->receiver == receiver && it->recurring)
				return (enum rig_prio)p;
		}
	}

	return RIG_PRIO_NONE;
}

static void slot_insert(struct rig_queue_slot *s, const struct rig_item *it, bool at_back)
{
	if (s->n == RIG_QUEUE_MAX)
		return;

	if (at_back) {
		s->items[s->n++] = *it;
	} else {
		memmove(&s->items[1], &s->items[0], s->n * sizeof(s->items[0]));
		s->items[0] = *it;
		s->n++;
	}
}

static void slot_remove_equal(struct rig_queue_slot *s, const struct rig_item *it)
{
	size_t i = 0;

	while (i < s->n) {
		if (item_eq(&s->items[i], it)) {
			memmove(&s->items[i], &s->items[i + 1], (s->n - i - 1) * sizeof(s->items[0]));
			s->n--;
		} else {
			i++;
		}
	}
}

void rig_queue_add(struct rig *r, enum rig_prio prio, const struct rig_item *in, bool unique)
{
	struct rig_item it = *in;

	// checkCommandAvailable: unknown to the rig file means not added
	if (r->have_caps && it.cmd < 0)
		return;
	if (!r->have_caps && it.cmd != RIG_CMD_ID)
		return;

	if (it.recurring && recurring_prio(r, it.cmd, it.receiver) == prio)
		return;
	if (it.recurring && prio == RIG_PRIO_IMMEDIATE)
		return;

	if (unique)
		slot_remove_equal(&r->q[prio], &it);

	// A recurring add also queues an immediate get (not for Transceiver ID)
	if (it.recurring && it.cmd != RIG_CMD_ID &&
			!(it.cmd >= 0 && r->kind[it.cmd] == K_TRANSCEIVER_ID)) {
		struct rig_item now = it;

		now.recurring = false;
		now.has_value = false;
		slot_insert(&r->q[RIG_PRIO_IMMEDIATE], &now, true);
	}

	slot_insert(&r->q[prio], &it, false);
}

static void queue_clear(struct rig *r)
{
	size_t p;

	for (p = 0; p < RIG_PRIO_COUNT; p++)
		r->q[p].n = 0;
}

int rig_cmd_index(const struct rig *r, const char *type)
{
	size_t i;

	for (i = 0; i < r->def.n_cmds; i++) {
		if (strcmp(r->def.cmds[i].type, type) == 0)
			return (int)i;
	}

	return -1;
}

static void add_named(struct rig *r, enum rig_prio prio, const char *type, bool recurring,
		uint8_t receiver, bool has_value, int value)
{
	struct rig_item it;

	it.cmd = rig_cmd_index(r, type);
	it.receiver = receiver;
	it.recurring = recurring;
	it.has_value = has_value;
	it.value = value;
	rig_queue_add(r, prio, &it, false);
}

//
// Build and send one command (receiveCommand / getCommand)
//

static void send_item(struct rig *r, const struct rig_item *it)
{
	uint8_t f[64];
	size_t n = 0;
	const struct rig_cmd *c;
	int k;

	f[n++] = 0xfe;
	f[n++] = 0xfe;

	if (it->cmd == RIG_CMD_ID) {
		f[n++] = r->civ_addr;
		f[n++] = RIG_CONTROLLER_ADDR;
		f[n++] = 0x19;
		f[n++] = 0x00;
		f[n++] = 0xfd;
		r->ev.send(r->ev.ctx, f, n);
		return;
	}

	c = &r->def.cmds[it->cmd];
	k = r->kind[it->cmd];

	if (it->has_value && (it->value < c->min || it->value > c->max))
		return;

	f[n++] = r->civ_addr;
	f[n++] = RIG_CONTROLLER_ADDR;
	memcpy(f + n, c->bytes, c->n);
	n += c->n;

	// Certain commands need the receiver number added
	switch (k) {
	case K_FREQ_RX: case K_MODE_RX: case K_SCOPE_MODE: case K_SCOPE_SPAN: case K_SCOPE_REF:
	case K_SCOPE_HOLD: case K_SCOPE_SPEED: case K_SCOPE_RX_OTHER: case K_SCOPE_EDGE:
		f[n++] = it->receiver;
		break;
	default:
		break;
	}

	if (it->has_value) {
		if (!c->set)
			return;
		// uchar values: BCD encoded (bcdEncodeChar); phase 4 adds the other types
		f[n++] = bcd_encode_char(it->value);
	}

	f[n++] = 0xfd;
	r->ev.send(r->ev.ctx, f, n);
}

//
// Scheduler interval (MainController.cpp)
//

static void set_interval(struct rig *r)
{
	int ms;

	if (!r->have_caps) {
		r->interval_ms = 250;
		return;
	}

	if (r->poll_ms_user > 0) {
		r->interval_ms = r->poll_ms_user;
		return;
	}

	if (r->baudrate == 0) {
		r->interval_ms = 25;
		return;
	}

	ms = (int)(9600000u / r->baudrate * 20u / 1000u);
	if (ms < 25)
		ms = 25;
	r->interval_ms = r->def.has_fd_comms ? ms : ms * 4;
}

//
// Rig ID known: load rig file, set up the initial queue (getInitialRigState)
//

static void have_rig_id(struct rig *r, int model)
{
	char err[256];
	size_t i;
	uint8_t rx;

	if (rigfile_find(&r->def, r->rig_dir, model, err, sizeof(err)) != 0) {
		r->failed = true;
		if (r->ev.error != NULL)
			r->ev.error(r->ev.ctx, err);
		return;
	}

	for (i = 0; i < r->def.n_cmds; i++) {
		const struct kind_entry *k = kind_of(r->def.cmds[i].type);

		r->kind[i] = k != NULL ? (int)k->kind : K_NONE;
	}

	r->have_caps = true;
	r->st.rig_valid = true;
	r->st.model_id = model;
	memcpy(r->st.model, r->def.model, sizeof(r->st.model) < sizeof(r->def.model) ? sizeof(r->st.model) : sizeof(r->def.model));
	r->st.model[sizeof(r->st.model) - 1] = '\0';
	r->st.receivers = r->def.receivers > 0 ? r->def.receivers : 1;
	r->st.amp_max = r->def.spect_amp_max;
	set_interval(r);

	queue_clear(r);

	for (i = 0; i < r->def.n_periodic; i++) {
		const struct rig_periodic *p = &r->def.periodic[i];
		enum rig_prio prio = RIG_PRIO_NONE;
		size_t k;

		for (k = 0; k < sizeof(C_PRIO_NAMES) / sizeof(C_PRIO_NAMES[0]); k++) {
			if (strcmp(C_PRIO_NAMES[k].name, p->priority) == 0)
				prio = C_PRIO_NAMES[k].prio;
		}
		if (prio == RIG_PRIO_NONE)
			continue;

		if (p->vfo == -1) {
			for (rx = 0; rx < (uint8_t)r->st.receivers; rx++)
				add_named(r, prio, p->command, true, rx, false, 0);
		} else {
			add_named(r, prio, p->command, true, (uint8_t)p->vfo, false, 0);
		}
	}

	if (r->def.has_spectrum) {
		add_named(r, RIG_PRIO_HIGH, "Scope On/Off", true, 0, true, 1);
		add_named(r, RIG_PRIO_HIGH, "Scope Data Output", false, 0, true, 1);
		for (rx = 0; rx < (uint8_t)r->st.receivers; rx++)
			add_named(r, RIG_PRIO_MEDIUM_HIGH, "Scope Ref", false, rx, false, 0);
	}

	if (r->ev.changed != NULL)
		r->ev.changed(r->ev.ctx, RADIO_CHG_RIG);
}

//
// Bounded string copy between fields of the same struct (no snprintf aliasing)
//

static void copy_name(char *dst, size_t size, const char *src)
{
	size_t n = strlen(src);

	if (n >= size)
		n = size - 1;
	memmove(dst, src, n);
	dst[n] = '\0';
}

//
// Mode lookup (parseMode)
//

static void set_mode(struct rig *r, struct radio_mode *m, int reg, int data, int filter)
{
	size_t i;

	m->valid = true;
	m->reg = reg;
	m->data = data;
	m->filter = filter;
	m->num = -1;
	m->name[0] = '\0';
	m->filter_name[0] = '\0';

	for (i = 0; i < r->def.n_modes; i++) {
		if (r->def.modes[i].reg == reg) {
			m->num = r->def.modes[i].num;
			copy_name(m->name, sizeof(m->name), r->def.modes[i].name);
			break;
		}
	}
	for (i = 0; i < r->def.n_filters; i++) {
		if (r->def.filters[i].num == filter) {
			copy_name(m->filter_name, sizeof(m->filter_name), r->def.filters[i].name);
			break;
		}
	}
}

static struct radio_value *field(struct rig *r, size_t off)
{
	return (struct radio_value *)(void *)((uint8_t *)&r->st + off);
}

static void set_value(struct rig *r, size_t off, int v)
{
	struct radio_value *f = field(r, off);

	f->valid = true;
	f->value = v;
}

//
// parseCommand: payload after FE FE to from, FD removed
//

static uint32_t parse_command(struct rig *r, const uint8_t *p, size_t len, uint8_t from, int64_t now_ms)
{
	const struct kind_entry *ke = NULL;
	int cmd = -1;
	size_t used = 0;
	size_t i;
	uint8_t receiver = 0;
	uint8_t vfo = 0;
	const uint8_t *d;
	size_t dl;

	if (len == 0)
		return 0;

	// Before the rig file is known only the Transceiver ID reply matters
	if (!r->have_caps) {
		if (len >= 3 && p[0] == 0x19 && p[1] == 0x00) {
			r->civ_addr = from;
			have_rig_id(r, p[2]);
			return RADIO_CHG_RIG;
		}
		return 0;
	}

	if (r->def.has_command29 && len >= 2 && p[0] == 0x29) {
		receiver = p[1];
		p += 2;
		len -= 2;
	}

	// Longest prefix match, 4 bytes down to 1
	for (used = 4; used > 0; used--) {
		if (used > len)
			continue;
		for (i = 0; i < r->def.n_cmds; i++) {
			const struct rig_cmd *c = &r->def.cmds[i];

			if (c->n == used && memcmp(c->bytes, p, used) == 0) {
				cmd = (int)i;
				break;
			}
		}
		if (cmd >= 0)
			break;
	}

	if (cmd < 0) {
		r->st.frames_unmatched++;
		return 0;
	}

	d = p + used;
	dl = len - used;
	ke = kind_of(r->def.cmds[cmd].type);
	r->st.frames_decoded++;

	if (ke == NULL)
		return 0;

	switch (ke->kind) {
	case K_FREQ_RX:
		if (dl < 1)
			return 0;
		receiver = d[0];
		d++;
		dl--;
		// fall through
	case K_FREQ:
	case K_FREQ_UNSEL:
		if (dl == 0 || receiver >= RADIO_MAX_RX)
			return 0;
		vfo = ke->kind == K_FREQ_UNSEL ? 1 : 0;
		r->st.rx[receiver].freq_hz[vfo] = bcd_freq(d, dl);
		r->st.rx[receiver].freq_valid[vfo] = true;
		return RADIO_CHG_FREQ;

	case K_MODE_RX:
		if (dl < 1)
			return 0;
		receiver = d[0];
		d++;
		dl--;
		// fall through
	case K_MODE:
	case K_MODE_UNSEL:
	case K_DATAMODE_FILTER: {
		struct radio_mode *m;
		int reg;
		int data;
		int filter;

		if (receiver >= RADIO_MAX_RX)
			return 0;
		vfo = ke->kind == K_MODE_UNSEL ? 1 : 0;
		m = &r->st.rx[receiver].mode[vfo];
		reg = m->reg;
		data = m->data;
		filter = m->filter;

		if (ke->kind == K_DATAMODE_FILTER) {
			if (dl < 2)
				return 0;
			filter = bcd1(d[1]);
			data = bcd1(d[0]);
		} else {
			if (dl >= 1)
				reg = bcd1(d[0]);
			if (dl == 2)
				filter = d[1];
			if (dl == 3) {
				data = d[1];
				filter = d[2];
			}
		}
		set_mode(r, m, reg, data, filter);
		return RADIO_CHG_MODE;
	}

	case K_BCD1:
		if (dl < 1)
			return 0;
		set_value(r, ke->field, bcd1(d[0]));
		return RADIO_CHG_FLAGS;

	case K_RAW1:
		if (dl < 1)
			return 0;
		set_value(r, ke->field, d[0]);
		return RADIO_CHG_FLAGS;

	case K_BCD2:
		if (dl < 2)
			return 0;
		set_value(r, ke->field, bcd2(d[0], d[1]));
		return RADIO_CHG_LEVELS;

	case K_KEY_SPEED:
		if (dl < 2)
			return 0;
		set_value(r, ke->field, (int)((double)bcd2(d[0], d[1]) / 6.071 + 6.0 + 0.5));
		return RADIO_CHG_LEVELS;

	case K_CW_PITCH: {
		double level;

		if (dl < 2)
			return 0;
		level = (double)(uint8_t)bcd2(d[0], d[1]);
		// round((((600 / 255) * level) + 300) / 5) * 5
		set_value(r, ke->field, (int)(((600.0 / 255.0) * level + 300.0) / 5.0 + 0.5) * 5);
		return RADIO_CHG_LEVELS;
	}

	case K_METER: {
		struct radio_meter_value *mv = &r->st.meter[ke->field];
		int raw;

		if (dl < 2)
			return 0;
		raw = (uint8_t)bcd2(d[0], d[1]);
		mv->valid = true;
		mv->raw = raw;
		mv->cal = meter_cal(&r->def, (int)ke->field, raw, &mv->red);
		return RADIO_CHG_METER;
	}

	case K_BOOL:
		if (dl < 1)
			return 0;
		set_value(r, ke->field, d[0] != 0);
		return RADIO_CHG_FLAGS;

	case K_FILTER_WIDTH: {
		const int pass = dl >= 1 ? bcd1(d[0]) : 0;
		const struct radio_mode *m = &r->st.rx[0].mode[0];
		int calc;

		if (dl < 1)
			return 0;
		if (m->valid && strcmp(m->name, "AM") == 0)
			calc = 200 + pass * 200;
		else if (pass <= 10)
			calc = 50 + pass * 50;
		else
			calc = 600 + (pass - 10) * 100;
		set_value(r, ke->field, calc);
		return RADIO_CHG_MODE;
	}

	case K_SCOPE_WAVE: {
		struct radio_scope_row *row;

		if (dl < 1 || r->def.spect_seq_max == 0)
			return 0;
		receiver = d[0];
		if (receiver >= RADIO_MAX_RX)
			return 0;
		row = &r->row[receiver];
		if (scope_asm_parse(&r->scope[receiver], d + 1, dl - 1, r->st.model_id,
					(size_t)r->def.spect_len_max, row)) {
			row->seq = ++r->row_seq;
			row->time_ms = now_ms;
			r->st.scope_rows++;
			return RADIO_CHG_SCOPE;
		}
		return 0;
	}

	case K_SCOPE_MODE:
		if (dl < 2 || d[0] >= RADIO_MAX_RX)
			return 0;
		r->st.rx[d[0]].scope.mode.valid = true;
		r->st.rx[d[0]].scope.mode.value = d[1];
		return RADIO_CHG_SCOPE_CFG;

	case K_SCOPE_SPAN: {
		uint64_t hz;

		if (dl < 2 || d[0] >= RADIO_MAX_RX)
			return 0;
		receiver = d[0];
		hz = parse_frequency(d + 1, dl - 1, 3);
		for (i = 0; i < r->def.n_spans; i++) {
			if (r->def.spans[i].freq == hz) {
				r->st.rx[receiver].scope.span_hz.valid = true;
				r->st.rx[receiver].scope.span_hz.value = (int)hz;
			}
		}
		return RADIO_CHG_SCOPE_CFG;
	}

	case K_SCOPE_EDGE:
		if (dl < 2 || d[0] >= RADIO_MAX_RX)
			return 0;
		r->st.rx[d[0]].scope.edge.valid = true;
		r->st.rx[d[0]].scope.edge.value = bcd1(d[1]);
		return RADIO_CHG_SCOPE_CFG;

	case K_SCOPE_HOLD:
		if (dl < 2 || d[0] >= RADIO_MAX_RX)
			return 0;
		r->st.rx[d[0]].scope.hold.valid = true;
		r->st.rx[d[0]].scope.hold.value = d[1] != 0;
		return RADIO_CHG_SCOPE_CFG;

	case K_SCOPE_REF: {
		int ref;

		if (dl < 4 || d[0] >= RADIO_MAX_RX)
			return 0;
		ref = bcd_uint(d[1], d[2]) / 10;
		if (d[3] != 0)
			ref *= -1 * d[3];
		r->st.rx[d[0]].scope.ref.valid = true;
		r->st.rx[d[0]].scope.ref.value = ref;
		return RADIO_CHG_SCOPE_CFG;
	}

	case K_SCOPE_SPEED:
		if (dl < 2 || d[0] >= RADIO_MAX_RX)
			return 0;
		r->st.rx[d[0]].scope.speed.valid = true;
		r->st.rx[d[0]].scope.speed.value = d[1];
		return RADIO_CHG_SCOPE_CFG;

	case K_TRANSCEIVER_ID:
	case K_SCOPE_RX_OTHER:
	case K_NONE:
	case K_IGNORE:
		break;
	}

	return 0;
}

//
// parseData: split on FD, check FE FE and the "to" address
//

static uint32_t parse_frame(struct rig *r, const uint8_t *f, size_t len, int64_t now_ms)
{
	// f includes FE FE .. FD
	if (len < 4 || f[0] != 0xfe || f[1] != 0xfe)
		return 0;

	switch (f[2]) {
	case 0xe0:
	case RIG_CONTROLLER_ADDR:
		break;
	case 0x00:
		// Our own broadcast echoed back: from E1 to 00
		if (f[3] == RIG_CONTROLLER_ADDR)
			return 0;
		break;
	default:
		return 0;
	}

	// Payload must not contain FE
	if (memchr(f + 4, 0xfe, len - 5) != NULL)
		return 0;

	return parse_command(r, f + 4, len - 5, f[3], now_ms);
}

void rig_input(struct rig *r, const uint8_t *civ, size_t len, int64_t now_ms)
{
	uint32_t groups = 0;
	size_t i;

	if (r->failed)
		return;

	for (i = 0; i < len; i++) {
		if (r->in_len < sizeof(r->in))
			r->in[r->in_len++] = civ[i];
		else
			r->in_len = 0;

		if (civ[i] == 0xfd) {
			size_t start = 0;

			// A frame starts at the last FE FE pair before FD
			while (start + 1 < r->in_len && !(r->in[start] == 0xfe && r->in[start + 1] == 0xfe))
				start++;
			if (start + 1 < r->in_len)
				groups |= parse_frame(r, r->in + start, r->in_len - start, now_ms);
			r->in_len = 0;
			if (r->failed)
				return;
		}
	}

	if (groups != 0 && r->ev.changed != NULL)
		r->ev.changed(r->ev.ctx, groups);
}

//
// Start: query the Transceiver ID by broadcast until the rig answers
//

void rig_init(struct rig *r, const char *rig_dir, uint32_t baudrate, int poll_ms,
		const struct rig_events *ev, int64_t now_ms)
{
	struct rig_item it;
	size_t i;

	memset(r, 0, sizeof(*r));
	snprintf(r->rig_dir, sizeof(r->rig_dir), "%s", rig_dir);
	r->baudrate = baudrate;
	r->poll_ms_user = poll_ms;
	r->ev = *ev;
	r->civ_addr = 0x00;
	r->counter = RIG_PRIO_IMMEDIATE;
	for (i = 0; i < RADIO_MAX_RX; i++)
		scope_asm_reset(&r->scope[i]);
	set_interval(r);
	r->t_next = now_ms + r->interval_ms;

	it.cmd = RIG_CMD_ID;
	it.receiver = 0;
	it.recurring = true;
	it.has_value = false;
	it.value = 0;
	rig_queue_add(r, RIG_PRIO_HIGH, &it, true);
}

//
// Scheduler: one item per interval (cachingQueue::run)
//

void rig_tick(struct rig *r, int64_t now_ms)
{
	enum rig_prio prio = RIG_PRIO_IMMEDIATE;
	struct rig_queue_slot *s;
	struct rig_item it;

	if (r->failed || now_ms < r->t_next)
		return;
	r->t_next = now_ms + r->interval_ms;

	if (r->q[RIG_PRIO_IMMEDIATE].n == 0) {
		const unsigned int c = r->counter;

		if (c % RIG_PRIO_HIGHEST == 0)
			prio = RIG_PRIO_HIGHEST;
		else if (c % RIG_PRIO_HIGH == 0)
			prio = RIG_PRIO_HIGH;
		else if (c % RIG_PRIO_MEDIUM_HIGH == 0)
			prio = RIG_PRIO_MEDIUM_HIGH;
		else if (c % RIG_PRIO_MEDIUM == 0)
			prio = RIG_PRIO_MEDIUM;
		else if (c % RIG_PRIO_MEDIUM_LOW == 0)
			prio = RIG_PRIO_MEDIUM_LOW;
		else if (c % RIG_PRIO_LOW == 0)
			prio = RIG_PRIO_LOW;
		else if (c % RIG_PRIO_LOWEST == 0) {
			prio = RIG_PRIO_LOWEST;
			r->counter = RIG_PRIO_IMMEDIATE;
		}
	}
	r->counter++;

	s = &r->q[prio];
	if (s->n == 0)
		return;

	it = s->items[s->n - 1];
	s->n--;

	if (it.recurring && prio != RIG_PRIO_IMMEDIATE)
		slot_insert(s, &it, false);

	send_item(r, &it);
}

int64_t rig_next_deadline(const struct rig *r)
{
	return r->failed ? 0 : r->t_next;
}
