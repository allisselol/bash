#include "common.h" // _POSIX_C_SOURCE 200809L должен быть определён ДО системных заголовков
#include "jobs.h"
#include "memory.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/wait.h>

JobVec jobs = {NULL, 0, 0, 1};
int current_job_id = -1;
int previous_job_id = -1;

//задание становится "текущим" (последнее добавленное или остановленное);
//прежнее текущее сдвигается в "предыдущее"
static void mark_current(int id){
    if(current_job_id == id) return; //уже текущее - ничего не сдвигаем
    previous_job_id = current_job_id;
    current_job_id = id;
}

//добавление задание
Job* jobs_add(pid_t pgid, char* cmdline){
    if(jobs.count == jobs.capacity){
        if(jobs.capacity == 0){
            jobs.capacity = 16;
        } else {
            jobs.capacity *= 2;
        }
        jobs.vector = er_realloc(jobs.vector, jobs.capacity*sizeof(Job));
    }
    jobs.vector[jobs.count] = (Job){
        .id = jobs.next_id++,
        .pgid = pgid,
        .cmdline = er_strdup(cmdline), //возвращаем указатель на новую строку
        .status = J_RUNNING
    };
    Job* job = &jobs.vector[jobs.count++];
    mark_current(job->id); //новое фоновое задание становится текущим
    return job;
}

Job* jobs_by_id(int id){
    for(int i = 0; i < jobs.count; i++){
        if(jobs.vector[i].id == id){
            return &jobs.vector[i];
        }
    }
    return NULL;
}

Job* jobs_by_pgid(pid_t pgid){
    for(int i = 0; i < jobs.count; i++){
        if(jobs.vector[i].pgid == pgid){
            return &jobs.vector[i];
        }
    }
    return NULL;
}

void jobs_remove(void){
    int tmp = 0; //создаем какой-нибудь указатель для сдвига удаленных блоков
    for(int i = 0; i < jobs.count; i++){
        if(jobs.vector[i].status == J_DONE){
            //если удаляем текущее/предыдущее задание - сбрасываем маркеры,
            //чтобы %+ / %- не указывали на несуществующее задание
            if(jobs.vector[i].id == current_job_id)  current_job_id = -1;
            if(jobs.vector[i].id == previous_job_id) previous_job_id = -1;
            free(jobs.vector[i].cmdline);
            continue;
        }
        if(tmp != i){
            jobs.vector[tmp] = jobs.vector[i];
        }
        tmp++;
    }
    jobs.count = tmp;

    if(jobs.count == 0){
        jobs.next_id = 1;
        current_job_id = -1;
        previous_job_id = -1;
    }
}

Job* resolve_job_spec(const char* spec){
    if(!spec || !*spec) return NULL;

    if(spec[0] == '%'){
        const char* rest = spec + 1;
        if(strcmp(rest, "+") == 0) return jobs_by_id(current_job_id);
        if(strcmp(rest, "-") == 0) return jobs_by_id(previous_job_id);
        char* end;
        long id = strtol(rest, &end, 10);
        if(*rest == '\0' || *end != '\0') return NULL;
        return jobs_by_id((int)id);
    }

    //голое число тоже трактуем как номер задания (уже было такое поведение в fg/bg)
    char* end;
    long id = strtol(spec, &end, 10);
    if(*spec == '\0' || *end != '\0') return NULL;
    return jobs_by_id((int)id);
}

void jobs_print(void){
    for(int i = 0; i < jobs.count; i++){
        Job* j = &jobs.vector[i];
        if(j->status == J_DONE) continue;

        char* status = (j->status == J_RUNNING) ? "Running" : "Stopped";
        char marker = ' ';
        if(j->id == current_job_id)       marker = '+';
        else if(j->id == previous_job_id) marker = '-';

        //формат по ТЗ: "[1]+  Running                 sleep 10 &"
        printf("[%d]%c  %-20s %s\n", j->id, marker, status, j->cmdline);
    }
}

