#include "common.h" // _POSIX_C_SOURCE 200809L должен быть определён ДО системных заголовков
#include "node.h"
#include "memory.h"
#include <stddef.h>
#include <stdlib.h>
Node* new_node(NodeType type){// Создаёт новый узел AST заданного типа, все остальные поля обнулены
    Node* node = er_calloc(1, sizeof(Node));// выделяем память под узел и обнуляем её
    node->type = type;// устанавливаем тип узла
    return node;
}
void add_redir(Node* command, Redir r){// Добавляет редирект r в конец списка редиректов узла command (сохраняя порядок)
    Redir* copy = er_calloc(1, sizeof(Redir));// новый элемент связного списка на куче
    *copy = r;
    copy->next = NULL; //(*copy).next, явно обнуляем указатель на следующий (защита от мусора)
    if(!(command->redirs)){// список редиректов у команды пока пуст
        command->redirs = copy;// новый редирект становится первым
    } else {
        Redir* tmp = command->redirs;  //если направлятели уже были
        while(tmp->next){// идём до конца списка
            tmp = tmp->next; // tmp теперь указывает на последний элемент списка
        }
        tmp->next = copy;// приклеиваем новый редирект в конец
    }
}
void free_redirs(Redir* redir){// рекурсивно освобождаем память, выделенную под список редиректов
    while(redir){// пока не дошли до конца списка (NULL)
        Redir* r = redir->next;// запоминаем указатель на следующий элемент списка
        free(redir->target);// освобождаем строку внутри редиректа (имя файла)
        free(redir);// освобождаем сам текущий элемент списка
        redir = r;
    }
}
void free_node(Node* node){//рекурсивно освобождаем память, выделенную под узел и его потомков
    if(!node) return;// базовый случай рекурсии: пустой узел - ничего делать не нужно
    free_node(node->left);
    free_node(node->right);
    if(node->argv){// если у узла есть массив аргументов (команда)
        for(int i = 0; node->argv[i]; i++){// освобождаем каждую строку-аргумент
            free(node->argv[i]);
        }
        free(node->argv);// освобождаем сам массив указателей
    }
    free_redirs(node->redirs); // освобождаем цепочку редиректов узла (если она есть)
    free(node);
} // и в самом конце - сам узел, когда всё, на что он указывал, уже свободно
