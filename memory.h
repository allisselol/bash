#pragma once
#include <stddef.h>

// критическая ошибка -> завершаем программу
void die(void);

// обёртки над malloc/calloc/realloc/strdup, которые сами
// проверяют результат и вызывают die() при нехватке памяти
void* er_malloc(size_t n);
void* er_calloc(size_t num, size_t n);
void* er_realloc(void* pointer, size_t n);
char* er_strdup(char* s);
