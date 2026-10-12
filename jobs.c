#include "common.h" // _POSIX_C_SOURCE 200809L должен быть определён ДО системных заголовков
#include "jobs.h"
#include "memory.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/wait.h>
JobVec jobs = {NULL, 0, 0, 1};// пустой растущий массив заданий; next_id=1 - нумерация как в bash, с [1]
int current_job_id = -1;
int previous_job_id = -1;
//задание становится "текущим" (последнее добавленное или остановленное);
//прежнее текущее сдвигается в "предыдущее"
static void mark_current(int id){
    if(current_job_id == id) return; //уже текущее - ничего не сдвигаем
    previous_job_id = current_job_id;// старое текущее становится предыдущим
    current_job_id = id;// новое задание - текущее
}
//добавление задание
Job* jobs_add(pid_t pgid, char* cmdline){
    if(jobs.count == jobs.capacity){// массив заполнен до предела выделенной памяти
        if(jobs.capacity == 0){
            jobs.capacity = 16;//начальный размер массива, чтобы не выделять по одному элементу
    } else {
            jobs.capacity *= 2;
    }
        jobs.vector = er_realloc(jobs.vector, jobs.capacity*sizeof(Job));// перевыделяем памят
    }
    jobs.vector[jobs.count] = (Job){// заполняем новый элемент через именованные поля
        .id = jobs.next_id++,// присваиваем id, затем увеличиваем счётчик для следующего
        .pgid = pgid,
        .cmdline = er_strdup(cmdline), //возвращаем указатель на новую строку
        .status = J_RUNNING// новое задание считается запущенным
    };
    Job* job = &jobs.vector[jobs.count++];
    mark_current(job->id); //новое фоновое задание становится текущим
    return job;
}

Job* jobs_by_id(int id){// Линейный поиск задания по его пользовательскому номеру
    for(int i = 0; i < jobs.count; i++){
        if(jobs.vector[i].id == id){
            return &jobs.vector[i];// нашли - возвращаем адрес элемента в массиве
        }
    }
    return NULL;
}

Job* jobs_by_pgid(pid_t pgid){// Линейный поиск задания по идентификатору группы процессов
    for(int i = 0; i < jobs.count; i++){
        if(jobs.vector[i].pgid == pgid){
            return &jobs.vector[i];
        }
    }
    return NULL;
}
// Удаляет из массива все завершённые (J_DONE) задания за один проход, сохраняя порядок остальных
void jobs_remove(void){
    int tmp = 0; //создаем какой-нибудь указатель для сдвига удаленных блоков
    for(int i = 0; i < jobs.count; i++){
        if(jobs.vector[i].status == J_DONE){// это задание завершилось - его нужно убрать
            //если удаляем текущее/предыдущее задание - сбрасываем маркеры,
            //чтобы %+ / %- не указывали на несуществующее задание
            if(jobs.vector[i].id == current_job_id)  current_job_id = -1;
            if(jobs.vector[i].id == previous_job_id) previous_job_id = -1;
            free(jobs.vector[i].cmdline);// освобождаем строку командной строки этого задания
            continue;
        }
        if(tmp != i){// перед этим элементом были удалённые - нужно сдвинуть
            jobs.vector[tmp] = jobs.vector[i];
        }
        tmp++;
    }
    jobs.count = tmp;// новый логический размер массива - количество выживших элементов

    if(jobs.count == 0){// заданий совсем не осталось
        jobs.next_id = 1;// нумерация начинается заново, как в bash
        current_job_id = -1;// маркеры точно сброшены
        previous_job_id = -1;
}
}
// Разбирает спецификатор задания из команды пользователя (%1, %+, %-, или просто число) в указатель на Job
Job* resolve_job_spec(const char* spec){
    if(!spec || !*spec) return NULL; // нет строки или она пустая - сразу отказ

    if(spec[0] == '%'){               // bash-стиль: начинается с "%"
        const char* rest = spec + 1;    // всё, что после "%"
        if(strcmp(rest, "+") == 0) return jobs_by_id(current_job_id); // %+ - текущее задание
        if(strcmp(rest, "-") == 0) return jobs_by_id(previous_job_id); // %- - предыдущее задание
        char* end;
        long id = strtol(rest, &end, 10);                // пробуем распознать число после "%"
        if(*rest == '\0' || *end != '\0') return NULL;    // строка была пустой или не ЧИСТО числовой
        return jobs_by_id((int)id);                        // %N - задание с таким номером
    }

    //голое число тоже трактуем как номер задания (уже было такое поведение в fg/bg)
    char* end;
    long id = strtol(spec, &end, 10); // без "%" - тоже пробуем как число целиком
    if(*spec == '\0' || *end != '\0') return NULL;
    return jobs_by_id((int)id);
}

