#include "common.h" // _POSIX_C_SOURCE 200809L должен быть определён ДО системных заголовков
#include "parser.h"
#include "memory.h"
#include "utils.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

Token* see(Parser* p){
    return &p->tv->vector[p->position];
}

Token* get(Parser* p){
    return &p->tv->vector[p->position++];
}

bool accept(Parser* p, TokenType token){
    if(see(p)->type == token){
        p->position++;
        return true;
    }
    return false;
}

//обязательное условие того, что токен должен быть соответствующего типа.
//Раньше при несовпадении вызывался die() - это убивало весь процесс интерпретатора,
//что прямо запрещено ТЗ (синтаксическая ошибка не должна прерывать интерпретатор).
//Теперь мы печатаем диагностику, поднимаем общий флаг syntax_error_flag и возвращаем
//false - вызывающая функция обязана сразу прекратить разбор и вернуть NULL, не читая
//больше токенов (see()/get() дальше не гарантированно безопасны после ошибки).
bool expect(Parser* p, TokenType token){
    if(syntax_error_flag) return false; //ошибка уже была где-то выше по стеку разбора
    if(accept(p, token)) return true;
    report_syntax_error("неожиданный токен");
    return false;
}

char* heredoc(char* end_pointer){
    size_t capacity = 256, count = 0;
    char* buffer = er_malloc(capacity);
    buffer[0] = '\0';
    while(true){
        printf("> "); //вторичное приглашение heredoc
        fflush(stdout);

        char* line = NULL;
        size_t cap = 0;
        ssize_t got = getline(&line, &cap, stdin);
        if(got < 0){ //конец файла - обрываем heredoc как есть
            free(line);
            break;
        }
        cutter(line);
        if(strcmp(line, end_pointer) == 0){
            free(line);
            break;
        }
        size_t len = strlen(line);
        while(count + len + 2 >= capacity){
            capacity *= 2;
            buffer = er_realloc(buffer, capacity);
        }
        memcpy(buffer + count, line, len);
        count += len;
        buffer[count++] = '\n';
        buffer[count] = '\0';
        free(line);
    }
    return buffer;
}

//возвращаем значение (наше слово). Раньше при отсутствии слова вызывался die() -
//теперь мягкая ошибка: диагностика + NULL, как и в expect()
char* expect_word(Parser* p){
    if(syntax_error_flag) return NULL;
    Token* token = get(p);
    if(token->type != TOK_WORD){
        report_syntax_error("ожидалось слово");
        return NULL;
    }
    return er_strdup(token->value);
}

void add_redir_file(Parser* p, Node* node, RedirType rt, int src){
    char* target = expect_word(p);
    if(!target) return; //ошибка уже отмечена в expect_word(); вызывающий код это проверит
    add_redir(node, (Redir){rt, src, -1, target, NULL});
}

//самая простая функция для обработки TOK_WORDов и направлятелей связанными с ними узлами
Node* parse_simple(Parser* p){
    Node* node = new_node(NODE_CMD);
    size_t capacity = 4, argc = 0;
    node->argv = er_calloc(capacity, sizeof(char*));
    bool seen_word = false;

    while(true){
        Token* token = see(p);

        if(token->type == TOK_WORD){
            seen_word = true;
            if(argc + 2 > capacity){
                capacity *= 2;
                node->argv = er_realloc(node->argv, capacity*sizeof(char*));
            }
            node->argv[argc++] = er_strdup(token->value); //копируем в новую память и прикрепляем независимый указатель
            node->argv[argc] = NULL;
            get(p);
            continue;
        }

        if(token->type == TOK_REDIR_IN){
            get(p);
            add_redir_file(p, node, R_IN, 0);
            if(syntax_error_flag) break;
            continue;
        }

        if(token->type == TOK_REDIR_OUT){
            get(p);
            add_redir_file(p, node, R_OUT, 1);
            if(syntax_error_flag) break;
            continue;
        }

        if(token->type == TOK_REDIR_OUT_APP){
            get(p);
            add_redir_file(p, node, R_APPEND, 1);
            if(syntax_error_flag) break;
            continue;
        }

        if(token->type == TOK_ALL_TO_FILE){
            get(p);
            add_redir_file(p, node, R_ALL_TO_FILE, -1);
            if(syntax_error_flag) break;
            continue;
        }

        // N> N>> N< 2> error.txt перенаправь std_err в error.txt
        if(token->type == TOK_FD_REDIR_OUT){
            int fd = token->fd;
            get(p);
            add_redir_file(p, node, R_OUT, fd);
            if(syntax_error_flag) break;
            continue;
        }

        if(token->type == TOK_FD_REDIR_IN){
            int fd = token->fd;
            get(p);
            add_redir_file(p, node, R_IN, fd);
            if(syntax_error_flag) break;
            continue;
        }

        if(token->type == TOK_FD_REDIR_OUT_APP){
            int fd = token->fd;
            get(p);
            add_redir_file(p, node, R_APPEND, fd);
            if(syntax_error_flag) break;
            continue;
        }

        // >&N <&N  echo hello >&2 перенаправь std_out в дескриптор std_err
        if(token->type == TOK_DUP_IN){
            int target_dup = token->fd;
            get(p);
            add_redir(node, (Redir){R_DUP_IN, 0, target_dup, NULL, NULL});
            continue;
        }

        if(token->type == TOK_DUP_OUT){
            int target_dup = token->fd;
            get(p);
            add_redir(node, (Redir){R_DUP_OUT, 1, target_dup, NULL, NULL});
            continue;
        }

        if(token->type == TOK_HEREDOC){
            get(p);
            char* end_pointer = expect_word(p); //так как следующий элемент после <<EOF
            if(syntax_error_flag) break; //end_pointer уже NULL, освобождать нечего
            char* body = heredoc(end_pointer);
            free(end_pointer);
            add_redir(node, (Redir){R_HEREDOC, 0, -1, body, NULL});
            continue;
        }

        if(token->type == TOK_HERESTR){
            get(p);
            char* str = expect_word(p);  //тут передается целая строка, так как <<< берет строку
            if(syntax_error_flag) break;
            add_redir(node, (Redir){R_HERESTR, 0, -1, str, NULL});
            continue;
        }

        break;
    }

    if(syntax_error_flag){
        free_node(node); //освобождаем всё, что уже успели собрать (argv, redirs)
        return NULL;
    }

    if(seen_word == false || !node->argv){
        free(node->argv);
        free(node);
        return NULL;
    }
    return node;
}

