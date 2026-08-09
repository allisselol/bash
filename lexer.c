#include "lexer.h"
#include "memory.h"
#include <ctype.h>
#include <string.h>
#include <stdio.h>  // EOF, fprintf
#include <stdlib.h> // free

//является ли текущий символ концом строки
bool cur_eof(Cursor* c){
    return (c->position >= c->length);
}

//посмотреть текущий символ
int cur_see(Cursor* c){
    if(cur_eof(c)){
        return EOF;
    } else {
        return (unsigned char)c->s[c->position];
    }
}

//получить текущий символ
int cur_get(Cursor* c){
    if(cur_eof(c)){
        return EOF;
    } else {
        int symbol = (unsigned char)c->s[c->position];
        c->position++;
        return symbol;
    }
}

//скипнуть пробелы
void skip_spaces(Cursor* c){
    while(!cur_eof(c) && isspace((unsigned char)c->s[c->position])){
        c->position++;
    }
}

//проверка строки с подстрокой
bool match_symbol(Cursor* c, char* z){
    size_t len = strlen(z);
    if(c->position + len > c->length) return false;
    if(strncmp(c->s + c->position, z, len) == 0){
        c->position += len;
        return true;
    }
    return false;
}

//словесный парсер кавычек мы типа прописали, теперь наша задача прописать чтение цифр
//возвращает -1 если что-то не так
int read_number(Cursor* c){
    int value = 0;
    size_t n = 0;
    while(!cur_eof(c) && isdigit(cur_see(c))){
        value = value*10 + (cur_get(c) - '0');
        n++;
    }

    if(!n) return -1;

    return value;
}

//ВРЕМЕННАЯ упрощённая версия read_words: читает символы до пробела/оператора.
//На следующем подшаге заменим на полноценную версию с кавычками, $VAR, $(), тильдой.
char* read_words(Cursor* c){
    size_t capacity = 64, count = 0;
    char* buffer = er_malloc(capacity);

    while(!cur_eof(c)){
        int symbol = cur_see(c);
        if(isspace(symbol) || strchr("|&;()<>", symbol)) break;
        cur_get(c);
        if((count + 2) >= capacity){
            capacity *= 2;
            buffer = er_realloc(buffer, capacity);
        }
        buffer[count++] = (char)symbol;
        buffer[count] = '\0';
    }

    if(count == 0){
        free(buffer);
        return NULL;
    }
    return buffer;
}

//наконец прописываем сам лексер
void lexer(char* line, TokenVector* tv){
    Cursor cur = {line, 0, strlen(line)};
    while(!cur_eof(&cur)){
        skip_spaces(&cur);
        if(cur_eof(&cur)) break;
        if(cur_see(&cur) == '#') break;

        if(match_symbol(&cur, "&&")){   //если попадание match_symbol как и все другие функции сдвигает каретку(курсор)(указатель) нашей строки
            tv_push(tv, (Token){TOK_AND, NULL, -1});
            continue;
        }

        if(match_symbol(&cur, "||")){
            tv_push(tv, (Token){TOK_OR, NULL, -1});
            continue;
        }

        if(match_symbol(&cur, "<<<")){
            tv_push(tv, (Token){TOK_HERESTR, NULL, -1});  //передает строку на stdin команды echo "smth" | cat - доп pipe
            continue;
        }

        if(match_symbol(&cur, "<<")){
            tv_push(tv, (Token){TOK_HEREDOC, NULL, -1});
            continue;
        }

        if(match_symbol(&cur, ">>")){
            tv_push(tv, (Token){TOK_REDIR_OUT_APP, NULL, -1});  //дозапись в конец файла
            continue;
        }

        if(match_symbol(&cur, "&>")){
            tv_push(tv, (Token){TOK_ALL_TO_FILE, NULL, -1});  //И std_out и ошибки std_err в один файл
            continue;
        }

        if(match_symbol(&cur, ">&")){  //дублирует std_out в файловый дескриптор с нужным номером >&N
            int number = read_number(&cur);
            if(number >= 0){
                if(number > 2) {
                    fprintf(stderr, "Недопустимый формат дескриптора\n");
                }
                tv_push(tv, (Token){TOK_DUP_OUT, NULL, number});
                continue;
            }
            tv_push(tv, (Token){TOK_WORD, er_strdup(">&"), -1});  //передаем как слово
            continue;
        }

        if(match_symbol(&cur, "<&")){  //Дублирует std_in // допустим <&3 читаем ввод из другого дескриптора
            int number = read_number(&cur);
            if(number >= 0){
                if(number > 2) {
                    fprintf(stderr, "Недопустимый формат дескриптора\n");
                }
                tv_push(tv, (Token){TOK_DUP_IN, NULL, number});
                continue;
            }
            tv_push(tv, (Token){TOK_WORD, er_strdup("<&"), -1});  //передаем как слово
            continue;
        }

        //однокомандные

        if(match_symbol(&cur, "|")){
            tv_push(tv, (Token){TOK_PIPE, NULL, -1});
            continue;
        }

        if(match_symbol(&cur, ";")){
            tv_push(tv, (Token){TOK_SEMI, NULL, -1});
            continue;
        }

        if(match_symbol(&cur, "&")){
            tv_push(tv, (Token){TOK_BG, NULL, -1});
            continue;
        }

        //отдельный дочерний баш(шел), со своей отдельной директорией и окружением
        if(match_symbol(&cur, "(")){
            tv_push(tv, (Token){TOK_LPAREN, NULL, -1});
            continue;
        }

        if(match_symbol(&cur, ")")){
            tv_push(tv, (Token){TOK_RPAREN, NULL, -1});
            continue;
        }

        if(match_symbol(&cur, ">")){
            tv_push(tv, (Token){TOK_REDIR_OUT, NULL, -1});  //перенаправляет стандартный std_out echo smth > file.txt
            continue;
        }

        if(match_symbol(&cur, "<")){
            tv_push(tv, (Token){TOK_REDIR_IN, NULL, -1});  //перенаправляет std_in ws -l < file.txt
            continue;
        }

        size_t save_position = cur.position;
        int fd = read_number(&cur);
        if(fd >= 0){
            if(match_symbol(&cur, ">>")){
                if(fd > 2) fprintf(stderr, "Недопустимый файловый дескриптор\n");
                tv_push(tv, (Token){TOK_FD_REDIR_OUT_APP, NULL, fd}); //тоже самое что и N> только дозапись
                continue;
            }
            if(match_symbol(&cur, ">")){
                if(fd > 2) fprintf(stderr, "Недопустимый файловый дескриптор\n");
                tv_push(tv, (Token){TOK_FD_REDIR_OUT, NULL, fd}); // echo privet 1> file.txt перезапишется в файл вместо вывода в консоль
                continue;
            }
            if(match_symbol(&cur, "<")){
                if(fd > 2) fprintf(stderr, "Недопустимый файловый дескриптор\n");
                tv_push(tv, (Token){TOK_FD_REDIR_IN, NULL, fd}); // Ввод с файла в дескриптор допустим sort (0< file.txt) - обычный ввод заменяется чтение из файла
                continue;
            }
            cur.position = save_position;
        }

        char* word = read_words(&cur);
        if(word){
            tv_push(tv, (Token){TOK_WORD, word, -1});
            continue;
        }

        if(!cur_eof(&cur)) cur.position++;
    }

    tv_push(tv, (Token){TOK_END, NULL, -1});
}
