#include "memory.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void die(void){
    fprintf(stderr, "Критическая ошибка!!!\n");
    exit(EXIT_FAILURE);
}

void* er_malloc(size_t n){
    void* p = malloc(n);
    if(!p) die();
    return p;
}

void* er_calloc(size_t num, size_t n){ //количество и размер одного
    void* p = calloc(num, n);
    if(!p) die();
    return p;
}

void* er_realloc(void* pointer, size_t n){ //новый размер в байтах по указателю
    void* p = realloc(pointer, n);
    if(!p) die();
    return p;
}

char* er_strdup(char* s){
    if(!s) return NULL;
    char* d = strdup(s);
    if(!d) die();
    return d;
}