// Печатает список активных (не завершённых) заданий в формате, похожем на bash
void jobs_print(void){
    for(int i = 0; i < jobs.count; i++){
        Job* j = &jobs.vector[i];
        if(j->status == J_DONE) continue; // завершённые задания в списке не показываем

        char* status = (j->status == J_RUNNING) ? "Running" : "Stopped"; // текст статуса
        char marker = ' ';                                                 // по умолчанию - без маркера
        if(j->id == current_job_id)       marker = '+';                  // это текущее задание
        else if(j->id == previous_job_id) marker = '-';                  // это предыдущее задание

        //формат по ТЗ: "[1]+  Running                 sleep 10 &"
        printf("[%d]%c  %-20s %s\n", j->id, marker, status, j->cmdline); // %-20s - выравнивание по левому краю
    }
}

volatile sig_atomic_t signchild = 0; //не получили сигнала ребенка
//функция нужна лишь преобразования флага, но функцию типа signal(), а у нее свой прототип
void on_child(int signal){
    (void)signal;     // параметр не используется - подавляем предупреждение компилятора
    signchild = 1;      // просто поднимаем флаг; никакой другой логики в обработчике сигнала быть не должно
}

//Функция вызывается если получили SIGCHLD - какой-то из дочерних процессов либо спит, либо (fg, bg), либо завершились
void get_children(void){
    int status;
    pid_t pid;
    while((pid = waitpid(-1, &status, WNOHANG | WUNTRACED | WCONTINUED)) > 0 ){ //ничего не произошло pid = (0), чистим мусор //заснувшие процессы //продолженные процессы
        pid_t pgroup = getpgid(pid);        // узнаём группу процессов отчитавшегося pid
        if(pgroup == -1){                     // getpgid не смог определить группу
            pgroup = pid;                        // используем сам pid как fallback
        }
        Job* job = jobs_by_pgid(pgroup);     // ищем задание по группе процессов
        if(!job) continue;                     // это не отслеживаемое задание - просто собрали статус, идём дальше
        if(WIFSTOPPED(status)){                // процесс был остановлен (например, Ctrl+Z)
            job->status = J_STOPPED;
            mark_current(job->id); //только что остановленное задание становится текущим
        }
        else if(WIFCONTINUED(status)) job->status = J_RUNNING; // процесс возобновлён после SIGCONT
        else if(WIFEXITED(status) || WIFSIGNALED(status)) job->status = J_DONE;  //завершился нормально или сигналом
    }
    signchild = 0; // все накопившиеся события обработаны - сбрасываем флаг
}
struct termios shell_settings;    // сохранённые настройки терминала самого шелла
pid_t shell_pgid;                   // id группы процессов самого шелла
int shell_terminal = -1; //пока не проинициализирован
bool shell_interactive = false; //main.c выставит в true, если stdin - терминал

