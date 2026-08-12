#pragma once
#include "node.h"

int my_cd(char** argv);
int my_pwd(void);
int my_export(char** argv);
int my_unset(char** argv);
int my_echo(char** argv);
int my_jobs(void);
int my_fg(char** argv);
int my_bg(char** argv);

//является ли узел встроенной командой
int is_mybuilt(Node* node);

//выполнить встроенную команду (предполагается, что is_mybuilt(node) == true)
int run_mybuilt(Node* node);
