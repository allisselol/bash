#pragma once
#include <stdbool.h>
#include "token.h"
#include "node.h"

//первое подобие парсера, он будет строить дерево из наших токенов
typedef struct {
    TokenVector* tv;
    size_t position;    //позиция, на каком токене сейчас находимся
} Parser;

//посмотреть текущий токен (не двигаясь)
Token* see(Parser* p);

//получить текущий токен и сдвинуться
Token* get(Parser* p);

//если текущий токен нужного типа - "проглотить" его и вернуть true
bool accept(Parser* p, TokenType token);

//ожидает токен нужного типа. При несовпадении печатает диагностику через
//report_syntax_error(), поднимает syntax_error_flag и возвращает false -
//вместо аварийного die(), которое раньше валило весь процесс интерпретатора
bool expect(Parser* p, TokenType token);

//читает многострочный ввод до строки-разделителя (для heredoc <<EOF)
char* heredoc(char* end_pointer);

//ожидает и возвращает следующее слово. При ошибке (не TOK_WORD) печатает
//диагностику, поднимает syntax_error_flag и возвращает NULL
char* expect_word(Parser* p);

//прочитать целевой файл для файлового редиректа и добавить его в узел
void add_redir_file(Parser* p, Node* node, RedirType rt, int src);

//команда с аргументами и редиректами
Node* parse_simple(Parser* p);

//сабшелл в круглых скобках (...)
Node* parse_minishell(Parser* p);

//простая команда или сабшелл
Node* parse_command(Parser* p);

// |
Node* parse_pipeline(Parser* p);

// && ||
Node* parse_and_or(Parser* p);

// ; &
Node* parse_line(Parser* p);
