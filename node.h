#pragma once

typedef enum {
    R_IN,   //Ввод из файла <
    R_OUT,
    R_APPEND,
    R_HEREDOC,
    R_HERESTR,
    R_ALL_TO_FILE,
    R_DUP_OUT,  // >&N  //дублировать stdout в определенный дескриптор
    R_DUP_IN    // <&N  //дублировать stdin из определенного дескриптора
} RedirType;

typedef struct Redir {
    RedirType type;
    int src;         //Какой файловый дескриптор перенаправляется
    int dup_target;  //какой дескриптор нужно дублировать
    char* target;
    struct Redir* next;
} Redir;

//узлы дерева отдельно
typedef enum{
    NODE_CMD,
    NODE_PIPE,
    NODE_AND,
    NODE_OR,
    NODE_SEQ,
    NODE_BG,
    NODE_MINISHELL
} NodeType;

typedef struct Node {
    Redir* redirs; //список направлятелей привязанных к узлу
    char **argv; //строка, а в ней n-ное кол-во слов
    NodeType type;
    struct Node* left;
    struct Node* right;
} Node;

//создать новый узел заданного типа (поля обнулены через calloc)
Node* new_node(NodeType type);

//добавить редирект в конец списка редиректов узла
void add_redir(Node* command, Redir r);

//освободить список редиректов
void free_redirs(Redir* redir);

//полностью освободить дерево (рекурсивно)
void free_node(Node* node);