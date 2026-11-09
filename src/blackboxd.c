/*
 * blackboxd: Last Second's flight recorder, a resource manager at /dev/blackbox (QNX OS 8.0).
 *
 * - write(): each call is a MsgSend() from the producer; this single dispatch thread receives it at
 *   the producer's priority (QNX priority inheritance), stamps and stores the samples, and replies.
 *   The samples live in this process, so they survive the producer crashing.
 * - A periodic timer pulse at a high priority (-P, default 60) checks every producer's heartbeat,
 *   so a missed deadline is noticed even while a runaway thread holds the CPU.
 * - A producer whose connection closes without DCMD_BB_GOODBYE (its process died) is a trigger.
 * - After a trigger the recorder keeps recording for -p ms, then freezes; read() returns the
 *   frozen window, oldest sample first.
 * - With -s dir, a low-priority thread also saves each frozen window there as CSV (the newest -k
 *   files are kept), so it survives a reboot. The dispatch thread never touches the disk: a
 *   producer's write() is never made to wait for a file system.
 *
 * usage: blackboxd [-n capacity] [-p post_ms] [-t tick_us] [-P tick_priority] [-s dir] [-k keep]
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/neutrino.h>
#include <sys/stat.h>
#include <sys/syspage.h>

struct bb_ocb;
#define IOFUNC_OCB_T struct bb_ocb
#define RESMGR_OCB_T struct bb_ocb
#include <sys/iofunc.h>
#include <sys/dispatch.h>

#include "blackbox.h"
#include "core.h"

struct bb_ocb {
    iofunc_ocb_t hdr; /* must be first */
    int producer;     /* id from DCMD_BB_HELLO, 0 if the client never declared itself */
};

static bb_core_t core;
static uint64_t cps;
static bb_sample_t reply_buf[256];

/* Saving frozen windows (-s): the dispatch thread only raises save_pending; the saver thread writes. */
static const char *save_dir;
static unsigned keep = 20;
static pthread_mutex_t save_mx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t save_cv = PTHREAD_COND_INITIALIZER;
static int save_pending, saving;

static struct bb_ocb *ocb_calloc(resmgr_context_t *ctp, iofunc_attr_t *attr)
{
    (void)ctp;
    (void)attr;
    return calloc(1, sizeof(struct bb_ocb));
}

static void ocb_free(struct bb_ocb *ocb)
{
    free(ocb);
}

static iofunc_funcs_t ocb_funcs = { .nfuncs = _IOFUNC_NFUNCS, .ocb_calloc = ocb_calloc, .ocb_free = ocb_free };
static iofunc_mount_t mountpoint = { .funcs = &ocb_funcs };

static double cycles_to_ms(uint64_t c)
{
    return (double)c * 1000.0 / (double)cps;
}

static int io_write(resmgr_context_t *ctp, io_write_t *msg, struct bb_ocb *ocb)
{
    int status = iofunc_write_verify(ctp, msg, &ocb->hdr, NULL);
    if (status != EOK) {
        return status;
    }
    if ((msg->i.xtype & _IO_XTYPE_MASK) != _IO_XTYPE_NONE) {
        return ENOSYS;
    }
    bb_sample_t batch[64];
    size_t const nbytes = _IO_WRITE_GET_NBYTES(msg);
    if (nbytes == 0 || nbytes % sizeof(bb_sample_t) != 0 || nbytes > sizeof batch) {
        return EINVAL;
    }
    if (resmgr_msgget(ctp, batch, nbytes, sizeof(msg->i)) != (ssize_t)nbytes) {
        return EIO;
    }
    bb_write(&core, ocb->producer, batch, (uint32_t)(nbytes / sizeof(bb_sample_t)), ClockCycles());
    _IO_SET_WRITE_NBYTES(ctp, nbytes); /* accepted even when frozen: the loop must never fail */
    return EOK;
}

