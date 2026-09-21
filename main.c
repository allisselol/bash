#include "common.h"
#include "memory.h"
#include "utils.h"
#include "token.h"
#include "lexer.h"
#include "node.h"
#include "parser.h"
#include "jobs.h"
#include "exec.h"

//для строки приглашения
char* welcome_string(void){
    char* cwd = gettingthiscwd();

    char* line_part;
    if(cwd != NULL){
        line_part = cwd;
    } else {
        line_part = "?";
    }

    char* user = getenv("USER");
    if(!user){
        user = "user";
    }

    //переносимая замена asprintf (GNU-расширение, не входит в строгий POSIX):
    //сначала узнаём нужную длину через snprintf(NULL, 0, ...), потом выделяем и печатаем
    int needed = snprintf(NULL, 0, "%s:%s$ ", user, line_part);
    char* line;
    if(needed < 0){
        line = er_strdup("$ ");
    } else {
        line = er_malloc((size_t)needed + 1);
        snprintf(line, (size_t)needed + 1, "%s:%s$ ", user, line_part);
    }

    free(cwd);
    return line;
}

// теперь проверка на bg
bool line_has_bg_tok(TokenVector* tv){
    for(size_t i = 0; i < tv->n; i++){
        if(tv->vector[i].type == TOK_BG) return true;
    }
    return false;
}

void newline_if_needed(void) {
    if(!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) return;

    struct termios oldt, raw;
    if(tcgetattr(STDIN_FILENO, &oldt) < 0){
        write(STDOUT_FILENO, "\n", 1); return;
    }

    raw = oldt;
    raw.c_lflag &= ~(ICANON | ECHO);
    raw.c_cc[VMIN]  = 0;
    raw.c_cc[VTIME] = 1;
    tcsetattr(STDIN_FILENO, TCSANOW, &raw);

    const char q[] = "\x1b[6n"; //это байт ESC (escape), код 0x1B, [ начало CSI (Control Sequence Introducer), Cursor Position Report.
    //x1b[12;1R - курсор в 12 строке 1 столбце
    (void)write(STDOUT_FILENO, q, sizeof(q) - 1);
    (void)tcdrain(STDOUT_FILENO);

    char buf[64];
    int n = 0;
    while(1) {
        int r = (int)read(STDIN_FILENO, buf + n, (sizeof(buf) - 1) - n);
        if(r <= 0) break;
        n += r;
        if(n >= 4 && buf[n-1] == 'R') break;
    }
    buf[n] = 0;

    tcsetattr(STDIN_FILENO, TCSANOW, &oldt);

    int row = 0, col = 0;
    if(n > 0 && sscanf(buf, "\x1b[%d;%dR", &row, &col) == 2) {
        (void)row;
        if(col != 1) write(STDOUT_FILENO, "\n", 1);
    } else {
        write(STDOUT_FILENO, "\n", 1);
    }
}

//Общая обработка одной командной строки: лексер -> парсер -> (если execute)
//выполнение. Переиспользуется основным циклом и режимом -c. line не
//освобождается здесь - об этом заботится вызывающий код.
//Возвращает код возврата последней команды, либо 2 при синтаксической ошибке
//(диагностика в этом случае уже напечатана внутри лексера/парсера).
static int process_line(char* line, bool execute){
    TokenVector tv = {0};
    syntax_error_flag = false; //сбрасываем перед разбором каждой новой строки
    lexer(line, &tv);

    Parser p = {
        .tv = &tv,
        .position = 0
    };

    Node* ast = NULL;
    if(!syntax_error_flag){
        ast = parse_line(&p);
    }

    //по ТЗ: лишние, не разобранные токены после конца конструкции (например,
    //висящая закрывающая скобка без открывающей) - это тоже синтаксическая
    //ошибка, а не молчаливое игнорирование остатка строки
    if(!syntax_error_flag && see(&p)->type != TOK_END){
        report_syntax_error("лишние символы после команды");
    }

    int status;
    if(syntax_error_flag){
        status = 2;
    } else if(execute && ast){
        status = run_node(ast, line);
    } else {
        status = 0;
    }

    free_node(ast);

    //очистка памяти после токенизации
    for(size_t i = 0; i < tv.n; i++){
        if(tv.vector[i].type == TOK_WORD && tv.vector[i].value){
            free(tv.vector[i].value);
        }
    }
    free(tv.vector);

    return status;
}

