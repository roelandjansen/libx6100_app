// SPDX-License-Identifier: GPL-3.0-or-later
//
// X6100 entry point, build check only
//

#include <stdio.h>
#include <stdlib.h>

#include <gnu/libc-version.h>

int main(void)
{
	printf("libx6100_app x6100, glibc %s\n", gnu_get_libc_version());

	return EXIT_SUCCESS;
}
