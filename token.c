#include "common.h" // _POSIX_C_SOURCE 200809L должен быть определён ДО системных заголовков
#include "token.h"
#include "memory.h"
#include <stdio.h>

//функция добавления токена в список(массив)(вектор) токенов
void tv_push(TokenVector* tv, Token t){
    if(tv->n == tv->capacity){
        if(tv->capacity){
            tv->capacity *= 2;
        } else {
            tv->capacity = 64;
        }
        tv->vector = er_realloc(tv->vector, tv->capacity * sizeof(Token));
    }
    tv->vector[tv->n++] = t;
}

//получить название типа токена строкой (для отладочной печати)
const char* token_type_name(TokenType type){
    switch(type){
        case TOK_WORD: return "WORD";
        case TOK_PIPE: return "PIPE";
        case TOK_AND: return "AND";
        case TOK_OR: return "OR";
        case TOK_SEMI: return "SEMI";
        case TOK_BG: return "BG";
        case TOK_LPAREN: return "LPAREN";
        case TOK_RPAREN: return "RPAREN";
        case TOK_REDIR_IN: return "REDIR_IN";
        case TOK_REDIR_OUT: return "REDIR_OUT";
        case TOK_REDIR_OUT_APP: return "REDIR_OUT_APP";
        case TOK_HEREDOC: return "HEREDOC";
        case TOK_HERESTR: return "HERESTR";
        case TOK_ALL_TO_FILE: return "ALL_TO_FILE";
        case TOK_DUP_OUT: return "DUP_OUT";
        case TOK_DUP_IN: return "DUP_IN";
        case TOK_FD_REDIR_OUT: return "FD_REDIR_OUT";
        case TOK_FD_REDIR_OUT_APP: return "FD_REDIR_OUT_APP";
        case TOK_FD_REDIR_IN: return "FD_REDIR_IN";
        case TOK_END: return "END";
    }
    return "UNKNOWN";
}

bool syntax_error_flag = false;

//напечатать диагностическое сообщение об ошибке синтаксиса и поднять syntax_error_flag
void report_syntax_error(const char* msg){
    fprintf(stderr, "mysh: синтаксическая ошибка: %s\n", msg);
    syntax_error_flag = true;
}

int last_exit_status = 0;
