/* Benchmark de CPU para validar a distribuicao do Lottery Scheduling
 * no MINIX 3.2.1.
 *
 * Os workers executam continuamente durante o mesmo intervalo.
 * Nao ha leitura de ProcFS nem escrita em arquivo durante a medicao.
 * Ao final, o processo pai para os dois workers, coleta os contadores
 * e somente depois grava os resultados.
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
#include <sys/time.h>
#include <sys/select.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>

#define NUM_WORKERS 2
#define LIMIT 1000000UL
#define BENCHMARK_SECONDS 30

static const int tickets[NUM_WORKERS] = {
    10, 30
};

struct lottery_stats {
    unsigned int tickets;
    unsigned long wins;
    unsigned long context_switches_in;
    unsigned long context_switches_out;
    unsigned long quantum_expirations;
    unsigned long cycles_hi;
    unsigned long cycles_lo;
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

/* Le todos os bytes solicitados de um pipe. */
static int read_full(int fd, void *buffer, size_t length)
{
    size_t done;
    ssize_t n;
    char *p;

    done = 0;
    p = (char *) buffer;

    while (done < length) {
        n = read(fd, p + done, length - done);

        if (n < 0 && errno == EINTR)
            continue;

        if (n <= 0)
            return -1;

        done += (size_t) n;
    }

    return 0;
}

/* Escreve todos os bytes solicitados em um pipe ou arquivo. */
static int write_full(int fd, const void *buffer, size_t length)
{
    size_t done;
    ssize_t n;
    const char *p;

    done = 0;
    p = (const char *) buffer;

    while (done < length) {
        n = write(fd, p + done, length - done);

        if (n < 0 && errno == EINTR)
            continue;

        if (n <= 0)
            return -1;

        done += (size_t) n;
    }

    return 0;
}

/* Calcula o tempo decorrido em microssegundos. */
static long long elapsed_us(const struct timeval *start,
                            const struct timeval *finish)
{
    return ((long long) finish->tv_sec - start->tv_sec) * 1000000LL
         + ((long long) finish->tv_usec - start->tv_usec);
}

/* Le um snapshot curto dos contadores disponibilizados pelo ProcFS. */
static int read_lottery_stats(pid_t pid, struct lottery_stats *stats)
{
    char path[64];
    char buffer[512];
    char *p;
    ssize_t bytes;
    unsigned int found;
    int fd;

    snprintf(path, sizeof(path), "/proc/%ld/lottery", (long) pid);

    fd = open(path, O_RDONLY);
    if (fd == -1)
        return -1;

    /* O arquivo e pequeno; uma leitura evita loop de fgets sem EOF. */
    bytes = read(fd, buffer, sizeof(buffer) - 1);
    close(fd);

    if (bytes <= 0)
        return -1;

    buffer[bytes] = '\0';
    memset(stats, 0, sizeof(*stats));
    found = 0;

    p = strstr(buffer, "tickets=");
    if (p != NULL && sscanf(p + 8, "%u", &stats->tickets) == 1)
        found |= 1;

    p = strstr(buffer, "wins=");
    if (p != NULL && sscanf(p + 5, "%lu", &stats->wins) == 1)
        found |= 2;

    p = strstr(buffer, "context_switches_in=");
    if (p != NULL && sscanf(p + 20, "%lu", &stats->context_switches_in) == 1)
        found |= 4;

    p = strstr(buffer, "context_switches_out=");
    if (p != NULL && sscanf(p + 21, "%lu", &stats->context_switches_out) == 1)
        found |= 8;

    p = strstr(buffer, "quantum_expirations=");
    if (p != NULL && sscanf(p + 19, "%lu", &stats->quantum_expirations) == 1)
        found |= 16;

    p = strstr(buffer, "cycles_hi=");
    if (p != NULL && sscanf(p + 10, "%lu", &stats->cycles_hi) == 1)
        found |= 32;

    p = strstr(buffer, "cycles_lo=");
    if (p != NULL && sscanf(p + 10, "%lu", &stats->cycles_lo) == 1)
        found |= 64;

    return found == 127 ? 0 : -1;
}

/*
 * Cada filho registra seus tickets, avisa ao pai que esta pronto
 * e aguarda o sinal de inicio. Durante a medicao, executa somente
 * a carga de CPU: nao acessa ProcFS, pipes ou arquivos.
 */
