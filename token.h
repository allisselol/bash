#pragma once
#include <stddef.h>
#include <stdbool.h>

// Токены
typedef enum{
    TOK_WORD,     //обычное слово, аргумент либо же команда
    TOK_PIPE,
    TOK_AND,
    TOK_OR,
    TOK_SEMI,  //;
    TOK_BG,
    TOK_LPAREN, TOK_RPAREN, // ( )
    TOK_REDIR_IN, TOK_REDIR_OUT, TOK_REDIR_OUT_APP,  // <, >, >>
    TOK_HEREDOC, TOK_HERESTR, // << <<<
    TOK_ALL_TO_FILE, TOK_DUP_OUT, TOK_DUP_IN,  // &> >&N <&N
    TOK_FD_REDIR_OUT, TOK_FD_REDIR_OUT_APP, TOK_FD_REDIR_IN, // N> N>> <N
    TOK_END
} TokenType;

//структура токена
typedef struct{
    TokenType type;
    char* value;
    int fd;
} Token;

typedef struct{
    Token* vector;
    size_t n;
    size_t capacity;
} TokenVector;

//функция добавления токена в список(массив)(вектор) токенов
void tv_push(TokenVector* tv, Token t);

//получить название типа токена строкой (для отладочной печати)
const char* token_type_name(TokenType type);

//общий флаг синтаксической ошибки: его может выставить и лексер (недопустимый
//символ), и парсер (неожиданный токен/конструкция). Используется вместо
//аварийного завершения всего процесса (die()) - main.c проверяет этот флаг
//после разбора каждой строки и решает: напечатать диагностику и продолжить
//(интерактивный режим) или завершиться с кодом 2 (неинтерактивный режим).
extern bool syntax_error_flag;

//напечатать диагностическое сообщение об ошибке синтаксиса и поднять syntax_error_flag
void report_syntax_error(const char* msg);

//код возврата последней выполненной команды - он же $? для подстановки в лексере,
//он же итоговый код возврата самого интерпретатора при завершении (см. ТЗ)
extern int last_exit_status;
