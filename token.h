#pragma once
#include <stddef.h>
#include <stdbool.h>
typedef enum{
    TOK_WORD,//обычное слово, аргумент либо же команда
    TOK_PIPE,
    TOK_AND,
    TOK_OR,
    TOK_SEMI,//;
    TOK_BG,
    TOK_LPAREN, TOK_RPAREN,
    TOK_REDIR_IN, TOK_REDIR_OUT, TOK_REDIR_OUT_APP,// <, >, >>
    TOK_HEREDOC, TOK_HERESTR, // << <<<
    TOK_ALL_TO_FILE, TOK_DUP_OUT, TOK_DUP_IN,// &> >&N <&N
    TOK_FD_REDIR_OUT, TOK_FD_REDIR_OUT_APP, TOK_FD_REDIR_IN, // N> N>> <N
    TOK_END
} TokenType;
typedef struct{
    TokenType type;
    char* value;
    int fd;
    int fd2;
} Token;

typedef struct{
    Token* vector;
    size_t n;
    size_t capacity;
} TokenVector;
void tv_push(TokenVector* tv, Token t);
const char* token_type_name(TokenType type);
extern bool syntax_error_flag;
void report_syntax_error(const char* msg);
extern int last_exit_status;
