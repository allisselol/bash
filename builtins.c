#include "common.h" // _POSIX_C_SOURCE 200809L должен быть определён ДО системных заголовков
#include "builtins.h"
#include "jobs.h"
#include "memory.h"
#include "utils.h"
#include "lexer.h" // tilda_koren
#include "token.h" // last_exit_status
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/wait.h>

int my_cd(char** argv){
    char* old = gettingthiscwd();
    char* arg = argv[1];

    if(!arg || strcmp(arg, "~") == 0){
        char* home = getenv("HOME");
        if(!home) home = "/";
        if(chdir(home) != 0){
            perror("cd");
            free(old);
            return 1;
        }
 }
    else if(strcmp(arg, "-") == 0){
        char* previous = getenv("OLDPWD");
        if(!previous){
            fprintf(stderr, "Прошлый путь не был получен\n");
            free(old);
            return 1;
        }
        if(chdir(previous) != 0){
            perror("cd");
            free(old);
            return 1;
        }
    } else {
        if(arg[0] == '~'){
            char* path = tilda_koren(arg);
            if(chdir(path) != 0){
                perror("cd");
                free(path);
                free(old);
                return 1;
            }
            free(path);
        } else {
            if(chdir(arg) != 0){
                perror("cd");
                free(old);
                return 1;
            }
        }
    }
    if(old) setenv("OLDPWD", old, 1);
    char* cwd = gettingthiscwd();
    if(cwd){
        setenv("PWD", cwd, 1);
        free(cwd);
    }
    free(old);
    return 0;
}
int my_pwd(void){
    char* cwd = gettingthiscwd();
    puts(cwd);
    free(cwd);
    return 0;
}
int my_export(char** argv){
    for(int i = 1; argv[i] != NULL; i++){
        char* equal = strchr(argv[i], '='); //возвращает адрес памяти
        if(!equal){
            fprintf(stderr, "Неправильный [export] прописан!\n");
            return 1;
        }
        *equal = '\0';
        if(setenv(argv[i], equal + 1, 1) != 0){ //в окружении текущего процесса появится переменная
            perror("export");
            *equal = '=';
            return 1;
        }
        *equal = '=';
    }
    return 0;
}

int my_unset(char** argv){
    for(int i = 1; argv[i] != NULL; i++){
        if(unsetenv(argv[i]) != 0){
            perror("unset");
            return 1;
        }
    }
    return 0;
}
int my_echo(char** argv){
    int i = 1, newline = 1;
    if(argv[1] && strcmp(argv[1], "-n") == 0){
        newline = 0; i = 2;
    }
    for(; argv[i]; i++){
        if(newline){
            if(i > 1){
                fputc(' ', stdout);
            }
        } else {
            if(i > 2){
                fputc(' ', stdout);
            }
        }
        fputs(argv[i], stdout);
    }

    if(newline){
        fputc('\n', stdout);
    }

    return 0;
}

int my_jobs(void){
    get_children();
    jobs_print();
    jobs_remove();
    return 0;
}

int my_fg(char** argv){
    if(!shell_interactive){
        //по ТЗ: в неинтерактивном режиме управление заданиями отключено
        fprintf(stderr, "mysh: fg: управление заданиями отключено (неинтерактивный режим)\n");
        return 1;
    }
    get_children(); //чистим зомби, сообщаем родителям(ю)
    Job* job = NULL;
    if(argv[1]){
        //поддержка %n, %+, %- (не только %n, как было раньше)
        job = resolve_job_spec(argv[1]);
        if(!job){
            fprintf(stderr, "mysh: fg: %s: нет такого задания\n", argv[1]);
            return 1;
        }
    } else {
        if(jobs.count == 0){
            fprintf(stderr, "У нас нет ни одного jobs\n");
            return 1;
        }

        job = jobs_by_id(current_job_id); //по ТЗ: без аргумента - текущее задание
        if(!job) job = &jobs.vector[jobs.count - 1]; //подстраховка, если маркер не выставлен
    }

    if(job->status == J_DONE){
        fprintf(stderr, "Задание уже завершено\n");
        return 1;
    }

    printf("%s\n", job->cmdline);

    int to_fg = put_job_fg(job, 1); //продолжить выполнение если было остановлено
    return to_fg;
}

