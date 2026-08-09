#include "lexer.h"
#include "memory.h"
#include "utils.h"
#include <ctype.h>
#include <string.h>
#include <stdio.h>  // EOF, fprintf, popen/pclose, fgetc
#include <stdlib.h> // free
#include <pwd.h>    // getpwnam

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

//Функция для чтения $()/`....` - команду внутри
char* read_command(char* command){
    char* cmd = NULL; //буфер для будущей команды в shell
    if(asprintf(&cmd, "sh -c '%s'", command) < 0) die();
    FILE* fp = popen(cmd, "r");
    free(cmd);
    if(!fp) return er_strdup("");

    size_t capacity = 256, count = 0;
    char* out = er_malloc(capacity);
    int symbol;
    while((symbol = fgetc(fp)) != EOF){
        if(count + 1 >= capacity){
            capacity *= 2;
            out = er_realloc(out, capacity);
        }
        out[count++] = (char)symbol;
    }
    out[count] = '\0';
    pclose(fp);
    cutter(out); //как правило команды всегда заканчивают вывод переводом на следующую строку
    return(out);
}

//функция для получения домашнего каталога(директории), полного пути к ней
char* tilda_koren(char* string){
    if(!string || string[0] != '~') return er_strdup(string);
    char* slash = strchr(string, '/');
    char* userpart = NULL;
    size_t userlen = 0;

    if(!slash){
        userpart = string + 1;
        userlen = strlen(userpart);
    } else {
        userpart = string + 1;
        userlen = (size_t)(slash - (string + 1));
    }

    char* home = NULL;
    if(userlen == 0){
        home = getenv("HOME");   //переменные окружения операционной системы
        if(!home) home = "";
    } else {
        char* copyname = strndup(userpart, userlen);
        struct passwd* pw = getpwnam(copyname);
        free(copyname);
        if(pw && pw->pw_dir){
            home = pw->pw_dir;
        } else {
            home = "";
        }
    }
    if(!slash) return er_strdup(home);
    char* r = path_join(home, slash+1);
    return r;
}

