/*
 * Implementacao compartilhada dos cenarios de benchmark Lottery.
 *
 * Cada executavel define:
 *   LOTTERY_NUM_WORKERS
 *   LOTTERY_SECONDS
 *   LOTTERY_TICKETS
 *   LOTTERY_PROFILE
 *
 * Compativel com o fluxo de PM/SCHED e ProcFS do MINIX 3.2.1.
 */

#ifndef LOTTERY_COMMON_H
#define LOTTERY_COMMON_H

#include <lib.h>
#include <minix/com.h>
#include <minix/callnr.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef LOTTERY_NUM_WORKERS
#error "LOTTERY_NUM_WORKERS must be defined by the test profile"
#endif

#ifndef LOTTERY_SECONDS
#error "LOTTERY_SECONDS must be defined by the test profile"
#endif

#ifndef LOTTERY_TICKETS
#error "LOTTERY_TICKETS must be defined by the test profile"
#endif

#ifndef LOTTERY_PROFILE
#define LOTTERY_PROFILE "Lottery Scheduling test"
#endif

#define LOTTERY_LIMIT 1000000UL

static int lottery_tickets[LOTTERY_NUM_WORKERS] = LOTTERY_TICKETS;
static int lottery_worker_count = LOTTERY_NUM_WORKERS;
static volatile sig_atomic_t lottery_stop_requested = 0;

#define LOTTERY_RESULTS_PATH "/test/lottery_results.txt"
static FILE *lottery_results_file = NULL;

static void lottery_result(const char *format, ...)
{
    va_list args;

    if (lottery_results_file == NULL)
        return;

    va_start(args, format);
    vfprintf(lottery_results_file, format, args);
    va_end(args);
}

static void lottery_close_results(void)
{
    if (lottery_results_file != NULL) {
        fclose(lottery_results_file);
        lottery_results_file = NULL;
    }
}

struct lottery_worker_report {
    int index;
    unsigned long units;
};

struct lottery_process_stats {
    unsigned int tickets;
    unsigned long wins;
    unsigned long context_in;
    unsigned long context_out;
    unsigned long quantum_expirations;
};

static void lottery_request_stop(int sig)
{
    (void) sig;
    lottery_stop_requested = 1;
}

/* Register tickets through PM -> SCHED -> kernel. */
static int lottery_set_tickets(int count)
{
    message m;

    memset(&m, 0, sizeof(m));
    m.m1_i1 = count;

    return _syscall(PM_PROC_NR, PM_SET_LOTTERY_TICKETS, &m);
}

static int lottery_read_byte(int fd, char *value)
{
    ssize_t n;

    do {
        n = read(fd, value, 1);
    } while (n == -1 && errno == EINTR);

    return n == 1 ? 0 : -1;
}

static int lottery_write_all(int fd, const void *buffer, size_t length)
{
    const char *p;
    size_t done;
    ssize_t n;

    p = (const char *) buffer;
    done = 0;

    while (done < length) {
        n = write(fd, p + done, length - done);

        if (n == -1 && errno == EINTR)
            continue;

        if (n <= 0)
            return -1;

        done += (size_t) n;
    }

    return 0;
}

static int lottery_read_all(int fd, void *buffer, size_t length)
{
    char *p;
    size_t done;
    ssize_t n;

    p = (char *) buffer;
    done = 0;

    while (done < length) {
        n = read(fd, p + done, length - done);

        if (n == -1 && errno == EINTR)
            continue;

        if (n <= 0)
            return -1;

        done += (size_t) n;
    }

    return 0;
}

/*
 * Same deterministic CPU-bound workload for every worker.
 * The signal is checked inside the loops so shutdown is cooperative
 * and does not wait for a complete 1,000,000-number pass.
 */
static unsigned long lottery_count_primes(unsigned long limit)
{
    unsigned long n, d, count;
    int prime;

    count = 0;

    for (n = 2; n <= limit && !lottery_stop_requested; n++) {
        prime = 1;

        for (d = 2; d <= n / d && !lottery_stop_requested; d++) {
            if (n % d == 0) {
                prime = 0;
                break;
            }
        }

        if (lottery_stop_requested)
            break;

        if (prime)
            count++;
    }

    return count;
}

/*
 * One-shot read of ProcFS. The parent calls this only after the worker
 * has stopped computing and is blocked on its release pipe, so counters
 * are stable and this I/O does not contaminate the measured interval.
 */
