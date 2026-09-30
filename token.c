#include "common.h" // _POSIX_C_SOURCE 200809L должен быть определён ДО системных заголовков
#include "token.h"  
#include "memory.h" 
#include <stdio.h>  
// Добавляет токен t в конец динамического массива tv (растущий буфер токенов)
void tv_push(TokenVector* tv, Token t){//функция добавления токена в список токенов
    if(tv->n == tv->capacity){  // массив заполнен до предела выделенной памяти
        if(tv->capacity){  // если уже было выделено хоть что-то
            tv->capacity *= 2;        
        } else {
            tv->capacity = 64; 
        }
        tv->vector = er_realloc(tv->vector, tv->capacity * sizeof(Token)); // перевыделяем память под новую ёмкость
    }
    tv->vector[tv->n++] = t;
}
//получить название типа токена строкой (для отладочной печати)
const char* token_type_name(TokenType type){
    switch(type){
        case TOK_WORD: return "WORD"; 
        case TOK_PIPE: return "PIPE";// |
        case TOK_AND: return "AND"; // &&
        case TOK_OR: return "OR";  // ||
        case TOK_SEMI: return "SEMI";  // ;
        case TOK_BG: return "BG";// &
        case TOK_LPAREN: return "LPAREN";// (
        case TOK_RPAREN: return "RPAREN"; // )
        case TOK_REDIR_IN: return "REDIR_IN"; // 
        case TOK_REDIR_OUT: return "REDIR_OUT";// >
        case TOK_REDIR_OUT_APP: return "REDIR_OUT_APP"; // >>
        case TOK_HEREDOC: return "HEREDOC"; // 
        case TOK_HERESTR: return "HERESTR";  // <
        case TOK_ALL_TO_FILE: return "ALL_TO_FILE"; // &> (весь вывод в файл)
        case TOK_DUP_OUT: return "DUP_OUT";  // >&
        case TOK_DUP_IN: return "DUP_IN"; // <&
        case TOK_FD_REDIR_OUT: return "FD_REDIR_OUT"; // N>
        case TOK_FD_REDIR_OUT_APP: return "FD_REDIR_OUT_APP";// N>>
        case TOK_FD_REDIR_IN: return "FD_REDIR_IN";  // N
        case TOK_END: return "END"; // конец потока токенов
    }return "UNKNOWN"; // защита: если добавили новый TokenType, но забыли case - не падаем и не читаем мусор
}
bool syntax_error_flag = false; // глобальный флаг: была ли синтаксическая ошибка при разборе текущей строки
//напечатать диагностическое сообщение об ошибке синтаксиса и поднять syntax_error_flag
void report_syntax_error(const char* msg){
    fprintf(stderr, "mysh: синтаксическая ошибка: %s\n", msg); // единая точка вывода всех синтаксических ошибок
    syntax_error_flag = true; // сигнал парсеру/main: разбор прерван, дальше не строим
}
int last_exit_status = 0; // код завершения последней выполненной команды (аналог $? в bash), старт - 0 (успех)
