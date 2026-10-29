#include "common.h" 
#include "exec.h" 
#include "builtins.h" 
#include "jobs.h"
#include "memory.h" 
#include "common.h" 
#include <stdio.h> 
#include <stdlib.h> 
#include <string.h> 
#include <unistd.h> 
#include <signal.h> 
#include <errno.h> 
#include <fcntl.h> 
#include <time.h> 
#include <sys/wait.h>

static bool in_bg_job = false;
static int exec_fail_status(const char* name){ // name - имя команды для сообщения об ошибке
    int code = (errno == ENOENT || errno == ENOTDIR) ? 127 : 126; // errno выставил неудачный execvp; нет файла -> 127, иначе 126
    fprintf(stderr, "mysh: %s: %s\n", name, strerror(errno)); // диагностика в stderr: "mysh: имя: текст ошибки"
    return code; // вызывающий сделает _exit(code) - это и будет статус команды
}
static bool apply_redir(Redir* r, bool fatal){ // r - один редирект из списка; fatal - убивать ли процесс при ошибке
    int fd; // временный дескриптор открытого файла
    // <
    if(r->type == R_IN){ // ввод из файла
        fd = open(r->target, O_RDONLY); // открываем файл только для чтения
        if(fd < 0){ // не открылся (нет файла, нет прав)
            if(fatal){ perror(r->target); _exit(1); } // потомок: сообщение и немедленный выход с кодом 1
            fprintf(stderr, "mysh: %s: %s\n", r->target, strerror(errno)); // builtin: только сообщение
            return false;
        }
        int dest = (r->src <= 0) ? 0 : r->src; // какой дескриптор подменяем: по умолчанию stdin (0), иначе явный N из N<
        if(dup2(fd, dest) < 0){ // делаем dest копией fd (теперь чтение из dest идёт из файла)
            close(fd); // не утекает дескриптор при ошибке
            if(fatal){ perror("dup2"); _exit(1); } // потомок: выход
            fprintf(stderr, "mysh: dup2: %s\n", strerror(errno)); // builtin: сообщение
            return false;
        }
        close(fd); // оригинальный fd больше не нужен: копия живёт в dest
    }
    // > и >> права 0666 с учётом umask процесса
    else if(r->type == R_OUT || r->type == R_APPEND){ // вывод в файл: перезапись или дозапись
        int flags = O_WRONLY | O_CREAT | (r->type == R_APPEND ? O_APPEND : O_TRUNC); // только запись, создать если нет, и обрезать (>) либо дописывать в конец (>>)
        fd = open(r->target, flags, 0666); // 0666 & ~umask - итоговые права нового файла
        if(fd < 0){ // не открылся
            if(fatal){ perror(r->target); _exit(1); } // потомок: выход
            fprintf(stderr, "mysh: %s: %s\n", r->target, strerror(errno)); 
            return false; 
        }
        int dest = (r->src <= 1) ? 1 : r->src; // по умолчанию stdout (1), иначе явный N из N> (например 2> )
        if(dup2(fd, dest) < 0){ // подменяем dest файлом
            close(fd); // закрываем, чтобы не утекал
            if(fatal){ perror("dup2"); _exit(1); } // потомок: выход
            fprintf(stderr, "mysh: dup2: %s\n", strerror(errno)); 
            return false; 
        }
        close(fd); // лишний дескриптор не нужен
    }
    // &>
    else if(r->type == R_ALL_TO_FILE){ // и stdout, и stderr в один файл
        fd = open(r->target, O_WRONLY | O_CREAT | O_TRUNC, 0666); // открываем с перезаписью
        if(fd < 0){ // не открылся
            if(fatal){ perror(r->target); _exit(1); } // потомок: выход
            fprintf(stderr, "mysh: %s: %s\n", r->target, strerror(errno)); 
            return false; 
        }
        if(dup2(fd, STDOUT_FILENO) < 0 || dup2(fd, STDERR_FILENO) < 0){ // и 1, и 2 становятся копиями файла
            close(fd); // закрываем
            if(fatal){ perror("dup2"); _exit(1); } // потомок: выход
            fprintf(stderr, "mysh: dup2: %s\n", strerror(errno)); 
            return false; 
        }
        close(fd); // оригинал не нужен
    }

    //дублирование вывода 2>&1 перенаправь std_err туда же куда и std_out
    else if(r->type == R_DUP_OUT){ // N>&M
        int dest = (r->src <= 1) ? 1 : r->src; // что перенаправляем (N); без N - stdout
        if(dup2(r->dup_target, dest) < 0){ // dest становится копией дескриптора M (dup_target)
            if(fatal){ perror("dup2"); _exit(1); } // потомок: выход (например M не открыт)
            fprintf(stderr, "mysh: dup2: %s\n", strerror(errno)); 
            return false;
        }
    }
    //Дублирование ввода <&N
    else if(r->type == R_DUP_IN){ // N<&M
        int dest = (r->src <= 0) ? 0 : r->src; // что перенаправляем; без N - stdin
        if(dup2(r->dup_target, dest) < 0){ // dest становится копией M
            if(fatal){ perror("dup2"); _exit(1); } // потомок: выход
            fprintf(stderr, "mysh: dup2: %s\n", strerror(errno)); 
            return false; // ошибка
        }
    }
    // <<EOF || // <<< "string smth"
    else if(r->type == R_HEREDOC || r->type == R_HERESTR){ // текст (не файл) подаём на stdin
        if(r->src > 0){ // heredoc/herestring бывают только для stdin
            if(fatal){ fprintf(stderr, "Ввод только в stdin\n"); _exit(1); } // потомок: сообщение и выход
            fprintf(stderr, "mysh: ввод только в stdin\n"); 
            return false; 
        }
        int p[2]; // p[0] - конец чтения, p[1] - конец записи
        if(pipe(p) < 0){ // создаём канал, через который "протолкнём" текст
            if(fatal){ perror("pipe"); _exit(1); } // потомок: выход
            fprintf(stderr, "mysh: pipe: %s\n", strerror(errno)); 
            return false;
        }
        size_t len = strlen(r->target); // длина текста heredoc / строки herestring
        if(len > 0) write(p[1], r->target, len); // кладём текст в канал (ограничено размером буфера канала)
        if(r->type == R_HERESTR) write(p[1], "\n", 1); //here-string, как в bash, заканчивается переводом строки
        close(p[1]); // закрываем запись, чтобы читатель увидел EOF после текста
        if(dup2(p[0], STDIN_FILENO) < 0){ // stdin становится концом чтения канала
            close(p[0]); // закрываем чтение
            if(fatal){ perror("dup2"); _exit(1); } // потомок: выход
            fprintf(stderr, "mysh: dup2: %s\n", strerror(errno)); 
            return false; // ошибка
        }
        close(p[0]); // оригинал больше не нужен
    }
    else { // тип редиректа неизвестен (защитная ветка)
        if(fatal){ // потомок
            fprintf(stderr, "Неизвестный тип перенаправления\n"); 
        } else { // builtin
            fprintf(stderr, "mysh: неизвестный тип перенаправления\n");
            return false; 
        }
    }
    return true; 
}
void do_one_redir(Redir* r){ //фатальная версия для потомков
    apply_redir(r, true); // при ошибке процесс сам завершится
}
//"безопасная" версия для builtin без fork (см. run_builtin_with_redirs):
//не завершает процесс интерпретатора при ошибке, а по ТЗ возвращает
//диагностику и false, чтобы вызывающий код мог отменить команду с кодом 1
bool do_one_redir_safe(Redir* r){
    return apply_redir(r, false); // шелл не умирает, ошибка идёт через возвращаемое значение
}
int do_redirs(Redir* redir_list){ // применить весь список редиректов по порядку (порядок важен: >f 2>&1 != 2>&1 >f)
    for(Redir* r = redir_list; r != NULL; r = r->next){ // обходим связный список
        do_one_redir(r); // применяем очередной
    }
    return 0; // ошибки обрабатываются внутри (через _exit)
}
//вычисляет, какой именно файловый дескриптор процесса подменит данный
//редирект (та же логика выбора dest, что и внутри apply_redir)
static int redir_target_fd(Redir* r){
    switch(r->type){ // выбор по типу редиректа
        case R_IN:          return (r->src <= 0) ? 0 : r->src; // по умолчанию stdin
        case R_OUT:
        case R_APPEND:      return (r->src <= 1) ? 1 : r->src; // по умолчанию stdout
        case R_DUP_OUT:     return (r->src <= 1) ? 1 : r->src; // по умолчанию stdout
        case R_DUP_IN:      return (r->src <= 0) ? 0 : r->src; // по умолчанию stdin
        case R_HEREDOC:
        case R_HERESTR:     return 0; // всегда stdin
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
    int count = 0; // сколько дескрипторов нужно сохранить
    for(Redir* r = node->redirs; r; r = r->next){ // считаем по списку редиректов
        count += (r->type == R_ALL_TO_FILE) ? 2 : 1; // &> затрагивает два дескриптора (1 и 2), остальные - один
    }
    int* saved_fd = er_malloc((size_t)count * sizeof(int)); // сюда пойдут копии исходных дескрипторов (результат dup)
    int* orig_fd  = er_malloc((size_t)count * sizeof(int)); // а тут запомним, чьими копиями они являются
    int nsaved = 0; // сколько уже сохранено
    //сохраняем исходные дескрипторы через dup ДО применения редиректов
    for(Redir* r = node->redirs; r; r = r->next){ // снова обходим список
        if(r->type == R_ALL_TO_FILE){ // особый случай: два дескриптора
            int fds[2] = {STDOUT_FILENO, STDERR_FILENO}; // 1 и 2
            for(int i = 0; i < 2; i++){ // сохраняем каждый
                orig_fd[nsaved]  = fds[i]; // запоминаем номер
                saved_fd[nsaved] = dup(fds[i]); // dup даёт новый дескриптор с тем же "хозяйством" (терминал/файл)
                nsaved++; // следующий слот
            }
        } else { // обычный редирект
            int fd = redir_target_fd(r); // какой дескриптор он заменит
            orig_fd[nsaved]  = fd; // запоминаем номер
            saved_fd[nsaved] = dup(fd); // сохраняем копию
            nsaved++; // следующий слот
        }
    }
    //применяем сами редиректы; при первой же ошибке открытия файла -
    //по ТЗ команда отменяется целиком, код возврата 1
    bool ok = true; // пока всё хорошо
    for(Redir* r = node->redirs; r; r = r->next){ // по порядку
        if(!do_one_redir_safe(r)){ // безопасная версия: шелл не завершается
            ok = false; // запоминаем неудачу
            break; // дальше не применяем
        }
    }
    int status = ok ? run_mybuilt(node) : 1; // выполняем builtin в ЭТОМ процессе, либо код 1 если редирект не удался
    fflush(NULL); //критично: сбрасываем буферы ДО восстановления исходных
                  //дескрипторов - иначе буферизованный вывод builtin'а рискует
                  //попасть не в перенаправленный файл, а в терминал (при следующем
                  //флаше уже после dup2 назад) или вовсе потеряться
    //восстанавливаем исходные дескрипторы в обратном порядке
    for(int i = nsaved - 1; i >= 0; i--){ // обратный порядок на случай нескольких редиректов одного дескриптора
        dup2(saved_fd[i], orig_fd[i]); // возвращаем оригинал на место
        close(saved_fd[i]); // закрываем сохранённую копию
    }
    free(saved_fd); // освобождаем массивы
    free(orig_fd);
    return status;
}
int run_command(Node* node){ // запуск одной простой команды (с аргументами и редиректами)
    if(node->argv == NULL || node->argv[0] == NULL){ // нет ни одного слова
        return 0; //если в узле банально аргументов нет, но как бы вышли без ошибок
    }
    if(is_mybuilt(node)){ // это встроенная команда (cd, exit, jobs ...)
        //builtin без конвейера всегда выполняется в самом процессе интерпретатора -
        //с редиректами (через dup, см. run_builtin_with_redirs) или без них
        if(node->redirs == NULL){ // редиректов нет
            return run_mybuilt(node); // просто вызываем
        }
        return run_builtin_with_redirs(node); // иначе с сохранением/восстановлением fd
    }
    fflush(NULL); // сбрасываем буферы ДО fork(), иначе child унаследует непропечатанный
                   // буфер родителя и может вывести его повторно
    pid_t pid = fork(); // создаём процесс для внешней команды
    if(pid < 0){ // fork не удался
        perror("fork"); // сообщение
        return 1; //вышли с ошибкой
    }
    if(pid == 0){ //потомок
        setpgid(0, 0); // создаём новую группу процессов (pgid = свой pid)
        signal(SIGINT, SIG_DFL); // возвращаем стандартную реакцию на Ctrl+C (шелл их игнорирует, а игнор переживает exec)
        signal(SIGTSTP, SIG_DFL); // Ctrl+Z
        signal(SIGQUIT, SIG_DFL); /* Ctrl+\ */
        signal(SIGTTIN, SIG_DFL); //cat &
        signal(SIGTTOU, SIG_DFL); // чтобы фоновая команда останавливалась при обращении к терминалу, как положено
        if(node->redirs) do_redirs(node->redirs); // применяем редиректы (при ошибке потомок завершится)
        execvp(node->argv[0], node->argv); // заменяем образ процесса программой; при успехе сюда не возвращаемся
        _exit(exec_fail_status(node->argv[0])); // exec не удался: сообщение и код 126/127; _exit, чтобы не сбрасывать копию буферов родителя
    } else { //РОДИТЕЛЬ (шелл)
        setpgid(pid, pid); // дублируем setpgid в родителе, чтобы не зависеть от гонки: кто раньше выполнится
        if(!in_bg_job && isatty(STDIN_FILENO)){ // терминал отдаём только если мы не фоновое задание и есть терминал
            tcsetpgrp(STDIN_FILENO, pid); //фоновые процессы не забирают терминал
        }
        int status = 0; //определяем на тот случай, если waitpid вышел с ошибкой, он не напишет ничего в status
        if(waitpid(pid, &status, WUNTRACED) < 0){ // ждём завершения ИЛИ остановки (WUNTRACED) потомка
            perror("waitpid"); // ошибка ожидания
            if(!in_bg_job){ // возвращаем терминал шеллу
                tcsetpgrp(STDIN_FILENO, shell_pgid); // передний план снова у шелла
                tcsetattr(STDIN_FILENO, TCSADRAIN, &shell_settings); // и настройки терминала шелла
            }
            return 1;
        }
        if(!in_bg_job){ // потомок завершился или остановился - забираем терминал обратно
            tcsetpgrp(STDIN_FILENO, shell_pgid); // передний план у шелла
            tcsetattr(STDIN_FILENO, TCSADRAIN, &shell_settings); // восстанавливаем режим терминала (потомок мог его испортить)
        }
        if(WIFSTOPPED(status)){ // процесс остановлен (Ctrl+Z), не завершён
            //имя задания - вся команда с аргументами ("sleep 100"), а не только argv[0]
            size_t len = 1;
            for(int a = 0; node->argv[a]; a++) len += strlen(node->argv[a]) + 1;
            char* name = er_malloc(len);
            name[0] = '\0';
            for(int a = 0; node->argv[a]; a++){
                if(a) strcat(name, " ");
                strcat(name, node->argv[a]);
            }
            Job* j = jobs_add(pid, name); // заносим в таблицу заданий (jobs_add копирует строку)
            free(name);
            if(j){ j->procs[0].state = P_STOPPED; j->status = J_STOPPED; } // помечаем как остановленное
            return 128 + SIGTSTP; // статус как у сигнала остановки
        }
        if(WIFEXITED(status)) return(WEXITSTATUS(status)); // нормальный выход: возвращаем его код
        if(WIFSIGNALED(status)) return 128 + WTERMSIG(status); // убит сигналом: 128 + номер сигнала
    }
    return 0; // сюда доходим лишь в редких случаях
}
//Выполнить один этап конвейера в УЖЕ созданном процессе (потомке run_pipeline).
//Простая внешняя команда запускается через execvp прямо в этом процессе, без
//второго fork. Раньше этап вызывал run_node -> run_command, который форкал ещё
//раз и делал setpgid(0, 0): настоящая программа оказывалась в отдельной группе
//вне группы конвейера (kill %n её не доставал), а процесс-прослойка забирал
//терминал даже у ФОНОВОГО конвейера - после `sleep 100 | sleep 100 &` шелл
//терял терминал и завершался. Остальные узлы (builtin, подшелл, вложенный
//конвейер) выполняются через run_node. Функция не возвращается.
static void run_stage(Node* n, char* cmdline){
    if(n && n->type == NODE_CMD && n->argv && n->argv[0] && !is_mybuilt(n)){ // это простая ВНЕШНЯЯ команда
        signal(SIGINT, SIG_DFL); // восстанавливаем стандартные сигналы
        signal(SIGTSTP, SIG_DFL);
        signal(SIGQUIT, SIG_DFL);
        signal(SIGTTIN, SIG_DFL);
        signal(SIGTTOU, SIG_DFL);
        if(n->redirs) do_redirs(n->redirs); // редиректы применяются ПОСЛЕ dup2 на канал, поэтому >file перекрывает канал (как в bash)
        execvp(n->argv[0], n->argv); // запускаем программу в этом же процессе
        _exit(exec_fail_status(n->argv[0])); // не вышло: 126/127
    }
    int st = run_node(n, cmdline); // builtin / подшелл / вложенный конвейер: выполняем рекурсивно
    fflush(NULL); //_exit не сбрасывает буферы stdio - без этого вывод builtin'а в канал теряется
    _exit(st); // завершаем этот процесс-этап с полученным статусом
}
//реализуем наш конвейер
//Дерево конвейера строится с уклоном влево: a | b | c  ->  PIPE(PIPE(a, b), c).
//Раньше левая часть запускалась как отдельный процесс-шелл, который сам запускал
//вложенный конвейер: при 3+ командах в ps появлялся лишний процесс mybash, а команды
//оказывались в разных группах процессов. Теперь дерево сначала "разворачивается" в
//плоский массив этапов, и для каждого этапа создаётся ровно один процесс - как в bash.
int run_pipeline(Node* node, bool flag_bg, char* cmdline, pid_t* out_pgid){ // flag_bg - запуск в фоне; out_pgid - вернуть группу вызывающему
    int count = 1; // число этапов = число символов | плюс 1
    for(Node* t = node; t && t->type == NODE_PIPE; t = t->left) count++; // идём вниз по левой ветке и считаем

    Node** stages = er_malloc((size_t)count * sizeof(Node*)); // этапы слева направо
    Node* t = node; // текущий узел при разворачивании
    for(int i = count - 1; i >= 1; i--){ // правые потомки - это этапы с конца
        stages[i] = t->right;
        t = t->left;
    }
    stages[0] = t; // самый левый этап - не PIPE-узел

    int npipes = count - 1; // между этапами - по одному каналу
    int (*fds)[2] = er_malloc((size_t)npipes * sizeof(int[2])); // fds[i] - канал между этапом i и i+1
    for(int i = 0; i < npipes; i++){ // создаём все каналы заранее
        if(pipe(fds[i]) < 0){
            perror("pipe");
            for(int k = 0; k < i; k++){ close(fds[k][0]); close(fds[k][1]); } // закрываем уже созданные
            free(fds); free(stages);
            return 1;
        }
    }

    pid_t* pids = er_malloc((size_t)count * sizeof(pid_t)); // pid каждого этапа
    pid_t pg = 0; // группа процессов конвейера = pid первого этапа
    int started = 0; // сколько этапов реально запущено
    for(int i = 0; i < count; i++){
        fflush(NULL); // сброс буферов перед fork
        pid_t pid = fork(); // один процесс на этап
        if(pid < 0){ // fork не удался
            perror("fork");
            break; // уже запущенные этапы доработают сами, ниже закроем каналы и подождём их
        }
        if(pid == 0){ // ---- потомок: этап номер i ----
            setpgid(0, i == 0 ? 0 : pg); // первый становится лидером новой группы, остальные входят в неё
            if(flag_bg) in_bg_job = true; // фон: больше не трогаем терминал (и все наши потомки тоже)
            if(!in_bg_job) tcsetpgrp(STDIN_FILENO, i == 0 ? getpid() : pg); // передний план: терминал группе конвейера
            signal(SIGINT, SIG_DFL); // стандартные реакции на Ctrl+C / Ctrl+Z
            signal(SIGTSTP, SIG_DFL);
            if(i > 0) dup2(fds[i - 1][0], STDIN_FILENO); // ввод - из канала слева
            if(i < npipes) dup2(fds[i][1], STDOUT_FILENO); // вывод - в канал справа
            for(int k = 0; k < npipes; k++){ close(fds[k][0]); close(fds[k][1]); } // оригиналы закрываем, иначе читатель не получит EOF
            run_stage(stages[i], cmdline); // выполняем этап (не возвращается)
        }
        if(i == 0) pg = pid; // группа конвейера - pid первого процесса
        setpgid(pid, pg); // родитель тоже ставит группу (защита от гонки)
        pids[i] = pid;
        started++;
    }

    for(int k = 0; k < npipes; k++){ close(fds[k][0]); close(fds[k][1]); } // родителю каналы не нужны: иначе читатель не увидит EOF
    free(fds);
    free(stages);
    if(out_pgid) *out_pgid = pg; // сообщаем группу вызывающему

    int result = 0;
    if(started < count){ // не все этапы запущены (ошибка fork)
        for(int i = 0; i < started; i++) waitpid(pids[i], NULL, 0);
        free(pids);
        return 1;
    }

    if(flag_bg){ // фоновый конвейер: не ждём
        Job* newjob = jobs_add(pg, cmdline); // регистрируем задание
        job_set_procs(newjob, pids, count); // и запоминаем pid каждого этапа конвейера
        printf("[%d] %d\n", jobs.vector[jobs.count -1].id, pg); // печатаем "[номер] pgid"
        free(pids);
        return 0; // статус 0
    }

    //Job control (Ctrl+Z на конвейере) работает только в самом шелле на терминале: не в фоне и не в подшелле
    bool jc = shell_interactive && !in_bg_job && getpgrp() == shell_pgid;
    if(!jc){ // без job control: ждём каждый этап по очереди
        for(int i = 0; i < count; i++){ // передний план: ждём все этапы
            int status = 0;
            if(waitpid(pids[i], &status, 0) < 0){
                perror("waitpid");
                if(i == count - 1) result = 1;
                continue;
            }
            if(i == count - 1){ // статус конвейера = статус последнего этапа
                if(WIFEXITED(status)) result = WEXITSTATUS(status);
                else if(WIFSIGNALED(status)) result = 128 + WTERMSIG(status);
            }
        }
    } else { // ждём события процессов всей группы: завершение ИЛИ остановку (WUNTRACED)
        bool* done = er_malloc((size_t)count * sizeof(bool));
        bool* stp  = er_malloc((size_t)count * sizeof(bool));
        for(int i = 0; i < count; i++){ done[i] = false; stp[i] = false; }
        int alive = count; // сколько этапов ещё не завершилось
        bool stopped_all = false;
        while(alive > 0){
            int status = 0;
            pid_t r = waitpid(-pg, &status, WUNTRACED);
            if(r < 0){
                if(errno == EINTR) continue; // прервали сигналом - повторяем
                break; // ECHILD: процессов больше нет
            }
            int idx = -1;
            for(int i = 0; i < count; i++) if(pids[i] == r){ idx = i; break; }
            if(idx < 0) continue; // чужой процесс
            if(WIFSTOPPED(status)){
                stp[idx] = true;
                int nstop = 0;
                for(int i = 0; i < count; i++) if(!done[i] && stp[i]) nstop++;
                if(nstop == alive){ stopped_all = true; break; } // все живые остановлены - возвращаем приглашение
                continue; // остановились не все - ждём остальных
            }
            done[idx] = true; stp[idx] = false; alive--;
            if(idx == count - 1){ // статус конвейера = статус последнего этапа
                if(WIFEXITED(status)) result = WEXITSTATUS(status);
                else if(WIFSIGNALED(status)) result = 128 + WTERMSIG(status);
            }
        }
        if(stopped_all){ // Ctrl+Z: превращаем конвейер в остановленное задание
            Job* j = jobs_add(pg, cmdline);
            job_set_procs(j, pids, count);
            for(int i = 0; i < count; i++) j->procs[i].state = done[i] ? P_DONE : (stp[i] ? P_STOPPED : P_RUNNING);
            j->status = J_STOPPED;
            result = 128 + SIGTSTP;
        }
        free(done); free(stp);
    }
    free(pids);
    return result;
}

int run_minishell(Node* node, bool flag_bg, char* cmdline, pid_t* out_pgid){ // подшелл ( ... )
    fflush(NULL); // сброс буферов
    pid_t pid = fork(); // подшелл - отдельный процесс (изоляция cd, переменных)
    if(pid < 0){ // fork не удался
        perror("fork");
        return 1;
    }
    if(pid == 0){ // потомок-подшелл 
        setpgid(0, 0); // своя группа
        pid_t pg = getpid(); // её номер
        if(flag_bg) in_bg_job = true; // фон - без терминала
        if(!in_bg_job) tcsetpgrp(STDIN_FILENO, pg); // передний план - терминал группе
        signal(SIGINT, SIG_DFL); signal(SIGTSTP, SIG_DFL); // стандартные реакции
        if(node->redirs) do_redirs(node->redirs); // редиректы подшелла ( ... ) > file
        int st = run_node(node->left, cmdline); // выполняем содержимое скобок
        fflush(NULL); //_exit не сбрасывает буферы stdio - иначе вывод builtin'а в канал/файл теряется
        _exit(st); // выходим с его статусом
    } else { //родитель 
        setpgid(pid, pid); // группа и со стороны родителя
        if(out_pgid) *out_pgid = pid; // возвращаем pgid
        if(flag_bg){ // в фоне - не ждём
            jobs_add(pid, cmdline); // регистрируем задание
            printf("[%d] %d\n", jobs.vector[jobs.count -1].id, pid); // "[номер] pid"
            return 0;
        } else { // на переднем плане - ждём
            int status = 0;
            if(waitpid(pid, &status, 0) < 0){ // ждём подшелл
                perror("waitpid");
                return 1;
            }
            if(WIFEXITED(status)) return(WEXITSTATUS(status)); // код выхода подшелла
            if(WIFSIGNALED(status)) return 128 + WTERMSIG(status); // убит сигналом
        }
    }
    return 0;
}
//самая главная функция в нашем коде, именно ради нее и было все написано
int run_node(Node* node, char* cmdline){ // обход дерева разбора; cmdline - исходная строка (для имени задания)
    if(!node) return 0; // пустое дерево - успех
    if(node->type == NODE_CMD) return run_command(node); // простая команда
    else if(node->type == NODE_PIPE) return run_pipeline(node, false, cmdline, NULL); // конвейер на переднем плане
    else if(node->type == NODE_AND){ // a && b
        int status = run_node(node->left, cmdline); // выполняем левую часть
        if(status == 0) status = run_node(node->right, cmdline); // правую - только при успехе левой
        return status; // статус последней выполненной
    }
    else if(node->type == NODE_OR){ // a || b
        int status = run_node(node->left, cmdline); // левая часть
        if(status != 0) status = run_node(node->right, cmdline); // правая - только при неудаче левой
        return status;
    }
    else if(node->type == NODE_SEQ){ // a ; b
        run_node(node->left, cmdline); // статус левой игнорируется
        return run_node(node->right, cmdline); // результат - статус правой
    }
    else if(node->type == NODE_BG){ // a &
        int tmp = 0; pid_t pgid = -1; // статус запуска (не используется) и pgid задания
        if(node->left->type == NODE_PIPE){ // фоновый конвейер
            tmp = run_pipeline(node->left, true, cmdline, &pgid);
            struct timespec now; // момент запуска
            clock_gettime(CLOCK_MONOTONIC, &now); // монотонные часы (не зависят от перевода времени)
            add_ms(now, BG_BLOCK_MS, &bg_until); // на короткое время блокируем приглашение, чтобы вывод "[1] pid" не затёрся
        }
        else if(node->left->type == NODE_MINISHELL){ // фоновый подшелл ( ... ) &
            tmp = run_minishell(node->left, true, cmdline, &pgid);// запускаем в фоне
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            add_ms(now, BG_BLOCK_MS, &bg_until);
        }
        else if(node->left->type == NODE_CMD){ // фоновая простая команда
            fflush(NULL); // сброс буферов
            pid_t pid = fork(); // отдельный процесс
            if(pid < 0){ // fork не удался
                perror("fork");
                return 1;
            }
            else if(pid == 0){ //потомок
                setpgid(0, 0); // своя группа (не передний план)
                signal(SIGINT, SIG_DFL); // стандартные сигналы
                signal(SIGTSTP, SIG_DFL);
                signal(SIGQUIT, SIG_DFL);
                Node* cmd = node->left; // сама команда
                if(cmd->redirs) do_redirs(cmd->redirs); // редиректы
                if(is_mybuilt(cmd)){ // builtin в фоне - выполняется в этом потомке
                    int built_command = run_mybuilt(cmd);
                    fflush(NULL); // та же причина: без сброса буферов вывод в редирект теряется
                    _exit(built_command);
                }
                execvp(cmd->argv[0], cmd->argv); // внешняя команда
                _exit(exec_fail_status(cmd->argv[0])); // не вышло: 126/127
            }
            else { //родитель 
                setpgid(pid, pid); // группа и с этой стороны
                jobs_add(pid, cmdline); // регистрируем задание
                printf("[%d] %d\n", jobs.vector[jobs.count -1].id, pid); // "[номер] pid"
                struct timespec now;
                clock_gettime(CLOCK_MONOTONIC, &now);
                add_ms(now, BG_BLOCK_MS, &bg_until);
            }
        }
        else {
            //&&, ||, ; (и всё прочее) под &: выражение выполняется целиком в фоновом
            //подшелле. run_minishell исполняет node->left УЗЛА-подшелла, поэтому
            //оборачиваем выражение во временный узел NODE_MINISHELL. Раньше сюда
            //передавался сам узел AND/OR/SEQ, и в фоне выполнялась только его левая часть
            Node wrapper; // временный узел на стеке
            memset(&wrapper, 0, sizeof(wrapper)); // обнуляем все поля (redirs = NULL и т.д.)
            wrapper.type = NODE_MINISHELL; // притворяемся подшеллом
            wrapper.left = node->left; // внутри - всё выражение
            tmp = run_minishell(&wrapper, true, cmdline, &pgid); // запускаем в фоне
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            add_ms(now, BG_BLOCK_MS, &bg_until);
        }
        (void)tmp; // подавляем предупреждение "переменная не используется"
        if(node->right) return run_node(node->right, cmdline); // после & может идти продолжение (a & b)
        return 0;
    }
    else if(node->type == NODE_MINISHELL) return run_minishell(node, false, cmdline, NULL); // подшелл на переднем плане
    else {
        return 1;
    }
}
