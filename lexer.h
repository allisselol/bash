#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "token.h"

//лексер
typedef struct{
    char* s;
    size_t position;
    size_t length;
} Cursor;

//является ли текущий символ концом строки
bool cur_eof(Cursor* c);

//посмотреть текущий символ (не двигая курсор)
int cur_see(Cursor* c);

//получить текущий символ и сдвинуть курсор
int cur_get(Cursor* c);

//скипнуть пробелы
void skip_spaces(Cursor* c);

//проверка: стоит ли в текущей позиции строка z; если да - "проглотить" её
bool match_symbol(Cursor* c, char* z);

//прочитать число (номер файлового дескриптора перед редиректом). -1, если числа нет
int read_number(Cursor* c);

//выполнить команду через popen и вернуть её вывод (для $() и `...`)
char* read_command(char* command);

//раскрыть ~ и ~user в начале строки в полный путь
char* tilda_koren(char* string);

//прочитать "слово": обрабатывает кавычки, экранирование, $VAR, ${VAR}, $(), `...`, ~
char* read_words(Cursor* c);

//главная функция лексера: разбирает всю строку line на токены и кладёт их в tv
void lexer(char* line, TokenVector* tv);
