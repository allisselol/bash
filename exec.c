#include "common.h" // _POSIX_C_SOURCE 200809L должен быть определён ДО системных заголовков
#include "exec.h"
#include "builtins.h"
#include "jobs.h"
#include "memory.h" // er_malloc (используется в run_builtin_with_redirs)
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

//Классификация ошибки после неудачного execvp, по ТЗ:
//- команда не найдена (ENOENT/ENOTDIR - не найден сам файл или компонент пути) -> 127
//- команда найдена, но не может быть выполнена (нет прав, это каталог,
//  неверный формат исполняемого файла и т.п.) -> 126
//Раньше здесь всегда безусловно возвращался 127, что не различало эти два случая.
static int exec_fail_status(const char* name){
    int code = (errno == ENOENT || errno == ENOTDIR) ? 127 : 126;
    fprintf(stderr, "mysh: %s: %s\n", name, strerror(errno));
    return code;
}

//Общее ядро применения одного редиректа. Раньше это было прямо в do_one_redir
//и при любой ошибке безусловно звало _exit(1) - это годится для форкнутого
//потомка (внешняя команда), но НЕ годится для builtin, который по ТЗ должен
//выполняться в самом процессе интерпретатора: там ошибка открытия файла должна
//не убивать весь шелл, а просто отменить выполнение команды с кодом 1.
//fatal=true  - поведение как раньше (для внешних команд в дочернем процессе)
//fatal=false - печатает диагностику и возвращает false вместо _exit()
static bool apply_redir(Redir* r, bool fatal){
    int fd;

    // <
    if(r->type == R_IN){
        fd = open(r->target, O_RDONLY);
        if(fd < 0){
            if(fatal){ perror(r->target); _exit(1); }
            fprintf(stderr, "mysh: %s: %s\n", r->target, strerror(errno));
            return false;
        }
        int dest = (r->src <= 0) ? 0 : r->src;
        if(dup2(fd, dest) < 0){
            close(fd);
            if(fatal){ perror("dup2"); _exit(1); }
            fprintf(stderr, "mysh: dup2: %s\n", strerror(errno));
            return false;
        }
        close(fd);
    }

    // > и >> (права 0666 с учётом umask процесса, как требует ТЗ - раньше
    // было жёстко захардкожено 0644, что игнорировало umask)
    else if(r->type == R_OUT || r->type == R_APPEND){
        int flags = O_WRONLY | O_CREAT | (r->type == R_APPEND ? O_APPEND : O_TRUNC);
        fd = open(r->target, flags, 0666);
        if(fd < 0){
            if(fatal){ perror(r->target); _exit(1); }
            fprintf(stderr, "mysh: %s: %s\n", r->target, strerror(errno));
            return false;
        }
        int dest = (r->src <= 1) ? 1 : r->src;
        if(dup2(fd, dest) < 0){
            close(fd);
            if(fatal){ perror("dup2"); _exit(1); }
            fprintf(stderr, "mysh: dup2: %s\n", strerror(errno));
            return false;
        }
        close(fd);
    }

    // &>
    else if(r->type == R_ALL_TO_FILE){
        fd = open(r->target, O_WRONLY | O_CREAT | O_TRUNC, 0666);
        if(fd < 0){
            if(fatal){ perror(r->target); _exit(1); }
            fprintf(stderr, "mysh: %s: %s\n", r->target, strerror(errno));
            return false;
        }
        if(dup2(fd, STDOUT_FILENO) < 0 || dup2(fd, STDERR_FILENO) < 0){
            close(fd);
            if(fatal){ perror("dup2"); _exit(1); }
            fprintf(stderr, "mysh: dup2: %s\n", strerror(errno));
            return false;
        }
        close(fd);
    }

    //дублирование вывода 2>&1 перенаправь std_err туда же куда и std_out
    else if(r->type == R_DUP_OUT){
        int dest = (r->src <= 1) ? 1 : r->src;
        if(dup2(r->dup_target, dest) < 0){
            if(fatal){ perror("dup2"); _exit(1); }
            fprintf(stderr, "mysh: dup2: %s\n", strerror(errno));
            return false;
        }
    }

    //Дублирование ввода <&N
    else if(r->type == R_DUP_IN){
        int dest = (r->src <= 0) ? 0 : r->src;
        if(dup2(r->dup_target, dest) < 0){
            if(fatal){ perror("dup2"); _exit(1); }
            fprintf(stderr, "mysh: dup2: %s\n", strerror(errno));
            return false;
        }
    }

    // <<EOF || // <<< "string smth"
    else if(r->type == R_HEREDOC || r->type == R_HERESTR){
        if(r->src > 0){
            if(fatal){ fprintf(stderr, "Ввод только в stdin\n"); _exit(1); }
            fprintf(stderr, "mysh: ввод только в stdin\n");
            return false;
        }

        int p[2];
        if(pipe(p) < 0){
            if(fatal){ perror("pipe"); _exit(1); }
            fprintf(stderr, "mysh: pipe: %s\n", strerror(errno));
            return false;
        }

        size_t len = strlen(r->target);
        if(len > 0) write(p[1], r->target, len);
        close(p[1]);

        if(dup2(p[0], STDIN_FILENO) < 0){
            close(p[0]);
            if(fatal){ perror("dup2"); _exit(1); }
            fprintf(stderr, "mysh: dup2: %s\n", strerror(errno));
            return false;
        }
        close(p[0]);
    }

    else {
        if(fatal){
            fprintf(stderr, "Неизвестный тип перенаправления\n");
        } else {
            fprintf(stderr, "mysh: неизвестный тип перенаправления\n");
            return false;
        }
    }

    return true;
}

