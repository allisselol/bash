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

char* cutter(char* s){
    size_t n = strlen(s);
    while(n && (s[n-1] == '\n' || s[n-1] == '\r')){
        s[--n] = '\0';
    }
    return s;
}

//получить текущую директорию //будет удобно в будущем для написания cd например
char* gettingthiscwd(void){
    size_t capacity = 256;
    while(1){
        char* buffer = er_malloc(capacity);
        if(getcwd(buffer, capacity)){
            return buffer;
        }
        free(buffer);
        if(errno != ERANGE){
            return er_strdup("?");
        }
        capacity *= 2; //если errno == ERANGE, тогда получается, что проблема в недостатке памяти
    }
}

//соеденить директории, пути
char* path_join(char* a, char* b){
    size_t len_a = strlen(a), len_b = strlen(b);
    bool need_stick = (len_a > 0 && a[len_a - 1] != '/');
    char* r = er_malloc(len_a + need_stick + len_b + 1);
    memcpy(r, a, len_a);
    size_t stick = len_a;
    if(need_stick == 1) r[stick++] = '/';
    memcpy(r + stick, b, len_b + 1);
    return r;
}

//получение полного пути к исполняемому файлу
char* read_exe_path(void){
#ifdef __APPLE__
    //на macOS нет /proc, поэтому используем системный API mach-o
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
