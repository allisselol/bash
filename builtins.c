#include "common.h" 
#include "builtins.h" 
#include "jobs.h" 
#include "memory.h" 
#include "utils.h" 
#include "lexer.h" 
#include "token.h" 
#include <stdio.h> 
#include <stdlib.h> 
#include <string.h> 
#include <unistd.h> 
#include <errno.h> 
#include <sys/wait.h> 

int my_cd(char** argv){
    char* old = gettingthiscwd(); // запоминаем каталог ДО смены - он станет $OLDPWD 
    char* arg = argv[1]; // аргумент команды: куда переходить (может быть NULL, если написали просто "cd")
    if(!arg || strcmp(arg, "~") == 0){ // ветка 1: "cd" без аргумента или "cd ~" - идём в домашний каталог
        char* home = getenv("HOME"); // берём путь домашнего каталога из окружения
        if(!home) home = "/"; // запасной вариант: если HOME не задан, идём в корень
        if(chdir(home) != 0){ // пытаемся сменить каталог; не 0 = ошибка
            perror("cd");
            free(old);
            return 1; 
        } 
     } 
    else if(strcmp(arg, "-") == 0){ // ветка 2: "cd -" - вернуться в предыдущий каталог
        char* previous = getenv("OLDPWD"); // предыдущий каталог хранится в переменной окружения OLDPWD
        if(!previous){ // если OLDPWD ещё ни разу не выставляли
            fprintf(stderr, "Прошлый путь не был получен\n"); // сообщение об ошибке в stderr
            free(old); 
            return 1; 
        } 
        if(chdir(previous) != 0){ // пробуем перейти в предыдущий каталог
            perror("cd"); 
            free(old);
            return 1; 
        } 
    } else { // ветка 3: все остальные случаи - обычный путь или путь, начинающийся с "~"
        if(arg[0] == '~'){ // путь вида "~user/dir" или "~/dir" - нужно раскрыть тильду
            char* path = tilda_koren(arg); 
            if(chdir(path) != 0){ // пробуем перейти по раскрытому пути
                perror("cd");
                free(path); 
                free(old); 
                return 1; 
            }
            free(path);
        } else {
            if(chdir(arg) != 0){ // переходим прямо по тому, что передал пользователь
                perror("cd");
                free(old); 
                return 1;
            } 
        } 
    } 
    if(old) setenv("OLDPWD", old, 1); // успех: записываем прежний каталог в $OLDPWD 
    char* cwd = gettingthiscwd(); // узнаём новый текущий каталог
    if(cwd){ // если получить его удалось
        setenv("PWD", cwd, 1); // обновляем переменную $PWD
        free(cwd); 
    }
    free(old);
    return 0; 
}
int my_pwd(void){ // встроенная pwd: печатает текущий каталог
    char* cwd = gettingthiscwd(); // получаем каталог (malloc-строка)
    puts(cwd); 
    free(cwd); 
    return 0; 
} 
int my_export(char** argv){ // встроенная export: аргументы вида ИМЯ=значение
    for(int i = 1; argv[i] != NULL; i++){ // идём по аргументам, начиная с 1 (argv[0] = "export")
        char* equal = strchr(argv[i], '='); //возвращает адрес памяти - ищем первый '=' внутри той же строки
        if(!equal){ // знака '=' нет - аргумент записан неправильно
            fprintf(stderr, "Неправильный [export] прописан!\n"); 
            return 1; 
        } 
        *equal = '\0'; // временно режем строку на две: "ИМЯ" и (за нулём) "значение"
        if(setenv(argv[i], equal + 1, 1) != 0){ //в окружении текущего процесса появится переменная (1 = перезаписать, если уже есть)
            perror("export"); // при ошибке печатаем причину
            *equal = '='; 
            return 1; 
        } 
        *equal = '='; // восстанавливаем исходную строку после успешного setenv
    } 
    return 0; 
}

