// SPDX-License-Identifier: GPL-3.0-or-later
//
// struct ui_ops on top of a session (receiver 0, selected VFO)
//

#include "ui/ui.h"

#include "core/rig.h"	/* RIG_CONTROLLER_ADDR, for the power frame */

static void op_get_state(void *ctx, struct radio_state *out)
{
	session_get_state(ctx, out);
}

static void op_get_info(void *ctx, struct session_info *out)
{
	session_get_info(ctx, out);
}

static bool op_get_scope_row(void *ctx, struct radio_scope_row *out)
{
	return session_get_scope_row(ctx, 0, out);
}

static const struct rig_def *op_get_rig(void *ctx)
{
	return session_rig_def(ctx);
}

static int op_set_freq(void *ctx, uint64_t hz)
{
	return session_set_freq(ctx, 0, hz);
}

static int op_tune(void *ctx, int steps)
{
	return session_tune(ctx, 0, steps);
}

static int op_set_mode(void *ctx, int mode_num, int data, int filter)
{
	return session_set_mode(ctx, 0, mode_num, data, filter);
}

static int op_set_value(void *ctx, const char *type, int value)
{
	return session_set_value(ctx, type, 0, value);
}

static int op_set_span(void *ctx, uint32_t half_span_hz)
{
	return session_set_span(ctx, 0, half_span_hz);
}

static void op_set_step(void *ctx, uint32_t hz)
{
	session_set_step(ctx, hz);
}

static uint32_t op_get_step(void *ctx)
{
	return session_get_step(ctx);
}

static void op_set_lock(void *ctx, bool locked)
{
	session_set_freq_lock(ctx, locked);
}

static bool op_get_lock(void *ctx)
{
	return session_get_freq_lock(ctx);
}

static void op_set_volume(void *ctx, uint8_t volume)
{
	session_set_volume(ctx, volume);
}

static int op_ptt(void *ctx, int on)
{
	return tx_ptt(ctx, on);
}

static int op_ptt_heartbeat(void *ctx)
{
	return tx_ptt_heartbeat(ctx);
}

static int op_atu_tune(void *ctx)
{
	return tx_tune(ctx);
}

static int op_get_value(void *ctx, const char *type)
{
	return session_get_value(ctx, type, 0);
}

// CI-V 18 01 / 18 00 to the address the rig gave at login, as a frame of our own (as
// wfview's powerOn / powerOff over the network, no FE preamble): in standby the rig
// answers no ID, so no rig file is loaded and a named set would be refused
static int op_power(void *ctx, int on)
{
	struct session_info info;
	uint8_t f[7];

	session_get_info(ctx, &info);
	if (info.civ_addr == 0x00)
		return -1;
	f[0] = 0xfe;
	f[1] = 0xfe;
	f[2] = info.civ_addr;
	f[3] = RIG_CONTROLLER_ADDR;
	f[4] = 0x18;
	f[5] = on ? 0x01 : 0x00;
	f[6] = 0xfd;
	return session_civ_send(ctx, f, sizeof(f));
}

void ui_ops_from_session(struct ui_ops *ops, struct session *s)
{
	ops->ctx = s;
	ops->get_state = op_get_state;
	ops->get_info = op_get_info;
	ops->get_scope_row = op_get_scope_row;
	ops->get_rig = op_get_rig;
	ops->set_freq = op_set_freq;
	ops->tune = op_tune;
	ops->set_mode = op_set_mode;
	ops->set_value = op_set_value;
	ops->get_value = op_get_value;
	ops->power = op_power;
	ops->set_span = op_set_span;
	ops->set_step = op_set_step;
	ops->get_step = op_get_step;
	ops->set_lock = op_set_lock;
	ops->get_lock = op_get_lock;
	ops->set_volume = op_set_volume;
	ops->ptt = op_ptt;
	ops->ptt_heartbeat = op_ptt_heartbeat;
	ops->atu_tune = op_atu_tune;
}
