/*
 * bbtool: the service tool for /dev/blackbox.
 *
 *   bbtool status            recorder state, window, trigger
 *   bbtool trigger           trigger by hand (it freezes after the post-trigger window)
 *   bbtool reset             clear the window and record again
 *   bbtool dump [-n last]    CSV of the frozen window on stdout (times in ms relative to the
 *                            trigger), and a summary on stderr: the culprit's last sample and the
 *                            longest gap in its samples
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <devctl.h>
#include "blackbox.h"

static double ms_between(uint64_t from, uint64_t to, uint64_t cps)
{
    return ((double)to - (double)from) * 1000.0 / (double)cps;
}

static int get_status(int fd, bb_status_t *st)
{
    int const rc = devctl(fd, DCMD_BB_STATUS, st, sizeof *st, NULL);
    if (rc != EOK) {
        fprintf(stderr, "bbtool: DCMD_BB_STATUS: %s\n", strerror(rc));
        return -1;
    }
    return 0;
}

static const char *state_name(uint32_t s)
{
    return s == BB_FROZEN ? "frozen" : s == BB_TRIGGERED ? "triggered" : "recording";
}

static const char *reason_name(uint32_t r)
{
    switch (r) {
    case BB_REASON_HEARTBEAT: return "missed heartbeat";
    case BB_REASON_PRODUCER_DIED: return "producer died";
    case BB_REASON_MANUAL: return "manual trigger";
    default: return "none";
    }
}

static int cmd_status(int fd)
{
    bb_status_t st;
    if (get_status(fd, &st) != 0) {
        return EXIT_FAILURE;
    }
    printf("state: %s; %u of %u samples held, %llu written, %u dropped while frozen; %u producer(s) declared\n",
           state_name(st.state), st.count, st.capacity, (unsigned long long)st.written, st.dropped, st.producers);
    if (st.reason != BB_REASON_NONE) {
        printf("trigger: %s", reason_name(st.reason));
        if (st.culprit) {
            printf(" by producer %u \"%s\"; its last sample came %.1f ms before the trigger", st.culprit, st.culprit_name,
                   ms_between(st.culprit_last_cycles, st.trigger_cycles, st.cycles_per_sec));
        }
        printf("; %.1f ms ago\n", ms_between(st.trigger_cycles, st.now_cycles, st.cycles_per_sec));
        if (st.culprit_end_cycles != 0 && st.reason != BB_REASON_PRODUCER_DIED) {
            printf("then: \"%s\" ended without a goodbye %.1f ms after the trigger (crashed or killed, not hung)\n", st.culprit_name,
                   ms_between(st.trigger_cycles, st.culprit_end_cycles, st.cycles_per_sec));
        }
    }
    return EXIT_SUCCESS;
}

static int cmd_dump(int fd, uint32_t last)
{
    bb_status_t st;
    if (get_status(fd, &st) != 0) {
        return EXIT_FAILURE;
    }
    if (st.state != BB_FROZEN) {
        fprintf(stderr, "bbtool: the recorder is %s; dump reads a frozen window (bbtool trigger first)\n", state_name(st.state));
        return EXIT_FAILURE;
    }
    bb_sample_t *const all = calloc(st.count ? st.count : 1, sizeof(bb_sample_t));
    if (all == NULL) {
        perror("bbtool: calloc");
        return EXIT_FAILURE;
    }
    size_t got = 0;
    for (;;) {
        ssize_t const r = read(fd, all + got, (st.count - got) * sizeof(bb_sample_t));
        if (r <= 0) {
            break;
        }
        got += (size_t)r / sizeof(bb_sample_t);
        if (got >= st.count) {
            break;
        }
    }
    size_t const first = (last && last < got) ? got - last : 0;
    printf("t_ms,producer,seq,v0,v1,v2,v3\n");
    for (size_t i = first; i < got; ++i) {
        bb_sample_t const *s = &all[i];
        printf("%.3f,%u,%u,%.3f,%.3f,%.3f,%.3f\n", ms_between(st.trigger_cycles, s->t_cycles, st.cycles_per_sec), s->producer,
               s->seq, s->v[0], s->v[1], s->v[2], s->v[3]);
    }
    fflush(stdout);

    /* Summary: the culprit's (or else the first producer's) longest gap and its last sample before the trigger. */
    uint32_t const who = st.culprit ? st.culprit : (got ? all[0].producer : 0);
    double gap = 0, gap_end = 0;
    const bb_sample_t *prev = NULL, *before = NULL;
    for (size_t i = 0; i < got; ++i) {
        if (all[i].producer != who) {
            continue;
        }
        if (prev != NULL) {
            double const g = ms_between(prev->t_cycles, all[i].t_cycles, st.cycles_per_sec);
            if (g > gap) {
                gap = g;
                gap_end = ms_between(st.trigger_cycles, all[i].t_cycles, st.cycles_per_sec);
            }
        }
        if (all[i].t_cycles <= st.trigger_cycles) {
            before = &all[i];
        }
        prev = &all[i];
    }
    fprintf(stderr, "bbtool: %zu samples read (%.1f s); trigger: %s", got,
            got ? ms_between(all[0].t_cycles, all[got - 1].t_cycles, st.cycles_per_sec) / 1000.0 : 0.0, reason_name(st.reason));
    if (st.culprit) {
        fprintf(stderr, " by \"%s\"", st.culprit_name);
    }
    fprintf(stderr, "\n");
    if (before != NULL) {
        fprintf(stderr, "bbtool: producer %u's last sample before the trigger: seq %u at %.1f ms, v = %.2f %.2f %.2f %.2f\n", who,
                before->seq, ms_between(st.trigger_cycles, before->t_cycles, st.cycles_per_sec), before->v[0], before->v[1],
                before->v[2], before->v[3]);
    }
    if (gap > 0) {
        fprintf(stderr, "bbtool: producer %u's longest gap between samples: %.1f ms, ending at %.1f ms\n", who, gap, gap_end);
    }
    free(all);
    return EXIT_SUCCESS;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: bbtool status | trigger | reset | dump [-n last]\n");
        return EXIT_FAILURE;
    }
    char const *const cmd = argv[1];
    int const fd = open(BB_PATH, strcmp(cmd, "dump") == 0 ? O_RDONLY : O_RDWR);
    if (fd == -1) {
        perror("bbtool: open " BB_PATH);
        return EXIT_FAILURE;
    }
    if (strcmp(cmd, "status") == 0) {
        return cmd_status(fd);
    }
    if (strcmp(cmd, "trigger") == 0 || strcmp(cmd, "reset") == 0) {
        int const rc = devctl(fd, strcmp(cmd, "trigger") == 0 ? DCMD_BB_TRIGGER : DCMD_BB_RESET, NULL, 0, NULL);
        if (rc != EOK) {
            fprintf(stderr, "bbtool: %s: %s\n", cmd, strerror(rc));
            return EXIT_FAILURE;
        }
        return EXIT_SUCCESS;
    }
    if (strcmp(cmd, "dump") == 0) {
        uint32_t last = 0;
        if (argc >= 4 && strcmp(argv[2], "-n") == 0) {
            last = (uint32_t)strtoul(argv[3], NULL, 0);
        }
        return cmd_dump(fd, last);
    }
    fprintf(stderr, "bbtool: unknown command %s\n", cmd);
    return EXIT_FAILURE;
}