int my_bg(char** argv){
    if(!shell_interactive){
        //по ТЗ: в неинтерактивном режиме управление заданиями отключено
        fprintf(stderr, "mysh: bg: управление заданиями отключено (неинтерактивный режим)\n");
        return 1;
    }
    get_children(); //чистим зомби, сообщаем родителям(ю)
    Job* job = NULL;
    if(argv[1]){
        //поддержка %n, %+, %- (не только %n, как было раньше)
        job = resolve_job_spec(argv[1]);
        if(!job){
            fprintf(stderr, "mysh: bg: %s: нет такого задания\n", argv[1]);
            return 1;
        }
    } else {
        if(jobs.count == 0){
            fprintf(stderr, "У нас нет ни одного jobs\n");
            return 1;
        }

        job = jobs_by_id(current_job_id); //по ТЗ: без аргумента - текущее задание
        if(!job) job = &jobs.vector[jobs.count - 1]; //подстраховка, если маркер не выставлен
    }

    if(job->status == J_RUNNING){
        printf("[%d] уже работает\n", job->id);
        return 1;
    }

    job->status = J_RUNNING;
    printf("%s\n", job->cmdline);
    put_job_bg(job, 1);

    return 0;
}

//exit: завершает интерпретатор.
//- без аргумента использует код возврата последней команды ($?/last_exit_status);
//- с аргументом - переданное число, приведённое к диапазону 0..255;
//- при наличии остановленных заданий первая попытка выхода только предупреждает
//  и не завершает интерпретатор; повторный exit завершает его безусловно.
int my_exit(char** argv){
    static bool exit_warned = false; //сохраняется между вызовами exit в течение сессии

    get_children(); //подтягиваем актуальные статусы заданий перед проверкой
    bool has_stopped = false;
    for(int i = 0; i < jobs.count; i++){
        if(jobs.vector[i].status == J_STOPPED){
            has_stopped = true;
            break;
        }
    }

    if(has_stopped){
        if(!exit_warned){
            fprintf(stderr, "mysh: есть остановленные задания (выполните exit ещё раз для выхода)\n");
            exit_warned = true;
            return 1; //не завершаем интерпретатор
        }
        //exit_warned уже true - пользователь подтвердил выход повторным exit
    } else {
        exit_warned = false; //остановленных заданий больше нет - сбрасываем на будущее
    }

    int code;
    if(argv[1]){
        code = atoi(argv[1]);
    } else {
        code = last_exit_status;
    }
    code = ((code % 256) + 256) % 256; //приводим к диапазону 0..255 (в т.ч. для отрицательных чисел)

    exit(code);
}

int my_help(void){
    printf("Встроенные команды mysh:\n");
    printf("  cd [каталог]              - сменить текущий каталог\n");
    printf("  pwd                       - показать текущий каталог\n");
    printf("  echo [-n] [аргументы]     - вывести аргументы\n");
    printf("  export ИМЯ=значение       - установить переменную окружения\n");
    printf("  unset ИМЯ                 - удалить переменную окружения\n");
    printf("  jobs                      - список заданий\n");
    printf("  fg [%%n]                   - перевести задание на передний план\n");
    printf("  bg [%%n]                   - продолжить остановленное задание в фоне\n");
    printf("  kill [-s СИГНАЛ] цель ... - отправить сигнал процессу/заданию\n");
    printf("  wait [pid|%%n]             - дождаться завершения задания(й)\n");
    printf("  exit [код]                - завершить работу интерпретатора\n");
    printf("  help                      - эта справка\n");
    return 0;
}

