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
//Статус задания НЕ хранится сам по себе, а вычисляется из состояний его процессов (как в bash):
//есть хоть один работающий - Running; работающих нет, но есть остановленные - Stopped; иначе Done.
static void job_recompute(Job* job){
    bool run = false, stop = false;
    for(int i = 0; i < job->nprocs; i++){
        if(job->procs[i].state == P_RUNNING) run = true;
        else if(job->procs[i].state == P_STOPPED) stop = true;
    }
    if(run) job->status = J_RUNNING;
    else if(stop) job->status = J_STOPPED;
    else job->status = J_DONE;
}
//заменить список процессов задания на настоящие pid'ы всех этапов конвейера (все стартуют работающими)
void job_set_procs(Job* job, const pid_t* pids, int n){
    free(job->procs);
    job->procs = er_malloc((size_t)n * sizeof(Proc));
    for(int i = 0; i < n; i++) job->procs[i] = (Proc){pids[i], P_RUNNING};
    job->nprocs = n;
}
//учесть событие waitpid для процесса pid задания: остановлен / продолжен / завершён
void job_note_event(Job* job, pid_t pid, int status){
    for(int i = 0; i < job->nprocs; i++){
        if(job->procs[i].pid != pid) continue;
        if(WIFSTOPPED(status)) job->procs[i].state = P_STOPPED;
        else if(WIFCONTINUED(status)) job->procs[i].state = P_RUNNING;
        else job->procs[i].state = P_DONE; //WIFEXITED / WIFSIGNALED
        break;
    }
    job_recompute(job);
}
//всей группе послали SIGCONT: все остановленные процессы считаем работающими
void job_all_running(Job* job){
    for(int i = 0; i < job->nprocs; i++)
        if(job->procs[i].state == P_STOPPED) job->procs[i].state = P_RUNNING;
    job_recompute(job);
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
        .status = J_RUNNING,
        .procs = NULL,
        .nprocs = 0
    };
    Job* job = &jobs.vector[jobs.count++];
    //по умолчанию в задании один процесс (pid == pgid); у конвейера список заменяет job_set_procs
    job->procs = er_malloc(sizeof(Proc));
    job->procs[0] = (Proc){pgid, P_RUNNING};
    job->nprocs = 1;
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
            free(jobs.vector[i].procs);
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
    for(int i = 0; i < jobs.count; i++){
        Job* job = &jobs.vector[i];
        if(job->status == J_DONE) continue;
        int status;
        pid_t pid;
        while((pid = waitpid(-job->pgid, &status, WNOHANG | WUNTRACED | WCONTINUED)) > 0){
            job_note_event(job, pid, status); //обновляем состояние именно ЭТОГО процесса, статус задания пересчитается из всех
            if(job->status == J_STOPPED) mark_current(job->id); //все живые остановлены - задание становится текущим
        }
        if(pid < 0 && errno == ECHILD){ //в группе больше нет процессов
            for(int k = 0; k < job->nprocs; k++) job->procs[k].state = P_DONE;
            job->status = J_DONE;
        }
    }
    while(waitpid(-1, NULL, WNOHANG) > 0){} //подбираем прочих завершившихся потомков (не из заданий), чтобы не было зомби
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
    if(continuee){ kill(-(job->pgid), SIGCONT); job_all_running(job); }

    int last_status = 0; //статус последнего завершившегося процесса группы
    while(true){
        process = waitpid(-job->pgid, &status, WUNTRACED);
        if(process < 0){
            if(errno == EINTR){  //если процессы прерваны сигналом, пробуем снова //SIGCHLD допустим прервал
                continue;
            }
            job->status = J_DONE; //ECHILD: в группе не осталось процессов - задание завершено целиком
            status = last_status;
            break;
        }
        job_note_event(job, process, status);
        if(WIFSTOPPED(status)){
            if(job->status == J_STOPPED) break; //остановлены все живые процессы - возвращаем приглашение
            continue; //остановился лишь один процесс, остальные работают - продолжаем ждать
        }
        //завершился один процесс группы; у конвейера есть и другие - ждём их всех
        last_status = status;
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
        job_all_running(job);
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