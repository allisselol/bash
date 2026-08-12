#include "exec.h"
#include "builtins.h"
#include "jobs.h"
#include "common.h" // BG_BLOCK_MS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <sys/wait.h>

void do_one_redir(Redir* r){
    int fd; //tmp дескриптор

    // <
    if(r->type == R_IN){
        fd = open(r->target, O_RDONLY);
        if(fd < 0){
            perror(r->target);
            _exit(1);  //немедленно завершает текущий процесс, без очистки буфера и прочего
        }
        if(r->src <= 0){
            if(dup2(fd, 0) < 0){
                perror("dup2");
                _exit(1);
            }
        } else {
            if(dup2(fd, r->src) < 0){
                perror("dup2");
                _exit(1); //0 успешное завершение, 1 - неудача // exit() сохраняет программу с сохранением буферов, а _exit() все сносит
            }
        }
        close(fd);
    }

    // >
    else if(r->type == R_OUT){
        fd = open(r->target, O_WRONLY | O_CREAT | O_TRUNC, 0644);

        if(fd < 0){
            perror(r->target);
            _exit(1);
        }
        if(r->src <= 1){
            if(dup2(fd, 1) < 0){
                perror("dup2");
                _exit(1);
            }
        } else {
            if(dup2(fd, r->src) < 0){
                perror("dup2");
                _exit(1);
            }
        }
        close(fd);
    }

    // >>
    else if(r->type == R_APPEND){
        fd = open(r->target, O_WRONLY | O_CREAT | O_APPEND, 0644);

        if(fd < 0){
            perror(r->target);
            _exit(1);
        }
        if(r->src <= 1){
            if(dup2(fd, 1) < 0){
                perror("dup2");
                _exit(1);
            }
        } else {
            if(dup2(fd, r->src) < 0){
                perror("dup2");
                _exit(1);
            }
        }
        close(fd);
    }

    // &>
    else if(r->type == R_ALL_TO_FILE){
        int fd2 = open(r->target, O_WRONLY | O_CREAT | O_TRUNC, 0644);

        if(fd2 < 0){
            perror(r->target);
            _exit(1);
        }

        if(dup2(fd2, STDOUT_FILENO) < 0){
            perror("dup2");
            _exit(1);
        }

        if(dup2(fd2, STDERR_FILENO) < 0){
            perror("dup2");
            _exit(1);
        }

        close(fd2);
    }

    //дублирование вывода 2>&1 перенаправь std_err туда же куда и std_out
    else if(r->type == R_DUP_OUT){
        int dest_fd;
        if(r->src <= 1) dest_fd = 1;
        else dest_fd = r->src;

        if(dup2(r->dup_target, dest_fd) < 0){
            perror("dup2");
            _exit(1);
        }
    }

    //Дублирование ввода <&N
    else if(r->type == R_DUP_IN){
        int dest_fd;
        if(r->src <= 0) dest_fd = 0;
        else dest_fd = r->src;

        if(dup2(r->dup_target, dest_fd) < 0){
            perror("dup2");
            _exit(1);
        }
    }

    // <<EOF || // <<< "string smth"
    else if(r->type == R_HEREDOC || r->type == R_HERESTR){

        if(r->src > 0){
            fprintf(stderr, "Ввод только в stdin\n");
            _exit(1);
        }

        int p[2];
        if(pipe(p) < 0){
            perror("pipe");
            _exit(1); //_exit(1) - чтобы родительские буферы случайно не трогать
        }

        size_t len = strlen(r->target);

        if(len > 0){
            write(p[1], r->target, len);
        }
        close(p[1]);

        //в какой файловый дескриптор нужно подменить ввод

        if(dup2(p[0], STDIN_FILENO) < 0){
            perror("dup2");
            _exit(1);
        }

        close(p[0]);
    }

    else {
        fprintf(stderr, "Неизвестный тип перенаправления\n");
    }
}

int do_redirs(Redir* redir_list){
    for(Redir* r = redir_list; r != NULL; r = r->next){
        do_one_redir(r);
    }
    return 0;
}

