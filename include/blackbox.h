/*
 * blackbox.h: the interface to Last Second's flight recorder, /dev/blackbox, on QNX OS 8.0.
 *
 * A producer opens /dev/blackbox, declares itself once with DCMD_BB_HELLO (a name and a heartbeat
 * deadline), then write()s one bb_sample_t per control-loop tick. The recorder keeps the newest
 * samples in RAM. When a producer misses its heartbeat or its process dies without DCMD_BB_GOODBYE,
 * the recorder keeps recording for a short post-trigger window and then freezes, so the last
 * seconds before the failure can be read back with read() (bbtool dump).
 */
#ifndef LAST_SECOND_BLACKBOX_H
#define LAST_SECOND_BLACKBOX_H

#include <stdint.h>

#define BB_PATH "/dev/blackbox"
#define BB_NAME_MAX 24
#define BB_MAX_PRODUCERS 16

/* One sample, 32 bytes. Producers fill seq and v[]; the recorder stamps t_cycles and producer. */
typedef struct bb_sample {
    uint64_t t_cycles; /* ClockCycles() when the recorder received the sample */
    uint32_t seq;      /* the producer's own counter */
    uint16_t producer; /* recorder-assigned id, 1..BB_MAX_PRODUCERS (0: undeclared writer) */
    uint16_t flags;    /* unused, 0 */
    float v[4];        /* producer-defined values */
} bb_sample_t;

_Static_assert(sizeof(bb_sample_t) == 32, "bb_sample_t is 32 bytes");

enum bb_state { BB_RECORDING = 0, BB_TRIGGERED = 1, BB_FROZEN = 2 };
enum bb_reason { BB_REASON_NONE = 0, BB_REASON_HEARTBEAT = 1, BB_REASON_PRODUCER_DIED = 2, BB_REASON_MANUAL = 3 };

typedef struct bb_hello {
    uint32_t deadline_us; /* trigger when this producer has not written for this long; 0: never */
    char name[BB_NAME_MAX];
} bb_hello_t;

typedef struct bb_status {
    uint32_t state;  /* enum bb_state */
    uint32_t reason; /* enum bb_reason */
    uint64_t cycles_per_sec;
    uint64_t now_cycles;
    uint64_t trigger_cycles;      /* when the trigger fired (0: not yet) */
    uint64_t culprit_last_cycles; /* the culprit's last sample before the trigger */
    uint32_t culprit;             /* producer id behind the trigger (0: none or manual) */
    uint32_t count;               /* samples in the window */
    uint32_t capacity;
    uint32_t dropped;             /* samples refused while frozen */
    uint32_t post_ms;             /* recording continues this long after a trigger */
    uint32_t producers;           /* declared producers connected now */
    uint64_t written;             /* samples accepted since the last reset */
    uint64_t culprit_end_cycles;  /* when the culprit's connection closed without a goodbye (0: not seen) */
    char culprit_name[BB_NAME_MAX];
} bb_status_t;

#if defined(__QNX__)
#include <devctl.h>
#define BB_DCMD_BASE 0x40
#define DCMD_BB_HELLO __DIOT(_DCMD_MISC, BB_DCMD_BASE + 0, bb_hello_t)
#define DCMD_BB_GOODBYE __DION(_DCMD_MISC, BB_DCMD_BASE + 1)
#define DCMD_BB_TRIGGER __DION(_DCMD_MISC, BB_DCMD_BASE + 2)
#define DCMD_BB_RESET __DION(_DCMD_MISC, BB_DCMD_BASE + 3)
#define DCMD_BB_STATUS __DIOF(_DCMD_MISC, BB_DCMD_BASE + 4, bb_status_t)
#endif

#endif