static int io_read(resmgr_context_t *ctp, io_read_t *msg, struct bb_ocb *ocb)
{
    int status = iofunc_read_verify(ctp, msg, &ocb->hdr, NULL);
    if (status != EOK) {
        return status;
    }
    if ((msg->i.xtype & _IO_XTYPE_MASK) != _IO_XTYPE_NONE) {
        return ENOSYS;
    }
    if (core.state != BB_FROZEN) {
        return EBUSY; /* trigger first (bbtool trigger): a moving window cannot be read consistently */
    }
    size_t want = msg->i.nbytes / sizeof(bb_sample_t);
    if (want > sizeof reply_buf / sizeof reply_buf[0]) {
        want = sizeof reply_buf / sizeof reply_buf[0];
    }
    uint32_t const index = (uint32_t)(ocb->hdr.offset / (off_t)sizeof(bb_sample_t));
    uint32_t const n = bb_window(&core, index, reply_buf, (uint32_t)want);
    ocb->hdr.offset += (off_t)(n * sizeof(bb_sample_t));
    _IO_SET_READ_NBYTES(ctp, n * sizeof(bb_sample_t));
    return _RESMGR_PTR(ctp, reply_buf, n * sizeof(bb_sample_t));
}

static int io_devctl(resmgr_context_t *ctp, io_devctl_t *msg, struct bb_ocb *ocb)
{
    int status = iofunc_devctl_default(ctp, msg, &ocb->hdr);
    if (status != _RESMGR_DEFAULT) {
        return status;
    }
    void *const data = _DEVCTL_DATA(msg->i);
    uint64_t const now = ClockCycles();
    size_t nbytes = 0;
    switch (msg->i.dcmd) {
    case DCMD_BB_HELLO: {
        bb_hello_t hello;
        memcpy(&hello, data, sizeof hello);
        hello.name[BB_NAME_MAX - 1] = '\0';
        if (ocb->producer > 0) {
            bb_remove_producer(&core, ocb->producer, 1, now);
        }
        int const id = bb_add_producer(&core, hello.name, hello.deadline_us, now);
        if (id < 0) {
            return EAGAIN;
        }
        ocb->producer = id;
        printf("blackboxd: producer %d \"%s\" declared, heartbeat deadline %u us\n", id, hello.name, hello.deadline_us);
        fflush(stdout);
        break;
    }
    case DCMD_BB_GOODBYE:
        bb_remove_producer(&core, ocb->producer, 1, now);
        ocb->producer = 0;
        break;
    case DCMD_BB_TRIGGER:
        bb_trigger(&core, BB_REASON_MANUAL, ocb->producer, now);
        break;
    case DCMD_BB_RESET: {
        pthread_mutex_lock(&save_mx);
        int const busy = save_pending || saving;
        pthread_mutex_unlock(&save_mx);
        if (busy) {
            return EBUSY; /* the saver is still reading the frozen window; ask again shortly */
        }
        bb_reset(&core, now);
        printf("blackboxd: reset, recording\n");
        fflush(stdout);
        break;
    }
    case DCMD_BB_STATUS:
        bb_get_status(&core, (bb_status_t *)data, now);
        nbytes = sizeof(bb_status_t);
        break;
    default:
        return ENOSYS;
    }
    memset(&msg->o, 0, sizeof msg->o);
    msg->o.nbytes = (uint32_t)nbytes;
    return _RESMGR_PTR(ctp, &msg->o, sizeof msg->o + nbytes);
}

static int io_close_ocb(resmgr_context_t *ctp, void *reserved, struct bb_ocb *ocb)
{
    if (ocb->producer > 0) {
        /* A declared producer that never said goodbye: the kernel is closing it because the
         * process died (crash, kill). */
        bb_remove_producer(&core, ocb->producer, 0, ClockCycles());
        ocb->producer = 0;
    }
    return iofunc_close_ocb_default(ctp, reserved, &ocb->hdr);
}