int my_unset(char** argv){ // встроенная unset: удаляет переменные окружения по именам
    for(int i = 1; argv[i] != NULL; i++){ // перебираем имена переменных
        if(unsetenv(argv[i]) != 0){ // удаляем переменную; не 0 = ошибка (например, недопустимое имя)
            perror("unset"); 
            return 1;
        } 
    } 
    return 0;
} 
int my_echo(char** argv){ // встроенная echo: печатает аргументы через пробел
    int i = 1, newline = 1; // i - индекс первого печатаемого слова; newline - печатать ли '\n' в конце
    if(argv[1] && strcmp(argv[1], "-n") == 0){ // если первым аргументом идёт флаг -n
        newline = 0; i = 2; // не печатаем перевод строки и пропускаем сам флаг
    } 
    for(; argv[i]; i++){ // печатаем слова до конца массива (NULL)
        if(newline){ // режим без -n: слова начинаются с индекса 1
            if(i > 1){ // перед всеми словами, кроме первого
                fputc(' ', stdout); 
            } 
        } else { // режим с -n: слова начинаются с индекса 2
            if(i > 2){ // перед всеми словами, кроме первого
                fputc(' ', stdout); 
            } 
        } 
        fputs(argv[i], stdout); // печатаем само слово без перевода строки
    } 

    if(newline){ // если флага -n не было
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
int my_fg(char** argv){ // встроенная fg: вернуть задание на передний план
    if(!shell_interactive){ // если шелл работает не с терминалом (-c, скрипт, pipe)
        //по ТЗ: в неинтерактивном режиме управление заданиями отключено
        fprintf(stderr, "mysh: fg: управление заданиями отключено (неинтерактивный режим)\n"); 
        return 1;
    } 
    get_children(); //чистим зомби, сообщаем родителям(ю) - актуализируем статусы заданий
    Job* job = NULL; // сюда запишем выбранное задание
    if(argv[1]){ // пользователь указал, какое задание нужно
        //поддержка %n, %+, %- (не только %n, как было раньше)
        job = resolve_job_spec(argv[1]); // превращаем спецификатор в указатель на задание (или NULL)
        if(!job){ // задание не нашлось
            fprintf(stderr, "mysh: fg: %s: нет такого задания\n", argv[1]); 
            return 1;
        }
    } else { // аргумента нет - берём текущее задание
        if(jobs.count == 0){ // список заданий пуст
            fprintf(stderr, "У нас нет ни одного jobs\n"); 
            return 1; 
        } 

        job = jobs_by_id(current_job_id); //по ТЗ: без аргумента - текущее задание ("+")
        if(!job) job = &jobs.vector[jobs.count - 1]; //подстраховка, если маркер не выставлен: берём последнее
    } 

    if(job->status == J_DONE){ 
        fprintf(stderr, "Задание уже завершено\n"); 
        return 1; 
    }

    printf("%s\n", job->cmdline); // как в bash: печатаем командную строку возвращаемого задания

    int to_fg = put_job_fg(job, 1); //продолжить выполнение если было остановлено (1 = послать SIGCONT)
    return to_fg; // код возврата задания становится результатом fg
} 

int my_bg(char** argv){ // встроенная bg: продолжить остановленное задание в фоне
    if(!shell_interactive){ // без терминала управление заданиями не работает
        fprintf(stderr, "mysh: bg: управление заданиями отключено (неинтерактивный режим)\n"); 
        return 1;
    } 
    get_children(); //чистим зомби, сообщаем родителям - актуализируем статусы
    Job* job = NULL; // выбранное задание
    if(argv[1]){ // указан спецификатор задания
        //поддержка %n, %+, %- (не только %n, как было раньше)
        job = resolve_job_spec(argv[1]); // ищем задание по спецификатору
        if(!job){ // не нашли
            fprintf(stderr, "mysh: bg: %s: нет такого задания\n", argv[1]);
            return 1; 
        } 
    } else {
        if(jobs.count == 0){ // заданий нет
            fprintf(stderr, "У нас нет ни одного jobs\n"); 
            return 1;
        } 

        job = jobs_by_id(current_job_id);
        if(!job) job = &jobs.vector[jobs.count - 1]; //подстраховка, если маркер не выставлен
    } 
    if(job->status == J_RUNNING){ // задание и так работает
        printf("[%d] уже работает\n", job->id); // сообщаем номер задания
        return 1; 
    } 

    job->status = J_RUNNING; // помечаем задание работающим ДО отправки сигнала
    printf("%s\n", job->cmdline); // печатаем команду продолжаемого задания
    put_job_bg(job, 1); // отправляем SIGCONT группе процессов, терминал не трогаем
    return 0; 
}
int my_exit(char** argv){ // встроенная exit
    static bool exit_warned = false; //сохраняется между вызовами exit в течение сессии (живёт всю программу)
    get_children(); //подтягиваем актуальные статусы заданий перед проверкой
    bool has_stopped = false; // найдено ли хотя бы одно остановленное задание
    for(int i = 0; i < jobs.count; i++){ // перебираем все задания
        if(jobs.vector[i].status == J_STOPPED){ 
            has_stopped = true; 
            break;
        } 
    } 
    if(has_stopped){ // есть остановленные задания
        if(!exit_warned){ // предупреждение ещё ни разу не показывали
            fprintf(stderr, "mysh: есть остановленные задания (выполните exit ещё раз для выхода)\n"); 
            exit_warned = true;
            return 1;
        }
    } else { 
        exit_warned = false; 
    } 

    int code; 
    if(argv[1]){ // передан аргумент
        code = atoi(argv[1]); // превращаем строку в число (при мусоре получится 0)
    } else {
        code = last_exit_status; // берём $? последней команды
    } 
    code = ((code % 256) + 256) % 256; 

    exit(code); // завершаем процесс; exit сбрасывает буферы stdio и не возвращается
} 

int my_help(void){ // встроенная help: печатает справку
    printf("Встроенные команды mysh:\n"); // заголовок
    printf("  cd [каталог]              - сменить текущий каталог\n"); // строка про cd
    printf("  pwd                       - показать текущий каталог\n"); // строка про pwd
    printf("  echo [-n] [аргументы]     - вывести аргументы\n"); // строка про echo
    printf("  export ИМЯ=значение       - установить переменную окружения\n"); // строка про export
    printf("  unset ИМЯ                 - удалить переменную окружения\n"); // строка про unset
    printf("  jobs                      - список заданий\n"); // строка про jobs
    printf("  fg [%%n]                   - перевести задание на передний план\n"); // %% печатает один символ %
    printf("  bg [%%n]                   - продолжить остановленное задание в фоне\n"); // %% печатает один символ %
    printf("  kill [-s СИГНАЛ] цель ... - отправить сигнал процессу/заданию\n"); // строка про kill
    printf("  wait [pid|%%n]             - дождаться завершения задания(й)\n"); // %% печатает один символ %
    printf("  exit [код]                - завершить работу интерпретатора\n"); // строка про exit
    printf("  help                      - эта справка\n"); // строка про help
    return 0; // успех
} // конец my_help
static const struct { const char* name; int sig; } signal_table[] = { // массив анонимных структур {имя, номер}
    {"HUP", SIGHUP}, {"INT", SIGINT}, {"QUIT", SIGQUIT}, {"ILL", SIGILL}, // первые сигналы
    {"TRAP", SIGTRAP}, {"ABRT", SIGABRT}, {"FPE", SIGFPE}, {"KILL", SIGKILL}, // следующие сигналы
    {"USR1", SIGUSR1}, {"SEGV", SIGSEGV}, {"USR2", SIGUSR2}, {"PIPE", SIGPIPE}, // пользовательские и др.
    {"ALRM", SIGALRM}, {"TERM", SIGTERM}, {"CHLD", SIGCHLD}, {"CONT", SIGCONT}, // таймер, завершение, потомок, продолжение
    {"STOP", SIGSTOP}, {"TSTP", SIGTSTP}, {"TTIN", SIGTTIN}, {"TTOU", SIGTTOU}, // сигналы остановки и терминала
};
static int parse_signal_name(const char* s){
    if(!s || !*s) return -1; // NULL или пустая строка - не сигнал
    if(s[0] == 'S' && s[1] == 'I' && s[2] == 'G') s += 3; //допускаем и "SIGTERM", и просто "TERM" (пропускаем префикс)
    for(size_t i = 0; i < sizeof(signal_table)/sizeof(signal_table[0]); i++){ // перебираем все записи таблицы (размер массива / размер элемента)
        if(strcmp(signal_table[i].name, s) == 0) return signal_table[i].sig; // имя совпало - возвращаем номер
    } // конец перебора таблицы
    char* end; // сюда strtol положит указатель на первый неразобранный символ
    long n = strtol(s, &end, 10); // пробуем разобрать строку как десятичное число
    if(*s != '\0' && *end == '\0') return (int)n; //это было просто число (строка непустая и разобрана целиком)
    return -1; // ни имя, ни число
} // конец parse_signal_name
int my_kill(char** argv){ // встроенная kill
    int sig = SIGTERM;
    int i = 1;
    if(argv[i] && strcmp(argv[i], "-s") == 0){ // форма "-s СИГНАЛ"
        if(!argv[i+1]){ // после -s ничего нет
            fprintf(stderr, "mysh: kill: опция -s требует аргумент\n"); // сообщение об ошибке
            return 1; // ошибка
        } // конец проверки аргумента
        int parsed = parse_signal_name(argv[i+1]); // разбираем имя/номер сигнала
        if(parsed < 0){ // не распознан
            fprintf(stderr, "mysh: kill: %s: неизвестный сигнал\n", argv[i+1]); 
            return 1; 
        } 
        sig = parsed; // запоминаем выбранный сигнал
        i += 2; // пропускаем "-s" и название сигнала
    } else if(argv[i] && argv[i][0] == '-' && argv[i][1] != '\0'){ // форма "-СИГНАЛ" (одиночный "-" не считается)
        int parsed = parse_signal_name(argv[i] + 1); // разбираем строку без ведущего дефиса
        if(parsed < 0){ // не распознан
            fprintf(stderr, "mysh: kill: %s: неизвестный сигнал\n", argv[i]);
            return 1; // ошибка
        } // конец проверки parsed
        sig = parsed; // запоминаем выбранный сигнал
        i += 1; // пропускаем один аргумент
    } 
    if(!argv[i]){ // после опций нет ни одной цели
        fprintf(stderr, "mysh: kill: не указана цель\n"); 
        return 1; 
    } 
    int status = 0; // общий код возврата: станет 1, если хоть одна цель не удалась
    for(; argv[i]; i++){ // обрабатываем каждую цель по очереди
        if(argv[i][0] == '%'){ // цель - задание (%n, %+, %-)
            Job* job = resolve_job_spec(argv[i]); // ищем задание
            if(!job){ // не нашли
                fprintf(stderr, "mysh: kill: %s: нет такого задания\n", argv[i]); 
                status = 1; 
                continue; 
            }
            if(kill(-(job->pgid), sig) < 0){ //отрицательный pgid - сигнал всей группе процессов
                fprintf(stderr, "mysh: kill: %s: %s\n", argv[i], strerror(errno)); // печатаем причину
                status = 1; // запоминаем неудачу
            } else if((sig == SIGTERM || sig == SIGHUP) && job->status == J_STOPPED){
                //как в bash: остановленный процесс не обработает SIGTERM/SIGHUP, пока его не разбудят - шлём ещё и SIGCONT
                kill(-(job->pgid), SIGCONT);
            } // конец проверки kill
        } else {char* end; // цель - числовой pid; end нужен для проверки strtol
            long pid = strtol(argv[i], &end, 10); // разбираем pid
            if(*argv[i] == '\0' || *end != '\0'){ // пустая строка или мусор после числа
                fprintf(stderr, "mysh: kill: %s: неверный аргумент\n", argv[i]);
                status = 1;
                continue; 
            } 
            if(kill((pid_t)pid, sig) < 0){ // посылаем сигнал одному процессу
                fprintf(stderr, "mysh: kill: (%ld): %s\n", pid, strerror(errno)); 
                status = 1; 
            } 
        }
    } 
    return status; 
} 
int my_wait(char** argv){ // встроенная wait
    if(!argv[1]){ // режим 1: аргументов нет
        //ждём, пока не останется ни одного задания в состоянии Running
        while(true){ // цикл ожидания
            bool any_running = false; // остались ли работающие задания
            for(int i = 0; i < jobs.count; i++){ // перебираем задания
                if(jobs.vector[i].status == J_RUNNING){ any_running = true; break; } // нашли работающее - дальше не ищем
            }
            if(!any_running) break; // работающих нет - ждать больше нечего
            //ждём ЦЕЛИКОМ группу каждого работающего задания (у конвейера процессов несколько)
            for(int i = 0; i < jobs.count; i++){ // перебираем задания
                if(jobs.vector[i].status != J_RUNNING) continue; // интересуют только работающие
                int st; // статус очередного завершившегося процесса группы
                while(true){ // собираем все процессы группы
                    pid_t r = waitpid(-jobs.vector[i].pgid, &st, 0); // блокирующе ждём любого процесса этой группы
                    if(r < 0){ // ошибка
                        if(errno == EINTR) continue; // прервал сигнал - повторяем
                        break; // ECHILD - в группе больше нет процессов
                    }
                }
                jobs.vector[i].status = J_DONE; // группа целиком завершилась
            }
        } 
        jobs_remove(); // убираем завершённые задания из списка
        return 0; // wait без аргументов возвращает 0
    } 
    Job* job = NULL; // задание, соответствующее цели (может остаться NULL)
    pid_t target_pid; // pid, который будем ждать
    if(argv[1][0] == '%'){ // цель задана спецификатором задания
        job = resolve_job_spec(argv[1]); // ищем задание
        if(!job){ // не нашли
            fprintf(stderr, "mysh: wait: %s: нет такого задания\n", argv[1]);
            return 1;
        }
        target_pid = job->pgid; // ждём лидера группы задания
    } else { // цель задана числом (pid)
        char* end; // для проверки strtol
        long pid = strtol(argv[1], &end, 10); // разбираем pid
        if(*argv[1] == '\0' || *end != '\0'){ // пустая строка или мусор
            fprintf(stderr, "mysh: wait: %s: неверный аргумент\n", argv[1]);
            return 1;
        }
        target_pid = (pid_t)pid; // приводим к типу pid_t
        job = jobs_by_pgid(target_pid); // пробуем найти связанное задание (если нет - останется NULL)
    }
    int status = 0; // статус завершения ждомого процесса
    if(job){ // у задания может быть несколько процессов (конвейер): ждём ВСЮ группу, а не только лидера
        int last_status = 0; // статус последнего завершившегося процесса группы
        while(true){ // собираем все процессы группы
            pid_t r = waitpid(-job->pgid, &status, 0); // блокирующе ждём любого процесса этой группы
            if(r < 0){ // ошибка
                if(errno == EINTR) continue; // прервал сигнал - повторяем
                break; 
            }
            last_status = status;
        }
        status = last_status; // код задания - код последнего завершившегося процесса
    } else { // просто pid, не являющийся заданием
        while(true){ // цикл повтора при EINTR
            pid_t r = waitpid(target_pid, &status, 0); // блокирующе ждём именно этот pid
            if(r < 0){ 
                if(errno == EINTR) continue; // прервал сигнал - повторяем
                if(errno == ECHILD) return 0; //уже завершилось раньше нас - трактуем как успех
                fprintf(stderr, "mysh: wait: %s\n", strerror(errno)); // другая ошибка - печатаем причину
                return 1; 
            }
            break; 
        } 
    }
    if(job) job->status = J_DONE; // помечаем найденное задание завершённым
    if(WIFEXITED(status)) return WEXITSTATUS(status); // нормальный выход - возвращаем его код
    if(WIFSIGNALED(status)) return 128 + WTERMSIG(status); // убит сигналом - 128 + номер сигнала (как в bash)
    return 0; // иные случаи считаем успехом
} 
int is_mybuilt(Node* node){ // проверка: является ли команда встроенной (1 - да, 0 - нет)
    if(!node || node->type != NODE_CMD || node->argv == NULL || node->argv[0] == NULL) // защита: узла нет / не простая команда / нет argv / нет имени
     return 0; // такие узлы не встроенные
    char* name = node->argv[0]; // имя команды лежит в argv[0]
    if(strcmp(name, "cd") == 0) // это cd
    return 1; 
    if(strcmp(name, "pwd") == 0) 
    return 1; 
    if(strcmp(name, "export") == 0) 
     return 1;
    if(strcmp(name, "exit") == 0) 
     return 1; 
    if(strcmp(name, "echo") == 0) 
     return 1; 
    if(strcmp(name, "unset") == 0)  
     return 1; 
    if(strcmp(name, "jobs") == 0)  
    return 1;   
    if(strcmp(name, "fg") == 0)  
     return 1;  
    if(strcmp(name, "bg") == 0)  
     return 1; 
    if(strcmp(name, "help") == 0)  
     return 1; 
    if(strcmp(name, "kill") == 0) 
    return 1;
    if(strcmp(name, "wait") == 0) 
    return 1; 
    return 0; 
} 
int run_mybuilt(Node* node){ // выполняет встроенную команду и возвращает её код возврата
    if(!node || node->type != NODE_CMD || node->argv == NULL || node->argv[0] == NULL){ // та же защитная проверка
        fprintf(stderr, "Пустая команда или ошибка парсинга\n"); // сообщение об ошибке
        return 1;
    } 
    char* a = node->argv[0]; // имя команды
    if(strcmp(a, "cd") == 0) return my_cd(node->argv); // cd получает argv
    if(strcmp(a, "pwd") == 0) return my_pwd(); // pwd аргументов не принимает
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
    return 1;
} 
