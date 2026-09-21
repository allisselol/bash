#pragma once

#define _POSIX_C_SOURCE 200809L //POSIX.1-2008: даёт getline, strdup, kill и т.д.

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <stdbool.h>
#include <ctype.h>
#include <pwd.h>
#include <termios.h>
#include <signal.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <time.h>

#ifdef __APPLE__
#include <mach-o/dyld.h> //_NSGetExecutablePath - аналог /proc/self/exe на macOS
#endif

#define BG_BLOCK_MS 300