static int name_cmp(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

/* Removes windows whose save was interrupted (a .part left by a power cut or a kill). */
static void remove_partial(void)
{
    DIR *const dir = opendir(save_dir);
    if (dir == NULL) {
        return;
    }
    struct dirent *e;
    while ((e = readdir(dir)) != NULL) {
        size_t const len = strlen(e->d_name);
        if (strncmp(e->d_name, "window-", 7) == 0 && len > 5 && strcmp(e->d_name + len - 5, ".part") == 0) {
            char path[PATH_MAX];
            snprintf(path, sizeof path, "%s/%s", save_dir, e->d_name);
            printf("blackboxd: removing %s, an interrupted save\n", path);
            unlink(path);
        }
    }
    closedir(dir);
    fflush(stdout);
}

/* Keeps the newest `keep` windows; the names sort by time. */
static void prune(void)
{
    DIR *const dir = opendir(save_dir);
    if (dir == NULL) {
        return;
    }
    char *names[512];
    size_t n = 0;
    struct dirent *e;
    while ((e = readdir(dir)) != NULL && n < sizeof names / sizeof names[0]) {
        size_t const len = strlen(e->d_name);
        if (strncmp(e->d_name, "window-", 7) == 0 && len > 4 && strcmp(e->d_name + len - 4, ".csv") == 0) {
            names[n++] = strdup(e->d_name);
        }
    }
    closedir(dir);
    qsort(names, n, sizeof names[0], name_cmp);
    for (size_t i = 0; i + keep < n; ++i) {
        char path[PATH_MAX];
        snprintf(path, sizeof path, "%s/%s", save_dir, names[i]);
        unlink(path);
    }
    for (size_t i = 0; i < n; ++i) {
        free(names[i]);
    }
}

/* Runs on the saver thread while the ring is frozen: the dispatch thread does not modify the
 * samples in that state, and DCMD_BB_RESET is refused until this returns. */
static void save_window(void)
{
    static unsigned saved;
    char stamp[32], path[PATH_MAX], part[PATH_MAX + 8];
    time_t const now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", &tm);
    snprintf(path, sizeof path, "%s/window-%s-%03u.csv", save_dir, stamp, ++saved);
    snprintf(part, sizeof part, "%s.part", path);
    FILE *const f = fopen(part, "w");
    if (f == NULL) {
        printf("blackboxd: cannot save the window to %s: %s\n", part, strerror(errno));
        fflush(stdout);
        return;
    }
    fprintf(f, "# Last Second window: trigger %s", bb_reason_name(core.reason));
    if (core.culprit != 0) {
        fprintf(f, " by \"%s\"", core.culprit_name);
    }
    fprintf(f, "; %u samples; times in ms relative to the trigger\nt_ms,producer,seq,v0,v1,v2,v3\n", core.count);
    bb_sample_t chunk[256];
    for (uint32_t i = 0; i < core.count;) {
        uint32_t const got = bb_window(&core, i, chunk, sizeof chunk / sizeof chunk[0]);
        if (got == 0) {
            break;
        }
        for (uint32_t k = 0; k < got; ++k) {
            bb_sample_t const *s = &chunk[k];
            fprintf(f, "%.3f,%u,%u,%.3f,%.3f,%.3f,%.3f\n", ((double)s->t_cycles - (double)core.trigger_cycles) * 1000.0 / (double)cps,
                    s->producer, s->seq, s->v[0], s->v[1], s->v[2], s->v[3]);
        }
        i += got;
    }
    int ok = fflush(f) == 0 && fsync(fileno(f)) == 0;
    ok = fclose(f) == 0 && ok;
    if (!ok || rename(part, path) != 0) {
        printf("blackboxd: saving %s failed: %s\n", path, strerror(errno));
        fflush(stdout);
        unlink(part);
        return;
    }
    /* The rename is a change to the directory: sync it too, or a power cut can leave only the .part. */
    int const dfd = open(save_dir, O_RDONLY);
    if (dfd == -1 || fsync(dfd) == -1) {
        sync();
    }
    if (dfd != -1) {
        close(dfd);
    }
    printf("blackboxd: saved the frozen window to %s\n", path);
    fflush(stdout);
    prune();
}

static void *saver(void *arg)
{
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&save_mx);
        while (!save_pending) {
            pthread_cond_wait(&save_cv, &save_mx);
        }
        save_pending = 0;
        saving = 1;
        pthread_mutex_unlock(&save_mx);
        save_window();
        pthread_mutex_lock(&save_mx);
        saving = 0;
        pthread_mutex_unlock(&save_mx);
    }
    return NULL;
}

