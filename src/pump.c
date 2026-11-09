/*
 * pump: a stand-in for an infusion pump's flow controller, recording every tick in /dev/blackbox.
 *
 * A 1 kHz loop blocked in MsgReceivePulse() on a timer pulse runs a PI controller that holds a flow
 * setpoint against a simulated first-order pump. Each tick writes one sample to the recorder:
 *   v[0] setpoint (ml/h), v[1] measured flow (ml/h), v[2] motor command (%), v[3] how late this
 *   tick woke up (us).
 * It declares a heartbeat deadline (-d ms), so the recorder triggers if the loop stops.
 *
 * usage: pump [-d deadline_ms] [-p priority] [-t run_seconds] [-c crash_after_ticks] [-N name]
 *   -c N dereferences a null pointer at tick N, to show the recorder keeping the samples before a
 *   crash. SIGTERM/SIGINT stop the loop cleanly (with a goodbye), so they are not a trigger.
 */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <devctl.h>
#include <sys/neutrino.h>
#include <sys/siginfo.h>
#include <sys/syspage.h>
#include "blackbox.h"

#define PULSE_CODE_TICK (_PULSE_CODE_MINAVAIL + 1)
#define PERIOD_US 1000u

static volatile sig_atomic_t stop;
static void on_signal(int sig)
{
    (void)sig;
    stop = 1;
}

static int cmp_u32(const void *a, const void *b)
{
    uint32_t const x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return (x > y) - (x < y);
}

int main(int argc, char **argv)
{
    unsigned deadline_ms = 5, run_s = 0, crash_after = 0;
    int priority = 30, opt;
    const char *name = "pump";
    while ((opt = getopt(argc, argv, "d:p:t:c:N:")) != -1) {
        switch (opt) {
        case 'd': deadline_ms = (unsigned)strtoul(optarg, NULL, 0); break;
        case 'p': priority = atoi(optarg); break;
        case 't': run_s = (unsigned)strtoul(optarg, NULL, 0); break;
        case 'c': crash_after = (unsigned)strtoul(optarg, NULL, 0); break;
        case 'N': name = optarg; break;
        default:
            fprintf(stderr, "usage: pump [-d deadline_ms] [-p priority] [-t run_seconds] [-c crash_after_ticks] [-N name]\n");
            return EXIT_FAILURE;
        }
    }
    struct sched_param sp = { .sched_priority = priority };
    if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) != EOK) {
        fprintf(stderr, "pump: cannot set priority %d\n", priority);
        return EXIT_FAILURE;
    }
    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);

    int const fd = open(BB_PATH, O_WRONLY);
    if (fd == -1) {
        perror("pump: open " BB_PATH);
        return EXIT_FAILURE;
    }
    bb_hello_t hello = { .deadline_us = deadline_ms * 1000u };
    strncpy(hello.name, name, sizeof hello.name - 1);
    int rc = devctl(fd, DCMD_BB_HELLO, &hello, sizeof hello, NULL);
    if (rc != EOK) {
        fprintf(stderr, "pump: DCMD_BB_HELLO: %s\n", strerror(rc));
        return EXIT_FAILURE;
    }

    int const chid = ChannelCreate(_NTO_CHF_PRIVATE);
    int const coid = ConnectAttach(0, 0, chid, _NTO_SIDE_CHANNEL, 0);
    struct sigevent ev;
    SIGEV_PULSE_INIT(&ev, coid, priority, PULSE_CODE_TICK, 0);
    timer_t timer;
    if (chid == -1 || coid == -1 || timer_create(CLOCK_MONOTONIC, &ev, &timer) == -1) {
        perror("pump: timer");
        return EXIT_FAILURE;
    }
    struct itimerspec const its = { .it_value = { 0, PERIOD_US * 1000 }, .it_interval = { 0, PERIOD_US * 1000 } };

    uint64_t const cps = SYSPAGE_ENTRY(qtime)->cycles_per_sec;
    uint64_t const period = cps * PERIOD_US / 1000000u;
    size_t const max_ticks = run_s ? (size_t)run_s * (1000000u / PERIOD_US) : 600000u;
    uint32_t *const write_ns = calloc(max_ticks, sizeof(uint32_t));
    if (write_ns == NULL) {
        perror("pump: calloc");
        return EXIT_FAILURE;
    }

    /* Simulated pump: flow follows the motor command with a 50 ms time constant. */
    float const dt = PERIOD_US / 1e6f, tau = 0.05f, gain = 2.0f, kp = 0.8f, ki = 6.0f;
    float flow = 0, integ = 0;
    printf("%s: running at %u Hz, priority %d, heartbeat deadline %u ms\n", name, 1000000u / PERIOD_US, priority, deadline_ms);
    fflush(stdout);

    timer_settime(timer, 0, &its, NULL);
    uint64_t expected = 0; /* set by the first tick: lateness is measured against that schedule */
    uint32_t seq = 0;
    size_t n = 0;
    while (!stop && n < max_ticks) {
        struct _pulse pulse;
        if (MsgReceivePulse(chid, &pulse, sizeof pulse, NULL) == -1 || pulse.code != PULSE_CODE_TICK) {
            continue;
        }
        uint64_t const woke = ClockCycles();
        if (expected == 0) {
            expected = woke;
        }
        float const late_us = woke > expected ? (float)((double)(woke - expected) * 1e6 / (double)cps) : 0.0f;
        expected += period;

        float const setpoint = seq < 300 ? 100.0f * (float)seq / 300.0f : 100.0f; /* ml/h, ramp in 0.3 s */
        float const err = setpoint - flow;
        integ += err * dt;
        float cmd = kp * err + ki * integ;
        if (cmd < 0) cmd = 0;
        if (cmd > 100) cmd = 100;
        flow += (gain * cmd - flow) * dt / tau;

        bb_sample_t s = { .seq = ++seq, .v = { setpoint, flow, cmd, late_us } };
        uint64_t const t0 = ClockCycles();
        if (write(fd, &s, sizeof s) != (ssize_t)sizeof s) {
            perror("pump: write");
        }
        uint64_t const t1 = ClockCycles();
        write_ns[n++] = (uint32_t)((t1 - t0) * 1000000000u / cps);

        if (crash_after && seq == crash_after) {
            printf("%s: crashing on purpose at tick %u\n", name, seq);
            fflush(stdout);
            *(volatile int *)0 = 1;
        }
    }
    devctl(fd, DCMD_BB_GOODBYE, NULL, 0, NULL);
    close(fd);

    qsort(write_ns, n, sizeof write_ns[0], cmp_u32);
    if (n > 0) {
        printf("%s: %zu ticks; write() to the recorder took p50 %.1f us, p99 %.1f us, max %.1f us\n", name, n,
               write_ns[n / 2] / 1000.0, write_ns[(n * 99) / 100] / 1000.0, write_ns[n - 1] / 1000.0);
    }
    free(write_ns);
    return EXIT_SUCCESS;
}
