#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>


double
getdetlatimeofday(struct timeval *begin, struct timeval *end)
{
    return (end->tv_sec + end->tv_usec * 1.0 / 1000000) -
           (begin->tv_sec + begin->tv_usec * 1.0 / 1000000);
}

int
main(int argc, char *argv[]) {
    int              pipefd[2] = {0};

    long long        i, size, count, sum, n;
    char            *buf;
    struct timeval   begin, end;

#ifdef __VMS
    int is_child = 0;
    if (argc == 4) {
        argc--;
        is_child = 1;
        pipefd[0] = atoi(argv[3]);
    }
#endif

    if (argc != 3) {
        printf("usage: ./pipe <size> <count>\n");
        return 1;
    }

    size = atoi(argv[1]);
    count = atoi(argv[2]);

    buf = malloc(size);
    if (buf == NULL) {
        perror("malloc");
        return 1;
    }

#ifdef __VMS
    if (!is_child)
#endif
    if (pipe(pipefd) == -1) {
        perror("pipe");
        return 1;
    }

#ifdef __VMS
    if (is_child)
#else
    if (fork() == 0)
#endif
    {
        sum = 0;
        while (sum < (count * size)) {
            n = read(pipefd[0], buf, size);
            if (n == -1) {
                perror("read");
                return 1;
            }

            sum += n;
        }

        if (sum != count * size) {
            fprintf(stderr, "sum error: %lld != %lld\n", sum, count * size);
            return 1;
        }

    } else {
#ifdef __VMS
        char *argv_copy[5];
        char read_fd[8];
        if (vfork() == 0) {
            /* exec myself with the pipe fd as the 3rd argument */
            snprintf(read_fd, 8, "%d", pipefd[0]);
            for (i = 0; i < 3; i++) {
                argv_copy[i] = argv[i];
            }
            argv_copy[3] = read_fd;
            argv_copy[4] = NULL;
            if (execvp(argv[0], argv_copy)) {
                perror("execvp");
                return 1;
            }
        }
        /* continue as the parent */
#endif
        gettimeofday(&begin, NULL);

        for (i = 0; i < count; i++) {
            if (write(pipefd[1], buf, size) != size) {
                perror("write");
                return 1;
            }
        }
        gettimeofday(&end, NULL);

        double tm = getdetlatimeofday(&begin, &end);
        printf("%.0fMB/s %.0fmsg/s\n",
            count * size * 1.0 / (tm * 1024 * 1024),
            count * 1.0 / tm);
    }

    return 0;
}
