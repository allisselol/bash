#include "common.h" // _POSIX_C_SOURCE 200809L должен быть определён ДО системных заголовков
#include "utils.h"
#include "memory.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <stdbool.h>

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
char* cutter(char* s){//Обрезает завершающие \n и \r у строки (нужна после getline, который их не убирает сам)
    size_t n = strlen(s);//текущая длина строки
    while(n && (s[n-1] == '\n' || s[n-1] == '\r')){//пока строка не пуста и последний символ - перевод строки
        s[--n] = '\0';//уменьшаем n и сразу ставим туда терминатор
    }
    return s;
}

//получить текущую директорию //будет удобно в будущем для написания cd например
char* gettingthiscwd(void){
    size_t capacity = 256;//буфер
    while(1){//растим буфер, пока не влезет
        char* buffer = er_malloc(capacity);// выделяем буфер нужного размера
        if(getcwd(buffer, capacity)){//пробуем прочитать cwd; успех - если вернулся не NULL
            return buffer;
        }// путь поместился, отдаём буфер как есть
        free(buffer);// неудачная попытка - освобождаем буфер перед повтором
        if(errno != ERANGE){   //какая то другая оштбка
            return er_strdup("?"); // дальше пытаться бессмысленно, возвращаем заглушку
        }capacity *= 2; //если errno == ERANGE, тогда получается, что проблема в недостатке памяти
    }
}
char* path_join(char* a, char* b){//соединить директории или пути
    size_t len_a = strlen(a), len_b = strlen(b);//длины
    bool need_stick = (len_a > 0 && a[len_a - 1] != '/');//нужен ли разделитель / 
    char* r = er_malloc(len_a + need_stick + len_b + 1);//память выделяю пдд а + и тд
    memcpy(r, a, len_a);// копируем a без завершающего нуля
    size_t stick = len_a;// текущая позиция записи в результате
    if(need_stick == 1) r[stick++] = '/';
    memcpy(r + stick, b, len_b + 1);// дописываем b вместе с его завершающим '\0'
    return r;
}//получается склеенная мтрока

//получение полного пути к исполняемому файлу
char* read_exe_path(void){
#ifdef __APPLE__//на macOS нет /proc, поэтому использую системный API mach-o
    uint32_t size = 256;
    char* buffer = er_malloc(size);
    if(_NSGetExecutablePath(buffer, &size) == 0){
        return buffer;
    }
    //буфер оказался мал - _NSGetExecutablePath записал в size нужный размер
    free(buffer);
    buffer = er_malloc(size);
    if(_NSGetExecutablePath(buffer, &size) == 0){
        return buffer;
    }
    free(buffer);
    return er_strdup("?");
#else
    size_t capacity = 256;
    while(1){
        char* buffer = er_malloc(capacity);
        ssize_t n = readlink("/proc/self/exe", buffer, capacity - 1); //без \0
        if(n >= 0){
            if((size_t)n < capacity - 1){
                buffer[n] = '\0';
                return buffer;
            }
        }
        free(buffer);
        capacity *= 2;
        if(capacity > 689920){
            return er_strdup("Слишком большой размер пути");
        }
    }
#endif
}