static int lottery_read_stats(pid_t pid, struct lottery_process_stats *stats)
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

    bytes = read(fd, buffer, sizeof(buffer) - 1);
    close(fd);

    if (bytes <= 0)
        return -1;

    buffer[bytes] = '\0';
    memset(stats, 0, sizeof(*stats));
    found = 0;

    p = strstr(buffer, "tickets=");
    if (p != NULL && sscanf(p + strlen("tickets="), "%u",
                            &stats->tickets) == 1)
        found |= 1;

    p = strstr(buffer, "wins=");
    if (p != NULL && sscanf(p + strlen("wins="), "%lu",
                            &stats->wins) == 1)
        found |= 2;

    p = strstr(buffer, "context_switches_in=");
    if (p != NULL && sscanf(p + strlen("context_switches_in="), "%lu",
                            &stats->context_in) == 1)
        found |= 4;

    p = strstr(buffer, "context_switches_out=");
    if (p != NULL && sscanf(p + strlen("context_switches_out="), "%lu",
                            &stats->context_out) == 1)
        found |= 8;

    p = strstr(buffer, "quantum_expirations=");
    if (p != NULL && sscanf(p + strlen("quantum_expirations="), "%lu",
                            &stats->quantum_expirations) == 1)
        found |= 16;

    return (found & 3) == 3 ? 0 : -1;
}

/*
 * Child lifecycle:
 * 1. Install cooperative stop handler.
 * 2. Register its own tickets and tell parent it is ready.
 * 3. Wait for the common start release.
 * 4. Run CPU-only work until SIGUSR1.
 * 5. Send completed units to parent and block on exit_gate.
 *
 * Keeping the child alive on exit_gate lets the parent collect final
 * /proc/PID/lottery counters before the kernel frees its process slot.
 */
static void lottery_worker_main(int index,
                                int start_gate,
                                int exit_gate,
                                int ready_write,
                                int report_write)
{
    char token;
    unsigned long units;
    volatile unsigned long checksum;
    struct lottery_worker_report report;

    if (signal(SIGUSR1, lottery_request_stop) == SIG_ERR) {
        (void) write(ready_write, "E", 1);
        _exit(2);
    }

    if (lottery_set_tickets(lottery_tickets[index]) != 0) {
        (void) write(ready_write, "E", 1);
        _exit(3);
    }

    if (write(ready_write, "R", 1) != 1)
        _exit(4);

    if (lottery_read_byte(start_gate, &token) != 0)
        _exit(5);

    close(start_gate);
    units = 0;
    checksum = 0;

    while (!lottery_stop_requested) {
        unsigned long result;

        result = lottery_count_primes(LOTTERY_LIMIT);

        /* Do not count a partially interrupted pass as a completed unit. */
        if (lottery_stop_requested)
            break;

        checksum += result;
        units++;
    }

    report.index = index;
    report.units = units;

    if (lottery_write_all(report_write, &report, sizeof(report)) != 0)
        _exit(6);

    close(report_write);
    (void) checksum;

    /* Wait until parent has captured final kernel/ProcFS statistics. */
    if (lottery_read_byte(exit_gate, &token) != 0)
        _exit(7);

    close(exit_gate);
    _exit(EXIT_SUCCESS);
}

static void lottery_close_pair(int pair[2])
{
    if (pair[0] >= 0) {
        close(pair[0]);
        pair[0] = -1;
    }
    if (pair[1] >= 0) {
        close(pair[1]);
        pair[1] = -1;
    }
}

#ifdef LOTTERY_CUSTOM_INPUT
static int lottery_read_integer(const char *prompt, int minimum,
                                int maximum, int *value)
{
    char input[64];
    char *end;
    long parsed;

    printf("%s", prompt);
    fflush(stdout);

    if (fgets(input, sizeof(input), stdin) == NULL)
        return -1;

    errno = 0;
    parsed = strtol(input, &end, 10);

    if (end == input || errno == ERANGE)
        return -1;

    while (*end == ' ' || *end == '\t' ||
           *end == '\r' || *end == '\n')
        end++;

    if (*end != '\0' || parsed < minimum || parsed > maximum)
        return -1;

    *value = (int) parsed;
    return 0;
}

