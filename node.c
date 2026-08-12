#include "node.h"
#include "memory.h"
#include <stddef.h>
#include <stdlib.h>

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

void free_redirs(Redir* redir){
    while(redir){
        Redir* r = redir->next;
        free(redir->target);
        free(redir);
        redir = r;
    }
}

void free_node(Node* node){
    if(!node) return;
    free_node(node->left);
    free_node(node->right);
    if(node->argv){
        for(int i = 0; node->argv[i]; i++){
            free(node->argv[i]);
        }
        free(node->argv);
    }
    free_redirs(node->redirs);
    free(node);
}
