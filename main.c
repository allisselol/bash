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

    char* line = NULL;

    if(asprintf(&line, "%s:%s$ ", user, line_part) < 0){
        line = er_strdup("$ ");
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

//теперь задача за малым, нужно лишь собрать все воедино
int main(void){
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

    using_history();
    stifle_history(1000);
    char* home = getenv("HOME");
    if(!home) home = "";
    char* history_of_file = path_join(home, ".my_history");

    FILE* historyfile = fopen(history_of_file, "a+");
    if(historyfile) fclose(historyfile);
    read_history(history_of_file);

    char* exe_path = read_exe_path();
    if(exe_path){
        setenv("SHELL", exe_path, 1); //если существует в окружении, перезаписать её новым значением
        free(exe_path);
    }

    //основной цикл
    while(true){
        get_children();
        jobs_remove();

        maybe_soft_block_prompt();

        char* string = welcome_string();
        newline_if_needed();
        char* line = readline(string);
        free(string);

        if(!line){
            putchar('\n');
            break;
        }
        cutter(line);
        if(*line == '\0'){
            free(line);
            continue;
        }

        add_history(line);
        TokenVector tv = {0};
        lexer(line, &tv);

        Parser p = {
            .tv = &tv,
            .position = 0
        };

        Node* ast = parse_line(&p);

        if(ast){
            (void)run_node(ast, line);
            free_node(ast);
        }

        //очистка памяти после токенизации
        for(size_t i = 0; i < tv.n; i++){
            if(tv.vector[i].type == TOK_WORD && tv.vector[i].value){
                free(tv.vector[i].value);
            }
        }
        free(tv.vector);
        free(line);
    }

    write_history(history_of_file);
    free(history_of_file);

    for(int i = 0; i < jobs.count; i++){
        free(jobs.vector[i].cmdline);
    }

    free(jobs.vector);
    return 0;
}