int put_job_fg(Job* job, int continuee){    //continue - останавливался ли процесс
    int status = 0;
    pid_t process;

    tcsetpgrp(shell_terminal, job->pgid);        // отдаём терминал группе процессов этого задания
    if(continuee) kill(-(job->pgid), SIGCONT);  // если задание было остановлено - пробуждаем всю его группу

    while(true){
        process = waitpid(-job->pgid, &status, WUNTRACED); // БЛОКИРУЮЩЕЕ ожидание любого процесса этой группы
        if(process < 0){
            if(errno == EINTR){  //если процессы прерваны сигналом, пробуем снова //SIGCHLD допустим прервал
                continue;
            } else break; // настоящая ошибка - прекращаем ожидание
        }
        if(WIFSTOPPED(status)){       // процесс остановлен (Ctrl+Z) - прерываем foreground-режим
            job->status = J_STOPPED;
            break;
        }
        if(WIFEXITED(status) || WIFSIGNALED(status)){ // процесс завершился
            job->status = J_DONE;
            break;
    }
    }
    tcsetpgrp(shell_terminal, shell_pgid);                    // забираем терминал обратно себе
    tcsetattr(shell_terminal, TCSADRAIN, &shell_settings);   // восстанавливаем свои настройки терминала
    if(WIFEXITED(status)){
        return WEXITSTATUS(status);        // реальный код возврата, если завершился нормально
    } else {
        return 128 + WTERMSIG(status); //возвращаем сигнал - стандартное соглашение bash (128+N)
    }
}

// Возобновляет остановленное задание В ФОНЕ - терминал не трогаем, не ждём завершения
void put_job_bg(Job* job, int continuee){
    if(continuee){
        kill(-(job->pgid), SIGCONT); // пробуждаем всю группу процессов, не забирая терминал
    }
}
//время задержки

// Добавляет ms миллисекунд к моменту времени a, результат пишет в out (через указатель)
void add_ms(struct timespec a, int ms, struct timespec* out){
    long ns = a.tv_nsec + (long)ms * 1000000L; // переводим ms в наносекунды и складываем с исходными
    out->tv_sec  = a.tv_sec + ns/1000000000L;    // "лишние" целые секунды переносим в tv_sec
    out->tv_nsec = ns % 1000000000L;              // остаток - корректные наносекунды в пределах секунды
}

// Сравнивает два timespec: -1 если a<b, 1 если a>b, 0 если равны
int timespec_cmp(struct timespec a, struct timespec b){
    if(a.tv_sec < b.tv_sec)
        return -1;                  // секунды меньше - a раньше
    else if(a.tv_sec > b.tv_sec)
        return 1;                    // секунды больше - a позже
    if(a.tv_nsec < b.tv_nsec)       // секунды равны - сравниваем наносекунды
        return -1;
    if(a.tv_nsec > b.tv_nsec)
        return 1;
    return 0;                        // полностью равны
}

struct timespec bg_until = (struct timespec){0, 0}; // момент, до которого нужно подождать; {0,0} = нет задержки

void maybe_soft_block_prompt(void){  //ждем до установленного времени
    if(bg_until.tv_sec==0 && bg_until.tv_nsec==0) return; // нет активной задержки - мгновенный выход
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now); // монотонные часы - не подвержены скачкам системного времени
    if(timespec_cmp(now, bg_until) < 0){   // целевой момент ещё не наступил
        struct timespec need;
        need.tv_sec  = bg_until.tv_sec  - now.tv_sec;        // наивная разница секунд
        long nsec = (long)bg_until.tv_nsec - (long)now.tv_nsec; // наивная разница наносекунд
        if(nsec < 0){                 // наносекунды "ушли в минус" - нужен заём из секунд
            need.tv_sec--;
            nsec += 1000000000L;
        }
        need.tv_nsec = nsec;
        if(need.tv_sec < 0){          // защита: на случай гонки по времени не допускаем отрицательный интервал
            need.tv_sec = 0;
            need.tv_nsec = 0;
        }
        nanosleep(&need, NULL);      // реальная пауза на вычисленный остаток времени
    }
    // одноразовый блок — обнуляем
    bg_until = (struct timespec){0,0}; // задержка одноразовая - сбрасываем после использования
}