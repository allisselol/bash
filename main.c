
#include "common.h"
#include "memory.h" 
#include "utils.h" 
#include "token.h" 
#include "lexer.h" 
#include "node.h" 
#include "parser.h" 
#include "jobs.h" 
#include "exec.h"
#include "readline.h"
char* welcome_string(void){
    char* cwd = gettingthiscwd(); // текущий каталог
    char* line_part; //вместо путми
    if(cwd != NULL){ // каталог получили
        line_part = cwd; // показываем его
    } else { // getcwd не удался
        line_part = "?";
    }
    char* user = getenv("USER"); // имя пользователя из окружения
    if(!user){ // переменной нет
        user = "user"; // запасное имя
    }
    int needed = snprintf(NULL, 0, "%s:%s$ ", user, line_part); // сколько символов нужно 
    char* line; // итоговое приглашение
    if(needed < 0){ // snprintf вернул ошибку
        line = er_strdup("$ "); // минимальное приглашение
    } else {
        line = er_malloc((size_t)needed + 1); // +1 байт под завершающий '\0'
        snprintf(line, (size_t)needed + 1, "%s:%s$ ", user, line_part); // формируем приглашение
    }
    free(cwd); 
    return line; 
}
// теперь проверка на bg 
bool line_has_bg_tok(TokenVector* tv){ // есть ли среди токенов '&' 
    for(size_t i = 0; i < tv->n; i++){ // проходим по всем токенам
        if(tv->vector[i].type == TOK_BG) return true; // нашли '&'
    }
    return false; 
}
 
void newline_if_needed(void) { // перевод строки, только если курсор не в начале строки
    if(!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) return; // не терминал - запрос позиции бессмыслен
 
    struct termios oldt, raw; // старые и "сырые" настройки терминала
    if(tcgetattr(STDIN_FILENO, &oldt) < 0){ // не смогли прочитать настройки
        write(STDOUT_FILENO, "\n", 1); return; // запасной вариант: просто перевод строки
    }
 
    raw = oldt; // начинаем с текущих настроек
    raw.c_lflag &= ~(ICANON | ECHO); // без построчного режима и без эха: читаем байты сразу, ответ не печатается на экране
    raw.c_cc[VMIN]  = 0; // read может вернуть 0 байт
    raw.c_cc[VTIME] = 1; // если за 0,1 сек ничего не пришло (защита от зависания)
    tcsetattr(STDIN_FILENO, TCSANOW, &raw); // применяем немедленно
 
    const char q[] = "\x1b[6n"; // ANSI-escape последовательность запроса позиции курсора: ESC[6n
    //x1b[12;1R - курсор в 12 строке 1 столбце
    (void)write(STDOUT_FILENO, q, sizeof(q) - 1); // отправляем запрос терминалу (без завершающего '\0')
    (void)tcdrain(STDOUT_FILENO); // ждём, пока запрос реально уйдёт
 
    char buf[64]; // сюда придёт ответ вида ESC[строка;столбецR
    int n = 0; // сколько байт прочитано
    while(1) { // читаем ответ кусками
        int r = (int)read(STDIN_FILENO, buf + n, (sizeof(buf) - 1) - n); // оставляем место под '\0'
        if(r <= 0) break; // таймаут или ошибка
        n += r; // накопили
        if(n >= 4 && buf[n-1] == 'R') break; //R - конец ответа
    }
    buf[n] = 0; // завершаем строку нулём
    tcsetattr(STDIN_FILENO, TCSANOW, &oldt); // возвращаем исходные настройки терминала
    int row = 0, col = 0; // позиция курсора
    if(n > 0 && sscanf(buf, "\x1b[%d;%dR", &row, &col) == 2) { // удалось разобрать два числа
        (void)row; // строка нам не нужна
        if(col != 1) write(STDOUT_FILENO, "\n", 1); // курсор не в первом столбце - переходим на новую строку
    } else { // ответа нет 
        write(STDOUT_FILENO, "\n", 1); // безопасный запасной вариант
    }
}
//Общая обработка одной командной строки: лексер -> парсер -> (если execute)
static int process_line(char* line, bool execute){
    TokenVector tv = {0}; // пустой вектор токенов
    syntax_error_flag = false; //сбрасываем перед разбором каждой новой строки
    lexer(line, &tv); // текст -> токены (здесь же подставляется $?)
 
    Parser p = { // состояние парсера
        .tv = &tv, // откуда брать токены
        .position = 0 // начинаем с первого
    };
 
    Node* ast = NULL; // корень дерева разбора
    if(!syntax_error_flag){ // лексер ошибок не нашёл
        ast = parse_line(&p); // токены -> дерево
    }
    if(!syntax_error_flag && see(&p)->type != TOK_END){ // парсер остановился не на конце
        report_syntax_error("лишние символы после команды"); // сообщение и флаг ошибки
    }
 
    int status; // итоговый код
    if(syntax_error_flag){ // была синтаксическая ошибка
        status = 2; // как в bash
    } else if(execute && ast){ // есть что выполнять
        status = run_node(ast, line); // выполняем дерево; line нужна для имени задания
    } else { // пустая строка / только разбор
        status = 0;
    }
 
    free_node(ast); // освобождаем дерево (argv, редиректы, узлы)
 
    //очистка памяти после токенизации
    for(size_t i = 0; i < tv.n; i++){ // обходим токены
        if(tv.vector[i].type == TOK_WORD && tv.vector[i].value){ // строка есть только у слов
            free(tv.vector[i].value); // освобождаем (дерево хранит свои копии)
        }
    }
    free(tv.vector); // сам массив токенов
 
    return status; // код возврата
}
static void print_tokens(TokenVector* tv){ // печать списка токенов
    for(size_t i = 0; i < tv->n; i++){ // по всем токенам
        Token t = tv->vector[i]; // копия токена
        if(t.type == TOK_WORD){ // слово
            printf("%-18s value=\"%s\"\n", token_type_name(t.type), t.value); // имя типа, выровненное по 18 символов, и значение
        } else if(t.fd >= 0){ // токен с номером дескриптора
            printf("%-18s fd=%d\n", token_type_name(t.type), t.fd);
        } else { // остальные токены (|, &&, ;, конец)
            printf("%s\n", token_type_name(t.type));
        }
    }
}
 
