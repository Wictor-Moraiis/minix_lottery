
/*
 * Benchmark for the original MINIX 3.2.1 scheduler.
 * User chooses 2..8 workers and execution duration.
 * All workers execute the same CPU-bound workload.
 */

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>

#define MAX_WORKERS 8
#define PRIME_LIMIT 1000000UL
#define MAX_DURATION 3600
#define RESULTS_PATH "original_results.txt"

static volatile sig_atomic_t stop_requested = 0;

struct worker_report {
    int index;
    unsigned long units;
};

static void stop_worker(int sig)
{
    (void)sig;
    stop_requested = 1;
}

static int read_integer(const char *prompt, int min, int max, int *value)
{
    char input[64];
    char *end;
    long number;

    printf("%s", prompt);
    fflush(stdout);

    if (fgets(input, sizeof(input), stdin) == NULL)
        return -1;

    errno = 0;
    number = strtol(input, &end, 10);

    if (end == input || errno == ERANGE)
        return -1;

    while (*end == ' ' || *end == '\t' ||
           *end == '\r' || *end == '\n')
        end++;

    if (*end != '\0' || number < min || number > max)
        return -1;

    *value = (int)number;
    return 0;
}

static int read_byte(int fd, char *value)
{
    ssize_t n;

    do {
        n = read(fd, value, 1);
    } while (n == -1 && errno == EINTR);

    return n == 1 ? 0 : -1;
}

static int write_all(int fd, const void *buffer, size_t length)
{
    const char *p = (const char *)buffer;
    size_t done = 0;
    ssize_t n;

    while (done < length) {
        n = write(fd, p + done, length - done);

        if (n == -1 && errno == EINTR)
            continue;

        if (n <= 0)
            return -1;

        done += (size_t)n;
    }

    return 0;
}

static int read_all(int fd, void *buffer, size_t length)
{
    char *p = (char *)buffer;
    size_t done = 0;
    ssize_t n;

    while (done < length) {
        n = read(fd, p + done, length - done);

        if (n == -1 && errno == EINTR)
            continue;

        if (n <= 0)
            return -1;

        done += (size_t)n;
    }

    return 0;
}

/* Same CPU-bound workload used by the Lottery benchmark. */
static unsigned long count_primes(unsigned long limit)
{
    unsigned long n, d, count = 0;
    int prime;

    for (n = 2; n <= limit && !stop_requested; n++) {
        prime = 1;

        for (d = 2; d <= n / d && !stop_requested; d++) {
            if (n % d == 0) {
                prime = 0;
                break;
            }
        }

        if (stop_requested)
            break;

        if (prime)
            count++;
    }

    return count;
}

static void worker_main(int index, int start_read,
                        int ready_write, int report_write)
{
    char token;
    unsigned long units = 0;
    volatile unsigned long checksum = 0;
    struct worker_report report;

    if (signal(SIGUSR1, stop_worker) == SIG_ERR)
        _exit(2);

    if (write(ready_write, "R", 1) != 1)
        _exit(3);

    close(ready_write);

    if (read_byte(start_read, &token) != 0)
        _exit(4);

    close(start_read);

    while (!stop_requested) {
        unsigned long result;

        result = count_primes(PRIME_LIMIT);

        if (stop_requested)
            break;

        checksum += result;
        units++;
    }

    report.index = index;
    report.units = units;

    if (write_all(report_write, &report, sizeof(report)) != 0)
        _exit(5);

    (void)checksum;
    close(report_write);

    _exit(EXIT_SUCCESS);
}

