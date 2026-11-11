
/* Benchmark de CPU para o MINIX 3.2.1 com Lottery Scheduling. */

#include <lib.h>
#include <minix/com.h>
#include <minix/callnr.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <unistd.h>
#include <fcntl.h>

#define NUM_WORKERS 2
#define LIMIT 1000000UL
#define SAMPLE_EVERY 10

/* Todos executam a mesma quantidade de trabalho. */
static const int workloads[NUM_WORKERS] = {
    160, 160
};

/* Cada trabalhador recebe uma quantidade diferente de tickets. */
static const int tickets[NUM_WORKERS] = {
    10, 30
};

/* Estrutura com os contadores do Lottery Scheduling. */
struct lottery_stats {
    unsigned int tickets;
    unsigned long wins;
    unsigned long context_switches_in;
    unsigned long context_switches_out;
    unsigned long quantum_expirations;
    unsigned long cycles_hi;
    unsigned long cycles_lo;
};

/* Conta numeros primos para gerar carga de CPU. */
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

/* Registra os tickets do proprio processo no PM/SCHED. */
static int set_lottery_tickets(int ticket_count)
{
    message m;

    memset(&m, 0, sizeof(m));
    m.m1_i1 = ticket_count;

    return _syscall(PM_PROC_NR, PM_SET_LOTTERY_TICKETS, &m);
}

/* Le todos os bytes solicitados de um pipe. */
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

/* Escreve todos os bytes solicitados em um pipe. */
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

/* Calcula o tempo decorrido em microssegundos. */
static long long elapsed_us(const struct timeval *start,
                            const struct timeval *finish)
{
    return ((long long)finish->tv_sec - start->tv_sec) * 1000000LL
         + ((long long)finish->tv_usec - start->tv_usec);
}

/* Le os contadores disponibilizados pelo ProcFS. */
static int read_lottery_stats(pid_t pid, struct lottery_stats *stats)
{
    char path[64];
    char line[128];
    unsigned int found = 0;
    FILE *fp;

    snprintf(path, sizeof(path),
             "/proc/%ld/lottery", (long)pid);

    fp = fopen(path, "r");

    if (fp == NULL)
        return -1;

    memset(stats, 0, sizeof(*stats));

    while (fgets(line, sizeof(line), fp) != NULL) {

        if (sscanf(line, "tickets=%u",
                   &stats->tickets) == 1)
            found |= 1;

        else if (sscanf(line, "wins=%lu",
                        &stats->wins) == 1)
            found |= 2;

        else if (sscanf(line, "context_switches_in=%lu",
                        &stats->context_switches_in) == 1)
            found |= 4;

        else if (sscanf(line, "context_switches_out=%lu",
                        &stats->context_switches_out) == 1)
            found |= 8;

        else if (sscanf(line, "quantum_expirations=%lu",
                        &stats->quantum_expirations) == 1)
            found |= 16;

        else if (sscanf(line, "cycles_hi=%lu",
                        &stats->cycles_hi) == 1)
            found |= 32;

        else if (sscanf(line, "cycles_lo=%lu",
                        &stats->cycles_lo) == 1)
            found |= 64;
    }

    fclose(fp);

    return found == 127 ? 0 : -1;
}

/* Grava uma fotografia dos contadores no arquivo de resultados. */
static void log_lottery_snapshot(
    int log_fd,
    int worker,
    pid_t pid,
    int repetitions,
    const struct timeval *start,
    const char *phase)
{
    struct lottery_stats stats;
    struct timeval now;
    long long elapsed;
    char buffer[512];
    int length;

    if (gettimeofday(&now, NULL) == -1)
        return;

    elapsed = elapsed_us(start, &now);

    if (read_lottery_stats(pid, &stats) == -1) {

        length = snprintf(
            buffer, sizeof(buffer),
            "Worker %d | PID %ld | ERRO ao ler "
            "/proc/%ld/lottery | errno=%d\n",
            worker, (long)pid, (long)pid, errno
        );

    } else {

        length = snprintf(
            buffer, sizeof(buffer),
            "T+%lld.%03llds | Worker %d | %s | PID=%ld "
            "| Repeticoes=%d | Tickets=%u | Wins=%lu "
            "| Context_in=%lu | Context_out=%lu "
            "| Quantum_exp=%lu | Cycles_hi=%lu | Cycles_lo=%lu\n",

            elapsed / 1000000LL,
            (elapsed % 1000000LL) / 1000LL,
            worker,
            phase,
            (long)pid,
            repetitions,
            stats.tickets,
            stats.wins,
            stats.context_switches_in,
            stats.context_switches_out,
            stats.quantum_expirations,
            stats.cycles_hi,
            stats.cycles_lo
        );
    }

    if (length > 0 && length < (int)sizeof(buffer))
        (void)write(log_fd, buffer, (size_t)length);
}

