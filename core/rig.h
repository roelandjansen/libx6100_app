// SPDX-License-Identifier: GPL-3.0-or-later
//
// Rig layer on top of the CI-V stream: model detection, rig file, command
// queue with wfview's priority scheduler, CI-V frame parsing and decoding
// into struct radio_state.
//
// Ports of wfview v3-initial src/cachingqueue.cpp (scheduler),
// src/radio/icomcommander.cpp (parseData / parseCommand / receiveCommand),
// src/rigcommander.cpp (getMeterCal) and src/MainController.cpp
// (initial queue), wfview is copyright 2017-2026 Elliott H. Liggett (W6EL)
// and Phil Taylor (M0VSE), GPLv3.
//
// Network thread only.
//

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core/rigfile.h"
#include "core/scope.h"
#include "model/radio_state.h"

#define RIG_CONTROLLER_ADDR	0xe1
#define RIG_QUEUE_MAX		256
#define RIG_PRIO_COUNT		24

enum rig_prio {
	RIG_PRIO_NONE = 0,
	RIG_PRIO_IMMEDIATE = 1,
	RIG_PRIO_HIGHEST = 2,
	RIG_PRIO_HIGH = 3,
	RIG_PRIO_MEDIUM_HIGH = 5,
	RIG_PRIO_MEDIUM = 7,
	RIG_PRIO_MEDIUM_LOW = 11,
	RIG_PRIO_LOW = 19,
	RIG_PRIO_LOWEST = 23
};

struct rig_item {
	int cmd;		// index into rig_def.cmds, RIG_CMD_ID for Transceiver ID before caps
	uint8_t receiver;
	bool recurring;
	bool has_value;
	int value;
};

#define RIG_CMD_ID	(-2)

struct rig_queue_slot {
	size_t n;
	struct rig_item items[RIG_QUEUE_MAX];	// front = index 0, taken from the back
};

struct rig_events {
	int (*send)(void *ctx, const uint8_t *civ, size_t len);
	void (*changed)(void *ctx, uint32_t groups);
	void (*error)(void *ctx, const char *msg);
	void *ctx;
};

struct rig {

	//
	// Wiring
	//
	struct rig_events ev;
	char rig_dir[512];
	uint32_t baudrate;
	int poll_ms_user;

	//
	// Rig
	//
	bool have_caps;
	bool failed;
	uint8_t civ_addr;
	struct rig_def def;
	int kind[RIG_MAX_CMDS];

	//
	// Queue
	//
	struct rig_queue_slot q[RIG_PRIO_COUNT];
	unsigned int counter;
	int interval_ms;
	int64_t t_next;

	//
	// Frame reassembly across CI-V packets
	//
	size_t in_len;
	uint8_t in[2048];

	//
	// Scope
	//
	struct scope_asm scope[RADIO_MAX_RX];

	//
	// State
	//
	struct radio_state st;
	struct radio_scope_row row[RADIO_MAX_RX];
	uint32_t row_seq;
};

//
// Lifecycle
//

void rig_init(struct rig *r, const char *rig_dir, uint32_t baudrate, int poll_ms,
		const struct rig_events *ev, int64_t now_ms);

//
// Traffic and timers
//

void rig_input(struct rig *r, const uint8_t *civ, size_t len, int64_t now_ms);
void rig_tick(struct rig *r, int64_t now_ms);
int64_t rig_next_deadline(const struct rig *r);

//
// Queue access (used by phase 4 controls)
//

int rig_cmd_index(const struct rig *r, const char *type);
void rig_queue_add(struct rig *r, enum rig_prio prio, const struct rig_item *it, bool unique);