int main(void)
{
    int workers, duration;
    int start_pipe[2], ready_pipe[2], report_pipe[2];
    pid_t pids[MAX_WORKERS];
    struct worker_report reports[MAX_WORKERS];
    FILE *results;
    char token;
    int i, status, waited = 0;
    int reports_ok = 1;
    unsigned long total_units = 0;
    pid_t ended;

    printf("\nORIGINAL MINIX SCHEDULER BENCHMARK\n");
    printf("Allowed workers: 2 to %d\n", MAX_WORKERS);
    printf("Allowed duration: 1 to %d seconds\n", MAX_DURATION);

    if (read_integer("Number of workers: ", 2,
                     MAX_WORKERS, &workers) != 0) {
        printf("Invalid worker count.\n");
        return EXIT_FAILURE;
    }

    if (read_integer("Execution duration in seconds: ", 1,
                     MAX_DURATION, &duration) != 0) {
        printf("Invalid duration.\n");
        return EXIT_FAILURE;
    }

    results = fopen(RESULTS_PATH, "a");

    if (results == NULL) {
        perror("Could not open results file");
        return EXIT_FAILURE;
    }

    if (pipe(start_pipe) == -1 ||
        pipe(ready_pipe) == -1 ||
        pipe(report_pipe) == -1) {
        perror("pipe");
        fclose(results);
        return EXIT_FAILURE;
    }

    for (i = 0; i < workers; i++) {
        pids[i] = fork();

        if (pids[i] == -1) {
            perror("fork");
            reports_ok = 0;
            break;
        }

        if (pids[i] == 0) {
            close(start_pipe[1]);
            close(ready_pipe[0]);
            close(report_pipe[0]);

            worker_main(i, start_pipe[0],
                        ready_pipe[1], report_pipe[1]);
        }
    }

    close(start_pipe[0]);
    close(ready_pipe[1]);
    close(report_pipe[1]);

    if (i != workers) {
        close(start_pipe[1]);
        close(ready_pipe[0]);
        close(report_pipe[0]);

        while (wait(&status) > 0)
            ;

        fclose(results);
        return EXIT_FAILURE;
    }

    /* Wait until every worker is ready. */
    for (i = 0; i < workers; i++) {
        if (read_byte(ready_pipe[0], &token) != 0 || token != 'R')
            reports_ok = 0;
    }

    close(ready_pipe[0]);

    if (!reports_ok) {
        for (i = 0; i < workers; i++)
            kill(pids[i], SIGUSR1);

        close(start_pipe[1]);
        close(report_pipe[0]);

        while (wait(&status) > 0)
            ;

        fclose(results);
        return EXIT_FAILURE;
    }

    fprintf(results, "\n========================================\n");
    fprintf(results, "ORIGINAL MINIX SCHEDULER BENCHMARK\n");
    fprintf(results, "Workers: %d | Duration: %d seconds\n",
            workers, duration);
    fprintf(results, "========================================\n");

    printf("\nAll workers ready.\n");
    printf("Benchmark running for %d seconds...\n", duration);
    fflush(stdout);

    /* Release all workers for the measurement. */
    for (i = 0; i < workers; i++) {
        if (write(start_pipe[1], "S", 1) != 1)
            reports_ok = 0;
    }

    close(start_pipe[1]);

    if (!reports_ok) {
        for (i = 0; i < workers; i++)
            kill(pids[i], SIGUSR1);
    } else {
        i = duration;

        while (i > 0)
            i = sleep((unsigned int)i);
    }

    printf("Measurement finished. Collecting results...\n");

    for (i = 0; i < workers; i++) {
        if (kill(pids[i], SIGUSR1) == -1 && errno != ESRCH)
            reports_ok = 0;
    }

    for (i = 0; i < workers; i++) {
        if (read_all(report_pipe[0], &reports[i],
                     sizeof(reports[i])) != 0) {
            reports_ok = 0;
            break;
        }

        if (reports[i].index < 0 || reports[i].index >= workers) {
            reports_ok = 0;
            break;
        }

        total_units += reports[i].units;
    }

    close(report_pipe[0]);

    fprintf(results, "\n=== FINAL RESULTS ===\n");

    for (i = 0; i < workers && reports_ok; i++) {
        double share = total_units > 0 ?
            100.0 * (double)reports[i].units / total_units : 0.0;

        fprintf(results,
                "Worker %d | PID=%ld | Completed work units: %lu | Work share: %.2f%%\n",
                reports[i].index + 1,
                (long)pids[reports[i].index],
                reports[i].units, share);
    }

    fprintf(results, "Total completed work units: %lu\n", total_units);
    fprintf(results, "========================================\n");
    fflush(results);

    while ((ended = wait(&status)) > 0) {
        waited++;

        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
            reports_ok = 0;
    }

    if (waited != workers)
        reports_ok = 0;

    fclose(results);

    printf("Benchmark complete.\n");
    printf("Results saved in %s\n", RESULTS_PATH);

    return reports_ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
