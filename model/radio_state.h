// SPDX-License-Identifier: GPL-3.0-or-later
//
// Radio state as decoded from CI-V. Written on the network thread, read by
// the UI as a snapshot (session_get_state). A field the rig file does not
// define stays invalid; the UI hides it.
//

#pragma once

#include <stdbool.h>
#include <stdint.h>

#define RADIO_MAX_RX		2
#define RADIO_SCOPE_MAX_BINS	1024
#define RADIO_NAME_LEN		48

//
// Change groups for notification
//

#define RADIO_CHG_FREQ		0x0001u
#define RADIO_CHG_MODE		0x0002u
#define RADIO_CHG_METER		0x0004u
#define RADIO_CHG_SCOPE		0x0008u
#define RADIO_CHG_LEVELS	0x0010u
#define RADIO_CHG_FLAGS		0x0020u
#define RADIO_CHG_SCOPE_CFG	0x0040u
#define RADIO_CHG_RIG		0x0080u

enum radio_meter {
	RADIO_METER_S,
	RADIO_METER_CENTER,
	RADIO_METER_POWER,
	RADIO_METER_SWR,
	RADIO_METER_ALC,
	RADIO_METER_COMP,
	RADIO_METER_VD,
	RADIO_METER_ID,
	RADIO_METER_COUNT
};

struct radio_value {
	bool valid;
	int value;
};

struct radio_meter_value {
	bool valid;
	int raw;
	double cal;
	bool red;
};

struct radio_mode {
	bool valid;
	int reg;
	int num;
	int data;
	int filter;
	char name[RADIO_NAME_LEN];
	char filter_name[RADIO_NAME_LEN];
};

struct radio_scope_cfg {
	struct radio_value mode;
	struct radio_value span_hz;
	struct radio_value edge;
	struct radio_value hold;
	struct radio_value speed;
	struct radio_value ref;
};

struct radio_scope_row {
	bool valid;
	uint32_t seq;
	int64_t time_ms;
	uint64_t low_hz;
	uint64_t high_hz;
	bool oor;
	uint16_t len;
	uint8_t bins[RADIO_SCOPE_MAX_BINS];
};

struct radio_rx {

	//
	// VFOs: [0] selected / A, [1] unselected / B
	//
	bool freq_valid[2];
	uint64_t freq_hz[2];
	struct radio_mode mode[2];

	//
	// Scope configuration
	//
	struct radio_scope_cfg scope;
};

struct radio_state {

	//
	// Rig identity
	//
	bool rig_valid;
	int model_id;
	char model[RADIO_NAME_LEN];
	int receivers;
	int amp_max;

	//
	// Per receiver
	//
	struct radio_rx rx[RADIO_MAX_RX];

	//
	// Meters
	//
	struct radio_meter_value meter[RADIO_METER_COUNT];

	//
	// Levels and switches (BCD / bool values as wfview delivers them)
	//
	struct radio_value af_gain;
	struct radio_value rf_gain;
	struct radio_value squelch;
	struct radio_value rf_power;
	struct radio_value mic_gain;
	struct radio_value nr_level;
	struct radio_value nb_level;
	struct radio_value pbt_inner;
	struct radio_value pbt_outer;
	struct radio_value cw_pitch;
	struct radio_value key_speed;
	struct radio_value monitor_gain;
	struct radio_value agc;
	struct radio_value preamp;
	struct radio_value attenuator;
	struct radio_value tuning_step;
	struct radio_value tuner;
	struct radio_value split;
	struct radio_value transmit;
	struct radio_value noise_blanker;
	struct radio_value noise_reduction;
	struct radio_value auto_notch;
	struct radio_value compressor;
	struct radio_value monitor;
	struct radio_value vox;
	struct radio_value break_in;
	struct radio_value dial_lock;
	struct radio_value overflow;
	struct radio_value filter_width;

	uint32_t frames_decoded;
	uint32_t frames_unmatched;
	uint32_t scope_rows;
	uint32_t scope_dropped;
};
