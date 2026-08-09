#pragma once

#define POSIX_C_SOURCE 199309L
#define _GNU_SOURCE //Дополнительные функции

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
#include <readline/readline.h> //ввод строки, стрелочки по командам
#include <readline/history.h>  //хранит список введеных ранее команд
#include <fcntl.h>
#include <time.h>

#ifdef __APPLE__
#include <mach-o/dyld.h> //_NSGetExecutablePath - аналог /proc/self/exe на macOS
#endif

#define BG_BLOCK_MS 300
