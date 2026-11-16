#include <lib.h>
#include <minix/com.h>
#include <minix/callnr.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <unistd.h>

#define NUM_WORKERS 6
#define LIMIT 1000000UL

/*
 * Cada trabalhador recebe uma carga diferente.
 * A soma das repeticoes e 102.
 */
static const int workloads[NUM_WORKERS] = {
    32, 24, 18, 12, 9, 7
};

/*
 * Conta a quantidade de numeros primos
 * existentes ate o limite informado.
 */
static unsigned long count_primes(unsigned long limit)
{
    unsigned long number;
    unsigned long divisor;
    unsigned long count;
    int is_prime;

    count = 0;

    for (number = 2; number <= limit; number++) {

        is_prime = 1;

        for (divisor = 2; divisor <= number / divisor; divisor++) {

            if (number % divisor == 0) {
                is_prime = 0;
                break;
            }
        }

        if (is_prime)
            count++;
    }

    return count;
}

static int set_lottery_tickets(int tickets)
{
    message m;

    memset(&m, 0, sizeof(m));

    m.m1_i1 = tickets;

    return _syscall(
        PM_PROC_NR,
        PM_SET_LOTTERY_TICKETS,
        &m
    );
}

/*
 * Calcula o tempo decorrido em microssegundos.
 */
static long long elapsed_microseconds(
    struct timeval start,
    struct timeval finish)
{
    return ((long long)finish.tv_sec - (long long)start.tv_sec)
            * 1000000LL
         + ((long long)finish.tv_usec - (long long)start.tv_usec);
}

int main(void)
{
    int gate[2];
    int i;
    int created;
    int status;
    int failed;
    int iteration;
    int repetitions;

    pid_t pid;

    ssize_t bytes_read;
    ssize_t bytes_written;

    char start_signal;
    char start_signals[NUM_WORKERS];

    unsigned long total;

    struct timeval benchmark_start;
    struct timeval finish_time;

    long long elapsed;

    created = 0;
    failed = 0;

    /* Cria o canal de sincronizacao */
    if (pipe(gate) == -1) {
        perror("pipe");
        return EXIT_FAILURE;
    }

    fflush(stdout);

    /*
     * Todos os filhos herdarao o mesmo instante inicial.
     */
    if (gettimeofday(&benchmark_start, NULL) == -1) {
        perror("gettimeofday");
        close(gate[0]);
        close(gate[1]);
        return EXIT_FAILURE;
    }

    /* Cria os processos trabalhadores */
    for (i = 0; i < NUM_WORKERS; i++) {

        pid = fork();

        if (pid < 0) {
            perror("fork");
            failed = 1;
            break;
        }

        if (pid == 0) {

        close(gate[1]);

        /* Registra os tickets do próprio trabalhador. */
        if (set_lottery_tickets(workloads[i]) != 0) {
            fprintf(stderr,
                "Worker %d: falha ao registrar tickets.\n",
                i + 1);

            exit(EXIT_FAILURE);
        }

        /* Aguarda a liberação para iniciar. */
        bytes_read = read(gate[0], &start_signal, 1);

        close(gate[0]);

        /* Continua o processamento... */
    }

            if (bytes_read != 1) {
                fprintf(stderr,
                    "Worker %d: falha ao iniciar.\n", i + 1);
                exit(EXIT_FAILURE);
            }

            total = 0;
            repetitions = workloads[i];

            /* Executa a carga de trabalho */
            for (iteration = 0;
                 iteration < repetitions;
                 iteration++) {

                total += count_primes(
                    LIMIT - (unsigned long)iteration * 1000UL
                );
            }

            /* Registra o instante de conclusao */
            if (gettimeofday(&finish_time, NULL) == -1) {
                perror("gettimeofday");
                exit(EXIT_FAILURE);
            }

            elapsed = elapsed_microseconds(
                benchmark_start, finish_time
            );

            printf(
                "Worker %d | Repeticoes: %d | Resultado: %lu | "
                "Conclusao: %lld.%03lld segundos\n",
                i + 1,
                repetitions,
                total,
                elapsed / 1000000LL,
                (elapsed % 1000000LL) / 1000LL
            );

            fflush(stdout);
            exit(EXIT_SUCCESS);
        }

        created++;
    }

    /* Libera os trabalhadores */
    close(gate[0]);

    for (i = 0; i < created; i++) {
        start_signals[i] = 'S';
    }

    if (created > 0) {

        bytes_written = write(
            gate[1],
            start_signals,
            (size_t)created
        );

        if (bytes_written != created) {
            perror("write");
            failed = 1;
        }
    }

    close(gate[1]);

    /* Aguarda todos os trabalhadores */
    for (i = 0; i < created; i++) {

        pid = wait(&status);

        if (pid == -1) {
            perror("wait");
            failed = 1;
            break;
        }

        if (!WIFEXITED(status) ||
            WEXITSTATUS(status) != EXIT_SUCCESS) {
            failed = 1;
        }
    }

    if (created != NUM_WORKERS)
        failed = 1;

    if (failed)
        return EXIT_FAILURE;

    return EXIT_SUCCESS;
}