static int lottery_configure_custom(void)
{
    char prompt[96];
    int i;
    int value;

    printf("\nCUSTOM LOTTERY CONFIGURATION\n");
    printf("Allowed workers: 2 to %d | tickets per worker: 1 to %d\n",
           LOTTERY_NUM_WORKERS, SCHEDULING_MAX_TICKETS);

    if (lottery_read_integer("Number of workers: ", 2,
                             LOTTERY_NUM_WORKERS, &value) != 0) {
        printf("Invalid worker count.\n");
        return -1;
    }

    lottery_worker_count = value;

    for (i = 0; i < lottery_worker_count; i++) {
        snprintf(prompt, sizeof(prompt),
                 "Tickets for Worker %d: ", i + 1);

        if (lottery_read_integer(prompt, 1,
                                 SCHEDULING_MAX_TICKETS, &value) != 0) {
            printf("Invalid ticket count for Worker %d.\n", i + 1);
            return -1;
        }

        lottery_tickets[i] = value;
    }

    printf("\nDuration is fixed at %d seconds for comparable tests.\n",
           LOTTERY_SECONDS);
    return 0;
}
#endif


int main(void)
{
    int start_gates[LOTTERY_NUM_WORKERS][2];
    int exit_gates[LOTTERY_NUM_WORKERS][2];
    int ready_pipe[2];
    int report_pipe[2];
    pid_t pids[LOTTERY_NUM_WORKERS];
    struct lottery_worker_report reports[LOTTERY_NUM_WORKERS];
    struct lottery_process_stats stats[LOTTERY_NUM_WORKERS];
    int stats_ok[LOTTERY_NUM_WORKERS];
    int created, i, j, status, ready_ok, start_ok;
    int received[LOTTERY_NUM_WORKERS];
    int reports_ok, waited;
    char token;
    pid_t ended;
    unsigned long total_tickets, total_wins, total_units;

    created = 0;
    ready_ok = 1;
    start_ok = 1;
    reports_ok = 1;
    waited = 0;
    total_tickets = 0;
    total_wins = 0;
    total_units = 0;

#ifdef LOTTERY_CUSTOM_INPUT
    if (lottery_configure_custom() != 0)
        return EXIT_FAILURE;
#endif

    lottery_results_file = fopen(LOTTERY_RESULTS_PATH, "a");
    if (lottery_results_file == NULL) {
        perror("Could not open Lottery results file");
        return EXIT_FAILURE;
    }

    for (i = 0; i < lottery_worker_count; i++) {
        start_gates[i][0] = -1;
        start_gates[i][1] = -1;
        exit_gates[i][0] = -1;
        exit_gates[i][1] = -1;
        stats_ok[i] = 0;
        received[i] = 0;
        reports[i].index = -1;
        reports[i].units = 0;
        memset(&stats[i], 0, sizeof(stats[i]));
        total_tickets += (unsigned long) lottery_tickets[i];
    }

    ready_pipe[0] = ready_pipe[1] = -1;
    report_pipe[0] = report_pipe[1] = -1;

    lottery_result("\n\n============================================================\n");
    lottery_result("LOTTERY SCHEDULING BENCHMARK\n");
    lottery_result("Profile: %s\n", LOTTERY_PROFILE);
    lottery_result("Workers: %d | duration: %d seconds\n",
                   lottery_worker_count, LOTTERY_SECONDS);
    lottery_result("============================================================\n");
    printf("Starting benchmark: %s\n", LOTTERY_PROFILE);
    printf("Detailed results will be appended to %s\n", LOTTERY_RESULTS_PATH);
    fflush(stdout);

    /* Allocate all synchronization pipes before forking. */
    for (i = 0; i < lottery_worker_count; i++) {
        if (pipe(start_gates[i]) == -1 ||
            pipe(exit_gates[i]) == -1) {
            perror("pipe worker");
            goto setup_error;
        }
    }

    if (pipe(ready_pipe) == -1 || pipe(report_pipe) == -1) {
        perror("pipe control");
        goto setup_error;
    }

    for (i = 0; i < lottery_worker_count; i++) {
        pids[i] = fork();

        if (pids[i] == -1) {
            perror("fork");
            goto fork_error;
        }

        if (pids[i] == 0) {
            close(ready_pipe[0]);
            close(report_pipe[0]);

            for (j = 0; j < lottery_worker_count; j++) {
                if (start_gates[j][1] >= 0)
                    close(start_gates[j][1]);
                if (exit_gates[j][1] >= 0)
                    close(exit_gates[j][1]);

                if (j != i) {
                    if (start_gates[j][0] >= 0)
                        close(start_gates[j][0]);
                    if (exit_gates[j][0] >= 0)
                        close(exit_gates[j][0]);
                }
            }

            lottery_worker_main(i,
                start_gates[i][0],
                exit_gates[i][0],
                ready_pipe[1],
                report_pipe[1]);
            _exit(1);
        }

        created++;
    }

    /* Parent keeps only the write side of gates and read side of pipes. */
    close(ready_pipe[1]);
    ready_pipe[1] = -1;
    close(report_pipe[1]);
    report_pipe[1] = -1;

    for (i = 0; i < lottery_worker_count; i++) {
        close(start_gates[i][0]);
        start_gates[i][0] = -1;
        close(exit_gates[i][0]);
        exit_gates[i][0] = -1;
    }

    /* Wait until every child has successfully registered its own tickets. */
    for (i = 0; i < lottery_worker_count; i++) {
        if (lottery_read_byte(ready_pipe[0], &token) != 0 || token != 'R') {
            fprintf(stderr, "Worker %d failed to register Lottery tickets.\n",
                    i + 1);
            ready_ok = 0;
        }
    }

    close(ready_pipe[0]);
    ready_pipe[0] = -1;

    if (!ready_ok)
        goto runtime_error;

    lottery_result("\nRegistered participants:\n");
    for (i = 0; i < lottery_worker_count; i++) {
        lottery_result("Worker %d | PID=%ld | tickets=%d | expected share=%.2f%%\n",
                       i + 1, (long) pids[i], lottery_tickets[i],
                       100.0 * (double) lottery_tickets[i] /
                       (double) total_tickets);
    }

    lottery_result("\nSTART: releasing all workers for %d seconds.\n",
                   LOTTERY_SECONDS);
    fflush(lottery_results_file);
    printf("Workers registered. Benchmark running for %d seconds...\n",
           LOTTERY_SECONDS);
    fflush(stdout);

    for (i = 0; i < lottery_worker_count; i++) {
        if (write(start_gates[i][1], "S", 1) != 1)
            start_ok = 0;
        close(start_gates[i][1]);
        start_gates[i][1] = -1;
    }

    if (!start_ok)
        goto runtime_error;

    /*
     * Parent sleeps for the entire measurement window. No progress
     * printing, ProcFS polling, or frequent wakeups during the run.
     */
    i = LOTTERY_SECONDS;
    while (i > 0)
        i = sleep((unsigned int) i);

    printf("Measurement finished. Collecting final statistics...\n");
    fflush(stdout);
    lottery_result("\nEND: stopping workers and collecting final statistics.\n");

    for (i = 0; i < lottery_worker_count; i++) {
        if (kill(pids[i], SIGUSR1) == -1 && errno != ESRCH) {
            perror("SIGUSR1");
            reports_ok = 0;
        }
    }

    /* Receive one small, atomic report from each stopped worker. */
    for (i = 0; i < lottery_worker_count; i++) {
        struct lottery_worker_report report;

        if (lottery_read_all(report_pipe[0], &report, sizeof(report)) != 0) {
            fprintf(stderr, "Could not receive final report from a worker.\n");
            reports_ok = 0;
            break;
        }

        if (report.index < 0 || report.index >= lottery_worker_count ||
            received[report.index]) {
            fprintf(stderr, "Invalid worker report received.\n");
            reports_ok = 0;
            continue;
        }

        reports[report.index] = report;
        received[report.index] = 1;
    }

    close(report_pipe[0]);
    report_pipe[0] = -1;

    if (!reports_ok)
        goto runtime_error;

    /*
     * All workers are now blocked on their exit gates: their ticket and
     * win counters are stable, and their kernel process slots still exist.
     */
    for (i = 0; i < lottery_worker_count; i++) {
        stats_ok[i] = (lottery_read_stats(pids[i], &stats[i]) == 0);
        if (stats_ok[i])
            total_wins += stats[i].wins;
        else
            fprintf(stderr,
                    "Warning: could not read final wins for Worker %d (PID=%ld).\n",
                    i + 1, (long) pids[i]);

        total_units += reports[i].units;
    }

    lottery_result("\n=== FINAL RESULTS ===\n");
    lottery_result("Measurement window: %d seconds\n", LOTTERY_SECONDS);

    for (i = 0; i < lottery_worker_count; i++) {
        double expected_share;
        double wins_share;
        double units_share;

        expected_share = 100.0 * (double) lottery_tickets[i] /
                         (double) total_tickets;
        wins_share = (stats_ok[i] && total_wins > 0) ?
                     100.0 * (double) stats[i].wins / (double) total_wins : 0.0;
        units_share = (total_units > 0) ?
                      100.0 * (double) reports[i].units /
                      (double) total_units : 0.0;

        lottery_result("\nWorker %d | PID=%ld\n", i + 1, (long) pids[i]);
        lottery_result("  Tickets: %d | expected share: %.2f%%\n",
                       lottery_tickets[i], expected_share);

        if (stats_ok[i]) {
            lottery_result("  Wins: %lu | share among measured workers: %.2f%%\n",
                           stats[i].wins, wins_share);
            lottery_result("  Context in/out: %lu/%lu | quantum expirations: %lu\n",
                           stats[i].context_in, stats[i].context_out,
                           stats[i].quantum_expirations);
        } else {
            lottery_result("  Wins: unavailable (use F9 snapshot to inspect live counters)\n");
        }

        lottery_result("  Completed work units: %lu | work share: %.2f%%\n",
                       reports[i].units, units_share);
    }

    lottery_result("\nTotal Lottery wins collected: %lu\n", total_wins);
    lottery_result("Total completed work units: %lu\n", total_units);
    lottery_result("END: releasing workers and cleaning up.\n");
    fflush(lottery_results_file);
    printf("Benchmark complete. Results appended to %s\n",
           LOTTERY_RESULTS_PATH);
    fflush(stdout);

    /* Let workers exit only after their final ProcFS snapshot is read. */
    for (i = 0; i < lottery_worker_count; i++) {
        (void) write(exit_gates[i][1], "X", 1);
        close(exit_gates[i][1]);
        exit_gates[i][1] = -1;
    }

    while ((ended = wait(&status)) > 0) {
        waited++;
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
            reports_ok = 0;
    }

    if (waited != lottery_worker_count)
        reports_ok = 0;

    lottery_close_results();
    return reports_ok ? EXIT_SUCCESS : EXIT_FAILURE;

fork_error:
    /* Close start gates so children still waiting for the common start exit. */
    for (i = 0; i < lottery_worker_count; i++) {
        if (start_gates[i][1] >= 0) {
            close(start_gates[i][1]);
            start_gates[i][1] = -1;
        }
        if (exit_gates[i][1] >= 0) {
            close(exit_gates[i][1]);
            exit_gates[i][1] = -1;
        }
    }
    if (ready_pipe[0] >= 0) close(ready_pipe[0]);
    if (ready_pipe[1] >= 0) close(ready_pipe[1]);
    if (report_pipe[0] >= 0) close(report_pipe[0]);
    if (report_pipe[1] >= 0) close(report_pipe[1]);
    while (wait(&status) > 0) ;
    lottery_result("\nBENCHMARK ABORTED: failure while creating workers.\n");
    lottery_close_results();
    return EXIT_FAILURE;

runtime_error:
    for (i = 0; i < lottery_worker_count; i++) {
        if (start_gates[i][1] >= 0) {
            close(start_gates[i][1]);
            start_gates[i][1] = -1;
        }
        if (exit_gates[i][1] >= 0) {
            (void) write(exit_gates[i][1], "X", 1);
            close(exit_gates[i][1]);
            exit_gates[i][1] = -1;
        }
        if (i < created)
            (void) kill(pids[i], SIGUSR1);
    }
    if (ready_pipe[0] >= 0) close(ready_pipe[0]);
    if (ready_pipe[1] >= 0) close(ready_pipe[1]);
    if (report_pipe[0] >= 0) close(report_pipe[0]);
    if (report_pipe[1] >= 0) close(report_pipe[1]);
    while (wait(&status) > 0) ;
    lottery_result("\nBENCHMARK ABORTED: runtime or worker error.\n");
    lottery_close_results();
    return EXIT_FAILURE;

setup_error:
    for (i = 0; i < lottery_worker_count; i++) {
        lottery_close_pair(start_gates[i]);
        lottery_close_pair(exit_gates[i]);
    }
    lottery_close_pair(ready_pipe);
    lottery_close_pair(report_pipe);
    lottery_result("\nBENCHMARK ABORTED: setup error.\n");
    lottery_close_results();
    return EXIT_FAILURE;
}

#endif /* LOTTERY_COMMON_H */
