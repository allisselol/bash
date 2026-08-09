#pragma once

// обрезает завершающие \n и \r у строки (модифицирует её на месте)
char* cutter(char* s);

// получить текущую рабочую директорию (аналог getcwd, но с авто-расширением буфера)
char* gettingthiscwd(void);

// склеить два пути через '/', если нужно
char* path_join(char* a, char* b);

// получить полный путь к исполняемому файлу шелла
// (на Linux — через /proc/self/exe, на macOS — через _NSGetExecutablePath)
char* read_exe_path(void);
