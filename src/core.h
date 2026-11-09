/*
 * core.h: the recorder's logic, free of QNX calls so it can be unit-tested on any host.
 * blackboxd (the resource manager) calls these from its single dispatch thread; times are cycle
 * counts from ClockCycles().
 */
#ifndef LAST_SECOND_CORE_H
#define LAST_SECOND_CORE_H

#include <stdint.h>
#include "blackbox.h"

typedef struct bb_producer {
    int used;
    uint64_t deadline_cycles; /* 0: no heartbeat check */
    uint64_t last_cycles;     /* last sample, or the hello if none yet */
    uint64_t written;
    char name[BB_NAME_MAX];
} bb_producer_t;

typedef struct bb_core {
    bb_sample_t *buf;
    uint32_t capacity;
    uint32_t head;  /* next slot to write */
    uint32_t count; /* samples held, at most capacity */
    uint32_t state;
    uint32_t reason;
    uint64_t cps;
    uint64_t post_cycles;
    uint32_t post_ms;
    uint64_t trigger_cycles;
    uint64_t culprit_last;
    uint64_t culprit_end; /* the culprit closed without a goodbye (crash or kill) at this time */
    uint32_t culprit;
    char culprit_name[BB_NAME_MAX];
    uint32_t dropped;
    uint64_t written;
    bb_producer_t prod[BB_MAX_PRODUCERS + 1]; /* index = producer id; 0 unused */
} bb_core_t;

void bb_init(bb_core_t *c, bb_sample_t *storage, uint32_t capacity, uint64_t cycles_per_sec, uint32_t post_ms);

/* Declares a producer; returns its id (1..BB_MAX_PRODUCERS), or -1 when the table is full. */
int bb_add_producer(bb_core_t *c, const char *name, uint32_t deadline_us, uint64_t now);

/* Removes a producer. clean = 0 means it went away without saying goodbye (its process died): that
 * is a trigger, or, if the producer already caused the trigger (a crashed loop also misses its
 * heartbeat), it is recorded as the culprit's end. */
void bb_remove_producer(bb_core_t *c, int id, int clean, uint64_t now);

/* Stores n samples from producer id (0: undeclared writer); returns how many were kept. */
uint32_t bb_write(bb_core_t *c, int id, const bb_sample_t *s, uint32_t n, uint64_t now);

/* The first trigger wins; later ones are ignored until bb_reset(). */
void bb_trigger(bb_core_t *c, uint32_t reason, int culprit, uint64_t now);

/* Called periodically: checks heartbeats while recording, and freezes once the post-trigger
 * window has passed. */
void bb_tick(bb_core_t *c, uint64_t now);

/* Copies up to n samples of the window, oldest first, starting at index; returns the number copied. */
uint32_t bb_window(const bb_core_t *c, uint32_t index, bb_sample_t *out, uint32_t n);

/* Clears the window and starts recording again; declared producers stay declared. */
void bb_reset(bb_core_t *c, uint64_t now);

void bb_get_status(const bb_core_t *c, bb_status_t *st, uint64_t now);

const char *bb_reason_name(uint32_t reason);
const char *bb_state_name(uint32_t state);

#endif
