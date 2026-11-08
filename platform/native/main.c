// SPDX-License-Identifier: GPL-3.0-or-later
//
// Native (SDL) entry point, build check only
//

#include <stdio.h>
#include <stdlib.h>

#include <SDL.h>

int main(int argc, char *argv[])
{
	(void)argc;
	(void)argv;

	SDL_version ver;

	SDL_GetVersion(&ver);
	printf("libx6100_app native, SDL %u.%u.%u\n", ver.major, ver.minor, ver.patch);

	return EXIT_SUCCESS;
}
