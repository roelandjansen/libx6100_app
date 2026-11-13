// SPDX-License-Identifier: GPL-3.0-or-later
//
// wfview .rig file reader (QSettings INI format).
//
// Format as written by wfview main (rig creator / QSettings):
//	[General] Version=2.10
//	[Rig] scalar keys, and arrays as Name\size=n plus Name\<i>\<Key>=value (1-based)
// QSettings escapes a backslash in a value as "\\"; command strings are
// stored as text like \\x27\\x00, i.e. "\x27\x00" after unescaping.
//

#include "core/rigfile.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define C_LINE_MAX	4096

static void set_err(char *err, size_t n, const char *fmt, const char *a, long b)
{
	if (err != NULL && n > 0)
		snprintf(err, n, fmt, a, b);
}

//
// String helpers
//

static void trim(char *s)
{
	size_t n = strlen(s);
	size_t i = 0;

	while (n > 0 && (s[n - 1] == '\r' || s[n - 1] == '\n' || s[n - 1] == ' ' || s[n - 1] == '\t'))
		s[--n] = '\0';
	while (s[i] == ' ' || s[i] == '\t')
		i++;
	if (i > 0)
		memmove(s, s + i, n - i + 1);
}

static void copy_str(char *dst, size_t size, const char *src)
{
	size_t n = strlen(src);

	if (n >= size)
		n = size - 1;
	memmove(dst, src, n);
	dst[n] = '\0';
}

//
// QSettings value unescape: surrounding quotes removed, \\ -> \, \" -> "
//

static void unescape(char *v)
{
	size_t n = strlen(v);
	size_t i;
	size_t o = 0;

	if (n >= 2 && v[0] == '"' && v[n - 1] == '"') {
		memmove(v, v + 1, n - 2);
		v[n - 2] = '\0';
	}

	for (i = 0; v[i] != '\0'; i++) {
		if (v[i] == '\\' && (v[i + 1] == '\\' || v[i + 1] == '"')) {
			v[o++] = v[i + 1];
			i++;
		} else {
			v[o++] = v[i];
		}
	}
	v[o] = '\0';
}

static int to_int(const char *v)
{
	return (int)strtol(v, NULL, 10);
}

static bool to_bool(const char *v)
{
	return strcmp(v, "true") == 0 || strcmp(v, "1") == 0;
}

