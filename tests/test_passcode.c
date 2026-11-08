// SPDX-License-Identifier: GPL-3.0-or-later
//
// Host test: passcode encoding against hand-computed table lookups
//

#include <stdio.h>
#include <stdlib.h>

#include "core/passcode.h"

static int G_FAILS;

#define CHECK(cond) do { \
	if (!(cond)) { \
		fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
		G_FAILS++; \
	} \
} while (0)

int main(void)
{
	uint8_t out[PASSCODE_MAX];
	size_t n;

	// 'a' (97) at 0 -> table[97] = 0x38; 'b' (98) at 1 -> table[99] = 0x2e
	n = passcode_encode("ab", out);
	CHECK(n == 2);
	CHECK(out[0] == 0x38);
	CHECK(out[1] == 0x2e);
	CHECK(out[2] == 0x00);

	// '~' (126) at 0 -> table[126] = 0x52; '~' at 1 -> 127 > 126 -> 32 + 0 -> table[32] = 0x47
	n = passcode_encode("~~", out);
	CHECK(n == 2);
	CHECK(out[0] == 0x52);
	CHECK(out[1] == 0x47);

	// Truncated at 16
	n = passcode_encode("abcdefghijklmnopqrstuvwxyz", out);
	CHECK(n == PASSCODE_MAX);

	// NULL and empty
	CHECK(passcode_encode(NULL, out) == 0);
	CHECK(passcode_encode("", out) == 0);
	CHECK(out[0] == 0x00);

	if (G_FAILS != 0) {
		fprintf(stderr, "test_passcode: %d failure(s)\n", G_FAILS);
		return EXIT_FAILURE;
	}

	return EXIT_SUCCESS;
}
