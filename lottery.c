/*
 * Benchmark simples para executar Lottery Scheduling no MINIX 3.2.1.
 *
 * Dois workers recebem tickets diferentes, sao liberados juntos e
 * executam uma carga finita de CPU. O pai apenas informa os tickets,
 * o inicio e o fim da execucao.
 *
 * As estatisticas ao vivo devem ser acompanhadas pelo dump F9 do IS.
 */

#include <lib.h>
#include <minix/com.h>
#include <minix/callnr.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define NUM_WORKERS 2
#define LIMIT 1000000UL
#define WORKLOAD_REPETITIONS 160

static const int tickets[NUM_WORKERS] = {
    10, 30
};

/* Conta numeros primos para manter o worker ocupado com CPU. */
static unsigned long count_primes(unsigned long limit)
{
    unsigned long n;
    unsigned long d;
    unsigned long count;
    int prime;

    count = 0;

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

/* Le exatamente um byte do pipe. */
static int read_signal(int fd, char *value)
{
    ssize_t n;

    do {
        n = read(fd, value, 1);
    } while (n == -1 && errno == EINTR);

    return n == 1 ? 0 : -1;
}

/* Escreve exatamente um byte no pipe. */
static int write_signal(int fd, char value)
{
    ssize_t n;

    do {
        n = write(fd, &value, 1);
    } while (n == -1 && errno == EINTR);

    return n == 1 ? 0 : -1;
}

/*
 * Cada worker registra seus tickets, avisa ao pai que esta pronto
 * e aguarda a liberacao. Depois executa apenas a carga de CPU.
 */
static void run_worker(int worker, int gate_fd, int ready_fd)
{
    char ready;
    char start_signal;
    unsigned long i;
    volatile unsigned long checksum;

    if (set_lottery_tickets(tickets[worker]) != 0) {
        (void) write_signal(ready_fd, 'E');
        close(ready_fd);
        close(gate_fd);
        _exit(EXIT_FAILURE);
    }

    if (write_signal(ready_fd, 'R') == -1) {
        close(ready_fd);
        close(gate_fd);
        _exit(EXIT_FAILURE);
    }

    close(ready_fd);

    if (read_signal(gate_fd, &start_signal) == -1) {
        close(gate_fd);
        _exit(EXIT_FAILURE);
    }

    close(gate_fd);

    checksum = 0;
    for (i = 0; i < WORKLOAD_REPETITIONS; i++)
        checksum += count_primes(LIMIT);

    /* Mantem o resultado observavel e evita eliminar a carga. */
    (void) checksum;

    _exit(EXIT_SUCCESS);
}

int main(void)
{
    int gates[NUM_WORKERS][2];
    int ready_pipe[2];
    pid_t workers[NUM_WORKERS];
    pid_t finished_pid;

    int i;
    int j;
    int created;
    int finished;
    int status;
    int ready_ok;
    int failed;
    int worker_index;
    char ready_status;

    created = 0;
    finished = 0;
    ready_ok = 1;
    failed = 0;

    printf("=== LOTTERY SCHEDULING ===\n");
    printf("Preparando workers...\n");
    fflush(stdout);

    for (i = 0; i < NUM_WORKERS; i++) {
        if (pipe(gates[i]) == -1) {
            perror("pipe");
            while (i > 0) {
                i--;
                close(gates[i][0]);
                close(gates[i][1]);
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

    /* Cria os dois workers. */
    for (i = 0; i < NUM_WORKERS; i++) {
        workers[i] = fork();

        if (workers[i] == -1) {
            perror("fork");
            failed = 1;
            break;
        }

        if (workers[i] == 0) {
            close(ready_pipe[0]);

            for (j = 0; j < NUM_WORKERS; j++) {
                close(gates[j][1]);
                if (j != i)
                    close(gates[j][0]);
            }

            run_worker(i, gates[i][0], ready_pipe[1]);
            _exit(EXIT_FAILURE);
        }

        created++;
    }

    /* O pai nao usa as extremidades de leitura dos gates. */
    for (i = 0; i < NUM_WORKERS; i++)
        close(gates[i][0]);

    close(ready_pipe[1]);

    if (created != NUM_WORKERS) {
        for (i = 0; i < NUM_WORKERS; i++)
            close(gates[i][1]);
        close(ready_pipe[0]);

        while (wait(&status) != -1)
            ;
        return EXIT_FAILURE;
    }

    /* Espera os dois workers confirmarem o registro dos tickets. */
    for (i = 0; i < NUM_WORKERS; i++) {
        if (read_signal(ready_pipe[0], &ready_status) == -1) {
            fprintf(stderr, "Falha aguardando o Worker %d.\n", i + 1);
            ready_ok = 0;
            break;
        }

        if (ready_status != 'R') {
            fprintf(stderr,
                    "Worker %d nao conseguiu registrar seus tickets.\n",
                    i + 1);
            ready_ok = 0;
        }
    }

    close(ready_pipe[0]);

    if (!ready_ok) {
        /* O EOF no gate libera qualquer worker que ainda esteja aguardando. */
        for (i = 0; i < NUM_WORKERS; i++)
            close(gates[i][1]);

        while (wait(&status) != -1)
            ;
        return EXIT_FAILURE;
    }

    printf("\nWorkers registrados com sucesso:\n");
    for (i = 0; i < NUM_WORKERS; i++)
        printf("Worker %d | PID=%ld | Tickets=%d\n",
               i + 1, (long) workers[i], tickets[i]);

    printf("\nInicio da execucao: liberando os dois workers.\n");
    fflush(stdout);

    /* Libera os dois workers; ambos ja estao registrados no Lottery. */
    for (i = 0; i < NUM_WORKERS; i++) {
        if (write_signal(gates[i][1], 'S') == -1) {
            fprintf(stderr, "Falha ao liberar o Worker %d.\n", i + 1);
            failed = 1;
        }
        close(gates[i][1]);
    }

    /* O pai fica bloqueado aqui ate os dois workers terminarem. */
    while (finished < created) {
        finished_pid = wait(&status);

        if (finished_pid == -1) {
            if (errno == EINTR)
                continue;
            perror("wait");
            failed = 1;
            break;
        }

        finished++;

        worker_index = -1;
        for (i = 0; i < created; i++) {
            if (workers[i] == finished_pid) {
                worker_index = i;
                break;
            }
        }

        if (worker_index == -1 ||
            !WIFEXITED(status) ||
            WEXITSTATUS(status) != EXIT_SUCCESS) {
            failed = 1;
        }
    }

    if (failed)
        printf("\nFim da execucao: ocorreu uma falha em algum worker.\n");
    else
        printf("\nFim da execucao: os dois workers concluiram a carga.\n");

    return failed ? EXIT_FAILURE : EXIT_SUCCESS;
}