//Напишем функцию для чтения
//'....' - делают все обычным текстом
//"..." - делают все обычным текстом, кроме $VAR, $(...), `...` // \ - экранирование(\"$`)
char* read_words(Cursor* c){
    size_t capacity = 64, count = 0;
    char* buffer = er_malloc(capacity);
    bool squote = false, dquote = false, start = true; //в начале слова

    while(!cur_eof(c)){
        int symbol = cur_see(c);
        if(squote == false && dquote == false){
            if(isspace(symbol)) break; //если не внутри кавычек, то пробел - разделитель слова
            if(strchr("|&;()<>", symbol)) break;
            if(symbol == '#') break;
        }
        cur_get(c);

        if(dquote == false && symbol == '\''){
            squote = !squote;
            continue;
        }

        if(squote == false && symbol == '"'){
            dquote = !dquote;
            continue;
        }

        //экранирование
        if(squote == false && symbol == '\\'){
            if(!cur_eof(c)){
                symbol = cur_get(c);
            } else break;
        }

        //условие $
        else if (squote == false && symbol == '$'){
            //если получаем скобку ( --- $(...)
            if(!cur_eof(c) && cur_see(c) == '('){
                cur_get(c);
                size_t depth = 1;
                size_t cap = 128, n = 0;
                char* massive = er_malloc(cap);
                while(!cur_eof(c) && depth){
                    int x = cur_get(c);
                    if(x == '(') depth++;
                    else if(x == ')'){
                        depth--;
                        if(depth == 0) break;
                    }
                    if((n+1) >= cap){
                        cap *=2;
                        massive = er_realloc(massive, cap);
                    }
                    massive[n++] = (char)x;
                }
                massive[n] = '\0';
                if(depth != 0){
                    fprintf(stderr, "Неверный синтаксис\n");
                    free(massive);
                    start = false;
                    continue;
                }
                char* read = read_command(massive);
                free(massive);
                size_t len = strlen(read);
                while(count + len + 1 >= capacity){
                    capacity *= 2;
                    buffer = er_realloc(buffer, capacity);
                }
                memcpy(buffer + count, read, len);
                count += len;
                buffer[count] = '\0';
                free(read);
                start = false;
                continue;
            }

            //проверка на неверный синтаксис
            else if (!cur_eof(c) && cur_see(c) == ')'){
                fprintf(stderr, "Неверный синтаксис\n");
                cur_get(c);
                start = false;
                continue;
            }

            //если получили условие { --- ${Var}
            else if(!cur_eof(c) && cur_see(c) == '{'){
                cur_get(c);
                size_t cap = 128, n = 0;
                char* massive = er_malloc(cap);
                while(!cur_eof(c) && cur_see(c) != '}'){
                    int x = cur_get(c);
                    if((n+1) >= cap){
                        cap *=2;
                        massive = er_realloc(massive, cap);
                    }
                    massive[n++] = (char)x;
                }
                if (cur_eof(c) || cur_see(c) != '}'){
                    fprintf(stderr, "Неверный синтаксис\n");
                    free(massive);
                    start = false;
                    continue;
                }
                cur_get(c);
                massive[n] = '\0';
                char* value = getenv(massive);
                free(massive);
                if(!value) value = "";
                size_t len = strlen(value);
                while (count + len + 1 >= capacity){
                    capacity *= 2;
                    buffer = er_realloc(buffer, capacity);
                }
                memcpy(buffer + count, value, len);
                count += len;
                buffer[count]='\0';
                start = false;
                continue;
            }

            else if (!cur_eof(c) && cur_see(c) == '}') {
                fprintf(stderr, "Неверный синтаксис\n");
                cur_get(c);  // считываем '}', чтобы не застрять
                start = false;
                continue;
            }
            //остался случай, когда $var
            else{
                size_t cap = 128, n = 0;
                char* massive = er_malloc(cap);
                while(!cur_eof(c)){
                    int x = cur_see(c);
                    if(isalnum(x) || x == '_'){
                        cur_get(c);
                    } else break;
                    if((n+1) >= cap){
                        cap *= 2;
                        massive = er_realloc(massive, cap);
                    }
                    massive[n++] = (char)x;
                }
                massive[n] = '\0';
                char* value = getenv(massive);  //допустим getenv ждет нуль-терминированную строку
                free(massive);
                if(!value) value = "";
                size_t len = strlen(value);
                while(count + len + 1 >= capacity){
                    capacity *= 2;
                    buffer = er_realloc(buffer, capacity);
                }
                memcpy(buffer + count, value, len);
                count += len;
                buffer[count] = '\0'; //добавляем на всякий случай, так как все функции ждут "\0" в конце
                start = false;
                continue;
            }
        }
        //`......` - аналог $()
        else if(squote == false && symbol == '`'){
            size_t cap = 128, n = 0;
            char* massive = er_malloc(cap);
            while(!cur_eof(c) && cur_see(c) != '`'){
                int x = cur_get(c);
                if((n+1) >= cap){
                    cap *= 2;
                    massive = er_realloc(massive, cap);
                }
                massive[n++] = (char)x;
            }
            if (cur_eof(c) || cur_see(c) != '`'){
                fprintf(stderr, "Неверный синтаксис\n");
                free(massive);
                start = false;
                continue;
            }
            cur_get(c);
            massive[n] = '\0'; // <- этой строки не хватало в оригинале: без неё massive не был
                                //    null-terminated, и read_command читал мусор из кучи
            char* value = read_command(massive);
            free(massive);
            size_t len = strlen(value);
            while(count + len + 1 >= capacity){
                capacity *= 2;
                buffer = er_realloc(buffer, capacity);
            }
            memcpy(buffer + count, value, len);
            count += len;
            buffer[count] = '\0';
            free(value);
            start = false;
            continue;
        }
        //реализация нашей тильды
        else if(squote == false && dquote == false && start == true && symbol == '~'){
            size_t cap = 64, n = 0;
            char* massive = er_malloc(cap);
            massive[n++] = '~';
            while(!cur_eof(c)){
                int x = cur_see(c);
                if(isspace(x) || strchr("|&;()<>", x) || x == '#') break;
                cur_get(c);
                if((n+2) >= cap){
                    cap *= 2;
                    massive = er_realloc(massive, cap);
                }
                massive[n++] = (char)x;
            }
            massive[n] = '\0';
            char* value = tilda_koren(massive);
            free(massive);
            size_t len = strlen(value);
            while(count + len + 1 >= capacity){
                capacity *= 2;
                buffer = er_realloc(buffer, capacity);
            }
            memcpy(buffer + count, value, len);
            count += len;
            buffer[count] = '\0';
            free(value);
            start = false;
            continue;
        }

        if((count + 2) >= capacity){
            capacity *= 2;
            buffer = er_realloc(buffer, capacity);
        }

        buffer[count++] = symbol;
        buffer[count] = '\0';
        start = false;
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
