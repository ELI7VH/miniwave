/* miniwave keyseq test suite */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include "keyseq.h"

static int test_pass = 0, test_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { test_pass++; printf("  PASS: %s\n", msg); } \
    else { test_fail++; printf("  FAIL: %s\n", msg); } \
} while(0)

/* Mock MIDI handler — records notes */
#define MAX_EVENTS 256
static struct { uint8_t status, d1, d2; } events[MAX_EVENTS];
static int num_events = 0;

static void mock_midi(void *state, uint8_t status, uint8_t d1, uint8_t d2) {
    (void)state;
    if (num_events < MAX_EVENTS) {
        events[num_events].status = status;
        events[num_events].d1 = d1;
        events[num_events].d2 = d2;
        num_events++;
    }
}

static void reset_events(void) { num_events = 0; }

/* Simulate N seconds of ticks at given sample rate */
static void sim_ticks(KeySeq *ks, float seconds, int sr) {
    float dt = 1.0f / (float)sr;
    int samples = (int)(seconds * sr);
    for (int i = 0; i < samples; i++) {
        keyseq_tick(ks, dt);
    }
}

/* ══════════════════════════════════════════════════════════════════════ */

static void test_expr_basics(void) {
    printf("\n== Expression basics ==\n");
    KeySeqExpr e; KeySeqCtx ctx = {0};

    ke_compile(&e, "1+2");
    CHECK(ke_eval(&e, &ctx) == 3.0f, "1+2 = 3");

    ke_compile(&e, "10-3*2");
    CHECK(ke_eval(&e, &ctx) == 4.0f, "10-3*2 = 4 (precedence)");

    ke_compile(&e, "(10-3)*2");
    CHECK(ke_eval(&e, &ctx) == 14.0f, "(10-3)*2 = 14");

    ke_compile(&e, "7%3");
    CHECK(ke_eval(&e, &ctx) == 1.0f, "7%3 = 1");

    ke_compile(&e, "5>3");
    CHECK(ke_eval(&e, &ctx) == 1.0f, "5>3 = 1");

    ke_compile(&e, "3>5");
    CHECK(ke_eval(&e, &ctx) == 0.0f, "3>5 = 0");

    ke_compile(&e, "3<=3");
    CHECK(ke_eval(&e, &ctx) == 1.0f, "3<=3 = 1");

    ke_compile(&e, "3>=4");
    CHECK(ke_eval(&e, &ctx) == 0.0f, "3>=4 = 0");

    ke_compile(&e, "abs(-5)");
    CHECK(ke_eval(&e, &ctx) == 5.0f, "abs(-5) = 5");

    ke_compile(&e, "sin(0)");
    CHECK(fabsf(ke_eval(&e, &ctx)) < 0.001f, "sin(0) ≈ 0");

    ke_compile(&e, "cos(0)");
    CHECK(fabsf(ke_eval(&e, &ctx) - 1.0f) < 0.001f, "cos(0) ≈ 1");

    ke_compile(&e, "if(1,10,20)");
    CHECK(ke_eval(&e, &ctx) == 10.0f, "if(1,10,20) = 10");

    ke_compile(&e, "if(0,10,20)");
    CHECK(ke_eval(&e, &ctx) == 20.0f, "if(0,10,20) = 20");

    ke_compile(&e, "pi");
    CHECK(fabsf(ke_eval(&e, &ctx) - 3.14159f) < 0.001f, "pi ≈ 3.14159");
}

