// SPDX-License-Identifier: GPL-3.0-or-later
//
// wfview .rig file reader (QSettings INI format, [General] Version=2.10).
//
// Only the keys this client uses are read; unknown keys are ignored, and
// the version is informational only, both as wfview does. Only Icom rig
// files (Manufacturer=0) are used, as wfview filters by manufacturer.
//

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RIG_NAME_LEN		48
#define RIG_CMD_BYTES		8
#define RIG_MAX_CMDS		256
#define RIG_MAX_PERIODIC	96
#define RIG_MAX_MODES		32
#define RIG_MAX_FILTERS		16
#define RIG_MAX_SPANS		24
#define RIG_MAX_SCOPEMODES	8
#define RIG_MAX_METERS		128
#define RIG_MAX_INPUTS		16
#define RIG_MAX_BANDS		48
#define RIG_MAX_SIMPLE		16

struct rig_cmd {
	char type[RIG_NAME_LEN];
	uint8_t bytes[RIG_CMD_BYTES];
	size_t n;
	int min;
	int max;
	int nbytes;
	bool cmd29;
	bool get;
	bool set;
	bool admin;
};

struct rig_periodic {
	char command[RIG_NAME_LEN];
	char priority[16];
	int vfo;
};

struct rig_mode {
	int num;
	int reg;
	int min;
	int max;
	char name[RIG_NAME_LEN];
};

struct rig_filter {
	int num;
	int modes;
	char name[RIG_NAME_LEN];
};

struct rig_span {
	int num;
	uint32_t freq;
	char name[RIG_NAME_LEN];
};

struct rig_named {
	int num;
	int reg;
	char name[RIG_NAME_LEN];
};

struct rig_meter {
	char meter[RIG_NAME_LEN];
	int rig_val;
	double actual;
	bool red_line;
};

struct rig_band {
	int num;
	int bsr;
	uint64_t start;
	uint64_t end;
	int bytes;
	char region[16];
	char name[RIG_NAME_LEN];
};

struct rig_def {

	//
	// [Rig] scalars
	//
	char version[16];
	int manufacturer;
	char model[RIG_NAME_LEN];
	int civ_address;
	int receivers;
	int vfos;
	int spect_seq_max;
	int spect_amp_max;
	int spect_len_max;
	bool has_spectrum;
	bool has_transmit;
	bool has_fd_comms;
	bool has_command29;

	//
	// Arrays
	//
	size_t n_cmds;
	struct rig_cmd cmds[RIG_MAX_CMDS];
	size_t n_periodic;
	struct rig_periodic periodic[RIG_MAX_PERIODIC];
	size_t n_modes;
	struct rig_mode modes[RIG_MAX_MODES];
	size_t n_filters;
	struct rig_filter filters[RIG_MAX_FILTERS];
	size_t n_spans;
	struct rig_span spans[RIG_MAX_SPANS];
	size_t n_scope_modes;
	struct rig_named scope_modes[RIG_MAX_SCOPEMODES];
	size_t n_meters;
	struct rig_meter meters[RIG_MAX_METERS];
	size_t n_inputs;
	struct rig_named inputs[RIG_MAX_INPUTS];
	size_t n_attenuators;
	struct rig_named attenuators[RIG_MAX_SIMPLE];
	size_t n_preamps;
	struct rig_named preamps[RIG_MAX_SIMPLE];
	size_t n_bands;
	struct rig_band bands[RIG_MAX_BANDS];

	char path[512];
};

//
// Loading
//

int rigfile_load(struct rig_def *r, const char *path, char *err, size_t err_size);
int rigfile_find(struct rig_def *r, const char *dir, int civ_address, char *err, size_t err_size);

//
// Lookup
//

const struct rig_cmd *rigfile_cmd(const struct rig_def *r, const char *type);