static int on_tick(message_context_t *ctp, int code, unsigned flags, void *handle)
{
    (void)ctp;
    (void)code;
    (void)flags;
    (void)handle;
    bb_tick(&core, ClockCycles());
    return 0;
}

/* Prints state changes once, after the message that caused them has been handled. */
static void report(uint32_t *last_state)
{
    static uint64_t reported_end;
    if (core.culprit_end != reported_end) {
        reported_end = core.culprit_end;
        if (core.culprit_end != 0 && core.reason != BB_REASON_PRODUCER_DIED) {
            printf("blackboxd: producer %u \"%s\" then ended without a goodbye, %.1f ms after the trigger: it crashed or was killed, not hung\n",
                   core.culprit, core.culprit_name, cycles_to_ms(core.culprit_end - core.trigger_cycles));
            fflush(stdout);
        }
    }
    if (core.state == *last_state) {
        return;
    }
    uint64_t const now = ClockCycles();
    if (core.state == BB_TRIGGERED || (core.state == BB_FROZEN && *last_state == BB_RECORDING)) {
        if (core.culprit != 0) {
            printf("blackboxd: TRIGGER %s: producer %u \"%s\", last sample %.1f ms before the trigger\n",
                   bb_reason_name(core.reason), core.culprit, core.culprit_name,
                   cycles_to_ms(core.trigger_cycles - core.culprit_last));
        } else {
            printf("blackboxd: TRIGGER %s\n", bb_reason_name(core.reason));
        }
    }
    if (core.state == BB_FROZEN) {
        bb_sample_t first;
        double span = 0;
        if (core.count > 0 && bb_window(&core, 0, &first, 1) == 1) {
            span = cycles_to_ms(now - first.t_cycles) / 1000.0;
        }
        printf("blackboxd: FROZEN with %u samples (%.1f s), %u ms after the trigger; read with bbtool dump\n",
               core.count, span, core.post_ms);
        if (save_dir != NULL) {
            pthread_mutex_lock(&save_mx);
            save_pending = 1;
            pthread_cond_signal(&save_cv);
            pthread_mutex_unlock(&save_mx);
        }
    }
    fflush(stdout);
    *last_state = core.state;
}