static int dump_tokens_mode(void){ // режим --dump-tokens
    char* line = NULL; // буфер строки (getline выделит сам)
    size_t cap = 0; // его ёмкость
    ssize_t got = getline(&line, &cap, stdin); // читаем одну строку
    if(got < 0){ free(line); return 0; } // ввода нет - выходим
    cutter(line); // убираем '\n'
 
    TokenVector tv = {0}; // вектор токенов
    syntax_error_flag = false; // сброс флага
    lexer(line, &tv); // токенизация
 
    print_tokens(&tv); // печать
    int status = syntax_error_flag ? 2 : 0; // 2 при ошибке лексера
 
    for(size_t i = 0; i < tv.n; i++){ // освобождаем строки слов
        if(tv.vector[i].type == TOK_WORD && tv.vector[i].value) free(tv.vector[i].value);
    }
    free(tv.vector); // массив токенов
    free(line); // исходная строка
    return status; // становится кодом выхода программы
}
 
static const char* node_type_name(NodeType t){ // имя типа узла для печати дерева
    switch(t){
        case NODE_CMD:       return "CMD";
        case NODE_PIPE:      return "PIPE";
        case NODE_AND:       return "AND";
        case NODE_OR:        return "OR";
        case NODE_SEQ:       return "SEQ";
        case NODE_BG:        return "BG";
        case NODE_MINISHELL: return "MINISHELL";
    }
    return "?";
}
 
static void print_ast_node(Node* n, int depth){ // рекурсивная печать дерева
    if(!n) return; // пустой узел - конец рекурсии
    for(int i = 0; i < depth; i++) printf("  "); // отступ по глубине
    printf("%s", node_type_name(n->type)); // имя типа
 
    if(n->type == NODE_CMD && n->argv){ // у команды печатаем аргументы
        printf(" argv=[");
        for(int i = 0; n->argv[i]; i++) printf("%s%s", i ? " " : "", n->argv[i]); // пробел между, но не перед первым
        printf("]");
    }
    if(n->redirs){ // и редиректы, если есть
        printf(" redirs=[");
        for(Redir* r = n->redirs; r; r = r->next){ // обход связного списка
            printf("(type=%d src=%d target=%s) ", r->type, r->src, r->target ? r->target : "(null)"); // target бывает NULL (например 2>&1)
        }
        printf("]");
    }
    printf("\n");
 
    print_ast_node(n->left, depth + 1); // сначала левый потомок
    print_ast_node(n->right, depth + 1); // потом правый
}
 
