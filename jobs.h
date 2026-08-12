#pragma once
#include <sys/types.h>
#include <signal.h>
#include <termios.h>
#include <time.h>
#include <stdbool.h>
#include "token.h"

// jobs bg, fg
typedef enum {
    J_RUNNING,
    J_DONE,
    J_STOPPED
} Jobstate;

typedef struct {
    int id;
    pid_t pgid;
    char* cmdline;
    Jobstate status;
} Job;

typedef struct {
    Job* vector;
    int count;
    int capacity;
    int next_id;  //допустим next_id++ чтобы следующий получил новый айди
} JobVec;

extern JobVec jobs;

//добавление задания
Job* jobs_add(pid_t pgid, char* cmdline);
Job* jobs_by_id(int id);
Job* jobs_by_pgid(pid_t pgid);
void jobs_remove(void);
void jobs_print(void);

//SIGCHLD
extern volatile sig_atomic_t signchild; //не получили сигнала ребенка
void on_child(int signal);
//реальная обработка: вызывается когда signchild==1 (или явно, например из builtin jobs)
void get_children(void);

//терминал/группа процессов самого шелла
extern struct termios shell_settings;
extern pid_t shell_pgid;
extern int shell_terminal;

int  put_job_fg(Job* job, int continuee);
void put_job_bg(Job* job, int continuee);

//задержка перед следующим приглашением после запуска фонового задания
extern struct timespec bg_until;
void add_ms(struct timespec a, int ms, struct timespec* out);
int  timespec_cmp(struct timespec a, struct timespec b);
void maybe_soft_block_prompt(void);