int main(int argc, char **argv)
{
    uint32_t capacity = 30000, post_ms = 200, tick_us = 1000;
    int tick_prio = 60, save_prio = 8, opt;
    while ((opt = getopt(argc, argv, "n:p:t:P:s:k:")) != -1) {
        switch (opt) {
        case 'n': capacity = (uint32_t)strtoul(optarg, NULL, 0); break;
        case 'p': post_ms = (uint32_t)strtoul(optarg, NULL, 0); break;
        case 't': tick_us = (uint32_t)strtoul(optarg, NULL, 0); break;
        case 'P': tick_prio = atoi(optarg); break;
        case 's': save_dir = optarg; break;
        case 'k': keep = (unsigned)strtoul(optarg, NULL, 0); break;
        default:
            fprintf(stderr, "usage: blackboxd [-n capacity] [-p post_ms] [-t tick_us] [-P tick_priority] [-s dir] [-k keep]\n");
            return EXIT_FAILURE;
        }
    }
    if (capacity == 0 || tick_us == 0) {
        fprintf(stderr, "blackboxd: capacity and tick must be positive\n");
        return EXIT_FAILURE;
    }
    cps = SYSPAGE_ENTRY(qtime)->cycles_per_sec;
    bb_sample_t *const storage = calloc(capacity, sizeof(bb_sample_t));
    if (storage == NULL) {
        perror("blackboxd: storage");
        return EXIT_FAILURE;
    }
    bb_init(&core, storage, capacity, cps, post_ms);

    if (save_dir != NULL) {
        if (mkdir(save_dir, 0750) == -1 && errno != EEXIST) {
            printf("blackboxd: cannot create %s (%s); frozen windows stay in RAM only\n", save_dir, strerror(errno));
            save_dir = NULL;
        } else {
            remove_partial();
            pthread_attr_t attr_t;
            struct sched_param sp = { .sched_priority = save_prio };
            pthread_attr_init(&attr_t);
            pthread_attr_setinheritsched(&attr_t, PTHREAD_EXPLICIT_SCHED);
            pthread_attr_setschedpolicy(&attr_t, SCHED_FIFO);
            pthread_attr_setschedparam(&attr_t, &sp);
            pthread_t tid;
            if (pthread_create(&tid, &attr_t, saver, NULL) != EOK) {
                printf("blackboxd: cannot start the saver thread; frozen windows stay in RAM only\n");
                save_dir = NULL;
            }
            pthread_attr_destroy(&attr_t);
        }
    }

    dispatch_t *const dpp = dispatch_create_channel(-1, DISPATCH_FLAG_NOLOCK);
    if (dpp == NULL) {
        perror("blackboxd: dispatch_create_channel");
        return EXIT_FAILURE;
    }
    static resmgr_connect_funcs_t connect_funcs;
    static resmgr_io_funcs_t io_funcs;
    static iofunc_attr_t attr;
    iofunc_func_init(_RESMGR_CONNECT_NFUNCS, &connect_funcs, _RESMGR_IO_NFUNCS, &io_funcs);
    io_funcs.read = io_read;
    io_funcs.write = io_write;
    io_funcs.devctl = io_devctl;
    io_funcs.close_ocb = io_close_ocb;
    iofunc_attr_init(&attr, S_IFCHR | 0666, NULL, NULL);
    attr.mount = &mountpoint;

    resmgr_attr_t rattr;
    memset(&rattr, 0, sizeof rattr);
    rattr.nparts_max = 1;
    rattr.msg_max_size = 4096;
    if (resmgr_attach(dpp, &rattr, BB_PATH, _FTYPE_ANY, 0, &connect_funcs, &io_funcs, &attr) == -1) {
        perror("blackboxd: resmgr_attach " BB_PATH);
        return EXIT_FAILURE;
    }

    /* The watchdog: a periodic pulse to our own channel, delivered at tick_prio. */
    int const code = pulse_attach(dpp, MSG_FLAG_ALLOC_PULSE, 0, on_tick, NULL);
    int const coid = message_connect(dpp, MSG_FLAG_SIDE_CHANNEL);
    if (code == -1 || coid == -1) {
        perror("blackboxd: watchdog pulse");
        return EXIT_FAILURE;
    }
    struct sigevent ev;
    SIGEV_PULSE_INIT(&ev, coid, tick_prio, code, 0);
    timer_t timer;
    if (timer_create(CLOCK_MONOTONIC, &ev, &timer) == -1) {
        perror("blackboxd: timer_create");
        return EXIT_FAILURE;
    }
    struct itimerspec const its = { .it_value = { 0, (long)tick_us * 1000 }, .it_interval = { 0, (long)tick_us * 1000 } };
    timer_settime(timer, 0, &its, NULL);

    printf("blackboxd: recording at %s: %u samples, post-trigger %u ms, watchdog every %u us at priority %d\n",
           BB_PATH, capacity, post_ms, tick_us, tick_prio);
    if (save_dir != NULL) {
        printf("blackboxd: frozen windows are also saved to %s (newest %u kept), at priority %d\n", save_dir, keep, save_prio);
    }
    fflush(stdout);

    dispatch_context_t *ctp = dispatch_context_alloc(dpp);
    uint32_t last_state = BB_RECORDING;
    for (;;) {
        dispatch_context_t *const got = dispatch_block(ctp);
        if (got == NULL) {
            if (errno == EINTR) {
                continue;
            }
            perror("blackboxd: dispatch_block");
            return EXIT_FAILURE;
        }
        ctp = got;
        dispatch_handler(ctp);
        report(&last_state);
    }
}