static void test_expr_variables(void) {
    printf("\n== Expression variables ==\n");
    KeySeqExpr e;
    KeySeqCtx ctx = { .n=60, .v=0.8f, .t=0.125f, .g=0.1f, .i=5, .root=48, .rv=1.0f, .time=1.5f, .bu=0.3f, .gate=1.0f, .dt=0.0000208f };

    ke_compile(&e, "n");
    CHECK(ke_eval(&e, &ctx) == 60.0f, "n = 60");

    ke_compile(&e, "v");
    CHECK(fabsf(ke_eval(&e, &ctx) - 0.8f) < 0.001f, "v = 0.8");

    ke_compile(&e, "root");
    CHECK(ke_eval(&e, &ctx) == 48.0f, "root = 48");

    ke_compile(&e, "i");
    CHECK(ke_eval(&e, &ctx) == 5.0f, "i = 5");

    ke_compile(&e, "time");
    CHECK(fabsf(ke_eval(&e, &ctx) - 1.5f) < 0.001f, "time = 1.5");

    ke_compile(&e, "gate");
    CHECK(ke_eval(&e, &ctx) == 1.0f, "gate = 1");

    ke_compile(&e, "dt");
    CHECK(ke_eval(&e, &ctx) > 0.0f, "dt > 0");

    ke_compile(&e, "n+1");
    CHECK(ke_eval(&e, &ctx) == 61.0f, "n+1 = 61");

    ke_compile(&e, "v*0.6");
    CHECK(fabsf(ke_eval(&e, &ctx) - 0.48f) < 0.001f, "v*0.6 = 0.48");

    ke_compile(&e, "root+i*7%12");
    CHECK(ke_eval(&e, &ctx) == 48.0f + (float)(5*7%12), "root+i*7%12");
}

static void test_noise(void) {
    printf("\n== Noise ==\n");
    KeySeqExpr e; KeySeqCtx ctx = {0};

    ke_compile(&e, "noise(0)");
    float v = ke_eval(&e, &ctx);
    CHECK(v != 0.0f, "noise(0) != 0 (diagonal fix)");

    ke_compile(&e, "noise(1.5,2.3)");
    v = ke_eval(&e, &ctx);
    CHECK(v != 0.0f, "noise(1.5,2.3) != 0");

    ke_compile(&e, "noise(1.1,2.2,3.3)");
    v = ke_eval(&e, &ctx);
    CHECK(v != 0.0f, "noise(1.1,2.2,3.3) != 0");

    /* Deterministic */
    ke_compile(&e, "noise(3.7)");
    float a = ke_eval(&e, &ctx);
    float b = ke_eval(&e, &ctx);
    CHECK(a == b, "noise deterministic");

    /* Varies */
    ke_compile(&e, "noise(time)");
    int varies = 0;
    float prev = 0;
    for (int i = 0; i < 10; i++) {
        ctx.time = (float)i * 0.3f;
        v = ke_eval(&e, &ctx);
        if (i > 0 && v != prev) varies = 1;
        prev = v;
    }
    CHECK(varies, "noise(time) varies");

    /* Range */
    float mn = 999, mx = -999;
    for (int i = 0; i < 1000; i++) {
        ctx.time = (float)i * 0.01f;
        v = ke_eval(&e, &ctx);
        if (v < mn) mn = v; if (v > mx) mx = v;
    }
    CHECK(mn < 0.3f && mx > 0.5f, "noise range within [0, 1]");
}

static void test_rand(void) {
    printf("\n== Rand ==\n");
    KeySeqExpr e; KeySeqCtx ctx = {0};
    ke_compile(&e, "rand()");

    ke_rand_state = 42;
    float r1 = ke_eval(&e, &ctx);
    float r2 = ke_eval(&e, &ctx);
    CHECK(r1 != r2, "rand() varies between calls");

    ke_rand_state = 42;
    float r1b = ke_eval(&e, &ctx);
    CHECK(r1 == r1b, "rand() deterministic from seed");

    CHECK(r1 >= 0.0f && r1 <= 1.0f, "rand() in [0,1]");
}

