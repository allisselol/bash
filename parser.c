#include "common.h" // _POSIX_C_SOURCE 200809L должен быть определён ДО системных заголовков
#include "parser.h"
#include "memory.h"
#include "utils.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

Token* see(Parser* p){//Возвращает указатель на токен в ТЕКУЩЕЙ позиции, НЕ сдвигая позицию (аналог cur_see в лексере)
    return &p->tv->vector[p->position];
}
Token* get(Parser* p){
    return &p->tv->vector[p->position++];
}
bool accept(Parser* p, TokenType token){//Пробует "принять" токен ожидаемого типа: если совпал - съедает его и возвращает true, иначе ничего не трогает
    if(see(p)->type == token){//подсматриваем тип текущего токена, не трогая позицию
        p->position++;//совпало - сдвигаем позицию, "съедая" токен
        return true;
    }
    return false;
}
bool expect(Parser* p, TokenType token){// Требует токен ожидаемого типа как ОБЯЗАТЕЛЬНЫЙ; при несовпадении - это синтаксическая ошибка
    if(syntax_error_flag) return false; //ошибка уже была где-то выше по стеку разбора
    if(accept(p, token)) return true;// пробуем принять токен обычным образом
    report_syntax_error("неожиданный токен");// не получилось - сообщаем об ошибке и поднимаем флаг
    return false;
}
char* heredoc(char* end_pointer){// Читает многострочный ввод heredoc до тех пор, пока пользователь не введёт строку end_pointer целиком
    size_t capacity = 256, count = 0;
    char* buffer = er_malloc(capacity);
    buffer[0] = '\0';//растим буфер
    while(true){
        printf("> "); //вторичное приглашение heredoc
        fflush(stdout);// обязателен - иначе приглашение может застрять в буфере stdout и не показаться
        char* line = NULL;// getline сама выделит память под строку, если line == NULL
        size_t cap = 0;
        ssize_t got = getline(&line, &cap, stdin);// читаем одну строку с stdin (включая \n на конце)
        if(got < 0){ //конец файла - обрываем heredoc как есть
            free(line);
            break;
        }
        cutter(line);// обрезаем \n/\r, чтобы можно было сравнить строку с меткой буквально
        if(strcmp(line, end_pointer) == 0){// строка совпала с меткой-разделителем - heredoc закончен штатно
            free(line);
            break;
        }
        size_t len = strlen(line);// длина строки, которую только что ввёл пользователь
        while(count + len + 2 >= capacity){// +2: сама строка + символ '\n'
            capacity *= 2;
            buffer = er_realloc(buffer, capacity);
        }
        memcpy(buffer + count, line, len);// дописываем текст строки (без \n - он уже обрезан)
        count += len;
        buffer[count++] = '\n';// добавляем символ перевода строки, чтобы сохранить формат ввода
        buffer[count] = '\0';// сразу терминируем на промежуточном шаге, чтобы можно было безопасно использовать buffer как строку
        free(line);
    }
    return buffer;// весь собранный текст heredoc (без финальной строки-метки)
}
char* expect_word(Parser* p){//следующий токен ОБЯЗАТЕЛЬНО должен быть словом
    if(syntax_error_flag) return NULL;
    Token* token = get(p);// съедаем токен, чтобы продвинуться вперёд
    if(token->type != TOK_WORD){//это действительно слово
        report_syntax_error("ожидалось слово");// не слово - фиксируем ошибку синтаксиса
        return NULL;
    }
    return er_strdup(token->value);// возвращаем НЕЗАВИСИМУЮ копию строки (токены живут своей жизнью)
}
// Читает имя файла как слово и прикрепляет к узлу node новый редирект типа rt на дескриптор src
void add_redir_file(Parser* p, Node* node, RedirType rt, int src){
    char* target = expect_word(p);// ожидаем слово - имя файла редиректа
    if(!target) return; //ошибка уже была зафиксирована в expect_word
    add_redir(node, (Redir){rt, src, -1, target, NULL});// создаём новый редирект и прикрепляем его к узлу node
}
//самая простая функция для обработки TOK_WORDов и направлятелей связанными с ними узлами
Node* parse_simple(Parser* p){
    Node* node = new_node(NODE_CMD);//создаю узел
    size_t capacity = 4, argc = 0;
    node->argv = er_calloc(capacity, sizeof(char*));//сразу обнуленные ячейки
    bool seen_word = false;// флаг: встретили ли хотя бы одно слово (саму команду)
    while(true){
        Token* token = see(p);
        if(token->type == TOK_WORD){
            seen_word = true;// команда реально содержит хотя бы одно слов
            if(argc + 2 > capacity){// +2: одно слово + NULL в конце
                capacity *= 2;
                node->argv = er_realloc(node->argv, capacity*sizeof(char*));// перевыделяем память под массив указателей на слова
            }
            node->argv[argc++] = er_strdup(token->value); //копируем в новую память и прикрепляем независимый указатель
            node->argv[argc] = NULL;
            get(p);
            continue;
        }
        if(token->type == TOK_REDIR_IN){// < input.txt
            get(p);
            add_redir_file(p, node, R_IN, 0);// читаем имя файла и прикрепляем редирект на fd 0 (stdin)
            if(syntax_error_flag) break;
            continue;
        }
        if(token->type == TOK_REDIR_OUT){// > output.txt
            get(p);
            add_redir_file(p, node, R_OUT, 1);// редирект на fd 1 (stdout), перезапись файла
            if(syntax_error_flag) break;
            continue;
        }
        if(token->type == TOK_REDIR_OUT_APP){// >> output.txt
            get(p);
            add_redir_file(p, node, R_APPEND, 1);// редирект на fd 1, дозапись в конец файла
            if(syntax_error_flag) break;
            continue;
        }
        if(token->type == TOK_ALL_TO_FILE){// &> output.txt
            get(p);
            add_redir_file(p, node, R_ALL_TO_FILE, -1);// и stdout, и stderr вместе в один файл
            if(syntax_error_flag) break;
            continue;
        }
        // N> N>> N< 2> error.txt перенаправь std_err в error.txt
        if(token->type == TOK_FD_REDIR_OUT){// N> output.txt
            int fd = token->fd;// номер fd лексер уже распознал и положил в токен
            get(p);//N>
            add_redir_file(p, node, R_OUT, fd);// редирект на fd, перезапись файла
            if(syntax_error_flag) break;
            continue;
        }
        if(token->type == TOK_FD_REDIR_IN){// N< input.txt
            int fd = token->fd;
            get(p);
            add_redir_file(p, node, R_IN, fd);// редирект чтения на этот fd
            if(syntax_error_flag) break;
            continue;
        }
        if(token->type == TOK_FD_REDIR_OUT_APP){// N>> output.txt
            int fd = token->fd;
            get(p);
            add_redir_file(p, node, R_APPEND, fd);// редирект на fd, дозапись в конец файла
            if(syntax_error_flag) break;
            continue;
        }
        // >&N <&N  echo hello >&2 перенаправь std_out в дескриптор std_err
        if(token->type == TOK_DUP_IN){// <&N
            int target_dup = token->fd;
            get(p);
            add_redir(node, (Redir){R_DUP_IN, 0, target_dup, NULL, NULL});// редирект stdin на другой fd
            continue;
        }
        if(token->type == TOK_DUP_OUT){// >&N
            int target_dup = token->fd;
            get(p);
            add_redir(node, (Redir){R_DUP_OUT, 1, target_dup, NULL, NULL});// редирект stdout на другой fd
            continue;
        }
        if(token->type == TOK_HEREDOC){// <<EOF
            get(p);
            char* end_pointer = expect_word(p); //так как следующий элемент после <<EOF
            if(syntax_error_flag) break; //end_pointer уже NULL, освобождать нечего
            char* body = heredoc(end_pointer);
            free(end_pointer);
            add_redir(node, (Redir){R_HEREDOC, 0, -1, body, NULL});// редирект stdin на текст heredoc
            continue;
        }
        if(token->type == TOK_HERESTR){// <<< "string smth"
            get(p);
            char* str = expect_word(p);  //тут передается целая строка, так как <<< берет строку
            if(syntax_error_flag) break;
            add_redir(node, (Redir){R_HERESTR, 0, -1, str, NULL});// редирект stdin на текст herestr
            continue;
        }
        break;
    }
    if(syntax_error_flag){
        free_node(node); //освобождаем всё, что уже успели собрать (argv, redirs)
        return NULL;
    }
    if(seen_word == false || !node->argv){// слов не было вообще (например, только редиректы без команды)
        free(node->argv);
        free(node);
        return NULL;
    }
    return node;// успешно собранный узел простой команды со всеми аргументами и редиректами
}
//обработка минишела ()
Node* parse_minishell(Parser* p){// минишелл - это скобки, внутри которых может быть целая строка с ;/&, и даже вложенные скобки
    if(!expect(p, TOK_LPAREN)) return NULL; // обязательная открывающая (
    Node* massive = parse_line(p); // рекурсивный вызов САМОЙ верхнеуровневой функции грамматики внутри скобок может быть ЦЕЛАЯ строка с ;/&, и даже вложенные скобки
    if(syntax_error_flag){
        free_node(massive); // освобождаем то, что успели разобрать внутри скобок
        return NULL;
    }
    if(!expect(p, TOK_RPAREN)){ // обязательная закрывающая )
        free_node(massive); // не нашли её - содержимое скобок всё равно нужно освободить
        return NULL;
    }
    Node* node = new_node(NODE_MINISHELL); // создаём узел-обёртку только теперь, когда скобки закрыты успешно
    node->left = massive; // всё содержимое скобок становится левым поддеревом
    while(true){
        Token* token = see(p); // подсматриваем токен ПОСЛЕ закрывающей скобки
        if(token->type == TOK_REDIR_IN){// < input.txt
            get(p);  //<
            char* target = expect_word(p); // ожидаем имя файла
            if(syntax_error_flag){ free_node(node); return NULL; } // free_node(node) освободит и massive внутри
            add_redir(node, (Redir){R_IN, 0, -1, target, NULL}); // редирект крепится к MINISHELL, не к massive
            continue;
        }
        if(token->type == TOK_REDIR_OUT){
            get(p); // >
            char* target = expect_word(p);
            if(syntax_error_flag){ free_node(node); return NULL; }
            add_redir(node, (Redir){R_OUT, 1, -1, target, NULL});// редирект stdout на файл, перезапись
            continue;
        }
        if(token->type == TOK_REDIR_OUT_APP){
            get(p); //>>
            char* target = expect_word(p);
            if(syntax_error_flag){ free_node(node); return NULL; }
            add_redir(node, (Redir){R_APPEND, 1, -1, target, NULL});// редирект дозаписи в конец файла
            continue;
        }
        if(token->type == TOK_ALL_TO_FILE){
            get(p);//&>
            char* target = expect_word(p);
            if(syntax_error_flag){ free_node(node); return NULL; }
            add_redir(node, (Redir){R_ALL_TO_FILE, -1, -1, target, NULL});// редирект stdout+stderr в один файл
            continue;
        }
        break; 
    }
    return node; // готовый узел подшелла, с содержимым внутри и редиректами снаружи
}
//Перейдем к функциям самих главных узлов, а не листиков
//Функция определения подузла
Node* parse_command(Parser* p){
    if(see(p)->type == TOK_LPAREN){ //подшелл
        return parse_minishell(p);
    }
    return parse_simple(p); 
}
// |
Node* parse_pipeline(Parser* p){
    Node* left = parse_command(p);              // разбираем первую команду пайплайна
    if(!left) return NULL; 
    while((accept(p, TOK_PIPE))){                 // пока встречаем "|" (и сразу съедаем его)
        Node* right = parse_command(p);             // разбираем команду после "|"
        if(!right){
            if(!syntax_error_flag) report_syntax_error("ожидалась команда после '|'"); // не задваиваем ошибку
            free_node(left);                           // освобождаем уже накопленную часть пайплайна
            return NULL;
        }
        Node* pipe = new_node(NODE_PIPE);
        pipe->left = left;
        pipe->right = right;
        left = pipe; //дерево с уклоном влево - левоассоциативное накопление цепочки "|"
    }
    return left;
}
// && ||
Node* parse_and_or(Parser* p){
    Node* left = parse_pipeline(p);  // "команда" на этом уровне - целый пайплайн
    if(!left) return NULL;
    while(true){
        if(accept(p, TOK_AND)){        // встретили "&&"
            Node* right = parse_pipeline(p);
            if(!right){
                if(!syntax_error_flag) report_syntax_error("ожидалась команда после '&&'");
                free_node(left);
                return NULL;
            }
            Node* a = new_node(NODE_AND);
            a->left = left; a->right = right; left = a; // левоассоциативное накопление
        } else if(accept(p, TOK_OR)){  // встретили "||"
            Node* right = parse_pipeline(p);
            if(!right){
                if(!syntax_error_flag) report_syntax_error("ожидалась команда после '||'");
                free_node(left);
                return NULL;
            }
            Node* b = new_node(NODE_OR);
            b->left = left; b->right = right; left = b;
        } else break; // ни "&&", ни "||" - цепочка закончилась
    }
    return left;
}
// ; &
Node* parse_line(Parser* p){
    Node* result = NULL; // накопитель - стартует пустым, в отличие от нижних уровней
    while(1){
        Node* term = parse_and_or(p); // "терм" на этом уровне - целая цепочка &&/||
        if(!term){
            if(syntax_error_flag){
                free_node(result); // реальная ошибка - освобождаем всё, что успели накопить
                return NULL;
            }
            break; //настоящий конец ввода - команд больше нет, это не ошибка
        }
        bool had_bg = false;
        if(accept(p, TOK_BG)){           // сразу после терма стоит "&" - фоновый запуск
            Node* bg = new_node(NODE_BG);
            bg->left  = term;
            bg->right = NULL;
            term = bg;                     // заменяем term на обёртку NODE_BG
            had_bg = true;
        }
        if(!result){
            result = term;                  // первый терм - просто становится накопителем
        } else {
            Node* seq = new_node(NODE_SEQ); // следующие термы оборачиваются в NODE_SEQ
            seq->left  = result;
            seq->right = term;
            result = seq;
        }
        if(accept(p, TOK_SEMI)) continue; // явный разделитель ";" - ищем следующий терм
        if(had_bg) continue;               // "&" сам действует как разделитель - тоже продолжаем
        break;                               // ни ";", ни "&" не было - строка (или подшелл) закончилась
    }
    return result; // корень построенного дерева для всей строки (или содержимого скобок)
}