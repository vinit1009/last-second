/* core.c: ring buffer, producers, heartbeat checks and the trigger -> freeze state machine. */
#include <string.h>
#include "core.h"

static void copy_name(char *dst, const char *src)
{
    size_t i = 0;
    if (src != NULL) {
        for (; i + 1 < BB_NAME_MAX && src[i] != '\0'; ++i) {
            dst[i] = src[i];
        }
    }
    dst[i] = '\0';
}

void bb_init(bb_core_t *c, bb_sample_t *storage, uint32_t capacity, uint64_t cycles_per_sec, uint32_t post_ms)
{
    memset(c, 0, sizeof *c);
    c->buf = storage;
    c->capacity = capacity;
    c->cps = cycles_per_sec;
    c->post_ms = post_ms;
    c->post_cycles = cycles_per_sec * post_ms / 1000u;
}

int bb_add_producer(bb_core_t *c, const char *name, uint32_t deadline_us, uint64_t now)
{
    for (int id = 1; id <= BB_MAX_PRODUCERS; ++id) {
        bb_producer_t *p = &c->prod[id];
        if (!p->used) {
            memset(p, 0, sizeof *p);
            p->used = 1;
            p->deadline_cycles = c->cps * deadline_us / 1000000u;
            p->last_cycles = now;
            copy_name(p->name, name);
            return id;
        }
    }
    return -1;
}

void bb_trigger(bb_core_t *c, uint32_t reason, int culprit, uint64_t now)
{
    if (c->state != BB_RECORDING) {
        return;
    }
    c->state = BB_TRIGGERED;
    c->reason = reason;
    c->trigger_cycles = now;
    c->culprit = 0;
    c->culprit_last = 0;
    c->culprit_end = 0;
    c->culprit_name[0] = '\0';
    if (culprit > 0 && culprit <= BB_MAX_PRODUCERS && c->prod[culprit].used) {
        c->culprit = (uint32_t)culprit;
        c->culprit_last = c->prod[culprit].last_cycles;
        copy_name(c->culprit_name, c->prod[culprit].name);
    }
    if (c->post_cycles == 0) {
        c->state = BB_FROZEN;
    }
}

void bb_remove_producer(bb_core_t *c, int id, int clean, uint64_t now)
{
    if (id <= 0 || id > BB_MAX_PRODUCERS || !c->prod[id].used) {
        return;
    }
    if (!clean) {
        if (c->state == BB_RECORDING) {
            bb_trigger(c, BB_REASON_PRODUCER_DIED, id, now);
            c->culprit_end = now;
        } else if (c->culprit == (uint32_t)id && c->culprit_end == 0) {
            c->culprit_end = now;
        }
    }
    c->prod[id].used = 0;
}

uint32_t bb_write(bb_core_t *c, int id, const bb_sample_t *s, uint32_t n, uint64_t now)
{
    if (id > 0 && id <= BB_MAX_PRODUCERS && c->prod[id].used) {
        c->prod[id].last_cycles = now;
    }
    if (c->state == BB_FROZEN || c->capacity == 0) {
        c->dropped += n;
        return 0;
    }
    for (uint32_t i = 0; i < n; ++i) {
        bb_sample_t *slot = &c->buf[c->head];
        *slot = s[i];
        slot->t_cycles = now;
        slot->producer = (uint16_t)(id > 0 ? id : 0);
        c->head = (c->head + 1) % c->capacity;
        if (c->count < c->capacity) {
            ++c->count;
        }
    }
    c->written += n;
    if (id > 0 && id <= BB_MAX_PRODUCERS && c->prod[id].used) {
        c->prod[id].written += n;
    }
    return n;
}

void bb_tick(bb_core_t *c, uint64_t now)
{
    if (c->state == BB_RECORDING) {
        for (int id = 1; id <= BB_MAX_PRODUCERS; ++id) {
            bb_producer_t const *p = &c->prod[id];
            if (p->used && p->deadline_cycles != 0 && now > p->last_cycles && now - p->last_cycles > p->deadline_cycles) {
                bb_trigger(c, BB_REASON_HEARTBEAT, id, now);
                break;
            }
        }
    } else if (c->state == BB_TRIGGERED && now - c->trigger_cycles >= c->post_cycles) {
        c->state = BB_FROZEN;
    }
}

uint32_t bb_window(const bb_core_t *c, uint32_t index, bb_sample_t *out, uint32_t n)
{
    if (index >= c->count) {
        return 0;
    }
    if (n > c->count - index) {
        n = c->count - index;
    }
    uint32_t const oldest = (c->count < c->capacity) ? 0 : c->head;
    for (uint32_t i = 0; i < n; ++i) {
        out[i] = c->buf[(oldest + index + i) % c->capacity];
    }
    return n;
}

void bb_reset(bb_core_t *c, uint64_t now)
{
    c->head = 0;
    c->count = 0;
    c->state = BB_RECORDING;
    c->reason = BB_REASON_NONE;
    c->trigger_cycles = 0;
    c->culprit = 0;
    c->culprit_last = 0;
    c->culprit_end = 0;
    c->culprit_name[0] = '\0';
    c->dropped = 0;
    c->written = 0;
    for (int id = 1; id <= BB_MAX_PRODUCERS; ++id) {
        if (c->prod[id].used) {
            c->prod[id].last_cycles = now; /* a fresh heartbeat period for everyone */
        }
    }
}

void bb_get_status(const bb_core_t *c, bb_status_t *st, uint64_t now)
{
    memset(st, 0, sizeof *st);
    st->state = c->state;
    st->reason = c->reason;
    st->cycles_per_sec = c->cps;
    st->now_cycles = now;
    st->trigger_cycles = c->trigger_cycles;
    st->culprit_last_cycles = c->culprit_last;
    st->culprit_end_cycles = c->culprit_end;
    st->culprit = c->culprit;
    st->count = c->count;
    st->capacity = c->capacity;
    st->dropped = c->dropped;
    st->post_ms = c->post_ms;
    st->written = c->written;
    for (int id = 1; id <= BB_MAX_PRODUCERS; ++id) {
        st->producers += c->prod[id].used ? 1u : 0u;
    }
    copy_name(st->culprit_name, c->culprit_name);
}

const char *bb_reason_name(uint32_t reason)
{
    switch (reason) {
    case BB_REASON_HEARTBEAT: return "missed heartbeat";
    case BB_REASON_PRODUCER_DIED: return "producer died";
    case BB_REASON_MANUAL: return "manual trigger";
    default: return "none";
    }
}

const char *bb_state_name(uint32_t state)
{
    switch (state) {
    case BB_TRIGGERED: return "triggered";
    case BB_FROZEN: return "frozen";
    default: return "recording";
    }
}