static int dump_ast_mode(void){ // режим --dump-ast
    char* line = NULL; // буфер строки
    size_t cap = 0;
    ssize_t got = getline(&line, &cap, stdin); // одна строка из stdin
    if(got < 0){ free(line); return 0; } // ввода нет
    cutter(line); // убираем '\n'
 
    TokenVector tv = {0};
    syntax_error_flag = false;
    lexer(line, &tv); // токены
 
    Parser p = { .tv = &tv, .position = 0 }; // парсер с начала
    Node* ast = NULL;
    if(!syntax_error_flag){ // лексер ошибок не нашёл
        ast = parse_line(&p); // строим дерево
    }
    if(!syntax_error_flag && see(&p)->type != TOK_END){ // остались лишние токены
        report_syntax_error("лишние символы после команды");
    }
 
    int status;
    if(syntax_error_flag){ // ошибка
        status = 2;
    } else { // печатаем дерево
        print_ast_node(ast, 0);
        status = 0;
    }
 
    free_node(ast); // дерево
    for(size_t i = 0; i < tv.n; i++){ // строки слов
        if(tv.vector[i].type == TOK_WORD && tv.vector[i].value) free(tv.vector[i].value);
    }
    free(tv.vector);
    free(line);
    return status;
}
//теперь задача за малым, нужно лишь собрать все воедино
int main(int argc, char** argv){
    //--dump-tokens / --dump-ast обрабатываются ДО инициализации терминала и
    //job control - это чисто отладочная печать, процессы не запускаются
    if(argc >= 2 && strcmp(argv[1], "--dump-tokens") == 0){ // argc >= 2: иначе argv[1] == NULL
        return dump_tokens_mode();
    }
    if(argc >= 2 && strcmp(argv[1], "--dump-ast") == 0){
        return dump_ast_mode();
    }
 
    char* dash_c_command = NULL; // строка для режима -c (NULL = режим не включён)
    if(argc >= 2 && strcmp(argv[1], "-c") == 0){ // ./mybash -c '...'
        if(argc < 3){ // после -c нет команды
            fprintf(stderr, "mysh: -c: требуется аргумент - командная строка\n");
            return 2;
        }
        dash_c_command = argv[2]; // копировать не нужно: argv живёт всё время работы
    }
 
    shell_terminal = STDIN_FILENO; // терминал шелла - дескриптор 0
 
    if(isatty(shell_terminal)){ // job control нужен только на терминале
        while(tcgetpgrp(shell_terminal) != (shell_pgid = getpgrp())){ // пока наша группа не на переднем плане
            kill(-shell_pgid, SIGTTIN); //отправляем всем процессам, что занято
        }
        shell_pgid = getpid(); // лидер собственной группы
        if(setpgid(shell_pgid, shell_pgid) < 0) perror("setpgid"); // создаём группу pgid = pid
        tcsetpgrp(shell_terminal, shell_pgid); // отдаём терминал своей группе
        tcgetattr(shell_terminal, &shell_settings); // запоминаем настройки терминала для последующего восстановления
    }
 
    struct sigaction sa = {0}; // описание обработчика сигнала
    sa.sa_handler = on_child; // функция из jobs.c
    sigemptyset(&sa.sa_mask); //очищаем набор блокируемых сигналов внутри этой структуры
    sa.sa_flags = SA_RESTART; //прерванные системные вызовы автоматически перезапускаются
    sigaction(SIGCHLD, &sa, NULL); // устанавливаем обработчик завершения/остановки детей
    signal(SIGINT,  SIG_IGN); // Ctrl+C не убивает шелл
    signal(SIGTSTP, SIG_IGN); // Ctrl+Z не останавливает шелл
    signal(SIGQUIT, SIG_IGN); // Ctrl+\ не убивает шелл
    signal(SIGTTIN, SIG_IGN); // шелл не останавливается при чтении из терминала не с переднего плана
    signal(SIGTTOU, SIG_IGN); // и при записи/смене настроек терминала
    char* exe_path = read_exe_path(); // путь к исполняемому файлу шелла
    if(exe_path){ // получилось определить
        setenv("SHELL", exe_path, 1); //если существует в окружении, перезаписать её новым значением
        free(exe_path); // setenv скопировал строку
    }
    bool interactive = isatty(STDIN_FILENO); // интерактивный режим = stdin терминал
    shell_interactive = interactive; //делаем видимым и для builtin'ов (fg/bg) из jobs.h
    //last_exit_status - глобальная переменная из token.h/.c: код возврата последней
    //команды. Используется и для подстановки $?, и как итоговый код самого
    //интерпретатора при завершении (см. ТЗ)
 
    //режим -c 'команда': выполняем ровно одну строку и завершаемся - без
    //приглашения и без цикла чтения. По ТЗ "-c" - это ВСЕГДА неинтерактивный
    //режим, вне зависимости от того, подключён ли stdin к терминалу
    if(dash_c_command){ // режим -c
        shell_interactive = false; // принудительно неинтерактивный
        last_exit_status = process_line(dash_c_command, true); // выполняем одну строку
        for(int i = 0; i < jobs.count; i++){ free(jobs.vector[i].cmdline); free(jobs.vector[i].procs); } // освобождаем имена заданий и списки их процессов
        free(jobs.vector); // и таблицу
        return last_exit_status; // код последней команды
    }
    while(true){//основной цикл
        if(interactive){ // только на терминале
            //после конвейера/подшелла терминал мог остаться за их группой процессов -
            //возвращаем его шеллу, иначе чтение ввода идёт из фоновой группы и падает
            tcsetpgrp(shell_terminal, shell_pgid); // терминал снова у шелла
            tcsetattr(shell_terminal, TCSADRAIN, &shell_settings); // и его настройки восстановлены
        }
        get_children(); // собираем статусы завершившихся детей (без зомби)
        jobs_remove(); // убираем завершённые задания из таблицы
        maybe_soft_block_prompt(); // после запуска фона чуть ждём, чтобы не затереть вывод "[1] pid"
        char* line = NULL; // буфер строки ввода
        size_t cap = 0; // его ёмкость
        ssize_t got; // сколько прочитано (<0 - конец ввода)
        if(interactive){ // на терминале: неканонический ввод со стрелками и историей
            char* string = welcome_string(); // строка приглашения
            fflush(stdout); // сначала сбрасываем буфер stdout, чтобы вывод команд не оказался после приглашения
            newline_if_needed(); // не прилипаем к выводу без '\n'
            line = rl_readline(string); // приглашение печатает сама rl_readline
            free(string); // приглашение больше не нужно
            got = line ? (ssize_t)strlen(line) : -1; // NULL = Ctrl+D на пустой строке
        } else { // pipe/файл/скрипт: обычное построчное чтение
            got = getline(&line, &cap, stdin); // читаем строку любой длины
        }
        if(got < 0){ //конец файла (Ctrl+D) или ошибка чтения
            free(line); // освобождаем буфер
            if(interactive) putchar('\n'); // чтобы внешний терминал начал с новой строки
            break; // выходим из цикла
        }
        cutter(line); // убираем '\n' в конце
        if(*line == '\0'){ // пустая строка (просто Enter)
            free(line);
            continue; // сразу новое приглашение
        }
        get_children(); //пока ждали ввод, фоновые процессы могли завершиться: убираем зомби ДО запуска команды (иначе ps покажет <defunct>)
        last_exit_status = process_line(line, true); // выполняем и запоминаем статус (для $? и итогового кода)
        if(last_exit_status == 2 && syntax_error_flag && !interactive){ // синтаксическая ошибка в неинтерактивном режиме
            //неинтерактивный режим: синтаксическая ошибка прерывает выполнение
            //(диагностика уже напечатана внутри process_line -> лексер/парсер)
            free(line);
            break;
        }
        free(line); // строка выполнена - освобождаем
    }
    for(int i = 0; i < jobs.count; i++){ // очистка перед выходом
        free(jobs.vector[i].cmdline); // имя каждого задания
        free(jobs.vector[i].procs); // и список его процессов
    }
    free(jobs.vector); // таблица заданий
    rl_history_free(); // история введённых команд
    return last_exit_status; // итоговый код шелла - статус последней команды
}