static void test_dsl_parse(void) {
    printf("\n== DSL parsing ==\n");
    KeySeq ks;
    keyseq_init(&ks);
    ks.bpm = 120.0f;

    keyseq_parse(&ks, "t0.25; g0.2; gated; algo; n:n+1; v:v-0.03; end:v<=0");
    CHECK(ks.algo_mode == 1, "algo mode on");
    CHECK(fabsf(ks.step_beats - 0.25f) < 0.001f, "step = 0.25");
    CHECK(fabsf(ks.gate_beats - 0.2f) < 0.001f, "gate = 0.2");
    CHECK(ks.gated == 1, "gated on");
    CHECK(ks.expr_n.valid, "n expr valid");
    CHECK(ks.expr_v.valid, "v expr valid");
    CHECK(ks.expr_end.valid, "end expr valid");
    CHECK(ks.enabled == 1, "enabled");

    keyseq_parse(&ks, "t0.125; loop; 0,12,7,12; v1,0.7,0.5,0.3");
    CHECK(ks.algo_mode == 0, "offsets mode");
    CHECK(ks.num_steps == 4, "4 offset steps");
    CHECK(ks.offsets[1] == 12, "offset[1] = 12");
    CHECK(ks.loop == 1, "loop on");
    CHECK(fabsf(ks.levels[2] - 0.5f) < 0.001f, "level[2] = 0.5");

    keyseq_parse(&ks, "t0.1; gated; algo; n:n+1; v:v*0.6; seed:42; end:v<0.2; frame:sin(time*6)*50");
    CHECK(ks.expr_seed.valid, "seed expr valid");
    CHECK(ks.expr_frame.valid, "frame expr valid");
    CHECK(ks.expr_end.valid, "end expr valid (v<0.2)");

    keyseq_parse(&ks, "t0.1; algo; n:n; v:v; filter_cutoff:noise(time)*5000+5000");
    CHECK(ks.num_params == 1, "1 param parsed");
    CHECK(strcmp(ks.params[0].name, "filter_cutoff") == 0, "param name = filter_cutoff");
    CHECK(ks.params[0].expr.valid, "param expr valid");
}

static void test_end_condition_runtime(void) {
    printf("\n== End condition runtime ==\n");
    KeySeq ks;
    keyseq_init(&ks);
    keyseq_bind(&ks, NULL, mock_midi, NULL, 0);
    ks.bpm = 120.0f;

    /* v:v*0.6 with end:v<0.2 should stop after a few steps */
    keyseq_parse(&ks, "t0.3; gated; algo; n:n+1; v:v*0.6; end:v<0.2");

    reset_events();
    keyseq_note_on(&ks, 60, 100);
    CHECK(ks.playing == 1, "playing after note_on");

    /* Step 0 fires immediately (C4, v=0.787) */
    CHECK(num_events == 1, "1 event after note_on");
    /* Step 0 = root note (unmodified) */
    CHECK(events[0].d1 == 60, "first note = 60 (root)");

    /* Simulate enough time for 10 steps (should stop before that) */
    float step_sec = 0.3f * 60.0f / 120.0f; /* 0.15s */
    int steps_played = 1; /* step 0 already played */

    for (int s = 0; s < 10; s++) {
        sim_ticks(&ks, step_sec + 0.001f, 48000);
        if (!ks.playing) break;
        steps_played++;
    }

    printf("  steps played: %d, playing: %d, algo_v: %f\n",
           steps_played, ks.playing, ks.algo_v);

    /* v sequence: 0.787, 0.472, 0.283, 0.170 → end at step 3 (v=0.170 < 0.2) */
    CHECK(!ks.playing, "stopped playing");
    CHECK(steps_played <= 5, "stopped within 5 steps");

    /* Count note-on events (0x90) */
    int note_ons = 0;
    for (int i = 0; i < num_events; i++) {
        if ((events[i].status & 0xF0) == 0x90 && events[i].d2 > 0) note_ons++;
    }
    printf("  total note-ons: %d\n", note_ons);
    CHECK(note_ons <= 5, "reasonable number of note-ons");
}