int main(void)
{
    int gates[NUM_WORKERS][2];
    int ready_pipe[2];

    int i, j;
    int created = 0;
    int reaped = 0;
    int status;
    int failed = 0;
    int start_ok = 1;
    int ticket_result;
    int iteration;
    int log_fd;

    char ready_status;
    pid_t pid;

    unsigned long total;

    struct timeval start, finish;
    long long elapsed;

    signal(SIGPIPE, SIG_IGN);

    /* Cria os pipes de sincronizacao. */
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

    /* Cria o arquivo de resultados. */
    log_fd = open(
        "resultado_lottery.txt",
        O_WRONLY | O_CREAT | O_TRUNC | O_APPEND,
        0644
    );

    if (log_fd == -1) {
        perror("resultado_lottery.txt");

        close(ready_pipe[0]);
        close(ready_pipe[1]);

        for (i = 0; i < NUM_WORKERS; i++) {
            close(gates[i][0]);
            close(gates[i][1]);
        }

        return EXIT_FAILURE;
    }

    {
        const char *header =
            "=== BENCHMARK LOTTERY SCHEDULING ===\n"
            "Workers: 2 | Workload: 160 | Tickets: 10 e 30\n\n";

        (void)write(log_fd, header, strlen(header));
    }

    fflush(NULL);

    /* Cria os processos trabalhadores. */
    for (i = 0; i < NUM_WORKERS; i++) {

        pid = fork();

        if (pid < 0) {
            perror("fork");
            failed = 1;
            break;
        }

        if (pid == 0) {

            close(ready_pipe[0]);

            for (j = 0; j < NUM_WORKERS; j++) {
                close(gates[j][1]);

                if (j != i)
                    close(gates[j][0]);
            }

            /* Registra os tickets do trabalhador. */
            ticket_result = set_lottery_tickets(tickets[i]);

            if (ticket_result != 0) {

                fprintf(stderr,
                    "Worker %d: falha ao registrar %d tickets "
                    "(retorno=%d, errno=%d).\n",
                    i + 1, tickets[i], ticket_result, errno
                );

                ready_status = 'E';

                (void)write_full(
                    ready_pipe[1], &ready_status, 1
                );

                close(ready_pipe[1]);
                close(gates[i][0]);
                close(log_fd);

                _exit(EXIT_FAILURE);
            }

            /* Informa ao pai que esta pronto. */
            ready_status = 'R';

            if (write_full(
                    ready_pipe[1], &ready_status, 1) == -1) {

                close(ready_pipe[1]);
                close(gates[i][0]);
                close(log_fd);

                _exit(EXIT_FAILURE);
            }

            close(ready_pipe[1]);

            /* Aguarda o sinal de inicio do pai. */
            if (read_full(
                    gates[i][0], &start, sizeof(start)) == -1) {

                close(gates[i][0]);
                close(log_fd);

                _exit(EXIT_FAILURE);
            }

            close(gates[i][0]);

            total = 0;

            /* Executa o trabalho e registra os contadores. */
            for (iteration = 0;
                 iteration < workloads[i];
                 iteration++) {

                total += count_primes(
                    LIMIT - (unsigned long)iteration * 1000UL
                );

                if ((iteration + 1) % SAMPLE_EVERY == 0) {

                    log_lottery_snapshot(
                        log_fd,
                        i + 1,
                        getpid(),
                        iteration + 1,
                        &start,
                        "ANDAMENTO"
                    );
                }
            }

            if (gettimeofday(&finish, NULL) == -1) {
                close(log_fd);
                _exit(EXIT_FAILURE);
            }

            elapsed = elapsed_us(&start, &finish);

            /* Registra a ultima fotografia antes de sair. */
            log_lottery_snapshot(
                log_fd,
                i + 1,
                getpid(),
                workloads[i],
                &start,
                "FINAL"
            );

            printf(
                "Worker %d | Tickets: %d | Repeticoes: %d "
                "| Resultado: %lu | Conclusao: %lld.%03lld segundos\n",

                i + 1,
                tickets[i],
                workloads[i],
                total,
                elapsed / 1000000LL,
                (elapsed % 1000000LL) / 1000LL
            );

            fflush(stdout);

            close(log_fd);

            _exit(EXIT_SUCCESS);
        }

        created++;
    }

    /* O pai fecha as extremidades de leitura dos gates. */
    for (i = 0; i < NUM_WORKERS; i++)
        close(gates[i][0]);

    close(ready_pipe[1]);

    /* Aguarda todos os trabalhadores ficarem prontos. */
    for (i = 0; i < created; i++) {

        if (read_full(
                ready_pipe[0], &ready_status, 1) == -1) {

            fprintf(stderr,
                "Pai: nao recebeu todos os sinais de prontidao.\n"
            );

            failed = 1;
            start_ok = 0;
            break;
        }

        if (ready_status != 'R') {

            fprintf(stderr,
                "Pai: um trabalhador nao ficou pronto.\n"
            );

            failed = 1;
            start_ok = 0;
        }
    }

    close(ready_pipe[0]);

    /* Define o mesmo instante inicial para os trabalhadores. */
    if (start_ok && gettimeofday(&start, NULL) == -1) {
        perror("gettimeofday");
        failed = 1;
        start_ok = 0;
    }

    /* Libera todos os trabalhadores. */
    for (i = 0; i < NUM_WORKERS; i++) {

        if (i < created && start_ok) {

            if (write_full(
                    gates[i][1], &start, sizeof(start)) == -1) {

                fprintf(stderr,
                    "Pai: falha ao liberar Worker %d.\n",
                    i + 1
                );

                failed = 1;
            }
        }

        close(gates[i][1]);
    }

    /* Aguarda o encerramento dos trabalhadores. */
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

        if (!WIFEXITED(status) ||
            WEXITSTATUS(status) != EXIT_SUCCESS) {

            failed = 1;
        }
    }

    {
        const char *footer =
            "\n=== BENCHMARK CONCLUIDO ===\n";

        (void)write(log_fd, footer, strlen(footer));
    }

    close(log_fd);

    if (created != NUM_WORKERS)
        failed = 1;

    return failed ? EXIT_FAILURE : EXIT_SUCCESS;
}
