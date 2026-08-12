#pragma once
#include <sys/types.h>
#include <stdbool.h>
#include "node.h"

//применить один редирект к файловым дескрипторам текущего процесса
void do_one_redir(Redir* r);

//применить весь список редиректов узла
int do_redirs(Redir* redir_list);

//выполнить простую команду (builtin или fork+exec)
int run_command(Node* node);

//выполнить конвейер (NODE_PIPE)
int run_pipeline(Node* node, bool flag_bg, char* cmdline, pid_t* out_pgid);

//выполнить сабшелл (NODE_MINISHELL)
int run_minishell(Node* node, bool flag_bg, char* cmdline, pid_t* out_pgid);

//главная функция обхода дерева
int run_node(Node* node, char* cmdline);