static void run_worker(int worker, int gate_fd, int ready_fd)
{
    char status;
    char start_signal;
    unsigned long rounds;
    unsigned long limit;
    volatile unsigned long checksum;

    if (set_lottery_tickets(tickets[worker]) != 0) {
        status = 'E';
        (void) write_full(ready_fd, &status, 1);
        close(ready_fd);
        close(gate_fd);
        _exit(EXIT_FAILURE);
    }

    status = 'R';

    if (write_full(ready_fd, &status, 1) == -1) {
        close(ready_fd);
        close(gate_fd);
        _exit(EXIT_FAILURE);
    }

    close(ready_fd);

    if (read_full(gate_fd, &start_signal, 1) == -1) {
        close(gate_fd);
        _exit(EXIT_FAILURE);
    }

    close(gate_fd);

    rounds = 0;
    checksum = 0;

    for (;;) {
        /* Varia o limite para evitar repetir exatamente a mesma chamada. */
        limit = LIMIT - ((rounds % 1000UL) * 100UL);
        checksum += count_primes(limit);
        rounds++;
    }
}

int main(void)
{
    int gates[NUM_WORKERS][2];
    int ready_pipe[2];
    pid_t workers[NUM_WORKERS];

    int i;
    int j;
    int created;
    int status;
    int failed;
    int ready_ok;
    int wait_result;
    struct timeval delay;
    int log_fd;
    int length;

    char ready_status;
    char start_signal;

    struct lottery_stats stats[NUM_WORKERS];
    int stats_ok[NUM_WORKERS];

    time_t start;
    time_t finish;
    long elapsed;
    unsigned long total_worker_wins;
    double share;

    char line[512];

    created = 0;
    failed = 0;
    ready_ok = 1;
    log_fd = -1;
    start_signal = 'S';

    signal(SIGPIPE, SIG_IGN);

    /*
     * Diagnostico desde o inicio: se o programa travar, o terminal
     * mostra exatamente em qual fase isso aconteceu.
     */
    printf("=== BENCHMARK LOTTERY SCHEDULING ===\n");
    printf("Preparando pipes e workers...\n");
    fflush(stdout);

    /* Cria os pipes individuais de liberacao dos workers. */
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

    /* Cria os dois processos trabalhadores. */
    for (i = 0; i < NUM_WORKERS; i++) {
        printf("Criando Worker %d...\n", i + 1);
        fflush(stdout);
        workers[i] = fork();

        if (workers[i] < 0) {
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

    /* O pai nao utiliza as extremidades de leitura dos gates. */
    for (i = 0; i < NUM_WORKERS; i++)
        close(gates[i][0]);

    close(ready_pipe[1]);

    printf("Workers criados: %d. Aguardando registro dos tickets...\n",
           created);
    fflush(stdout);

    if (created != NUM_WORKERS)
        ready_ok = 0;

    /* Aguarda os workers confirmarem o registro dos tickets. */
    for (i = 0; i < created; i++) {
        if (read_full(ready_pipe[0], &ready_status, 1) == -1) {
            fprintf(stderr,
                    "Pai: nao recebeu a confirmacao do Worker %d.\n",
                    i + 1);
            ready_ok = 0;
            break;
        }

        if (ready_status != 'R') {
            fprintf(stderr,
                    "Pai: Worker %d falhou ao registrar os tickets.\n",
                    i + 1);
            ready_ok = 0;
        }
    }

    close(ready_pipe[0]);

    if (!ready_ok) {
        /* Fechar os gates faz os filhos ainda bloqueados encerrarem. */
        for (i = 0; i < NUM_WORKERS; i++)
            close(gates[i][1]);

        while (wait(&status) != -1)
            ;

        return EXIT_FAILURE;
    }

    printf("Ambos os workers estao prontos. Iniciando medicao.\n");
    printf("Workers: 2 | Duracao: %d segundos | Tickets: 10 e 30\n",
           BENCHMARK_SECONDS);
    printf("Carga: count_primes() continuo, sem I/O nos workers.\n\n");
    fflush(stdout);

    /*
     * Libera os dois workers e inicia uma unica espera temporizada.
     * O pai dorme pela janela inteira. Assim ele nao precisa disputar
     * a USER_Q 30 vezes para atualizar um contador de segundos.
     */
    for (i = 0; i < NUM_WORKERS; i++) {
        if (write_full(gates[i][1], &start_signal, 1) == -1) {
            fprintf(stderr, "Pai: falha ao iniciar Worker %d.\n", i + 1);
            failed = 1;
        }
        close(gates[i][1]);
    }

    if (failed) {
        for (i = 0; i < created; i++)
            (void) kill(workers[i], SIGKILL);
        while (wait(&status) != -1)
            ;
        return EXIT_FAILURE;
    }

    start = time(NULL);
    if (start == (time_t)-1) {
        perror("time");
        for (i = 0; i < created; i++)
            (void) kill(workers[i], SIGKILL);
        while (wait(&status) != -1)
            ;
        return EXIT_FAILURE;
    }

    printf("Medindo por %d segundos...\n", BENCHMARK_SECONDS);
    fflush(stdout);

    delay.tv_sec = BENCHMARK_SECONDS;
    delay.tv_usec = 0;
    do {
        wait_result = select(0, NULL, NULL, NULL, &delay);
    } while (wait_result == -1 && errno == EINTR);

    if (wait_result == -1) {
        perror("select");
        failed = 1;
    }

    finish = time(NULL);
    if (finish == (time_t)-1) {
        perror("time");
        finish = start;
        failed = 1;
    }
    elapsed = (long)(finish - start);

    /*
     * Para e aguarda cada worker antes de ler o ProcFS. WUNTRACED
     * confirma que o processo realmente recebeu SIGSTOP.
     */
    for (i = 0; i < NUM_WORKERS; i++) {
        printf("Parando Worker %d (PID=%ld)...\n",
               i + 1, (long) workers[i]);
        fflush(stdout);

        if (kill(workers[i], SIGSTOP) == -1) {
            perror("SIGSTOP");
            failed = 1;
            continue;
        }

        if (waitpid(workers[i], &status, WUNTRACED) == -1) {
            perror("waitpid WUNTRACED");
            failed = 1;
        } else if (!WIFSTOPPED(status)) {
            fprintf(stderr, "Worker %d nao confirmou estado parado.\n",
                    i + 1);
            failed = 1;
        }
    }

    printf("Workers parados. Coletando estatisticas...\n");
    fflush(stdout);

    total_worker_wins = 0;
    for (i = 0; i < NUM_WORKERS; i++) {
        printf("Lendo /proc/%ld/lottery...\n", (long) workers[i]);
        fflush(stdout);

        stats_ok[i] = (read_lottery_stats(workers[i], &stats[i]) == 0);
        if (stats_ok[i]) {
            printf("Estatisticas do Worker %d lidas.\n", i + 1);
            total_worker_wins += stats[i].wins;
        } else {
            fprintf(stderr, "Falha ao ler /proc/%ld/lottery.\n",
                    (long) workers[i]);
            failed = 1;
        }
    }

    log_fd = open("resultado_lottery.txt",
                  O_WRONLY | O_CREAT | O_TRUNC,
                  0644);

    length = snprintf(
        line, sizeof(line),
        "=== BENCHMARK LOTTERY SCHEDULING ===\n"
        "Workers: 2 | Duracao configurada: %d segundos "
        "| Tickets: 10 e 30\n"
        "Carga: count_primes() continuo, sem I/O nos workers.\n"
        "Tempo medido: %ld segundos\n\n",
        BENCHMARK_SECONDS,
        elapsed
    );

    printf("%s", line);

    if (log_fd >= 0)
        (void) write_full(log_fd, line, (size_t) length);
    else
        perror("resultado_lottery.txt");

    for (i = 0; i < NUM_WORKERS; i++) {
        if (!stats_ok[i])
            continue;

        if (total_worker_wins > 0)
            share = 100.0 * (double) stats[i].wins /
                    (double) total_worker_wins;
        else
            share = 0.0;

        length = snprintf(
            line, sizeof(line),
            "Worker %d | PID=%ld | Tickets=%u | Wins=%lu "
            "| Share entre workers=%.2f%% "
            "| Context_in=%lu | Context_out=%lu "
            "| Quantum_exp=%lu | Cycles_hi=%lu | Cycles_lo=%lu\n",
            i + 1,
            (long) workers[i],
            stats[i].tickets,
            stats[i].wins,
            share,
            stats[i].context_switches_in,
            stats[i].context_switches_out,
            stats[i].quantum_expirations,
            stats[i].cycles_hi,
            stats[i].cycles_lo
        );

        printf("%s", line);

        if (log_fd >= 0)
            (void) write_full(log_fd, line, (size_t) length);
    }

    if (log_fd >= 0) {
        const char *footer = "\n=== BENCHMARK CONCLUIDO ===\n";
        (void) write_full(log_fd, footer, strlen(footer));
        close(log_fd);
    }

    /*
     * Os contadores ja foram coletados. Agora os filhos podem ser
     * encerrados sem perder as informacoes disponibilizadas pelo ProcFS.
     */
    for (i = 0; i < NUM_WORKERS; i++) {
        if (kill(workers[i], SIGKILL) == -1 && errno != ESRCH) {
            perror("SIGKILL");
            failed = 1;
        }
    }

    while (wait(&status) != -1) {
        if (!WIFEXITED(status) && !WIFSIGNALED(status))
            failed = 1;
    }

    printf("\n=== BENCHMARK CONCLUIDO ===\n");

    return failed ? EXIT_FAILURE : EXIT_SUCCESS;
}
