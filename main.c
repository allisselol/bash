#include "common.h"
#include "memory.h"
#include "utils.h"

// ВРЕМЕННЫЙ main. На следующих шагах сюда добавятся:
// - инициализация терминала и обработчиков сигналов (job control)
// - вызов лексера/парсера для каждой введённой строки
// - вызов исполнителя (run_node) для построенного AST
int main(void){
    char* cwd = gettingthiscwd();
    printf("mybash (шаг 1): пока просто эхо ввода. cwd = %s\n", cwd);
    free(cwd);

    char* exe = read_exe_path();
    printf("путь к исполняемому файлу: %s\n", exe);
    free(exe);

    while(1){
        char* line = readline("mybash$ ");
        if(!line){ // Ctrl+D
            printf("\n");
            break;
        }
        if(line[0] != '\0'){
            add_history(line);
            cutter(line);
            printf("вы ввели: %s\n", line);
        }
        free(line);
    }

    return 0;
}