//таблица имён сигналов - без 'SIG' в начале, само числовое значение берём из
//<signal.h> платформы (а не хардкодим), поэтому это переносимо между Linux/macOS,
//где номера некоторых сигналов (SIGCHLD, SIGCONT и т.п.) физически различаются
static const struct { const char* name; int sig; } signal_table[] = {
    {"HUP", SIGHUP}, {"INT", SIGINT}, {"QUIT", SIGQUIT}, {"ILL", SIGILL},
    {"TRAP", SIGTRAP}, {"ABRT", SIGABRT}, {"FPE", SIGFPE}, {"KILL", SIGKILL},
    {"USR1", SIGUSR1}, {"SEGV", SIGSEGV}, {"USR2", SIGUSR2}, {"PIPE", SIGPIPE},
    {"ALRM", SIGALRM}, {"TERM", SIGTERM}, {"CHLD", SIGCHLD}, {"CONT", SIGCONT},
    {"STOP", SIGSTOP}, {"TSTP", SIGTSTP}, {"TTIN", SIGTTIN}, {"TTOU", SIGTTOU},
};

//разобрать имя/номер сигнала ("TERM", "SIGTERM", "9" и т.п.). -1, если не распознано
static int parse_signal_name(const char* s){
    if(!s || !*s) return -1;
    if(s[0] == 'S' && s[1] == 'I' && s[2] == 'G') s += 3; //допускаем и "SIGTERM", и просто "TERM"

    for(size_t i = 0; i < sizeof(signal_table)/sizeof(signal_table[0]); i++){
        if(strcmp(signal_table[i].name, s) == 0) return signal_table[i].sig;
    }

    char* end;
    long n = strtol(s, &end, 10);
    if(*s != '\0' && *end == '\0') return (int)n; //это было просто число

    return -1;
}

//kill [-s СИГНАЛ | -СИГНАЛ] цель ...
//Сигнал по умолчанию - SIGTERM. Цель - pid (число) либо спецификатор задания (%n/%+/%-),
//в этом случае сигнал уходит всей группе процессов задания.
int my_kill(char** argv){
    int sig = SIGTERM;
    int i = 1;

    if(argv[i] && strcmp(argv[i], "-s") == 0){
        if(!argv[i+1]){
            fprintf(stderr, "mysh: kill: опция -s требует аргумент\n");
            return 1;
        }
        int parsed = parse_signal_name(argv[i+1]);
        if(parsed < 0){
            fprintf(stderr, "mysh: kill: %s: неизвестный сигнал\n", argv[i+1]);
            return 1;
        }
        sig = parsed;
        i += 2;
    } else if(argv[i] && argv[i][0] == '-' && argv[i][1] != '\0'){
        int parsed = parse_signal_name(argv[i] + 1);
        if(parsed < 0){
            fprintf(stderr, "mysh: kill: %s: неизвестный сигнал\n", argv[i]);
            return 1;
        }
        sig = parsed;
        i += 1;
    }

    if(!argv[i]){
        fprintf(stderr, "mysh: kill: не указана цель\n");
        return 1;
    }

    int status = 0;
    for(; argv[i]; i++){
        if(argv[i][0] == '%'){
            Job* job = resolve_job_spec(argv[i]);
            if(!job){
                fprintf(stderr, "mysh: kill: %s: нет такого задания\n", argv[i]);
                status = 1;
                continue;
            }
            if(kill(-(job->pgid), sig) < 0){ //отрицательный pgid - сигнал всей группе процессов
                fprintf(stderr, "mysh: kill: %s: %s\n", argv[i], strerror(errno));
                status = 1;
            }
        } else {
            char* end;
            long pid = strtol(argv[i], &end, 10);
            if(*argv[i] == '\0' || *end != '\0'){
                fprintf(stderr, "mysh: kill: %s: неверный аргумент\n", argv[i]);
                status = 1;
                continue;
            }
            if(kill((pid_t)pid, sig) < 0){
                fprintf(stderr, "mysh: kill: (%ld): %s\n", pid, strerror(errno));
                status = 1;
            }
        }
    }
    return status;
}

