// SPDX-License-Identifier: GPL-3.0-or-later
//
// Username / password encoding for the login and stream request packets.
//

#pragma once

#include <stddef.h>
#include <stdint.h>

#define PASSCODE_MAX	16

//
// Encoding
//

size_t passcode_encode(const char *in, uint8_t out[PASSCODE_MAX]);
