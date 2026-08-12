#include "node.h"
#include "memory.h"
#include <stddef.h>

Node* new_node(NodeType type){
    Node* node = er_calloc(1, sizeof(Node));
    node->type = type;
    return node;
}

void add_redir(Node* command, Redir r){
    Redir* copy = er_calloc(1, sizeof(Redir));
    *copy = r;
    copy->next = NULL; //(*copy).next
    if(!(command->redirs)){
        command->redirs = copy;
    } else {
        Redir* tmp = command->redirs;  //если направлятели уже были
        while(tmp->next){
            tmp = tmp->next;
        }
        tmp->next = copy;
    }
}
