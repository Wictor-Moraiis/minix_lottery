/*
 * Benchmark temporizado para Lottery Scheduling no MINIX 3.2.1.
 * Dois workers com 10 e 30 tickets executam carga de CPU durante
 * uma janela fixa. O pai dorme durante a medicao e encerra os filhos
 * cooperativamente com SIGUSR1. As estatisticas do kernel sao vistas
 * pela tecla F9 do Information Server.
 */

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
#include <unistd.h>

#define NUM_WORKERS 2
#define LIMIT 1000000UL
#define BENCHMARK_SECONDS 30

static const int tickets[NUM_WORKERS] = { 10, 30 };
static volatile sig_atomic_t stop_requested = 0;

static void request_stop(int sig)
{
    (void) sig;
    stop_requested = 1;
}

static int set_lottery_tickets(int ticket_count)
{
    message m;
    memset(&m, 0, sizeof(m));
    m.m1_i1 = ticket_count;
    return _syscall(PM_PROC_NR, PM_SET_LOTTERY_TICKETS, &m);
}

/* Verifica o pedido de parada durante a carga, sem esperar o fim de 1M. */
static unsigned long count_primes(unsigned long limit)
{
    unsigned long n, d, count;
    int prime;

    count = 0;
    for (n = 2; n <= limit && !stop_requested; n++) {
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

static int read_byte(int fd, char *c)
{
    ssize_t n;
    do { n = read(fd, c, 1); } while (n < 0 && errno == EINTR);
    return n == 1 ? 0 : -1;
}

static int write_byte(int fd, char c)
{
    ssize_t n;
    do { n = write(fd, &c, 1); } while (n < 0 && errno == EINTR);
    return n == 1 ? 0 : -1;
}

static void worker_main(int index, int gate_read, int ready_write,
                        int result_write)
{
    char c;
    unsigned long rounds, checksum;

    if (signal(SIGUSR1, request_stop) == SIG_ERR)
        _exit(2);

    if (set_lottery_tickets(tickets[index]) != 0) {
        (void) write_byte(ready_write, 'E');
        _exit(3);
    }

    if (write_byte(ready_write, 'R') != 0)
        _exit(4);

    close(ready_write);

    if (read_byte(gate_read, &c) != 0)
        _exit(5);
    close(gate_read);

    rounds = 0;
    checksum = 0;
    while (!stop_requested) {
        checksum += count_primes(LIMIT);
        rounds++;
    }

    /* Reporta apenas um numero pequeno ao processo pai. */
    (void) checksum;
    (void) write(result_write, &rounds, sizeof(rounds));
    close(result_write);
    _exit(0);
}

static int read_full(int fd, void *buffer, size_t size)
{
    char *p;
    ssize_t n;
    size_t done;

    p = (char *) buffer;
    done = 0;
    while (done < size) {
        n = read(fd, p + done, size - done);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return -1;
        done += (size_t) n;
    }
    return 0;
}

int main(void)
{
    int gates[NUM_WORKERS][2];
    int ready[2], results[2];
    pid_t pids[NUM_WORKERS], ended;
    unsigned long rounds[NUM_WORKERS];
    char c;
    int i, j, created, ok, status, completed;
    unsigned long total;

    created = 0;
    ok = 1;
    completed = 0;

    printf("=== LOTTERY SCHEDULING BENCHMARK ===\n");
    printf("Duracao: %d segundos | carga: count_primes(%lu)\n",
           BENCHMARK_SECONDS, LIMIT);
    fflush(stdout);

    for (i = 0; i < NUM_WORKERS; i++) {
        if (pipe(gates[i]) < 0) { perror("pipe"); return 1; }
    }
    if (pipe(ready) < 0 || pipe(results) < 0) {
        perror("pipe");
        return 1;
    }

    for (i = 0; i < NUM_WORKERS; i++) {
        pids[i] = fork();
        if (pids[i] < 0) {
            perror("fork");
            ok = 0;
            break;
        }
        if (pids[i] == 0) {
            close(ready[0]);
            close(results[0]);
            for (j = 0; j < NUM_WORKERS; j++) {
                close(gates[j][1]);
                if (j != i)
                    close(gates[j][0]);
            }
            worker_main(i, gates[i][0], ready[1], results[1]);
            _exit(1);
        }
        created++;
    }

    close(ready[1]);
    close(results[1]);
    for (i = 0; i < NUM_WORKERS; i++)
        close(gates[i][0]);

    if (!ok || created != NUM_WORKERS) {
        for (i = 0; i < NUM_WORKERS; i++)
            close(gates[i][1]);
        close(ready[0]);
        close(results[0]);
        while (wait(&status) > 0) ;
        return 1;
    }

    for (i = 0; i < NUM_WORKERS; i++) {
        if (read_byte(ready[0], &c) != 0 || c != 'R') {
            fprintf(stderr, "Falha no registro dos tickets do Worker %d.\n", i + 1);
            ok = 0;
        }
    }
    close(ready[0]);

    if (!ok) {
        for (i = 0; i < NUM_WORKERS; i++) {
            close(gates[i][1]);
            (void) kill(pids[i], SIGUSR1);
        }
        close(results[0]);
        while (wait(&status) > 0) ;
        return 1;
    }

    printf("Workers prontos: PID %ld (%d tickets), PID %ld (%d tickets).\n",
           (long) pids[0], tickets[0], (long) pids[1], tickets[1]);
    printf("Inicio da medicao: liberando os workers por %d segundos.\n",
           BENCHMARK_SECONDS);
    fflush(stdout);

    for (i = 0; i < NUM_WORKERS; i++) {
        (void) write_byte(gates[i][1], 'S');
        close(gates[i][1]);
    }

    /* O pai dorme; nao fica imprimindo nem competindo pelo quantum. */
    i = BENCHMARK_SECONDS;
    while (i > 0)
        i = sleep((unsigned int) i);

    printf("Fim da janela. Solicitando parada cooperativa...\n");
    fflush(stdout);

    for (i = 0; i < NUM_WORKERS; i++)
        (void) kill(pids[i], SIGUSR1);

    for (i = 0; i < NUM_WORKERS; i++) {
        if (read_full(results[0], &rounds[i], sizeof(rounds[i])) != 0) {
            rounds[i] = 0;
            ok = 0;
        }
    }
    close(results[0]);

    while ((ended = wait(&status)) > 0) {
        completed++;
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
            ok = 0;
    }

    total = rounds[0] + rounds[1];
    printf("\n=== RESULTADO ===\n");
    printf("Worker 1 | tickets=%d | unidades=%lu\n", tickets[0], rounds[0]);
    printf("Worker 2 | tickets=%d | unidades=%lu\n", tickets[1], rounds[1]);
    printf("Total de unidades: %lu | workers encerrados: %d/%d\n",
           total, completed, NUM_WORKERS);

    if (total > 0) {
        printf("Participacao por unidades: Worker 1 %.2f%% | Worker 2 %.2f%%\n",
               100.0 * (double) rounds[0] / (double) total,
               100.0 * (double) rounds[1] / (double) total);
        printf("Proporcao esperada pelos tickets: 25%% / 75%%\n");
    }

    return ok ? 0 : 1;
}
