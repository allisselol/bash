#include "token.h"
#include "memory.h"

//функция добавления токена в список(массив)(вектор) токенов
void tv_push(TokenVector* tv, Token t){
    if(tv->n == tv->capacity){
        if(tv->capacity){
            tv->capacity *= 2;
        } else {
            tv->capacity = 64;
        }
        tv->vector = er_realloc(tv->vector, tv->capacity * sizeof(Token));
    }
    tv->vector[tv->n++] = t;
}
