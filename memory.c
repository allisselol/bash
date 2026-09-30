#include "common.h" // _POSIX_C_SOURCE 200809L должен быть определён ДО системных заголовков
#include "memory.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
void die(void){//аварийное завершение программы при критической ошибке (например, нехватка памяти)
fprintf(stderr, "критическая ошибка!!!\n");//сообщение в stderr, а не stdout — чтобы не мешалось с обычным выводом
exit(EXIT_FAILURE);//завершаем процесс с кодом ошибки (1), дальше код не выполняется
}
void* er_malloc(size_t n){//обёртка над malloc с проверкой результата
void* p = malloc(n);//пытаемся выделить n байт
if(!p) die();//если malloc вернул NULL — память кончилась, аварийно завершаемся
return p;
}
void* er_calloc(size_t num, size_t n){ 
   void* p = calloc(num, n);//выделяем num элементов по n байт каждый, память обнуляется автоматически
if(!p) die();//проверка на нехватку памяти, как и в er_malloc
return p;
}
void* er_realloc(void* pointer, size_t n){ //новый размер в байтах по указателю
void* p = realloc(pointer, n);//пытаемся изменить размер уже выделенного блока pointer до n байт
if(!p) die();//если не получилось — старый pointer остался бы валиден, но мы просто падаем, не разбираясь
return p;
}
char* er_strdup(char* s){
if(!s) return NULL;//особый случай: дублировать "ничего" — это не ошибка памяти, а осознанный NULL на входе
char* d = strdup(s);//выделяем память и копируем строку s целиком
if(!d) die();//а вот если s не NULL, но strdup не смог выделить память — это уже реальная ошибка
return d;
}