static int hexval(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

//
// "\x27\x00" -> 27 00
//

static size_t parse_cmd_bytes(const char *v, uint8_t *out, size_t cap)
{
	size_t n = 0;
	size_t i = 0;

	while (v[i] != '\0' && n < cap) {
		if (v[i] == '\\' && v[i + 1] == 'x' && hexval(v[i + 2]) >= 0 && hexval(v[i + 3]) >= 0) {
			out[n++] = (uint8_t)(hexval(v[i + 2]) * 16 + hexval(v[i + 3]));
			i += 4;
		} else {
			// A literal character in the command string
			out[n++] = (uint8_t)v[i];
			i++;
		}
	}

	return n;
}

//
// Array key "Name\<i>\<Key>": returns index (0-based) or -1
//

static int array_key(const char *key, const char *name, const char **field)
{
	const size_t ln = strlen(name);
	char *end;
	long idx;

	if (strncmp(key, name, ln) != 0 || key[ln] != '\\')
		return -1;
	idx = strtol(key + ln + 1, &end, 10);
	if (end == key + ln + 1 || *end != '\\' || idx < 1)
		return -1;
	*field = end + 1;
	return (int)(idx - 1);
}

static void grow(size_t *n, int idx)
{
	if ((size_t)idx + 1 > *n)
		*n = (size_t)idx + 1;
}

static void rig_key(struct rig_def *r, const char *key, const char *val)
{
	const char *f;
	int i;

	if (strcmp(key, "Manufacturer") == 0)
		r->manufacturer = to_int(val);
	else if (strcmp(key, "Model") == 0)
		copy_str(r->model, sizeof(r->model), val);
	else if (strcmp(key, "CIVAddress") == 0)
		r->civ_address = to_int(val);
	else if (strcmp(key, "NumberOfReceivers") == 0)
		r->receivers = to_int(val);
	else if (strcmp(key, "NumberOfVFOs") == 0)
		r->vfos = to_int(val);
	else if (strcmp(key, "SpectrumSeqMax") == 0)
		r->spect_seq_max = to_int(val);
	else if (strcmp(key, "SpectrumAmpMax") == 0)
		r->spect_amp_max = to_int(val);
	else if (strcmp(key, "SpectrumLenMax") == 0)
		r->spect_len_max = to_int(val);
	else if (strcmp(key, "HasSpectrum") == 0)
		r->has_spectrum = to_bool(val);
	else if (strcmp(key, "HasTransmit") == 0)
		r->has_transmit = to_bool(val);
	else if (strcmp(key, "HasFDComms") == 0)
		r->has_fd_comms = to_bool(val);
	else if (strcmp(key, "HasCommand29") == 0)
		r->has_command29 = to_bool(val);
	else if ((i = array_key(key, "Commands", &f)) >= 0 && i < RIG_MAX_CMDS) {
		struct rig_cmd *c = &r->cmds[i];

		grow(&r->n_cmds, i);
		if (strcmp(f, "Type") == 0)
			copy_str(c->type, sizeof(c->type), val);
		else if (strcmp(f, "String") == 0)
			c->n = parse_cmd_bytes(val, c->bytes, sizeof(c->bytes));
		else if (strcmp(f, "Min") == 0)
			c->min = to_int(val);
		else if (strcmp(f, "Max") == 0)
			c->max = to_int(val);
		else if (strcmp(f, "Bytes") == 0)
			c->nbytes = to_int(val);
		else if (strcmp(f, "Command29") == 0)
			c->cmd29 = to_bool(val);
		else if (strcmp(f, "GetCommand") == 0)
			c->get = to_bool(val);
		else if (strcmp(f, "SetCommand") == 0)
			c->set = to_bool(val);
		else if (strcmp(f, "Admin") == 0)
			c->admin = to_bool(val);
	} else if ((i = array_key(key, "Periodic", &f)) >= 0 && i < RIG_MAX_PERIODIC) {
		struct rig_periodic *p = &r->periodic[i];

		grow(&r->n_periodic, i);
		if (strcmp(f, "Command") == 0)
			copy_str(p->command, sizeof(p->command), val);
		else if (strcmp(f, "Priority") == 0)
			copy_str(p->priority, sizeof(p->priority), val);
		else if (strcmp(f, "VFO") == 0)
			p->vfo = to_int(val);
	} else if ((i = array_key(key, "Modes", &f)) >= 0 && i < RIG_MAX_MODES) {
		struct rig_mode *m = &r->modes[i];

		grow(&r->n_modes, i);
		if (strcmp(f, "Num") == 0)
			m->num = to_int(val);
		else if (strcmp(f, "Reg") == 0)
			m->reg = to_int(val);
		else if (strcmp(f, "Min") == 0)
			m->min = to_int(val);
		else if (strcmp(f, "Max") == 0)
			m->max = to_int(val);
		else if (strcmp(f, "Name") == 0)
			copy_str(m->name, sizeof(m->name), val);
	} else if ((i = array_key(key, "Filters", &f)) >= 0 && i < RIG_MAX_FILTERS) {
		struct rig_filter *x = &r->filters[i];

		grow(&r->n_filters, i);
		if (strcmp(f, "Num") == 0)
			x->num = to_int(val);
		else if (strcmp(f, "Modes") == 0)
			x->modes = to_int(val);
		else if (strcmp(f, "Name") == 0)
			copy_str(x->name, sizeof(x->name), val);
	} else if ((i = array_key(key, "Spans", &f)) >= 0 && i < RIG_MAX_SPANS) {
		struct rig_span *s = &r->spans[i];

		grow(&r->n_spans, i);
		if (strcmp(f, "Num") == 0)
			s->num = to_int(val);
		else if (strcmp(f, "Freq") == 0)
			s->freq = (uint32_t)strtoul(val, NULL, 10);
		else if (strcmp(f, "Name") == 0)
			copy_str(s->name, sizeof(s->name), val);
	} else if ((i = array_key(key, "Meters", &f)) >= 0 && i < RIG_MAX_METERS) {
		struct rig_meter *m = &r->meters[i];

		grow(&r->n_meters, i);
		if (strcmp(f, "Meter") == 0)
			copy_str(m->meter, sizeof(m->meter), val);
		else if (strcmp(f, "RigVal") == 0)
			m->rig_val = to_int(val);
		else if (strcmp(f, "ActualVal") == 0)
			m->actual = strtod(val, NULL);
		else if (strcmp(f, "RedLine") == 0)
			m->red_line = to_bool(val);
	} else if ((i = array_key(key, "Bands", &f)) >= 0 && i < RIG_MAX_BANDS) {
		struct rig_band *b = &r->bands[i];

		grow(&r->n_bands, i);
		if (strcmp(f, "Num") == 0)
			b->num = to_int(val);
		else if (strcmp(f, "BSR") == 0)
			b->bsr = to_int(val);
		else if (strcmp(f, "Start") == 0)
			b->start = strtoull(val, NULL, 10);
		else if (strcmp(f, "End") == 0)
			b->end = strtoull(val, NULL, 10);
		else if (strcmp(f, "Bytes") == 0)
			b->bytes = to_int(val);
		else if (strcmp(f, "Region") == 0)
			copy_str(b->region, sizeof(b->region), val);
		else if (strcmp(f, "Name") == 0)
			copy_str(b->name, sizeof(b->name), val);
	} else {
		struct {
			const char *name;
			size_t *n;
			struct rig_named *arr;
			int cap;
		} named[] = {
			{ "ScopeModes", &r->n_scope_modes, r->scope_modes, RIG_MAX_SCOPEMODES },
			{ "Inputs", &r->n_inputs, r->inputs, RIG_MAX_INPUTS },
			{ "Attenuators", &r->n_attenuators, r->attenuators, RIG_MAX_SIMPLE },
			{ "Preamps", &r->n_preamps, r->preamps, RIG_MAX_SIMPLE },
		};
		size_t k;

		for (k = 0; k < sizeof(named) / sizeof(named[0]); k++) {
			if ((i = array_key(key, named[k].name, &f)) >= 0 && i < named[k].cap) {
				struct rig_named *e = &named[k].arr[i];

				grow(named[k].n, i);
				if (strcmp(f, "Num") == 0)
					e->num = to_int(val);
				else if (strcmp(f, "Reg") == 0)
					e->reg = to_int(val);
				else if (strcmp(f, "Name") == 0)
					copy_str(e->name, sizeof(e->name), val);
				return;
			}
		}
	}
}

//
// Load a complete .rig file
//

int rigfile_load(struct rig_def *r, const char *path, char *err, size_t err_size)
{
	FILE *fp = fopen(path, "r");
	char line[C_LINE_MAX];
	char section[64] = "";
	long lineno = 0;

	memset(r, 0, sizeof(*r));
	r->civ_address = -1;
	copy_str(r->path, sizeof(r->path), path);

	if (fp == NULL) {
		set_err(err, err_size, "%s: cannot open (%ld)", path, (long)errno);
		return -1;
	}

	while (fgets(line, sizeof(line), fp) != NULL) {
		char *eq;

		lineno++;
		trim(line);
		if (line[0] == '\0' || line[0] == ';' || line[0] == '#')
			continue;

		if (line[0] == '[') {
			char *end = strchr(line, ']');

			if (end != NULL) {
				*end = '\0';
				copy_str(section, sizeof(section), line + 1);
			}
			continue;
		}

		eq = strchr(line, '=');
		if (eq == NULL)
			continue;
		*eq = '\0';
		trim(line);
		unescape(eq + 1);

		if (strcmp(section, "General") == 0 && strcmp(line, "Version") == 0)
			copy_str(r->version, sizeof(r->version), eq + 1);
		else if (strcmp(section, "Rig") == 0)
			rig_key(r, line, eq + 1);
	}

	fclose(fp);

	if (r->manufacturer != 0) {
		set_err(err, err_size, "%s: not an Icom rig file (line %ld)", path, lineno);
		return -1;
	}
	if (r->civ_address < 0) {
		set_err(err, err_size, "%s: no CIVAddress (line %ld)", path, lineno);
		return -1;
	}

	return 0;
}

//
// CIVAddress of an Icom rig file, -1 when absent or not Icom
//

static int peek_civ(const char *path)
{
	FILE *fp = fopen(path, "r");
	char line[C_LINE_MAX];
	int civ = -1;
	int manuf = 0;

	if (fp == NULL)
		return -1;

	// Manufacturer defaults to Icom (0) when absent, as wfview
	while (fgets(line, sizeof(line), fp) != NULL) {
		if (strncmp(line, "CIVAddress=", 11) == 0)
			civ = to_int(line + 11);
		else if (strncmp(line, "Manufacturer=", 13) == 0)
			manuf = to_int(line + 13);
	}

	fclose(fp);
	return manuf == 0 ? civ : -1;
}

//
// Find and load the .rig file in dir whose CIVAddress matches
//

int rigfile_find(struct rig_def *r, const char *dir, int civ_address, char *err, size_t err_size)
{
	DIR *d = opendir(dir);
	struct dirent *e;
	char path[512];
	int found = 0;

	if (d == NULL) {
		set_err(err, err_size, "%s: cannot open rig directory (%ld)", dir, (long)errno);
		return -1;
	}

	while ((e = readdir(d)) != NULL) {
		const size_t n = strlen(e->d_name);

		if (n < 5 || strcmp(e->d_name + n - 4, ".rig") != 0)
			continue;
		snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
		if (peek_civ(path) == civ_address) {
			found = 1;
			break;
		}
	}
	closedir(d);

	if (!found) {
		if (err != NULL && err_size > 0)
			snprintf(err, err_size, "unknown model 0x%02x: no rig file", (unsigned int)civ_address);
		return -1;
	}

	return rigfile_load(r, path, err, err_size);
}

const struct rig_cmd *rigfile_cmd(const struct rig_def *r, const char *type)
{
	size_t i;

	for (i = 0; i < r->n_cmds; i++) {
		if (strcmp(r->cmds[i].type, type) == 0)
			return &r->cmds[i];
	}

	return NULL;
}
