#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/shm.h>
#include <sys/sem.h>
#include <sys/ipc.h>
#include <sys/types.h>
#include <sys/time.h>

typedef union {
    int val;
    struct semid_ds *buf;
    unsigned short *array ;
#ifndef __VMS
    struct seminfo *__buf;
    void   *__pad ;
#endif
} semun;

void sem_init(int sem_id, int sem_num, int init_valve)
{
    semun sem_union;
    sem_union.val = init_valve;
    if (semctl(sem_id, sem_num, SETVAL, sem_union))
    {
        perror("semctl");
        exit(-1);
    }
}

void sem_release(int sem_id, int sem_num)
{
    struct sembuf sem_b;
    sem_b.sem_num = sem_num;
    sem_b.sem_op = 1;
    sem_b.sem_flg = 0;
    if (semop(sem_id, &sem_b, 1) == -1)
    {
        perror("sem_release");
        exit(-1);
    }
}

void sem_reserve(int sem_id, int sem_num)
{
    struct sembuf sem_b;
    sem_b.sem_num = sem_num;
    sem_b.sem_op = -1;
    sem_b.sem_flg = 0;
    if (semop(sem_id, &sem_b, 1) == -1)
    {
        perror("sem_reserve");
        exit(-1);
    }
}

double getdetlatimeofday(struct timeval *begin, struct timeval *end)
{
    return (end->tv_sec + end->tv_usec * 1.0 / 1000000) -
           (begin->tv_sec + begin->tv_usec * 1.0 / 1000000);
}

int main(int argc, char *argv[])
{
#define SHM_KEY 0x1234
#define SEM_KEY 0x5678
#define WRITE_SEM 0
#define READ_SEM 1

    pid_t pid;
    int sem_id;
    int shm_id;
    long long count, i, size;
    struct timeval begin, end;

#ifdef __VMS
    static char* child_flag = "----";
    int is_child = 0;
    if (argc == 4 && !strcmp(argv[3], child_flag)) {
        argc--;
        is_child = 1;
    }
#endif

    if (argc != 3)
    {
        printf("usage: ./shm <size> <count>\n");
        return 1;
    }

    size = atoi(argv[1]);
    count = atoi(argv[2]);
    unsigned char *buf = malloc(size);

#ifdef __VMS
    if (is_child)
#else
    pid = fork();
    if (pid == -1)
    {
        perror("fork");
        return -1;
    }
    else if (pid == 0) // child
#endif
    {
        sem_id = semget(SEM_KEY, 2, 0600 | IPC_CREAT);
        if (sem_id == -1)
        {
            perror("child: semget");
            return -1;
        }
        sem_init(sem_id, WRITE_SEM, 1);
        sem_init(sem_id, READ_SEM, 0);

        shm_id = shmget(SHM_KEY, size, IPC_CREAT | 0600);
        if (shm_id == -1)
        {
            perror("child: shmget");
            return -1;
        }
        void *addr = shmat(shm_id, NULL, 0);
        if (addr == (void *)-1)
        {
            perror("child: shmat");
            return -1;
        }

        for (i = 0; i < count; i++)
        {
            sem_reserve(sem_id, READ_SEM);
            memcpy(buf, addr, size);
            // printf(">>>>>>>>%d\n", *(int*)buf);
            sem_release(sem_id, WRITE_SEM);
        }

        if (shmdt(addr) == -1)
        {
            perror("child: shmdt");
            return -1;
        }
    }
    else // parent
    {
#ifdef __VMS
        char *argv_copy[5];
        int sleep_time = 5;
        if (vfork() == 0) {
            /* exec myself with child_flag as the 3rd argument */
            for (i = 0; i < 3; i++) {
                argv_copy[i] = argv[i];
            }
            argv_copy[3] = child_flag;
            argv_copy[4] = NULL;
            if (execvp(argv[0], argv_copy)) {
                perror("execvp");
                return 1;
            }
        }
        /* continue as the parent */
        while (sleep_time > 0) {
            sleep_time = sleep(sleep_time);
        }
#else
        sleep(1);
#endif
        sem_id = semget(SEM_KEY, 0, 0);
        if (sem_id == -1)
        {
            perror("parent: semget");
            return -1;
        }

        shm_id = shmget(SHM_KEY, 0, 0);
        if (shm_id == -1)
        {
            perror("parent: shmget");
            return -1;
        }
        void *addr = (int *)shmat(shm_id, NULL, 0);
        if (addr == (void *)-1)
        {
            perror("parent: shmat");
            return -1;
        }

        gettimeofday(&begin, NULL);

        for (i = 0; i < count; i++)
        {
            sem_reserve(sem_id, WRITE_SEM);
            // *(int*)buf = i;
            memcpy(addr, buf, size);
            sem_release(sem_id, READ_SEM);
        }

        gettimeofday(&end, NULL);

        double tm = getdetlatimeofday(&begin, &end);
        printf("%.0fMB/s %.0fmsg/s\n",
               count * size * 1.0 / (tm * 1024 * 1024),
               count * 1.0 / tm);

        sem_reserve(sem_id, WRITE_SEM);
        semun dummy = {0};
        semctl(sem_id, 0, IPC_RMID, dummy);
        if (shmdt(addr) == -1)
        {
            perror("parent: shmdt");
            return -1;
        }
        shmctl(shm_id, IPC_RMID, 0);
    }

    return 0;
}
