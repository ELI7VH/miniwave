/* miniwave — key sequence (arpeggiator chain)
 *
 * Two input modes: offsets (static pattern) or algorithm (per-step expressions).
 * Plus per-frame note modulation (noteAlgo) for pitch/param animation.
 *
 * Expression language:
 *   Vars:  n v t g i root rv time bu gate
 *   Ops:   + - * / % > < ( )
 *   Funcs: if(cond,a,b) sin(x) cos(x) abs(x) rand() noise(x) noise(x,y) noise(x,y,z)
 *
 * DSL:
 *   "t0.125 g0.9 gated algo n:n+1 v:v-0.03"
 *   "t0.125 gated algo n:if(i%2,root+5,root-5) v:v-0.01"
 *   "t0.125 gated algo n:root+sin(i*0.5)*12 v:v-0.02"
 *
 *   frame:<expr>  — per-sample cents offset on playing notes (noteAlgo)
 *   "frame:sin(time*6)*50"  — vibrato ±50 cents at 6Hz
 */

#ifndef MINIWAVE_KEYSEQ_H
#define MINIWAVE_KEYSEQ_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

/* FastNoiseLite — single-header noise library */
#define FNL_IMPL
#include "FastNoiseLite.h"

#define KEYSEQ_MAX_STEPS 32
#define KE_EPSILON 0.00001f
#define KEYSEQ_EXPR_MAX  64

/* ══════════════════════════════════════════════════════════════════════════
 *  Noise via FastNoiseLite
 * ══════════════════════════════════════════════════════════════════════════ */

static fnl_state g_fnl;
static int g_fnl_init = 0;

static void ke_ensure_fnl(void) {
    if (!g_fnl_init) {
        g_fnl = fnlCreateState();
        g_fnl.noise_type = FNL_NOISE_PERLIN;
        g_fnl.frequency = 1.0f;
        g_fnl_init = 1;
    }
}

static void ke_fnl_seed(int seed) {
    ke_ensure_fnl();
    g_fnl.seed = seed;
}

static float ke_noise2d(float x, float y) {
    ke_ensure_fnl();
    return fnlGetNoise2D(&g_fnl, x, y);
}

static float ke_noise3d(float x, float y, float z) {
    ke_ensure_fnl();
    return fnlGetNoise3D(&g_fnl, x, y, z);
}

/* ══════════════════════════════════════════════════════════════════════════
 *  Expression evaluator
 * ══════════════════════════════════════════════════════════════════════════ */

enum {
    KE_NUM = 0,
    /* variables */
    KE_VAR_N, KE_VAR_V, KE_VAR_T, KE_VAR_G,
    KE_VAR_I, KE_VAR_ROOT, KE_VAR_RV,
    KE_VAR_TIME, KE_VAR_BU, KE_VAR_GATE, KE_VAR_HELD, KE_VAR_DT,
    /* operators */
    KE_ADD, KE_SUB, KE_MUL, KE_DIV, KE_MOD, KE_GT, KE_LT, KE_GTE, KE_LTE,
    /* grouping */
    KE_LPAREN, KE_RPAREN, KE_COMMA,
    /* functions (1 arg) */
    KE_ABS, KE_SIN, KE_COS,
    /* functions (0 args) */
    KE_RAND,
    /* functions (multi-arg, resolved at eval) */
    KE_NOISE,   /* 1-3 args, 0-1 */
    KE_NOISEB,  /* 1-3 args, -1 to 1 (bipolar/raw) */
    KE_IF,      /* 3 args: cond, then, else */
    /* GLSL-style functions */
    KE_FLOOR, KE_CEIL, KE_MIN, KE_MAX, KE_CLAMP, KE_STEP, KE_SMOOTHSTEP,
    KE_END
};

typedef struct { uint8_t type; float value; } KETok;
typedef struct { KETok ops[KEYSEQ_EXPR_MAX]; int len; int valid; } KeySeqExpr;

typedef struct {
    float n, v, t, g, i, root, rv;
    float time;  /* seconds since note start */
    float bu;    /* normalized beat position (0-1) within step */
    float gate;  /* normalized position within gate (0-1, 0=start, 1=gate end) */
    float held;  /* 1.0 if root key held, 0.0 if released */
    float dt;    /* seconds per sample (1/sampleRate) */
    float seed;  /* noise seed (int cast for FNL, float for legacy) */
    /* Per-evaluation RNG state — if non-NULL, used instead of globals */
    uint32_t *local_rand;
    fnl_state *local_fnl;
} KeySeqCtx;

/* ── PRNG for rand() — deterministic from context ── */
static uint32_t ke_rand_state = 1;
static float ke_randf(void) {
    ke_rand_state ^= ke_rand_state << 13;
    ke_rand_state ^= ke_rand_state >> 17;
    ke_rand_state ^= ke_rand_state << 5;
    return (float)(ke_rand_state & 0x7FFFFFFF) / (float)0x7FFFFFFF;
}

