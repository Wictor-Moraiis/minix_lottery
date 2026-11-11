/* Benchmark de CPU para o MINIX 3.2.1 original (sem Lottery Scheduling). */
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <unistd.h>

#define NUM_WORKERS 6
#define LIMIT 1000000UL

/* Total: 102 repeticoes. */
static const int workloads[NUM_WORKERS] = { 32, 24, 18, 12, 9, 7 };

static unsigned long count_primes(unsigned long limit)
{
    unsigned long n, d, count = 0;
    int prime;

    for (n = 2; n <= limit; n++) {
        prime = 1;
        for (d = 2; d <= n / d; d++) {
            if (n % d == 0) {
                prime = 0;
                break;
            }
        }
        if (prime)
            count++;
    }
    return count;
}

static int read_full(int fd, void *buffer, size_t length)
{
    size_t done = 0;
    ssize_t n;
    char *p = (char *)buffer;

    while (done < length) {
        n = read(fd, p + done, length - done);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return -1;
        done += (size_t)n;
    }
    return 0;
}

static int write_full(int fd, const void *buffer, size_t length)
{
    size_t done = 0;
    ssize_t n;
    const char *p = (const char *)buffer;

    while (done < length) {
        n = write(fd, p + done, length - done);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return -1;
        done += (size_t)n;
    }
    return 0;
}

static long long elapsed_us(const struct timeval *start,
                            const struct timeval *finish)
{
    return ((long long)finish->tv_sec - start->tv_sec) * 1000000LL
         + ((long long)finish->tv_usec - start->tv_usec);
}

int main(void)
{
    int gates[NUM_WORKERS][2];
    int ready_pipe[2];
    int i, j, created = 0, reaped = 0, status, failed = 0;
    int start_ok = 1;
    char ready_status;
    int iteration;
    pid_t pid;
    unsigned long total;
    struct timeval start, finish;
    long long elapsed;

    /* Evita que um pipe sem leitor encerre o pai via SIGPIPE. */
    signal(SIGPIPE, SIG_IGN);

    for (i = 0; i < NUM_WORKERS; i++) {
        if (pipe(gates[i]) == -1) {
            perror("pipe");
            for (j = 0; j < i; j++) {
                close(gates[j][0]);
                close(gates[j][1]);
            }
            return EXIT_FAILURE;
        }
    }

    if (pipe(ready_pipe) == -1) {
        perror("pipe ready");
        for (i = 0; i < NUM_WORKERS; i++) {
            close(gates[i][0]);
            close(gates[i][1]);
        }
        return EXIT_FAILURE;
    }

    fflush(NULL);

    for (i = 0; i < NUM_WORKERS; i++) {
        pid = fork();
        if (pid < 0) {
            perror("fork");
            failed = 1;
            break;
        }

        if (pid == 0) {
            close(ready_pipe[0]);

            /* Cada filho mantem somente a leitura do proprio pipe. */
            for (j = 0; j < NUM_WORKERS; j++) {
                close(gates[j][1]);
                if (j != i)
                    close(gates[j][0]);
            }

            ready_status = 'R';
            if (write_full(ready_pipe[1], &ready_status, 1) == -1) {
                fprintf(stderr, "Worker %d: falha ao sinalizar prontidao.\n", i + 1);
                close(ready_pipe[1]);
                close(gates[i][0]);
                _exit(EXIT_FAILURE);
            }
            close(ready_pipe[1]);

            if (read_full(gates[i][0], &start, sizeof(start)) == -1) {
                fprintf(stderr, "Worker %d: falha na sincronizacao.\n", i + 1);
                close(gates[i][0]);
                _exit(EXIT_FAILURE);
            }
            close(gates[i][0]);

            total = 0;
            for (iteration = 0; iteration < workloads[i]; iteration++)
                total += count_primes(LIMIT - (unsigned long)iteration * 1000UL);

            if (gettimeofday(&finish, NULL) == -1) {
                perror("gettimeofday");
                _exit(EXIT_FAILURE);
            }

            elapsed = elapsed_us(&start, &finish);
            printf("Worker %d | Repeticoes: %d | Resultado: %lu | "
                   "Conclusao: %lld.%03lld segundos\n",
                   i + 1, workloads[i], total,
                   elapsed / 1000000LL, (elapsed % 1000000LL) / 1000LL);
            fflush(stdout);
            _exit(EXIT_SUCCESS);
        }
        created++;
    }

    /* O pai fecha as leituras dos gates e aguarda todos ficarem prontos. */
    for (i = 0; i < NUM_WORKERS; i++)
        close(gates[i][0]);

    close(ready_pipe[1]);
    for (i = 0; i < created; i++) {
        if (read_full(ready_pipe[0], &ready_status, 1) == -1) {
            fprintf(stderr, "Pai: nao recebeu todos os sinais de prontidao.\n");
            failed = 1;
            start_ok = 0;
            break;
        }
        if (ready_status != 'R') {
            fprintf(stderr, "Pai: um trabalhador nao ficou pronto.\n");
            failed = 1;
            start_ok = 0;
        }
    }
    close(ready_pipe[0]);

    /* O cronometro comeca somente depois de todos os filhos estarem prontos. */
    if (start_ok && gettimeofday(&start, NULL) == -1) {
        perror("gettimeofday");
        failed = 1;
        start_ok = 0;
    }

    for (i = 0; i < NUM_WORKERS; i++) {
        if (i < created && start_ok &&
            write_full(gates[i][1], &start, sizeof(start)) == -1) {
            fprintf(stderr, "Pai: falha ao liberar Worker %d.\n", i + 1);
            failed = 1;
        }
        close(gates[i][1]);
    }

    while (reaped < created) {
        pid = wait(&status);
        if (pid == -1) {
            if (errno == EINTR)
                continue;
            perror("wait");
            failed = 1;
            break;
        }
        reaped++;
        if (!WIFEXITED(status) || WEXITSTATUS(status) != EXIT_SUCCESS)
            failed = 1;
    }

    if (created != NUM_WORKERS)
        failed = 1;
    return failed ? EXIT_FAILURE : EXIT_SUCCESS;
}
