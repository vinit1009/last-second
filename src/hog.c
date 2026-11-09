/*
 * hog: a runaway job for the demo. After -s ms it computes without blocking for -b ms at SCHED_FIFO
 * priority -p, then exits. Above the pump's priority, it stops the pump's loop for that long.
 *
 * usage: hog [-p priority] [-b busy_ms] [-s start_after_ms]
 */
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/neutrino.h>
#include <sys/syspage.h>

int main(int argc, char **argv)
{
    int priority = 40, opt;
    unsigned busy_ms = 20, start_ms = 0;
    while ((opt = getopt(argc, argv, "p:b:s:")) != -1) {
        switch (opt) {
        case 'p': priority = atoi(optarg); break;
        case 'b': busy_ms = (unsigned)strtoul(optarg, NULL, 0); break;
        case 's': start_ms = (unsigned)strtoul(optarg, NULL, 0); break;
        default:
            fprintf(stderr, "usage: hog [-p priority] [-b busy_ms] [-s start_after_ms]\n");
            return EXIT_FAILURE;
        }
    }
    if (start_ms) {
        usleep(start_ms * 1000u);
    }
    struct sched_param sp = { .sched_priority = priority };
    if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) != EOK) {
        fprintf(stderr, "hog: cannot set priority %d\n", priority);
        return EXIT_FAILURE;
    }
    printf("hog: computing for %u ms at priority %d\n", busy_ms, priority);
    fflush(stdout);
    uint64_t const cps = SYSPAGE_ENTRY(qtime)->cycles_per_sec;
    uint64_t const end = ClockCycles() + cps * busy_ms / 1000u;
    while (ClockCycles() < end) {
    }
    return EXIT_SUCCESS;
}
