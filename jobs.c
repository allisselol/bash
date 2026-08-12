#include "jobs.h"
#include "memory.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/wait.h>

JobVec jobs = {NULL, 0, 0, 1};

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
    return &jobs.vector[jobs.count++]; //возвращаем указатель на этот элемент
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
    }
}

void jobs_print(void){
    for(int i = 0; i < jobs.count; i++){
        char* status;
        if(jobs.vector[i].status == J_RUNNING){
            status = "Running";
        }
        else if(jobs.vector[i].status == J_DONE){
            continue;
        }
        else {
            status = "stoppped";
        }
        printf("[%d], %8s, %s \n",  jobs.vector[i].id, status, jobs.vector[i].cmdline);
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
        if(WIFSTOPPED(status)) job->status = J_STOPPED;
        else if(WIFCONTINUED(status)) job->status = J_RUNNING;
        else if(WIFEXITED(status) || WIFSIGNALED(status)) job->status = J_DONE;  //завершился нормально или сигналом
    }
    signchild = 0;
}

struct termios shell_settings;
pid_t shell_pgid;
int shell_terminal = -1; //пока не проинициализирован

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
