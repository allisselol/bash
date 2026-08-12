#include "builtins.h"
#include "jobs.h"
#include "memory.h"
#include "utils.h"
#include "lexer.h" // tilda_koren
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <readline/readline.h>
#include <readline/history.h>

int my_cd(char** argv){
    char* old = gettingthiscwd();
    char* arg = argv[1];

    if(!arg || strcmp(arg, "~") == 0){
        char* home = getenv("HOME");
        if(!home) home = "/";
        if(chdir(home) != 0){
            perror("cd");
            free(old);
            return 1;
        }
    }
    else if(strcmp(arg, "-") == 0){
        char* previous = getenv("OLDPWD");
        if(!previous){
            fprintf(stderr, "Прошлый путь не был получен\n");
            free(old);
            return 1;
        }
        if(chdir(previous) != 0){
            perror("cd");
            free(old);
            return 1;
        }
    } else {
        if(arg[0] == '~'){
            char* path = tilda_koren(arg);
            if(chdir(path) != 0){
                perror("cd");
                free(path);
                free(old);
                return 1;
            }
            free(path);
        } else {
            if(chdir(arg) != 0){
                perror("cd");
                free(old);
                return 1;
            }
        }
    }

    if(old) setenv("OLDPWD", old, 1);
    char* cwd = gettingthiscwd();
    if(cwd){
        setenv("PWD", cwd, 1);
        free(cwd);
    }
    free(old);
    return 0;
}

int my_pwd(void){
    char* cwd = gettingthiscwd();
    puts(cwd);
    free(cwd);
    return 0;
}

int my_export(char** argv){
    for(int i = 1; argv[i] != NULL; i++){
        char* equal = strchr(argv[i], '='); //возвращает адрес памяти
        if(!equal){
            fprintf(stderr, "Неправильный [export] прописан!\n");
            return 1;
        }
        *equal = '\0';
        if(setenv(argv[i], equal + 1, 1) != 0){ //в окружении текущего процесса появится переменная
            perror("export");
            *equal = '=';
            return 1;
        }
        *equal = '=';
    }
    return 0;
}

int my_unset(char** argv){
    for(int i = 1; argv[i] != NULL; i++){
        if(unsetenv(argv[i]) != 0){
            perror("unset");
            return 1;
        }
    }
    return 0;
}

int my_echo(char** argv){
    int i = 1, newline = 1;
    if(argv[1] && strcmp(argv[1], "-n") == 0){
        newline = 0; i = 2;
    }
    for(; argv[i]; i++){
        if(newline){
            if(i > 1){
                fputc(' ', stdout);
            }
        } else {
            if(i > 2){
                fputc(' ', stdout);
            }
        }
        fputs(argv[i], stdout);
    }

    if(newline){
        fputc('\n', stdout);
    }

    return 0;
}

int my_jobs(void){
    get_children();
    jobs_print();
    jobs_remove();
    return 0;
}

int my_fg(char** argv){
    get_children(); //чистим зомби, сообщаем родителям(ю)
    Job* job = NULL;
    if(argv[1]){
        int id; //идентификатор задания, номер в списке jobs
        if(argv[1][0] == '%'){
            id = atoi(argv[1] + 1);
        } else {
            id = atoi(argv[1]);
        }

        job = jobs_by_id(id);
        if(!job){
            fprintf(stderr, "fg job не был найден\n");
            return 1;
        }
    } else {
        if(jobs.count == 0){
            fprintf(stderr, "У нас нет ни одного jobs\n");
            return 1;
        }

        job = &jobs.vector[jobs.count - 1];
    }

    if(job->status == J_DONE){
        fprintf(stderr, "Задание уже завершено\n");
        return 1;
    }

    printf("%s\n", job->cmdline);

    int to_fg = put_job_fg(job, 1); //продолжить выполнение если было остановлено
    return to_fg;
}

int my_bg(char** argv){
    get_children(); //чистим зомби, сообщаем родителям(ю)
    Job* job = NULL;
    if(argv[1]){
        int id; //идентификатор задания, номер в списке jobs
        if(argv[1][0] == '%'){
            id = atoi(argv[1] + 1);
        } else {
            id = atoi(argv[1]);
        }

        job = jobs_by_id(id);
        if(!job){
            fprintf(stderr, "fg job не был найден\n");
            return 1;
        }
    } else {
        if(jobs.count == 0){
            fprintf(stderr, "У нас нет ни одного jobs\n");
            return 1;
        }

        job = &jobs.vector[jobs.count - 1];
    }

    if(job->status == J_RUNNING){
        printf("[%d] уже работает\n", job->id);
        return 1;
    }

    job->status = J_RUNNING;
    printf("%s\n", job->cmdline);
    put_job_bg(job, 1);

    return 0;
}

int is_mybuilt(Node* node){
    if(!node || node->type != NODE_CMD || node->argv == NULL || node->argv[0] == NULL) return 0;
    char* name = node->argv[0];

    if(strcmp(name, "cd") == 0) return 1;
    if(strcmp(name, "pwd") == 0) return 1;
    if(strcmp(name, "export") == 0) return 1;
    if(strcmp(name, "exit") == 0) return 1;
    if(strcmp(name, "echo") == 0) return 1;
    if(strcmp(name, "unset") == 0) return 1;
    if(strcmp(name, "history") == 0) return 1;
    if(strcmp(name, "jobs") == 0) return 1;
    if(strcmp(name, "fg") == 0) return 1;
    if(strcmp(name, "bg") == 0) return 1;

    return 0;
}

int run_mybuilt(Node* node){
    if(!node || node->type != NODE_CMD || node->argv == NULL || node->argv[0] == NULL){
        fprintf(stderr, "Пустая команда или ошибка парсинга\n");
        return 1;
    }

    char* a = node->argv[0];

    if(strcmp(a, "cd") == 0) return my_cd(node->argv);
    if(strcmp(a, "pwd") == 0) return my_pwd();
    if(strcmp(a, "export") == 0) return my_export(node->argv);
    if(strcmp(a, "unset") == 0) return my_unset(node->argv);
    if(strcmp(a, "jobs") == 0) return my_jobs();
    if(strcmp(a, "bg") == 0) return my_bg(node->argv);
    if(strcmp(a, "fg") == 0) return my_fg(node->argv);
    if(strcmp(a, "echo") == 0) return my_echo(node->argv);
    if(strcmp(a, "exit") == 0){   //stdlib :(
        exit(0);
    }
    if(strcmp(a, "history") == 0){
        if(node->argv[1] && strcmp(node->argv[1], "-c") == 0){
            clear_history();
            char* home = getenv("HOME");
            if(!home) home = ".";
            char* history_path = path_join((char*)home, ".my_history");
            remove(history_path);
            free(history_path);
            printf("История очищена\n");
            return 0;
        }
        HIST_ENTRY** h = history_list(); //массив указателей поэтому **
        if(h){
            for(int i = 0; h[i] != NULL; i++){
                printf("%-4d|  - %s\n", i + history_base, h[i]->line); // history_base - просто корректный номер строки в истории // структура _hist_entry char* line
            }
        }
        return 0;
    }

    return 1; //команда не builtin, поэтому придется создавать shell'у отдельный процесс внешние команды типа ls -> для работы (exec)
}