/* ── Tokenizer ── */
static int ke_is_alnum(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'; }

static int ke_tokenize(const char *s, KETok *out, int max) {
    int n = 0;
    while (*s && n < max - 1) {
        while (*s == ' ') s++;
        if (!*s) break;

        /* operators */
        if (*s == '+') { out[n++].type = KE_ADD; s++; }
        else if (*s == '*') { out[n++].type = KE_MUL; s++; }
        else if (*s == '/') { out[n++].type = KE_DIV; s++; }
        else if (*s == '%') { out[n++].type = KE_MOD; s++; }
        else if (*s == '>' && s[1] == '=') { out[n++].type = KE_GTE; s += 2; }
        else if (*s == '<' && s[1] == '=') { out[n++].type = KE_LTE; s += 2; }
        else if (*s == '>') { out[n++].type = KE_GT; s++; }
        else if (*s == '<') { out[n++].type = KE_LT; s++; }
        else if (*s == '(') { out[n++].type = KE_LPAREN; s++; }
        else if (*s == ')') { out[n++].type = KE_RPAREN; s++; }
        else if (*s == ',') { out[n++].type = KE_COMMA; s++; }
        /* ternary ? : → convert to if() internally */
        else if (*s == '?') { out[n++].type = KE_LPAREN; s++; /* treat as sentinel below */ }
        else if (*s == ':') { out[n++].type = KE_COMMA; s++; }
        /* subtraction vs negative */
        else if (*s == '-') {
            int is_binary = n > 0 && (out[n-1].type == KE_NUM ||
                (out[n-1].type >= KE_VAR_N && out[n-1].type <= KE_VAR_DT) ||
                out[n-1].type == KE_RPAREN);
            if (is_binary) { out[n++].type = KE_SUB; s++; }
            else {
                char *end;
                float val = (float)strtod(s, &end);
                out[n].type = KE_NUM; out[n].value = val; n++;
                s = end;
            }
        }
        /* numbers */
        else if ((*s >= '0' && *s <= '9') || *s == '.') {
            char *end;
            float val = (float)strtod(s, &end);
            out[n].type = KE_NUM; out[n].value = val; n++;
            s = end;
        }
        /* keywords / variables */
        else if (*s >= 'a' && *s <= 'z') {
            char word[16]; int wl = 0;
            while (ke_is_alnum(*s) && wl < 15) word[wl++] = *s++;
            word[wl] = '\0';

            if (strcmp(word, "n") == 0)       out[n++].type = KE_VAR_N;
            else if (strcmp(word, "v") == 0)  out[n++].type = KE_VAR_V;
            else if (strcmp(word, "t") == 0)  out[n++].type = KE_VAR_T;
            else if (strcmp(word, "g") == 0)  out[n++].type = KE_VAR_G;
            else if (strcmp(word, "i") == 0)  out[n++].type = KE_VAR_I;
            else if (strcmp(word, "root") == 0) out[n++].type = KE_VAR_ROOT;
            else if (strcmp(word, "rv") == 0) out[n++].type = KE_VAR_RV;
            else if (strcmp(word, "time") == 0) out[n++].type = KE_VAR_TIME;
            else if (strcmp(word, "bu") == 0) out[n++].type = KE_VAR_BU;
            else if (strcmp(word, "gate") == 0) out[n++].type = KE_VAR_GATE;
            else if (strcmp(word, "held") == 0) out[n++].type = KE_VAR_HELD;
            else if (strcmp(word, "dt") == 0)   out[n++].type = KE_VAR_DT;
            else if (strcmp(word, "abs") == 0)   out[n++].type = KE_ABS;
            else if (strcmp(word, "sin") == 0)   out[n++].type = KE_SIN;
            else if (strcmp(word, "cos") == 0)   out[n++].type = KE_COS;
            else if (strcmp(word, "rand") == 0)  out[n++].type = KE_RAND;
            else if (strcmp(word, "noiseb") == 0) out[n++].type = KE_NOISEB;
            else if (strcmp(word, "noise") == 0) out[n++].type = KE_NOISE;
            else if (strcmp(word, "if") == 0)    out[n++].type = KE_IF;
            else if (strcmp(word, "floor") == 0) out[n++].type = KE_FLOOR;
            else if (strcmp(word, "ceil") == 0)  out[n++].type = KE_CEIL;
            else if (strcmp(word, "min") == 0)   out[n++].type = KE_MIN;
            else if (strcmp(word, "max") == 0)   out[n++].type = KE_MAX;
            else if (strcmp(word, "clamp") == 0) out[n++].type = KE_CLAMP;
            else if (strcmp(word, "step") == 0)  out[n++].type = KE_STEP;
            else if (strcmp(word, "smoothstep") == 0) out[n++].type = KE_SMOOTHSTEP;
            else if (strcmp(word, "pi") == 0)    { out[n].type = KE_NUM; out[n].value = 3.14159265f; n++; }
            else if (strcmp(word, "tau") == 0)   { out[n].type = KE_NUM; out[n].value = 6.28318530f; n++; }
            else if (strcmp(word, "eps") == 0)   { out[n].type = KE_NUM; out[n].value = KE_EPSILON; n++; }
            /* skip unknown */
        }
        else { s++; }
    }
    out[n].type = KE_END;
    return n;
}

/* ── Shunting-yard ── */
static int ke_precedence(uint8_t t) {
    if (t == KE_GT || t == KE_LT || t == KE_GTE || t == KE_LTE) return 0;
    if (t == KE_ADD || t == KE_SUB) return 1;
    if (t == KE_MUL || t == KE_DIV || t == KE_MOD) return 2;
    return -1;
}
static int ke_is_op(uint8_t t) { return t >= KE_ADD && t <= KE_LTE; }
static int ke_is_func(uint8_t t) { return t >= KE_ABS && t <= KE_SMOOTHSTEP; }

static void ke_compile(KeySeqExpr *expr, const char *src) {
    KETok infix[KEYSEQ_EXPR_MAX];
    int n = ke_tokenize(src, infix, KEYSEQ_EXPR_MAX);
    if (n <= 0) { expr->valid = 0; return; }

    KETok stack[KEYSEQ_EXPR_MAX];
    int sp = 0;
    /* Track arg counts for multi-arg functions */
    int arg_counts[KEYSEQ_EXPR_MAX];
    int acp = 0;

    expr->len = 0;

    for (int i = 0; i < n; i++) {
        KETok *tok = &infix[i];

        if (tok->type == KE_NUM || (tok->type >= KE_VAR_N && tok->type <= KE_VAR_DT)) {
            expr->ops[expr->len++] = *tok;
        }
        else if (ke_is_func(tok->type)) {
            stack[sp++] = *tok;
            arg_counts[acp++] = 1; /* at least 1 arg */
        }
        else if (tok->type == KE_LPAREN) {
            stack[sp++] = *tok;
        }
        else if (tok->type == KE_COMMA) {
            while (sp > 0 && stack[sp-1].type != KE_LPAREN)
                expr->ops[expr->len++] = stack[--sp];
            if (acp > 0) arg_counts[acp-1]++;
        }
        else if (tok->type == KE_RPAREN) {
            while (sp > 0 && stack[sp-1].type != KE_LPAREN)
                expr->ops[expr->len++] = stack[--sp];
            if (sp > 0) sp--; /* pop lparen */
            /* If top of stack is a function, pop it with arg count */
            if (sp > 0 && ke_is_func(stack[sp-1].type)) {
                KETok func = stack[--sp];
                func.value = (acp > 0) ? (float)arg_counts[--acp] : 1.0f;
                expr->ops[expr->len++] = func;
            }
        }
        else if (ke_is_op(tok->type)) {
            while (sp > 0 && ke_is_op(stack[sp-1].type) &&
                   ke_precedence(stack[sp-1].type) >= ke_precedence(tok->type))
                expr->ops[expr->len++] = stack[--sp];
            stack[sp++] = *tok;
        }
    }
    while (sp > 0) expr->ops[expr->len++] = stack[--sp];
    expr->valid = (expr->len > 0) ? 1 : 0;
}

/* ── Evaluate ── */
static float ke_eval(const KeySeqExpr *expr, const KeySeqCtx *ctx) {
    if (!expr->valid) return 0.0f;
    float stk[32];
    int sp = 0;

    for (int i = 0; i < expr->len && sp < 31; i++) {
        const KETok *t = &expr->ops[i];
        switch (t->type) {
        case KE_NUM:       stk[sp++] = t->value; break;
        case KE_VAR_N:     stk[sp++] = ctx->n; break;
        case KE_VAR_V:     stk[sp++] = ctx->v; break;
        case KE_VAR_T:     stk[sp++] = ctx->t; break;
        case KE_VAR_G:     stk[sp++] = ctx->g; break;
        case KE_VAR_I:     stk[sp++] = ctx->i; break;
        case KE_VAR_ROOT:  stk[sp++] = ctx->root; break;
        case KE_VAR_RV:    stk[sp++] = ctx->rv; break;
        case KE_VAR_TIME:  stk[sp++] = ctx->time; break;
        case KE_VAR_BU:    stk[sp++] = ctx->bu; break;
        case KE_VAR_GATE:  stk[sp++] = ctx->gate; break;
        case KE_VAR_HELD:  stk[sp++] = ctx->held; break;
        case KE_VAR_DT:    stk[sp++] = ctx->dt; break;
        /* binary ops */
        case KE_ADD: if (sp >= 2) { sp--; stk[sp-1] += stk[sp]; } break;
        case KE_SUB: if (sp >= 2) { sp--; stk[sp-1] -= stk[sp]; } break;
        case KE_MUL: if (sp >= 2) { sp--; stk[sp-1] *= stk[sp]; } break;
        case KE_DIV: if (sp >= 2) { sp--; stk[sp-1] = stk[sp] != 0 ? stk[sp-1]/stk[sp] : 0; } break;
        case KE_MOD: if (sp >= 2) { sp--; int a=(int)stk[sp-1],b=(int)stk[sp]; stk[sp-1] = b ? (float)(a%b) : 0; } break;
        case KE_GT:  if (sp >= 2) { sp--; stk[sp-1] = stk[sp-1] > stk[sp] ? 1.0f : 0.0f; } break;
        case KE_LT:  if (sp >= 2) { sp--; stk[sp-1] = stk[sp-1] < stk[sp] ? 1.0f : 0.0f; } break;
        case KE_GTE: if (sp >= 2) { sp--; stk[sp-1] = stk[sp-1] >= stk[sp] ? 1.0f : 0.0f; } break;
        case KE_LTE: if (sp >= 2) { sp--; stk[sp-1] = stk[sp-1] <= stk[sp] ? 1.0f : 0.0f; } break;
        /* 1-arg functions */
        case KE_ABS: if (sp >= 1) stk[sp-1] = fabsf(stk[sp-1]); break;
        case KE_SIN: if (sp >= 1) stk[sp-1] = sinf(stk[sp-1]); break;
        case KE_COS: if (sp >= 1) stk[sp-1] = cosf(stk[sp-1]); break;
        /* 0-arg */
        case KE_RAND: {
            if (ctx->local_rand) {
                uint32_t *rs = ctx->local_rand;
                *rs ^= *rs << 13; *rs ^= *rs >> 17; *rs ^= *rs << 5;
                stk[sp++] = (float)(*rs & 0x7FFFFFFF) / (float)0x7FFFFFFF;
            } else {
                stk[sp++] = ke_randf();
            }
            break;
        }
        /* multi-arg: arg count stored in t->value */
        case KE_NOISE: {
            int argc = (int)t->value;
            fnl_state *ns = ctx->local_fnl ? ctx->local_fnl : &g_fnl;
            ke_ensure_fnl();
            ns->seed = (int)ctx->seed;
            float nv;
            if (argc >= 3 && sp >= 3) { sp -= 2; nv = fnlGetNoise3D(ns, stk[sp-1], stk[sp], stk[sp+1]); }
            else if (argc >= 2 && sp >= 2) { sp--; nv = fnlGetNoise2D(ns, stk[sp-1], stk[sp]); }
            else if (sp >= 1) { float x = stk[sp-1]; nv = fnlGetNoise2D(ns, x, x * 0.7f + 13.37f); }
            else { nv = 0; }
            stk[sp > 0 ? sp-1 : 0] = (nv + 1.0f) * 0.5f; /* map [-1,1] → [0,1] */
            break;
        }
        case KE_NOISEB: {
            int argc = (int)t->value;
            fnl_state *ns = ctx->local_fnl ? ctx->local_fnl : &g_fnl;
            ke_ensure_fnl();
            ns->seed = (int)ctx->seed;
            float nv;
            if (argc >= 3 && sp >= 3) { sp -= 2; nv = fnlGetNoise3D(ns, stk[sp-1], stk[sp], stk[sp+1]); }
            else if (argc >= 2 && sp >= 2) { sp--; nv = fnlGetNoise2D(ns, stk[sp-1], stk[sp]); }
            else if (sp >= 1) { float x = stk[sp-1]; nv = fnlGetNoise2D(ns, x, x * 0.7f + 13.37f); }
            else { nv = 0; }
            stk[sp > 0 ? sp-1 : 0] = nv; /* raw [-1,1] */
            break;
        }
        case KE_IF: {
            if (sp >= 3) { sp -= 2; stk[sp-1] = stk[sp-1] != 0.0f ? stk[sp] : stk[sp+1]; }
            break;
        }
        /* GLSL-style functions */
        case KE_FLOOR: if (sp >= 1) stk[sp-1] = floorf(stk[sp-1]); break;
        case KE_CEIL:  if (sp >= 1) stk[sp-1] = ceilf(stk[sp-1]); break;
        case KE_MIN:   if (sp >= 2) { sp--; stk[sp-1] = stk[sp-1] < stk[sp] ? stk[sp-1] : stk[sp]; } break;
        case KE_MAX:   if (sp >= 2) { sp--; stk[sp-1] = stk[sp-1] > stk[sp] ? stk[sp-1] : stk[sp]; } break;
        case KE_CLAMP: /* clamp(x, lo, hi) */
            if (sp >= 3) { sp -= 2; float x=stk[sp-1],lo=stk[sp],hi=stk[sp+1]; stk[sp-1] = x<lo?lo:x>hi?hi:x; }
            break;
        case KE_STEP: /* step(edge, x) → 0 if x < edge, else 1 */
            if (sp >= 2) { sp--; stk[sp-1] = stk[sp] >= stk[sp-1] ? 1.0f : 0.0f; }
            break;
        case KE_SMOOTHSTEP: /* smoothstep(edge0, edge1, x) */
            if (sp >= 3) {
                sp -= 2;
                float e0=stk[sp-1], e1=stk[sp], x=stk[sp+1];
                float tt = (e1 != e0) ? (x - e0) / (e1 - e0) : 0;
                if (tt < 0) tt = 0; if (tt > 1) tt = 1;
                stk[sp-1] = tt * tt * (3.0f - 2.0f * tt);
            }
            break;
        default: break;
        }
    }
    return sp > 0 ? stk[0] : 0.0f;
}

/* ══════════════════════════════════════════════════════════════════════════
 *  Param bus — arbitrary named param expressions, routed to instruments
 * ══════════════════════════════════════════════════════════════════════════ */

#define KEYSEQ_MAX_PARAMS 16

typedef struct {
    char       name[24];     /* param name: "cutoff", "mod_index", etc. */
    KeySeqExpr expr;         /* per-step expression */
    KeySeqExpr frame_expr;   /* per-frame expression (optional) */
    float      value;        /* last evaluated value */
} KeySeqParam;

/* ══════════════════════════════════════════════════════════════════════════
 *  KeySeq struct
 * ══════════════════════════════════════════════════════════════════════════ */

typedef struct KeySeq {
    /* Offsets mode */
    int   offsets[KEYSEQ_MAX_STEPS];
    float levels[KEYSEQ_MAX_STEPS];
    int   num_steps;

    /* Shared params */
    float step_beats;
    float gate_beats;
    int   enabled;
    int   gated;
    int   loop;

    /* Algo mode */
    int        algo_mode;
    KeySeqExpr expr_n, expr_v, expr_t, expr_g;

    /* Per-frame note modulation (noteAlgo) */
    KeySeqExpr expr_frame;     /* evaluated per sample, returns cents offset */
    float      cents_mod;      /* current output — instruments read this */

    /* End condition */
    KeySeqExpr expr_end;       /* evaluated per step, nonzero = stop */

    /* Param bus */
    KeySeqParam params[KEYSEQ_MAX_PARAMS];
    int         num_params;

    /* Runtime */
    int   firing;
    int   playing;
    int   root_note;
    float root_velocity;
    int   current_step;
    float step_elapsed;
    float gate_elapsed;
    int   last_played_note;
    int   gate_open;
    float bpm;
    int   root_held;

    /* Running algo state */
    float algo_n, algo_v, algo_t, algo_g;

    /* Per-note timing */
    float note_time;
    float total_time;

    /* Noise/rand seed — per-instance, no global state mutation */
    KeySeqExpr expr_seed;       /* evaluated at note-on, result → noise domain offset */
    uint32_t runtime_seed;      /* active seed for current note */
    uint32_t rand_state;        /* per-keyseq PRNG state */
    fnl_state fnl;              /* per-keyseq noise state */

    /* Callbacks */
    void   *inst_state;
    void  (*midi_fn)(void *, uint8_t, uint8_t, uint8_t);
    void  (*param_fn)(void *state, const char *param_name, float value);
    void  (*graph_fn)(const char *json, int len);  /* broadcast step graph on trigger */
    uint8_t midi_channel;

    char  source[512];
} KeySeq;

/* ── Init / bind ── */

/* Global graph callback — set once by server before rack init */
static void (*g_keyseq_graph_fn)(const char *json, int len) = NULL;

static void keyseq_init(KeySeq *ks) {
    memset(ks, 0, sizeof(KeySeq));
    ks->step_beats = 0.125f;
    ks->bpm = 120.0f;
    ks->last_played_note = -1;
    ks->graph_fn = g_keyseq_graph_fn;
    ks->rand_state = 1;
    ks->fnl = fnlCreateState();
    ks->fnl.noise_type = FNL_NOISE_PERLIN;
    ks->fnl.frequency = 1.0f;
}

static void keyseq_bind(KeySeq *ks, void *inst_state,
                         void (*midi_fn)(void *, uint8_t, uint8_t, uint8_t),
                         void (*param_fn)(void *state, const char *name, float value),
                         uint8_t channel) {
    ks->inst_state = inst_state;
    ks->midi_fn = midi_fn;
    ks->param_fn = param_fn;
    ks->midi_channel = channel & 0x0F;
}

/* ── Param bus helpers ── */

static void keyseq_fire_params_step(KeySeq *ks, const KeySeqCtx *ctx) {
    if (!ks->param_fn) return;
    for (int p = 0; p < ks->num_params; p++) {
        if (ks->params[p].expr.valid) {
            ks->params[p].value = ke_eval(&ks->params[p].expr, ctx);
            ks->param_fn(ks->inst_state, ks->params[p].name, ks->params[p].value);
        }
    }
}

static void keyseq_fire_params_frame(KeySeq *ks, const KeySeqCtx *ctx) {
    if (!ks->param_fn) return;
    for (int p = 0; p < ks->num_params; p++) {
        if (ks->params[p].frame_expr.valid) {
            ks->params[p].value = ke_eval(&ks->params[p].frame_expr, ctx);
            ks->param_fn(ks->inst_state, ks->params[p].name, ks->params[p].value);
        }
    }
}

/* ── MIDI helpers ── */

static inline void keyseq_fire_on(KeySeq *ks, int note, int vel) {
    if (!ks->midi_fn || note < 0 || note > 127) return;
    /* Always release previous note before firing new one */
    if (ks->last_played_note >= 0) {
        ks->firing = 1;
        ks->midi_fn(ks->inst_state, (uint8_t)(0x80 | ks->midi_channel),
                    (uint8_t)ks->last_played_note, 0);
        ks->firing = 0;
    }
    ks->firing = 1;
    ks->midi_fn(ks->inst_state, (uint8_t)(0x90 | ks->midi_channel), (uint8_t)note, (uint8_t)vel);
    ks->firing = 0;
    ks->last_played_note = note;
    ks->gate_open = 1;
    ks->note_time = 0.0f;
}

static inline void keyseq_fire_off(KeySeq *ks, int note) {
    if (!ks->midi_fn || note < 0 || note > 127) return;
    ks->firing = 1;
    ks->midi_fn(ks->inst_state, (uint8_t)(0x80 | ks->midi_channel), (uint8_t)note, 0);
    ks->firing = 0;
    if (ks->last_played_note == note) { ks->last_played_note = -1; ks->gate_open = 0; }
}

/* ── Parse DSL ── */

static int keyseq_parse(KeySeq *ks, const char *dsl) {
    void *saved_state = ks->inst_state;
    void (*saved_fn)(void *, uint8_t, uint8_t, uint8_t) = ks->midi_fn;
    void (*saved_pfn)(void *, const char *, float) = ks->param_fn;
    void (*saved_gfn)(const char *, int) = ks->graph_fn;
    uint8_t saved_ch = ks->midi_channel;
    float saved_bpm = ks->bpm;

    if (ks->playing && ks->last_played_note >= 0)
        keyseq_fire_off(ks, ks->last_played_note);

    memset(ks, 0, sizeof(KeySeq));
    ks->step_beats = 0.125f;
    ks->last_played_note = -1;
    ks->enabled = 1;
    ks->inst_state = saved_state;
    ks->midi_fn = saved_fn;
    ks->param_fn = saved_pfn;
    ks->graph_fn = saved_gfn;
    ks->midi_channel = saved_ch;
    ks->bpm = saved_bpm;
    /* Restore per-instance noise state */
    ks->fnl = fnlCreateState();
    ks->fnl.noise_type = FNL_NOISE_PERLIN;
    ks->fnl.frequency = 1.0f;
    ks->rand_state = 1;

    if (!dsl || !*dsl) { ks->enabled = 0; return 0; }
    strncpy(ks->source, dsl, sizeof(ks->source) - 1);

    char buf[512];
    strncpy(buf, dsl, sizeof(buf) - 1); buf[sizeof(buf)-1] = '\0';
    int offsets_parsed = 0;

    /* Split on semicolons (expressions can contain spaces) */
    char *tok = buf;
    while (*tok) {
        while (*tok == ';' || *tok == ' ' || *tok == '\t') tok++;
        if (!*tok) break;
        char *end = tok;
        while (*end && *end != ';') end++;
        char saved = *end; *end = '\0';
        /* Trim trailing whitespace from token */
        char *trim = end - 1;
        while (trim > tok && (*trim == ' ' || *trim == '\t')) { *trim = '\0'; trim--; }
        /* Trim leading whitespace */
        while (*tok == ' ' || *tok == '\t') tok++;

        if (tok[0] == 't' && tok[1] >= '0' && tok[1] <= '9') {
            ks->step_beats = (float)strtod(tok + 1, NULL);
            if (ks->step_beats < 0.001f) ks->step_beats = 0.001f;
        }
        else if (tok[0] == 'g' && tok[1] >= '0' && tok[1] <= '9')
            ks->gate_beats = (float)strtod(tok + 1, NULL);
        else if (strcmp(tok, "gated") == 0) ks->gated = 1;
        else if (strcmp(tok, "loop") == 0)  ks->loop = 1;
        else if (strcmp(tok, "algo") == 0)  ks->algo_mode = 1;
        else if (strlen(tok) > 2 && tok[1] == ':' && strchr("nvtg", tok[0])) {
            switch (tok[0]) {
            case 'n': ke_compile(&ks->expr_n, tok+2); break;
            case 'v': ke_compile(&ks->expr_v, tok+2); break;
            case 't': ke_compile(&ks->expr_t, tok+2); break;
            case 'g': ke_compile(&ks->expr_g, tok+2); break;
            }
        }
        else if (strncmp(tok, "frame:", 6) == 0)
            ke_compile(&ks->expr_frame, tok + 6);
        else if (strncmp(tok, "end:", 4) == 0)
            ke_compile(&ks->expr_end, tok + 4);
        else if (strncmp(tok, "seed:", 5) == 0)
            ke_compile(&ks->expr_seed, tok + 5);
        else if (strchr(tok, ':') && tok[0] >= 'a' && tok[0] <= 'z') {
            /* Arbitrary param: name:expr or frame_name:expr */
            char *colon = strchr(tok, ':');
            int name_len = (int)(colon - tok);
            const char *expr_src = colon + 1;
            if (name_len > 0 && name_len < 24 && *expr_src &&
                ks->num_params < KEYSEQ_MAX_PARAMS) {
                /* Check for frame_ prefix → per-frame param */
                int is_frame = (name_len > 6 && strncmp(tok, "frame_", 6) == 0);
                KeySeqParam *p = &ks->params[ks->num_params];
                memset(p, 0, sizeof(*p));
                if (is_frame) {
                    strncpy(p->name, tok + 6, (size_t)(name_len - 6));
                    ke_compile(&p->frame_expr, expr_src);
                } else {
                    strncpy(p->name, tok, (size_t)name_len);
                    ke_compile(&p->expr, expr_src);
                }
                if (p->expr.valid || p->frame_expr.valid)
                    ks->num_params++;
            }
        }
        else if (tok[0] == 'v' && tok[1] >= '0' && tok[1] <= '9') {
            const char *p = tok + 1; int li = 0;
            while (*p && li < KEYSEQ_MAX_STEPS) {
                ks->levels[li++] = (float)strtod(p, NULL);
                while (*p && *p != ',') p++;
                if (*p == ',') p++;
            }
            float last = li > 0 ? ks->levels[li-1] : 1.0f;
            for (int i = li; i < KEYSEQ_MAX_STEPS; i++) ks->levels[i] = last;
        }
        else if ((tok[0] >= '0' && tok[0] <= '9') || tok[0] == '-') {
            if (!offsets_parsed) {
                const char *p = tok;
                while (*p && ks->num_steps < KEYSEQ_MAX_STEPS) {
                    ks->offsets[ks->num_steps++] = (int)strtol(p, NULL, 10);
                    while (*p && *p != ',') p++;
                    if (*p == ',') p++;
                }
                offsets_parsed = 1;
            }
        }

        *end = saved; tok = end;
    }

    if (ks->levels[0] == 0.0f)
        for (int i = 0; i < KEYSEQ_MAX_STEPS; i++) ks->levels[i] = 1.0f;
    if (ks->gate_beats <= 0.0f)
        ks->gate_beats = 1.0f;  /* default: full step duration */
    if (ks->algo_mode && !ks->expr_n.valid && !ks->expr_v.valid)
        ks->algo_mode = 0;
    if (ks->algo_mode) ks->num_steps = 1;

    fprintf(stderr, "[keyseq] parsed: %s steps=%d t=%.3f g=%.3f gated=%d loop=%d seed=%s\n",
            ks->algo_mode ? "ALGO" : "OFFSETS", ks->num_steps,
            ks->step_beats, ks->gate_beats, ks->gated, ks->loop,
            ks->expr_seed.valid ? "expr" : "auto");
    if (ks->algo_mode) {
        fprintf(stderr, "[keyseq]   n=%s v=%s t=%s g=%s end=%s frame=%s\n",
                ks->expr_n.valid ? "yes" : "-",
                ks->expr_v.valid ? "yes" : "-",
                ks->expr_t.valid ? "yes" : "-",
                ks->expr_g.valid ? "yes" : "-",
                ks->expr_end.valid ? "yes" : "-",
                ks->expr_frame.valid ? "yes" : "-");
    }
    fprintf(stderr, "[keyseq]   dsl: %s\n", ks->source);

    return ks->algo_mode ? 1 : ks->num_steps;
}

/* ── Trigger ── */

static int keyseq_note_on(KeySeq *ks, int note, int velocity) {
    if (!ks->enabled || (ks->num_steps == 0 && !ks->algo_mode)) return 0;

    if (ks->playing && ks->last_played_note >= 0)
        keyseq_fire_off(ks, ks->last_played_note);

    ks->root_note = note;
    ks->root_velocity = (float)velocity / 127.0f;
    ks->current_step = 0;
    ks->step_elapsed = 0.0f;
    ks->gate_elapsed = 0.0f;
    ks->note_time = 0.0f;
    ks->total_time = 0.0f;
    ks->playing = 1;
    ks->root_held = 1;
    ks->cents_mod = 0.0f;

    /* Evaluate seed expression or use wall-clock time */
    if (ks->expr_seed.valid) {
        KeySeqCtx sc = {
            .n = (float)note, .v = (float)velocity,
            .rv = ks->root_velocity, .root = (float)note,
            .i = 0, .time = 0, .gate = 0, .held = 1.0f
        };
        float sv = ke_eval(&ks->expr_seed, &sc);
        /* Hash the float into a well-distributed seed.
         * fmodf keeps it in range before cast to avoid uint32 overflow. */
        float hf = fmodf(fabsf(sv) * 2654435.761f, 4294967000.0f);
        uint32_t raw = (uint32_t)hf;
        ks->runtime_seed = raw ? raw : 1;
    } else {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        ks->runtime_seed = (uint32_t)(ts.tv_nsec ^ (ts.tv_sec * 1000003)) | 1;
    }
    ks->rand_state = ks->runtime_seed;
    ks->fnl.seed = (int)ks->runtime_seed;

    ks->algo_n = (float)note;
    ks->algo_v = ks->root_velocity;
    ks->algo_t = ks->step_beats;
    ks->algo_g = ks->gate_beats;

    fprintf(stderr, "[keyseq] trigger note=%d vel=%d seed=%u (%s) | n=%.2f v=%.4f t=%.3f g=%.3f gated=%d\n",
            note, velocity, ks->runtime_seed,
            ks->expr_seed.valid ? "expr" : "auto",
            ks->algo_n, ks->algo_v, ks->algo_t, ks->algo_g, ks->gated);
    fprintf(stderr, "[keyseq]   %s\n", ks->source);

    if (ks->algo_mode) {
        /* Step 0 = root note, algo starts at step 1 */
        int vel = (int)(ks->root_velocity * 127.0f);
        keyseq_fire_on(ks, note, vel > 0 ? vel : 1);
    } else {
        int target = note + ks->offsets[0];
        if (target < 0) target = 0; if (target > 127) target = 127;
        int vel = (int)(ks->root_velocity * ks->levels[0] * 127.0f);
        keyseq_fire_on(ks, target, vel > 0 ? vel : 1);
    }

    /* Broadcast step graph if callback is set */
    if (ks->graph_fn) ks->graph_fn(ks->source, (int)strlen(ks->source));

    return 1;
}

/* ── Release ── */

static int keyseq_note_off(KeySeq *ks, int note) {
    if (!ks->enabled || !ks->playing) return 0;
    if (note != ks->root_note) return 0;
    ks->root_held = 0;

    if (ks->gated) {
        if (ks->last_played_note >= 0) keyseq_fire_off(ks, ks->last_played_note);
        ks->playing = 0;
        ks->cents_mod = 0.0f;
        return 1;
    }
    return 0;
}

/* ── Tick (per sample) ── */

static void keyseq_tick(KeySeq *ks, float dt) {
    if (!ks->playing) return;

    ks->total_time += dt;
    ks->note_time += dt;

    float step_sec = (ks->algo_mode ? ks->algo_t : ks->step_beats) * 60.0f / ks->bpm;
    float gate_ratio = ks->algo_mode ? ks->algo_g : ks->gate_beats;
    float gate_sec = step_sec * gate_ratio;

    ks->step_elapsed += dt;
    ks->gate_elapsed += dt;

    /* Per-frame modulation (cents + params) */
    if (ks->gate_open && (ks->expr_frame.valid || ks->num_params > 0)) {
        float bu_val = (step_sec > 0) ? ks->step_elapsed / step_sec : 0.0f;
        KeySeqCtx fc = {
            .n = (float)ks->last_played_note, .v = ks->algo_v,
            .t = ks->algo_t, .g = ks->algo_g,
            .i = (float)ks->current_step, .root = (float)ks->root_note,
            .rv = ks->root_velocity,
            .time = ks->note_time, .bu = bu_val,
            .gate = (gate_sec > 0) ? ks->gate_elapsed / gate_sec : 0,
            .held = ks->root_held ? 1.0f : 0.0f,
            .dt = dt, .seed = (float)ks->runtime_seed,
            .local_rand = &ks->rand_state, .local_fnl = &ks->fnl
        };
        if (ks->expr_frame.valid)
            ks->cents_mod = ke_eval(&ks->expr_frame, &fc);
        keyseq_fire_params_frame(ks, &fc);
    } else if (!ks->gate_open) {
        ks->cents_mod = 0.0f;
    }

    /* Gate off */
    if (ks->gate_open && ks->gate_elapsed >= gate_sec) {
        if (ks->last_played_note >= 0) keyseq_fire_off(ks, ks->last_played_note);
    }

    /* Step advance */
    if (ks->step_elapsed >= step_sec) {
        ks->step_elapsed -= step_sec;
        ks->gate_elapsed = 0.0f;
        ks->current_step++;

        if (ks->algo_mode) {
            float bu_val = 0.0f;
            KeySeqCtx ctx = {
                .n = ks->algo_n, .v = ks->algo_v,
                .t = ks->algo_t, .g = ks->algo_g,
                .i = (float)ks->current_step, .root = (float)ks->root_note,
                .rv = ks->root_velocity,
                .time = ks->total_time, .bu = bu_val,
                .gate = 0, .held = ks->root_held ? 1.0f : 0.0f,
                .seed = (float)ks->runtime_seed,
                .local_rand = &ks->rand_state, .local_fnl = &ks->fnl
            };
            float new_n = ks->expr_n.valid ? ke_eval(&ks->expr_n, &ctx) : ctx.n;
            float new_v = ks->expr_v.valid ? ke_eval(&ks->expr_v, &ctx) : ctx.v;
            float new_t = ks->expr_t.valid ? ke_eval(&ks->expr_t, &ctx) : ctx.t;
            float new_g = ks->expr_g.valid ? ke_eval(&ks->expr_g, &ctx) : ctx.g;

            ks->algo_n = new_n; ks->algo_v = new_v;
            ks->algo_t = new_t > 0.001f ? new_t : 0.001f;
            ks->algo_g = new_g > 0.001f ? new_g : 0.001f;

            /* End condition */
            if (ks->expr_end.valid) {
                /* Re-evaluate with updated state */
                ctx.n = ks->algo_n; ctx.v = ks->algo_v;
                ctx.t = ks->algo_t; ctx.g = ks->algo_g;
                float end_val = ke_eval(&ks->expr_end, &ctx);
                if (end_val != 0.0f) {
                    fprintf(stderr, "[keyseq]   end @ step %d (n=%.2f v=%.4f)\n",
                            ks->current_step, ks->algo_n, ks->algo_v);
                    if (ks->last_played_note >= 0) keyseq_fire_off(ks, ks->last_played_note);
                    ks->playing = 0; ks->cents_mod = 0; return;
                }
            } else if (ks->algo_v <= KE_EPSILON) {
                /* Default: stop on velocity death */
                if (ks->last_played_note >= 0) keyseq_fire_off(ks, ks->last_played_note);
                ks->playing = 0; ks->cents_mod = 0; return;
            }

            int target = (int)roundf(ks->algo_n);
            if (target < 0) target = 0; if (target > 127) target = 127;
            /* Fractional n → cents detune */
            ks->cents_mod = (ks->algo_n - (float)target) * 100.0f;
            int vel = (int)(ks->algo_v * 127.0f);
            fprintf(stderr, "[keyseq]   step %d: n=%.2f(%d) v=%.4f t=%.3f g=%.3f cents=%.1f\n",
                    ks->current_step, ks->algo_n, target, ks->algo_v,
                    ks->algo_t, ks->algo_g, ks->cents_mod);
            keyseq_fire_on(ks, target, vel > 0 ? vel : 1);
            keyseq_fire_params_step(ks, &ctx);
        } else {
            if (ks->current_step >= ks->num_steps) {
                if (ks->loop) ks->current_step = 0;
                else { ks->playing = 0; ks->cents_mod = 0; return; }
            }
            int target = ks->root_note + ks->offsets[ks->current_step];
            if (target < 0) target = 0; if (target > 127) target = 127;
            int vel = (int)(ks->root_velocity * ks->levels[ks->current_step] * 127.0f);
            keyseq_fire_on(ks, target, vel > 0 ? vel : 1);
        }
    }
}

/* ── Stop ── */

static void keyseq_stop(KeySeq *ks) {
    if (ks->last_played_note >= 0) keyseq_fire_off(ks, ks->last_played_note);
    ks->playing = 0;
    ks->cents_mod = 0.0f;
}

#endif /* MINIWAVE_KEYSEQ_H */