//--- отладочные режимы: --dump-tokens / --dump-ast --------------------------
//Оба читают РОВНО ОДНУ строку из stdin, печатают результат разбора и
//завершают работу - не требуют инициализации терминала/job control,
//т.к. ничего не выполняют.

static void print_tokens(TokenVector* tv){
    for(size_t i = 0; i < tv->n; i++){
        Token t = tv->vector[i];
        if(t.type == TOK_WORD){
            printf("%-18s value=\"%s\"\n", token_type_name(t.type), t.value);
        } else if(t.fd >= 0){
            printf("%-18s fd=%d\n", token_type_name(t.type), t.fd);
        } else {
            printf("%s\n", token_type_name(t.type));
        }
    }
}

static int dump_tokens_mode(void){
    char* line = NULL;
    size_t cap = 0;
    ssize_t got = getline(&line, &cap, stdin);
    if(got < 0){ free(line); return 0; }
    cutter(line);

    TokenVector tv = {0};
    syntax_error_flag = false;
    lexer(line, &tv);

    print_tokens(&tv);
    int status = syntax_error_flag ? 2 : 0;

    for(size_t i = 0; i < tv.n; i++){
        if(tv.vector[i].type == TOK_WORD && tv.vector[i].value) free(tv.vector[i].value);
    }
    free(tv.vector);
    free(line);
    return status;
}