static void test_end_condition_v_less_zero(void) {
    printf("\n== End condition v<0 ==\n");
    KeySeq ks;
    keyseq_init(&ks);
    keyseq_bind(&ks, NULL, mock_midi, NULL, 0);
    ks.bpm = 120.0f;

    keyseq_parse(&ks, "t0.125; gated; algo; n:n+1; v:v-0.03; end:v<0");

    reset_events();
    keyseq_note_on(&ks, 60, 100);

    float step_sec = 0.125f * 60.0f / 120.0f;
    int steps = 0;
    for (int s = 0; s < 100; s++) {
        sim_ticks(&ks, step_sec + 0.001f, 48000);
        if (!ks.playing) break;
        steps++;
    }

    printf("  steps: %d, playing: %d, algo_v: %f\n", steps, ks.playing, ks.algo_v);
    CHECK(!ks.playing, "stopped playing (v<0)");

    /* v starts at ~0.787, decreases by 0.03 each step → ~26 steps to reach 0 */
    CHECK(steps > 20 && steps < 35, "reasonable step count for v-0.03 end:v<0");
}

static void test_offsets_mode(void) {
    printf("\n== Offsets mode ==\n");
    KeySeq ks;
    keyseq_init(&ks);
    keyseq_bind(&ks, NULL, mock_midi, NULL, 0);
    ks.bpm = 120.0f;

    keyseq_parse(&ks, "t0.125; 0,12,7; v1,0.8,0.6");

    reset_events();
    keyseq_note_on(&ks, 60, 100);

    float step_sec = 0.125f * 60.0f / 120.0f;
    /* Play 3 steps (one-shot, no loop) */
    for (int s = 0; s < 5; s++) {
        sim_ticks(&ks, step_sec + 0.001f, 48000);
    }

    int note_ons = 0;
    for (int i = 0; i < num_events; i++) {
        if ((events[i].status & 0xF0) == 0x90 && events[i].d2 > 0) {
            printf("  note_on: %d vel=%d\n", events[i].d1, events[i].d2);
            note_ons++;
        }
    }
    CHECK(note_ons == 3, "3 notes in one-shot offsets");
    CHECK(!ks.playing, "stopped after 3 offsets (no loop)");
}

static void test_dsl_no_spaces(void) {
    printf("\n== DSL space-stripped (as sent from UI) ==\n");
    KeySeq ks;
    keyseq_init(&ks);
    keyseq_bind(&ks, NULL, mock_midi, NULL, 0);
    ks.bpm = 120.0f;

    /* This is what buildDSL() now produces (spaces stripped) */
    keyseq_parse(&ks, "t0.3; gated; algo; n:n + noise(time * 5) * 6; v:v*0.6; g:1 - noise(time * 0.2) * 0.3; end:v<0.1");
    CHECK(ks.expr_n.valid, "n expr valid");
    CHECK(ks.expr_v.valid, "v expr valid");
    CHECK(ks.expr_g.valid, "g expr valid");
    CHECK(ks.expr_end.valid, "end expr valid");

    reset_events();
    keyseq_note_on(&ks, 60, 100);

    float step_sec = 0.3f * 60.0f / 120.0f;
    int steps = 0;
    for (int s = 0; s < 50; s++) {
        sim_ticks(&ks, step_sec + 0.001f, 48000);
        if (!ks.playing) break;
        steps++;
    }
    printf("  steps: %d, playing: %d, algo_v: %f\n", steps, ks.playing, ks.algo_v);
    CHECK(!ks.playing, "sequence terminated");
    CHECK(steps <= 10, "terminated within 10 steps");

    int note_ons = 0;
    for (int i = 0; i < num_events; i++)
        if ((events[i].status & 0xF0) == 0x90 && events[i].d2 > 0) note_ons++;
    printf("  note_ons: %d\n", note_ons);
    CHECK(note_ons <= 10, "reasonable note count");
}

int main(void) {
    test_expr_basics();
    test_expr_variables();
    test_noise();
    test_rand();
    test_dsl_parse();
    test_end_condition_runtime();
    test_end_condition_v_less_zero();
    test_offsets_mode();
    test_dsl_no_spaces();

    printf("\n========================================\n");
    printf("Results: %d passed, %d failed\n", test_pass, test_fail);
    return test_fail > 0 ? 1 : 0;
}
