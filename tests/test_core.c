/* Host unit tests for the recorder logic (src/core.c): cc -Iinclude -Isrc src/core.c tests/test_core.c */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "core.h"

static int failures, checks;
#define CHECK(cond)                                                              \
    do {                                                                         \
        ++checks;                                                                \
        if (!(cond)) {                                                           \
            ++failures;                                                          \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                        \
    } while (0)

enum { CPS = 1000000000u }; /* 1 GHz: 1 us = 1000 cycles */
static uint64_t ms(double v) { return (uint64_t)(v * 1e6); }

static bb_sample_t sample(uint32_t seq)
{
    bb_sample_t s;
    memset(&s, 0, sizeof s);
    s.seq = seq;
    s.v[0] = (float)seq;
    return s;
}

static void test_ring_keeps_the_newest_samples_oldest_first(void)
{
    bb_sample_t store[4], out[8];
    bb_core_t c;
    bb_init(&c, store, 4, CPS, 100);
    int const id = bb_add_producer(&c, "pump", 0, 0);
    for (uint32_t i = 0; i < 6; ++i) {
        bb_sample_t const s = sample(i);
        CHECK(bb_write(&c, id, &s, 1, ms(i)) == 1);
    }
    CHECK(c.count == 4);
    CHECK(bb_window(&c, 0, out, 8) == 4);
    for (uint32_t i = 0; i < 4; ++i) {
        CHECK(out[i].seq == i + 2);
        CHECK(out[i].producer == id);
        CHECK(out[i].t_cycles == ms(i + 2));
    }
    CHECK(bb_window(&c, 3, out, 8) == 1 && out[0].seq == 5);
    CHECK(bb_window(&c, 4, out, 8) == 0);
}

static void test_a_missed_heartbeat_triggers_then_freezes_after_the_post_window(void)
{
    bb_sample_t store[100];
    bb_core_t c;
    bb_init(&c, store, 100, CPS, 200);
    int const pump = bb_add_producer(&c, "pump", 5000, 0); /* 5 ms deadline */
    bb_sample_t const s = sample(1);
    bb_write(&c, pump, &s, 1, ms(10));
    bb_tick(&c, ms(14.9));
    CHECK(c.state == BB_RECORDING);
    bb_tick(&c, ms(15.1));
    CHECK(c.state == BB_TRIGGERED);
    CHECK(c.reason == BB_REASON_HEARTBEAT);
    CHECK(c.culprit == (uint32_t)pump);
    CHECK(strcmp(c.culprit_name, "pump") == 0);
    CHECK(c.culprit_last == ms(10));
    /* still recording during the post-trigger window, so the recovery is captured */
    CHECK(bb_write(&c, pump, &s, 1, ms(30)) == 1);
    bb_tick(&c, ms(215));
    CHECK(c.state == BB_TRIGGERED);
    bb_tick(&c, ms(215.2));
    CHECK(c.state == BB_FROZEN);
    CHECK(bb_write(&c, pump, &s, 1, ms(216)) == 0);
    CHECK(c.dropped == 1);
    CHECK(c.count == 2);
}

static void test_a_producer_that_dies_without_goodbye_triggers(void)
{
    bb_sample_t store[10];
    bb_core_t c;
    bb_init(&c, store, 10, CPS, 0); /* no post window: freeze at once */
    int const a = bb_add_producer(&c, "logger", 0, 0);
    bb_remove_producer(&c, a, 1, ms(1)); /* clean: said goodbye */
    CHECK(c.state == BB_RECORDING);
    int const pump = bb_add_producer(&c, "pump", 5000, ms(2));
    bb_sample_t const s = sample(7);
    bb_write(&c, pump, &s, 1, ms(3));
    bb_remove_producer(&c, pump, 0, ms(4)); /* the kernel closed it: the process died */
    CHECK(c.state == BB_FROZEN);
    CHECK(c.reason == BB_REASON_PRODUCER_DIED);
    CHECK(c.culprit == (uint32_t)pump);
    CHECK(c.culprit_last == ms(3));
    CHECK(c.culprit_end == ms(4));
    CHECK(strcmp(c.culprit_name, "pump") == 0);
}

static void test_a_crash_noticed_first_by_the_heartbeat_records_the_culprits_end(void)
{
    bb_sample_t store[10];
    bb_core_t c;
    bb_init(&c, store, 10, CPS, 200);
    int const pump = bb_add_producer(&c, "pump", 5000, 0);
    int const other = bb_add_producer(&c, "logger", 0, 0);
    bb_sample_t const s = sample(1);
    bb_write(&c, pump, &s, 1, ms(1));
    bb_tick(&c, ms(7)); /* the crashed loop stops writing: the heartbeat fires first */
    CHECK(c.state == BB_TRIGGERED && c.reason == BB_REASON_HEARTBEAT && c.culprit_end == 0);
    bb_remove_producer(&c, other, 0, ms(20)); /* another producer dying later is not the culprit */
    CHECK(c.culprit_end == 0);
    bb_remove_producer(&c, pump, 0, ms(48)); /* the process is torn down after its core dump */
    CHECK(c.reason == BB_REASON_HEARTBEAT && c.culprit_end == ms(48));
    bb_reset(&c, ms(300));
    CHECK(c.culprit_end == 0);
}

static void test_the_first_trigger_wins_until_reset(void)
{
    bb_sample_t store[10];
    bb_core_t c;
    bb_status_t st;
    bb_init(&c, store, 10, CPS, 50);
    int const pump = bb_add_producer(&c, "pump", 1000, 0);
    bb_trigger(&c, BB_REASON_MANUAL, 0, ms(1));
    bb_tick(&c, ms(5)); /* the pump is late too, but the manual trigger came first */
    CHECK(c.reason == BB_REASON_MANUAL);
    CHECK(c.culprit == 0);
    bb_tick(&c, ms(51));
    CHECK(c.state == BB_FROZEN);
    bb_reset(&c, ms(60));
    CHECK(c.state == BB_RECORDING && c.count == 0 && c.reason == BB_REASON_NONE);
    bb_tick(&c, ms(60.5)); /* reset gives every producer a fresh heartbeat period */
    CHECK(c.state == BB_RECORDING);
    bb_get_status(&c, &st, ms(61));
    CHECK(st.producers == 1 && st.capacity == 10 && st.post_ms == 50);
    (void)pump;
}

static void test_producers_without_a_deadline_are_never_late_and_the_table_is_bounded(void)
{
    bb_sample_t store[4];
    bb_core_t c;
    bb_init(&c, store, 4, CPS, 10);
    int last = 0;
    for (int i = 0; i < BB_MAX_PRODUCERS; ++i) {
        last = bb_add_producer(&c, "p", 0, 0);
    }
    CHECK(last == BB_MAX_PRODUCERS);
    CHECK(bb_add_producer(&c, "one too many", 0, 0) == -1);
    bb_tick(&c, ms(100000));
    CHECK(c.state == BB_RECORDING);
}

static void test_long_names_are_cut(void)
{
    bb_sample_t store[1];
    bb_core_t c;
    bb_init(&c, store, 1, CPS, 0);
    int const id = bb_add_producer(&c, "a-name-much-longer-than-the-limit-allows", 0, 0);
    CHECK(strlen(c.prod[id].name) == BB_NAME_MAX - 1);
}

int main(void)
{
    test_ring_keeps_the_newest_samples_oldest_first();
    test_a_missed_heartbeat_triggers_then_freezes_after_the_post_window();
    test_a_producer_that_dies_without_goodbye_triggers();
    test_a_crash_noticed_first_by_the_heartbeat_records_the_culprits_end();
    test_the_first_trigger_wins_until_reset();
    test_producers_without_a_deadline_are_never_late_and_the_table_is_bounded();
    test_long_names_are_cut();
    printf("%d checks, %d failed\n", checks, failures);
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