void do_one_redir(Redir* r){
    apply_redir(r, true);
}

//"безопасная" версия для builtin без fork (см. run_builtin_with_redirs):
//не завершает процесс интерпретатора при ошибке, а по ТЗ возвращает
//диагностику и false, чтобы вызывающий код мог отменить команду с кодом 1
bool do_one_redir_safe(Redir* r){
    return apply_redir(r, false);
}

int do_redirs(Redir* redir_list){
    for(Redir* r = redir_list; r != NULL; r = r->next){
        do_one_redir(r);
    }
    return 0;
}

//вычисляет, какой именно файловый дескриптор процесса подменит данный
//редирект (та же логика выбора dest, что и внутри apply_redir)
static int redir_target_fd(Redir* r){
    switch(r->type){
        case R_IN:          return (r->src <= 0) ? 0 : r->src;
        case R_OUT:
        case R_APPEND:      return (r->src <= 1) ? 1 : r->src;
        case R_DUP_OUT:     return (r->src <= 1) ? 1 : r->src;
        case R_DUP_IN:      return (r->src <= 0) ? 0 : r->src;
        case R_HEREDOC:
        case R_HERESTR:     return 0;
        case R_ALL_TO_FILE: return -1; //особый случай - трогает сразу 1 и 2, обрабатывается отдельно
    }
    return -1;
}

//Выполнить builtin С редиректами БЕЗ fork - именно так требует ТЗ: "для
//встроенной команды, выполняемой без конвейера, перенаправления применяются
//в самом процессе интерпретатора: исходные дескрипторы сохраняются вызовом
//dup, а после выполнения восстанавливаются". Раньше в этом случае код форкал
//дочерний процесс - из-за этого, например, `cd /tmp > log.txt` реально менял
//каталog только у уже завершившегося потомка, а сам шелл оставался на месте.
static int run_builtin_with_redirs(Node* node){
    int count = 0;
    for(Redir* r = node->redirs; r; r = r->next){
        count += (r->type == R_ALL_TO_FILE) ? 2 : 1;
    }

    int* saved_fd = er_malloc((size_t)count * sizeof(int));
    int* orig_fd  = er_malloc((size_t)count * sizeof(int));
    int nsaved = 0;

    //сохраняем исходные дескрипторы через dup ДО применения редиректов
    for(Redir* r = node->redirs; r; r = r->next){
        if(r->type == R_ALL_TO_FILE){
            int fds[2] = {STDOUT_FILENO, STDERR_FILENO};
            for(int i = 0; i < 2; i++){
                orig_fd[nsaved]  = fds[i];
                saved_fd[nsaved] = dup(fds[i]);
                nsaved++;
            }
        } else {
            int fd = redir_target_fd(r);
            orig_fd[nsaved]  = fd;
            saved_fd[nsaved] = dup(fd);
            nsaved++;
        }
    }

    //применяем сами редиректы; при первой же ошибке открытия файла -
    //по ТЗ команда отменяется целиком, код возврата 1
    bool ok = true;
    for(Redir* r = node->redirs; r; r = r->next){
        if(!do_one_redir_safe(r)){
            ok = false;
            break;
        }
    }

    int status = ok ? run_mybuilt(node) : 1;

    fflush(NULL); //критично: сбрасываем буферы ДО восстановления исходных
                  //дескрипторов - иначе буферизованный вывод builtin'а рискует
                  //попасть не в перенаправленный файл, а в терминал (при следующем
                  //флаше уже после dup2 назад) или вовсе потеряться

    //восстанавливаем исходные дескрипторы в обратном порядке
    for(int i = nsaved - 1; i >= 0; i--){
        dup2(saved_fd[i], orig_fd[i]);
        close(saved_fd[i]);
    }
    free(saved_fd);
    free(orig_fd);

    return status;
}

int run_command(Node* node){
    if(node->argv == NULL || node->argv[0] == NULL){
        return 0; //если в узле банально аргументов нет, но как бы вышли без ошибок
    }

    if(is_mybuilt(node)){
        //builtin без конвейера всегда выполняется в самом процессе интерпретатора -
        //с редиректами (через dup, см. run_builtin_with_redirs) или без них
        if(node->redirs == NULL){
            return run_mybuilt(node);
        }
        return run_builtin_with_redirs(node);
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

        execvp(node->argv[0], node->argv);
        _exit(exec_fail_status(node->argv[0]));
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
                _exit(exec_fail_status(cmd->argv[0]));
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