volatile sig_atomic_t signchild = 0; //не получили сигнала ребенка
//функция нужна лишь преобразования флага, но функцию типа signal(), а у нее свой прототип
void on_child(int signal){
    (void)signal;
    signchild = 1;
}

//Функция вызывается если получили SIGCHLD - какой-то из дочерних процессов либо спит, либо (fg, bg), либо завершились
void get_children(void){
    int status;
    pid_t pid;
    while((pid = waitpid(-1, &status, WNOHANG | WUNTRACED | WCONTINUED)) > 0 ){ //ничего не произошло pid = (0), чистим мусор //заснувшие процессы //продолженные процессы
        pid_t pgroup = getpgid(pid);
        if(pgroup == -1){
            pgroup = pid;
        }
        Job* job = jobs_by_pgid(pgroup);
        if(!job) continue;
        if(WIFSTOPPED(status)){
            job->status = J_STOPPED;
            mark_current(job->id); //только что остановленное задание становится текущим
        }
        else if(WIFCONTINUED(status)) job->status = J_RUNNING;
        else if(WIFEXITED(status) || WIFSIGNALED(status)) job->status = J_DONE;  //завершился нормально или сигналом
    }
    signchild = 0;
}

struct termios shell_settings;
pid_t shell_pgid;
int shell_terminal = -1; //пока не проинициализирован
bool shell_interactive = false; //main.c выставит в true, если stdin - терминал

int put_job_fg(Job* job, int continuee){    //continue - останавливался ли процесс
    int status = 0;
    pid_t process;

    tcsetpgrp(shell_terminal, job->pgid);
    if(continuee) kill(-(job->pgid), SIGCONT);

    while(true){
        process = waitpid(-job->pgid, &status, WUNTRACED);
        if(process < 0){
            if(errno == EINTR){  //если процессы прерваны сигналом, пробуем снова //SIGCHLD допустим прервал
                continue;
            } else break;
        }
        if(WIFSTOPPED(status)){
            job->status = J_STOPPED;
            break;
        }

        if(WIFEXITED(status) || WIFSIGNALED(status)){
            job->status = J_DONE;
            break;
        }
    }

    tcsetpgrp(shell_terminal, shell_pgid);
    tcsetattr(shell_terminal, TCSADRAIN, &shell_settings);

    if(WIFEXITED(status)){
        return WEXITSTATUS(status);
    } else {
        return 128 + WTERMSIG(status); //возвращаем сигнал
    }
}

void put_job_bg(Job* job, int continuee){
    if(continuee){
        kill(-(job->pgid), SIGCONT);
    }
}

//время задержки

void add_ms(struct timespec a, int ms, struct timespec* out){
    long ns = a.tv_nsec + (long)ms * 1000000L;
    out->tv_sec  = a.tv_sec + ns/1000000000L;
    out->tv_nsec = ns % 1000000000L;
}

int timespec_cmp(struct timespec a, struct timespec b){
    if(a.tv_sec < b.tv_sec)
        return -1;
    else if(a.tv_sec > b.tv_sec)
        return 1;
    if(a.tv_nsec < b.tv_nsec)
        return -1;
    if(a.tv_nsec > b.tv_nsec)
        return 1;
    return 0;
}

struct timespec bg_until = (struct timespec){0, 0};

void maybe_soft_block_prompt(void){  //ждем до установленного времени
    if(bg_until.tv_sec==0 && bg_until.tv_nsec==0) return;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    if(timespec_cmp(now, bg_until) < 0){
        struct timespec need;
        need.tv_sec  = bg_until.tv_sec  - now.tv_sec;
        long nsec = (long)bg_until.tv_nsec - (long)now.tv_nsec;
        if(nsec < 0){
            need.tv_sec--;
            nsec += 1000000000L;
        }
        need.tv_nsec = nsec;
        if(need.tv_sec < 0){
            need.tv_sec = 0;
            need.tv_nsec = 0;
        }
        nanosleep(&need, NULL);
    }
    // одноразовый блок — обнуляем
    bg_until = (struct timespec){0,0};
}