//wait [pid|%n] - без аргументов ждёт завершения ВСЕХ фоновых заданий (код 0);
//с аргументом ждёт конкретное задание и возвращает его код возврата
int my_wait(char** argv){
    if(!argv[1]){
        //ждём, пока не останется ни одного задания в состоянии Running
        while(true){
            bool any_running = false;
            for(int i = 0; i < jobs.count; i++){
                if(jobs.vector[i].status == J_RUNNING){ any_running = true; break; }
            }
            if(!any_running) break;

            int status;
            pid_t pid = waitpid(-1, &status, 0); //блокирующе ждём любого потомка
            if(pid < 0){
                if(errno == EINTR) continue;
                break; //ECHILD и прочее - потомков для ожидания больше нет
            }
            pid_t pgroup = getpgid(pid);
            if(pgroup == -1) pgroup = pid;
            Job* job = jobs_by_pgid(pgroup);
            if(job){
                if(WIFEXITED(status) || WIFSIGNALED(status)) job->status = J_DONE;
                else if(WIFSTOPPED(status)) job->status = J_STOPPED;
            }
        }
        jobs_remove();
        return 0;
    }

    //ждём конкретное задание/pid. Упрощение: ожидание идёт по pid ведущего
    //процесса задания (для одиночных фоновых команд этого достаточно;
    //отдельные процессы конвейера в фоне всё равно подбираются общим
    //обработчиком SIGCHLD/get_children).
    Job* job = NULL;
    pid_t target_pid;

    if(argv[1][0] == '%'){
        job = resolve_job_spec(argv[1]);
        if(!job){
            fprintf(stderr, "mysh: wait: %s: нет такого задания\n", argv[1]);
            return 1;
        }
        target_pid = job->pgid;
    } else {
        char* end;
        long pid = strtol(argv[1], &end, 10);
        if(*argv[1] == '\0' || *end != '\0'){
            fprintf(stderr, "mysh: wait: %s: неверный аргумент\n", argv[1]);
            return 1;
        }
        target_pid = (pid_t)pid;
        job = jobs_by_pgid(target_pid);
    }

    int status = 0;
    while(true){
        pid_t r = waitpid(target_pid, &status, 0);
        if(r < 0){
            if(errno == EINTR) continue;
            if(errno == ECHILD) return 0; //уже завершилось раньше нас - трактуем как успех
            fprintf(stderr, "mysh: wait: %s\n", strerror(errno));
            return 1;
        }
        break;
    }

    if(job) job->status = J_DONE;

    if(WIFEXITED(status)) return WEXITSTATUS(status);
    if(WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return 0;
}

int is_mybuilt(Node* node){
    if(!node || node->type != NODE_CMD || node->argv == NULL || node->argv[0] == NULL) return 0;
    char* name = node->argv[0];

    if(strcmp(name, "cd") == 0) return 1;
    if(strcmp(name, "pwd") == 0) return 1;
    if(strcmp(name, "export") == 0) return 1;
    if(strcmp(name, "exit") == 0) return 1;
    if(strcmp(name, "echo") == 0) return 1;
    if(strcmp(name, "unset") == 0) return 1;
    if(strcmp(name, "jobs") == 0) return 1;
    if(strcmp(name, "fg") == 0) return 1;
    if(strcmp(name, "bg") == 0) return 1;
    if(strcmp(name, "help") == 0) return 1;
    if(strcmp(name, "kill") == 0) return 1;
    if(strcmp(name, "wait") == 0) return 1;

    return 0;
}

int run_mybuilt(Node* node){
    if(!node || node->type != NODE_CMD || node->argv == NULL || node->argv[0] == NULL){
        fprintf(stderr, "Пустая команда или ошибка парсинга\n");
        return 1;
    }

    char* a = node->argv[0];

    if(strcmp(a, "cd") == 0) return my_cd(node->argv);
    if(strcmp(a, "pwd") == 0) return my_pwd();
    if(strcmp(a, "export") == 0) return my_export(node->argv);
    if(strcmp(a, "unset") == 0) return my_unset(node->argv);
    if(strcmp(a, "jobs") == 0) return my_jobs();
    if(strcmp(a, "bg") == 0) return my_bg(node->argv);
    if(strcmp(a, "fg") == 0) return my_fg(node->argv);
    if(strcmp(a, "echo") == 0) return my_echo(node->argv);
    if(strcmp(a, "exit") == 0) return my_exit(node->argv);
    if(strcmp(a, "help") == 0) return my_help();
    if(strcmp(a, "kill") == 0) return my_kill(node->argv);
    if(strcmp(a, "wait") == 0) return my_wait(node->argv);

    return 1; //команда не builtin, поэтому придется создавать shell'у отдельный процесс внешние команды типа ls -> для работы (exec)
}
