// SPDX-License-Identifier: GPL-3.0-or-later
//
// Excess network delay estimate from peer ping timestamps.
//
// Port of PingDelayTracker from wfview main include/pingdelaytracker.h,
// wfview is copyright 2017-2026 Elliott H. Liggett (W6EL) and Phil Taylor (M0VSE), GPLv3.
//

#pragma once

#include <stdbool.h>
#include <stdint.h>

#define PING_DELAY_DAY_MS	(24 * 60 * 60 * 1000)
#define PING_DELAY_WINDOW_MS	10000

struct ping_delay {

	//
	// State
	//
	bool valid;
	int excess;

	//
	// Remote clock unwrapping
	//
	int last_remote;
	int64_t remote_unwrapped;

	//
	// Baseline tracking
	//
	int64_t baseline;
	int64_t window_min;
	int64_t window_start;
};

//
// Tracking
//

void ping_delay_reset(struct ping_delay *pd);
int ping_delay_update(struct ping_delay *pd, int64_t local_mono_ms, uint32_t remote_stamp_ms);

//
// Thresholds
//

int ping_delay_drop_threshold_ms(int latency_ms);