static const char* node_type_name(NodeType t){
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

static void print_ast_node(Node* n, int depth){
    if(!n) return;
    for(int i = 0; i < depth; i++) printf("  ");
    printf("%s", node_type_name(n->type));

    if(n->type == NODE_CMD && n->argv){
        printf(" argv=[");
        for(int i = 0; n->argv[i]; i++) printf("%s%s", i ? " " : "", n->argv[i]);
        printf("]");
    }
    if(n->redirs){
        printf(" redirs=[");
        for(Redir* r = n->redirs; r; r = r->next){
            printf("(type=%d src=%d target=%s) ", r->type, r->src, r->target ? r->target : "(null)");
        }
        printf("]");
    }
    printf("\n");

    print_ast_node(n->left, depth + 1);
    print_ast_node(n->right, depth + 1);
}

static int dump_ast_mode(void){
    char* line = NULL;
    size_t cap = 0;
    ssize_t got = getline(&line, &cap, stdin);
    if(got < 0){ free(line); return 0; }
    cutter(line);

    TokenVector tv = {0};
    syntax_error_flag = false;
    lexer(line, &tv);

    Parser p = { .tv = &tv, .position = 0 };
    Node* ast = NULL;
    if(!syntax_error_flag){
        ast = parse_line(&p);
    }
    if(!syntax_error_flag && see(&p)->type != TOK_END){
        report_syntax_error("лишние символы после команды");
    }

    int status;
    if(syntax_error_flag){
        status = 2;
    } else {
        print_ast_node(ast, 0);
        status = 0;
    }

    free_node(ast);
    for(size_t i = 0; i < tv.n; i++){
        if(tv.vector[i].type == TOK_WORD && tv.vector[i].value) free(tv.vector[i].value);
    }
    free(tv.vector);
    free(line);
    return status;
}
//-----------------------------------------------------------------------------

//теперь задача за малым, нужно лишь собрать все воедино
int main(int argc, char** argv){
    //--dump-tokens / --dump-ast обрабатываются ДО инициализации терминала и
    //job control - это чисто отладочная печать, процессы не запускаются
    if(argc >= 2 && strcmp(argv[1], "--dump-tokens") == 0){
        return dump_tokens_mode();
    }
    if(argc >= 2 && strcmp(argv[1], "--dump-ast") == 0){
        return dump_ast_mode();
    }

    char* dash_c_command = NULL;
    if(argc >= 2 && strcmp(argv[1], "-c") == 0){
        if(argc < 3){
            fprintf(stderr, "mysh: -c: требуется аргумент - командная строка\n");
            return 2;
        }
        dash_c_command = argv[2];
    }

    shell_terminal = STDIN_FILENO;

    if(isatty(shell_terminal)){
        while(tcgetpgrp(shell_terminal) != (shell_pgid = getpgrp())){
            kill(-shell_pgid, SIGTTIN); //отправляем всем процессам, что занято
        }
        shell_pgid = getpid();
        if(setpgid(shell_pgid, shell_pgid) < 0) perror("setpgid");
        tcsetpgrp(shell_terminal, shell_pgid);
        tcgetattr(shell_terminal, &shell_settings);
    }

    struct sigaction sa = {0};
    sa.sa_handler = on_child;
    sigemptyset(&sa.sa_mask); //очищаем набор блокируемых сигналов внутри этой структуры
    sa.sa_flags = SA_RESTART; //прерванные системные вызовы автоматически перезапускаются
    sigaction(SIGCHLD, &sa, NULL);

    signal(SIGINT,  SIG_IGN);
    signal(SIGTSTP, SIG_IGN);
    signal(SIGQUIT, SIG_IGN);
    signal(SIGTTIN, SIG_IGN);
    signal(SIGTTOU, SIG_IGN);

    char* exe_path = read_exe_path();
    if(exe_path){
        setenv("SHELL", exe_path, 1); //если существует в окружении, перезаписать её новым значением
        free(exe_path);
    }

    bool interactive = isatty(STDIN_FILENO);
    shell_interactive = interactive; //делаем видимым и для builtin'ов (fg/bg) из jobs.h
    //last_exit_status - глобальная переменная из token.h/.c: код возврата последней
    //команды. Используется и для подстановки $?, и как итоговый код самого
    //интерпретатора при завершении (см. ТЗ)

    //режим -c 'команда': выполняем ровно одну строку и завершаемся - без
    //приглашения и без цикла чтения. По ТЗ "-c" - это ВСЕГДА неинтерактивный
    //режим, вне зависимости от того, подключён ли stdin к терминалу
    if(dash_c_command){
        shell_interactive = false;
        last_exit_status = process_line(dash_c_command, true);

        for(int i = 0; i < jobs.count; i++) free(jobs.vector[i].cmdline);
        free(jobs.vector);
        return last_exit_status;
    }

    //основной цикл
    while(true){
        get_children();
        jobs_remove();

        maybe_soft_block_prompt();

        char* string = welcome_string();
        if(interactive){
            newline_if_needed();
            printf("%s", string);
            fflush(stdout); //приглашение должно появиться до того, как getline заблокируется на вводе
        }
        free(string);

        char* line = NULL;
        size_t cap = 0;
        ssize_t got = getline(&line, &cap, stdin);

        if(got < 0){ //конец файла (Ctrl+D) или ошибка чтения
            free(line);
            if(interactive) putchar('\n');
            break;
        }
        cutter(line);
        if(*line == '\0'){
            free(line);
            continue;
        }

        last_exit_status = process_line(line, true);

        if(last_exit_status == 2 && syntax_error_flag && !interactive){
            //неинтерактивный режим: синтаксическая ошибка прерывает выполнение
            //(диагностика уже напечатана внутри process_line -> лексер/парсер)
            free(line);
            break;
        }

        free(line);
    }

    for(int i = 0; i < jobs.count; i++){
        free(jobs.vector[i].cmdline);
    }

    free(jobs.vector);
    return last_exit_status;
}