//обработка минишела ()
Node* parse_minishell(Parser* p){
    if(!expect(p, TOK_LPAREN)) return NULL;

    Node* massive = parse_line(p);
    if(syntax_error_flag){
        free_node(massive);
        return NULL;
    }

    if(!expect(p, TOK_RPAREN)){
        free_node(massive);
        return NULL;
    }

    Node* node = new_node(NODE_MINISHELL);
    node->left = massive;

    while(true){
        Token* token = see(p);

        if(token->type == TOK_REDIR_IN){
            get(p);
            char* target = expect_word(p);
            if(syntax_error_flag){ free_node(node); return NULL; }
            add_redir(node, (Redir){R_IN, 0, -1, target, NULL});
            continue;
        }

        if(token->type == TOK_REDIR_OUT){
            get(p);
            char* target = expect_word(p);
            if(syntax_error_flag){ free_node(node); return NULL; }
            add_redir(node, (Redir){R_OUT, 1, -1, target, NULL});
            continue;
        }

        if(token->type == TOK_REDIR_OUT_APP){
            get(p);
            char* target = expect_word(p);
            if(syntax_error_flag){ free_node(node); return NULL; }
            add_redir(node, (Redir){R_APPEND, 1, -1, target, NULL});
            continue;
        }

        if(token->type == TOK_ALL_TO_FILE){
            get(p);
            char* target = expect_word(p);
            if(syntax_error_flag){ free_node(node); return NULL; }
            add_redir(node, (Redir){R_ALL_TO_FILE, -1, -1, target, NULL});
            continue;
        }

        break;
    }

    return node;
}

//Перейдем к функциям самих главных узлов, а не листиков

//Функция определения подузла
Node* parse_command(Parser* p){
    if(see(p)->type == TOK_LPAREN){
        return parse_minishell(p);
    }
    return parse_simple(p);
}

// |
Node* parse_pipeline(Parser* p){
    Node* left = parse_command(p);
    if(!left) return NULL; //либо действительно пусто, либо уже была ошибка - разберутся выше
    while((accept(p, TOK_PIPE))){
        Node* right = parse_command(p);
        if(!right){
            if(!syntax_error_flag) report_syntax_error("ожидалась команда после '|'");
            free_node(left);
            return NULL;
        }
        Node* pipe = new_node(NODE_PIPE);
        pipe->left = left;
        pipe->right = right;
        left = pipe; //дерево с уклоном влево
    }
    return left;
}

// && ||
Node* parse_and_or(Parser* p){
    Node* left = parse_pipeline(p);
    if(!left) return NULL;
    while(true){
        if(accept(p, TOK_AND)){
            Node* right = parse_pipeline(p);
            if(!right){
                if(!syntax_error_flag) report_syntax_error("ожидалась команда после '&&'");
                free_node(left);
                return NULL;
            }
            Node* a = new_node(NODE_AND);
            a->left = left; a->right = right; left = a;
        } else if(accept(p, TOK_OR)){
            Node* right = parse_pipeline(p);
            if(!right){
                if(!syntax_error_flag) report_syntax_error("ожидалась команда после '||'");
                free_node(left);
                return NULL;
            }
            Node* b = new_node(NODE_OR);
            b->left = left; b->right = right; left = b;
        } else break;
    }

    return left;
}

// ; &
Node* parse_line(Parser* p){
    Node* result = NULL;

    while(1){
        Node* term = parse_and_or(p);
        if(!term){
            if(syntax_error_flag){
                free_node(result);
                return NULL;
            }
            break; //настоящий конец ввода - команд больше нет, это не ошибка
        }

        bool had_bg = false;
        if(accept(p, TOK_BG)){
            Node* bg = new_node(NODE_BG);
            bg->left  = term;
            bg->right = NULL;
            term = bg;
            had_bg = true;
        }

        if(!result){
            result = term;
        } else {
            Node* seq = new_node(NODE_SEQ);
            seq->left  = result;
            seq->right = term;
            result = seq;
        }

        if(accept(p, TOK_SEMI)) continue;
        if(had_bg) continue;
        break;
    }

    return result;
}