int run_command(Node* node){
    if(node->argv == NULL || node->argv[0] == NULL){
        return 0; //если в узле банально аргументов нет, но как бы вышли без ошибок
    }

    if(is_mybuilt(node) && node->redirs == NULL){
        return run_mybuilt(node);
    }

    fflush(NULL); // сбрасываем буферы ДО fork(), иначе child унаследует непропечатанный
                   // буфер родителя и может вывести его повторно
    pid_t pid = fork();
    if(pid < 0){
        perror("fork");
        return 1; //вышли с ошибкой
    }

    if(pid == 0){
        setpgid(0, 0);
        signal(SIGINT, SIG_DFL);
        signal(SIGTSTP, SIG_DFL);
        signal(SIGQUIT, SIG_DFL);
        signal(SIGTTIN, SIG_DFL); //cat &
        signal(SIGTTOU, SIG_DFL);

        if(node->redirs) do_redirs(node->redirs);

        if(is_mybuilt(node)){
            int built_in_command = run_mybuilt(node);
            fflush(NULL); // критично: _exit() не сбрасывает буферы stdio, а stdout сейчас
                           // может указывать на файл (после do_redirs), а не на терминал
            _exit(built_in_command);
        }

        execvp(node->argv[0], node->argv);

        perror("execvp");
        _exit(127); //команда не найдена
    } else {

        setpgid(pid, pid);
        if(isatty(STDIN_FILENO)){
            tcsetpgrp(STDIN_FILENO, pid); //фоновые процессы не забирают терминал
        }

        int status = 0; //определяем на тот случай, если waitpid вышел с ошибкой, он не напишет ничего в status
        if(waitpid(pid, &status, WUNTRACED) < 0){
            perror("waitpid");
            tcsetpgrp(STDIN_FILENO, shell_pgid);
            tcsetattr(STDIN_FILENO, TCSADRAIN, &shell_settings);
            return 1;
        }

        tcsetpgrp(STDIN_FILENO, shell_pgid);
        tcsetattr(STDIN_FILENO, TCSADRAIN, &shell_settings);

        if(WIFSTOPPED(status)){
            Job* j = jobs_add(pid, node->argv[0]);
            if(j) j->status = J_STOPPED;
            return 128 + SIGTSTP;
        }

        if(WIFEXITED(status)) return(WEXITSTATUS(status));
        if(WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    }
    return 0;
}

//реализуем наш конвейер
int run_pipeline(Node* node, bool flag_bg, char* cmdline, pid_t* out_pgid){
    int p[2];
    pid_t left_pid, right_pid;

    if(pipe(p) < 0){
        perror("pipe");
        return 1;
    }
    //для левой части дерева рекурсивно
    fflush(NULL);
    left_pid = fork();
    if(left_pid < 0){
        perror("fork");
        close(p[0]);
        close(p[1]);
        return 1;
    }
    if(left_pid == 0){
        setpgid(0, 0);
        if(!flag_bg) tcsetpgrp(STDIN_FILENO, getpid());
        signal(SIGINT, SIG_DFL);
        signal(SIGTSTP, SIG_DFL);
        dup2(p[1], STDOUT_FILENO);
        close(p[0]); close(p[1]);
        _exit(run_node(node->left, cmdline));
    }
    setpgid(left_pid, left_pid);
    pid_t pg = left_pid;

    //для правой части дерева рекурсивно
    fflush(NULL);
    right_pid = fork();
    if(right_pid < 0){
        perror("fork");
        close(p[0]);
        close(p[1]);
        return 1;
    }
    if(right_pid == 0){
        setpgid(0, pg);  //говорим становится главным в группе процессов
        if(!flag_bg) tcsetpgrp(STDIN_FILENO, pg);
        signal(SIGINT, SIG_DFL);
        signal(SIGTSTP, SIG_DFL);
        dup2(p[0], STDIN_FILENO);
        close(p[0]); close(p[1]);
        _exit(run_node(node->right, cmdline));
    }
    setpgid(right_pid, pg);

    close(p[0]); close(p[1]);
    if(out_pgid) *out_pgid = pg;

    if(flag_bg){
        jobs_add(pg, cmdline);
        printf("[%d] %d\n", jobs.vector[jobs.count -1].id, pg);
        return 0;
    } else {
        int status1 = 0, status2 = 0;
        if(waitpid(left_pid, &status1, 0) < 0){
            perror("waitpid");
            return 1;
        }

        if(waitpid(right_pid, &status2, 0) < 0){
            perror("waitpid");
            return 1;
        }

        if(WIFEXITED(status2)) return(WEXITSTATUS(status2));
        if(WIFSIGNALED(status2)) return 128 + WTERMSIG(status2);
    }
    return 0;
}

int run_minishell(Node* node, bool flag_bg, char* cmdline, pid_t* out_pgid){
    fflush(NULL);
    pid_t pid = fork();
    if(pid < 0){
        perror("fork");
        return 1;
    }
    if(pid == 0){
        setpgid(0, 0);
        pid_t pg = getpid();
        if(!flag_bg) tcsetpgrp(STDIN_FILENO, pg);
        signal(SIGINT, SIG_DFL); signal(SIGTSTP, SIG_DFL);
        if(node->redirs) do_redirs(node->redirs);
        _exit(run_node(node->left, cmdline));
    } else {
        setpgid(pid, pid);
        if(out_pgid) *out_pgid = pid;
        if(flag_bg){
            jobs_add(pid, cmdline);
            printf("[%d] %d\n", jobs.vector[jobs.count -1].id, pid);
            return 0;
        } else {
            int status = 0;
            if(waitpid(pid, &status, 0) < 0){
                perror("waitpid");
                return 1;
            }

            if(WIFEXITED(status)) return(WEXITSTATUS(status));
            if(WIFSIGNALED(status)) return 128 + WTERMSIG(status);
        }
    }
    return 0;
}

//самая главная функция в нашем коде, именно ради нее и было все написано
int run_node(Node* node, char* cmdline){
    if(!node) return 0;

    if(node->type == NODE_CMD) return run_command(node);

    else if(node->type == NODE_PIPE) return run_pipeline(node, false, cmdline, NULL);

    else if(node->type == NODE_AND){
        int status = run_node(node->left, cmdline);
        if(status == 0) status = run_node(node->right, cmdline);
        return status;
    }

    else if(node->type == NODE_OR){
        int status = run_node(node->left, cmdline);
        if(status != 0) status = run_node(node->right, cmdline);
        return status;
    }

    else if(node->type == NODE_SEQ){
        run_node(node->left, cmdline);
        return run_node(node->right, cmdline);
    }

    else if(node->type == NODE_BG){
        int tmp = 0; pid_t pgid = -1;

        if(node->left->type == NODE_PIPE){
            tmp = run_pipeline(node->left, true, cmdline, &pgid);

            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            add_ms(now, BG_BLOCK_MS, &bg_until);
        }

        else if(node->left->type == NODE_MINISHELL){
            tmp = run_minishell(node->left, true, cmdline, &pgid);

            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            add_ms(now, BG_BLOCK_MS, &bg_until);
        }

        else if(node->left->type == NODE_CMD){
            fflush(NULL);
            pid_t pid = fork();

            if(pid < 0){
                perror("fork");
                return 1;  //банально ошибка создания процесса
            }

            else if(pid == 0){
                setpgid(0, 0);
                signal(SIGINT, SIG_DFL);
                signal(SIGTSTP, SIG_DFL);
                signal(SIGQUIT, SIG_DFL);

                Node* cmd = node->left;

                if(cmd->redirs) do_redirs(cmd->redirs);

                if(is_mybuilt(cmd)){
                    int built_command = run_mybuilt(cmd);
                    fflush(NULL); // та же причина: без сброса буферов вывод в редирект теряется
                    _exit(built_command);
                }

                execvp(cmd->argv[0], cmd->argv);
                perror("execvp");
                _exit(127);
            }
            else {
                setpgid(pid, pid);
                jobs_add(pid, cmdline);
                printf("[%d] %d\n", jobs.vector[jobs.count -1].id, pid);

                struct timespec now;
                clock_gettime(CLOCK_MONOTONIC, &now);
                add_ms(now, BG_BLOCK_MS, &bg_until);
            }
        }

        else tmp = run_minishell(node->left, true, cmdline, &pgid);

        (void)tmp;
        if(node->right) return run_node(node->right, cmdline);
        return 0;
    }

    else if(node->type == NODE_MINISHELL) return run_minishell(node, false, cmdline, NULL);

    else {
        return 1; //неизвестный тип
    }
}
