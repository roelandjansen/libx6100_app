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
	struct radio_value ref;		/* tenths of a dB: -125 = -12.5 dB */
	struct radio_value vbw;		/* 0 narrow, 1 wide */
	struct radio_value rbw;		/* 0 wide, 1 mid, 2 narrow */
	struct radio_value center_type;	/* 0 filter centre, 1 carrier point centre, 2 carrier point centre (abs. freq.) */
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

	//
	// Meter and receiver values of this receiver (addition 13). A Command29 rig
	// (785x, 7610, 7760) answers these with the receiver byte; the single fields in
	// radio_state hold whichever receiver answered last. A one-receiver rig, or a
	// command the rig file does not mark Command29, fills rx[0]
	//
	struct radio_meter_value s_meter;
	struct radio_value af_gain;
	struct radio_value rf_gain;
	struct radio_value squelch;
	struct radio_value nr_level;
	struct radio_value nb_level;
	struct radio_value pbt_inner;
	struct radio_value pbt_outer;
	struct radio_value filter_width;
	struct radio_value filter_shape;	/* 0 sharp, 1 soft */
	struct radio_value agc;
	struct radio_value preamp;
	struct radio_value attenuator;	/* dB */
	struct radio_value antenna;	/* 0 ANT1, 1 ANT2 */
	struct radio_value antenna_rx;	/* RX-ANT of that antenna (Antenna command, second byte) */
	struct radio_value rx_antenna;	/* RX Antenna command */
	struct radio_value noise_blanker;
	struct radio_value noise_reduction;
	struct radio_value auto_notch;
	struct radio_value manual_notch;
	struct radio_value notch_pos;	/* Notch Filter: 0-255 */
	struct radio_value apf;		/* Audio Peak Filter: 0 off, else the rig's APF setting */
	struct radio_value apf_level;	/* APF Type Level: 0-255 */
	struct radio_value twin_peak;
	struct radio_value digisel;
	struct radio_value digisel_shift;	/* 0-255 */
	struct radio_value ip_plus;
	struct radio_value af_mute;
	struct radio_value sql_open;	/* Various Squelch: 1 the squelch is open */
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
	struct radio_value agc_rx[RADIO_MAX_RX];	/* per receiver: Command29 rigs (785x, 7610, 7760) answer with the receiver byte (2026-11-22) */
	struct radio_value preamp;
	struct radio_value attenuator;
	struct radio_value tuning_step;
	struct radio_value tuner;
	struct radio_value dual_watch;	// VFO Dual Watch: 0 off, 1 on (two receivers)
	struct radio_value rx_selected;	// VFO Main/Sub Band: 0 main, 1 sub is the selected (active) receiver
	struct radio_value scope_rx;	// Scope Main/Sub: 0 main, 1 sub on the (single) scope
	struct radio_value scope_dual;	// Scope Single/Dual: 0 one scope, 1 both receivers (addition 10)
	struct radio_value scope_on;	// Scope On/Off
	struct radio_value scope_output;	// Scope Data Output: the rig sends Scope Wave Data
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

	//
	// Addition 13: decoded (and so settable) since 2026-11-28; the per-receiver
	// ones are in rx[] as well
	//
	struct radio_value filter_shape;
	struct radio_value antenna;
	struct radio_value antenna_rx;
	struct radio_value rx_antenna;
	struct radio_value manual_notch;
	struct radio_value notch_pos;
	struct radio_value apf;
	struct radio_value apf_level;
	struct radio_value twin_peak;
	struct radio_value digisel;
	struct radio_value digisel_shift;
	struct radio_value ip_plus;
	struct radio_value af_mute;
	struct radio_value sql_open;
	struct radio_value comp_level;	/* Compressor Level: 0-255 */
	struct radio_value ssb_tx_bw;	/* SSB TX Bandwidth: 0 wide, 1 mid, 2 narrow */
	struct radio_value xfc;
	struct radio_value rit_on;
	struct radio_value rit_tx;	/* dTX: RIT TX Status */
	struct radio_value rit_hz;	/* RIT Frequency in Hz, -9999 to +9999 */

	uint32_t frames_decoded;
	uint32_t frames_unmatched;
	uint32_t scope_rows;
	uint32_t scope_dropped;
};
