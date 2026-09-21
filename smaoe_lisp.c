/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

/* smaoe_lisp.c - Bare-Metal Hybrid C89 + Macro Lisp Orchestration Engine
 * Compile: gcc -std=c89 -pedantic -Wall -Wextra -o smaoe_lisp smaoe_lisp.c
 * Run: ./smaoe_lisp [journal.dat]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <errno.h>
#include <time.h>

/* ---------- Compile-time bounds (formally fixed) ---------- */
#define MAX_AGENTS 8
#define AGENT_WORDS 8192
#define MAX_SYMBOLS 256
#define MAX_NAME 32
#define MSG_MAX 512
#define JOURNAL_REC_SIZE 64
#define MAGIC 0x534D4138UL /* "SMA8" */

/* BCPL-style word */
typedef long word;

/* =====================================================================
 * TAG and SYM definitions - BCPL packed word layout
 * Bits 0-1 : TAG
 * Bits 2-15: PAYLOAD (14-bit index / length / immediate)
 * Bits 16-31: FLAGS (generation, mark, etc.)
 * ===================================================================== */
#define TAG_NUM 0UL
#define TAG_CONS 1UL
#define TAG_SYM 2UL
#define TAG_FUNC 3UL

#define BIT_TAG_SHIFT 0
#define BIT_TAG_MASK 0x3UL
#define BIT_PAYLOAD_SHIFT 2
#define BIT_PAYLOAD_MASK 0x3FFFUL
#define BIT_FLAGS_SHIFT 16
#define BIT_FLAGS_MASK 0xFFFFUL

#define BF_GET(w,s,m) ( ((unsigned long)(w) >> (s)) & (m) )
#define BF_SET(w,s,m,v) ( ((unsigned long)(w) & ~((unsigned long)(m)<<(s))) | \
                          (((unsigned long)(v) & (m)) << (s)) )

#define MAKE_NUM(n) ( BF_SET(0UL, BIT_TAG_SHIFT, BIT_TAG_MASK, TAG_NUM) | \
                        BF_SET(0UL, BIT_PAYLOAD_SHIFT, BIT_PAYLOAD_MASK, (unsigned long)(n)) )
#define MAKE_CONS(p) ( BF_SET(0UL, BIT_TAG_SHIFT, BIT_TAG_MASK, TAG_CONS) | \
                        BF_SET(0UL, BIT_PAYLOAD_SHIFT, BIT_PAYLOAD_MASK, (unsigned long)(p)) )
#define MAKE_SYM(i) ( BF_SET(0UL, BIT_TAG_SHIFT, BIT_TAG_MASK, TAG_SYM) | \
                        BF_SET(0UL, BIT_PAYLOAD_SHIFT, BIT_PAYLOAD_MASK, (unsigned long)(i)) )
#define MAKE_FUNC(p) ( BF_SET(0UL, BIT_TAG_SHIFT, BIT_TAG_MASK, TAG_FUNC) | \
                        BF_SET(0UL, BIT_PAYLOAD_SHIFT, BIT_PAYLOAD_MASK, (unsigned long)(p)) )

#define TAG(v) BF_GET((unsigned long)(v), BIT_TAG_SHIFT, BIT_TAG_MASK)
#define PAYLOAD(v) BF_GET((unsigned long)(v), BIT_PAYLOAD_SHIFT, BIT_PAYLOAD_MASK)
#define FLAGS(v) BF_GET((unsigned long)(v), BIT_FLAGS_SHIFT, BIT_FLAGS_MASK)

/* ---------- CBMC annotations (ignored by gcc) ---------- */
#ifdef __CPROVER__
# define CBMC_ASSERT(c) __CPROVER_assert((c), #c)
# define CBMC_ASSUME(c) __CPROVER_assume(c)
#else
# define CBMC_ASSERT(c) ((void)0)
# define CBMC_ASSUME(c) ((void)0)
#endif

/* ---------- Forward declarations (C89) ---------- */
typedef struct legacy_agent legacy_agent_t;
typedef word (*eval_fn)(legacy_agent_t *, word);

/* ---------- Agent boundary (exactly as specified) ---------- */
struct legacy_agent {
    unsigned long id;
    char name[MAX_NAME];
    void *lisp_heap;
    eval_fn eval;
    int pipe_fd[2];
    word free_ptr;
    word heap_limit;
    word env;
    int alive;
    pid_t pid;
};

/* ---------- Global state ---------- */
static legacy_agent_t agents[MAX_AGENTS];
static int agent_count = 0;
static unsigned long global_clock = 0;
static FILE *journal = NULL;

/* Well-known symbols (interned at agent creation) */
static word S_NIL, S_T, S_QUOTE, S_IF, S_LAMBDA, S_MACRO,
            S_SETQ, S_PROGN, S_CAR, S_CDR, S_CONS, S_EVAL, S_MSG;

/* ---------- Arena primitives (BCPL style) ---------- */

/*@ requires \valid(a);
  @ requires a->lisp_heap != \null;
  @ assigns \nothing;
  @ ensures \result == (word*)a->lisp_heap;
  @*/
static word *heap_base(legacy_agent_t *a)
{
    CBMC_ASSERT(a != 0);
    CBMC_ASSERT(a->lisp_heap != 0);
    return (word *)a->lisp_heap;
}

/*@ requires \valid(a);
  @ requires n > 0;
  @ requires a->free_ptr >= 1;
  @ requires a->free_ptr <= a->heap_limit;
  @ assigns a->free_ptr, a->alive;
  @ ensures \result == 0 || (\result >= 1 && \result + n <= a->heap_limit);
  @*/
static word alloc_words(legacy_agent_t *a, word n)
{
    word p;
    CBMC_ASSERT(n > 0);
    CBMC_ASSERT(a->free_ptr >= 1);
    CBMC_ASSERT(a->free_ptr <= a->heap_limit);

    if (a->free_ptr + n > a->heap_limit) {
        a->alive = 0;
        CBMC_ASSERT(a->alive == 0);
        return 0;
    }
    p = a->free_ptr;
    a->free_ptr += n;

    CBMC_ASSERT(p >= 1);
    CBMC_ASSERT(p + n <= a->heap_limit);
    CBMC_ASSERT(a->free_ptr <= a->heap_limit);
    return p;
}

/*@ requires \valid(a);
  @ assigns a->free_ptr, a->alive, heap_base(a)[0 .. a->heap_limit-1];
  @ ensures \result == MAKE_NUM(0) || TAG(\result) == TAG_CONS;
  @*/
static word cons(legacy_agent_t *a, word car_val, word cdr_val)
{
    word p = alloc_words(a, 2);
    if (!p) return MAKE_NUM(0);

    CBMC_ASSERT(p + 1 < a->heap_limit);
    heap_base(a)[p] = car_val;
    heap_base(a)[p + 1] = cdr_val;
    return MAKE_CONS(p);
}

/*@ requires \valid(a);
  @ requires TAG(c) == TAG_CONS || c == MAKE_NUM(0);
  @ assigns \nothing;
  @*/
static word car(legacy_agent_t *a, word c)
{
    word idx;
    CBMC_ASSERT(TAG(c) == TAG_CONS || c == MAKE_NUM(0));
    if (TAG(c) != TAG_CONS) return MAKE_NUM(0);

    idx = PAYLOAD(c);
    CBMC_ASSERT(idx >= 1);
    CBMC_ASSERT(idx < a->heap_limit);
    return heap_base(a)[idx];
}

/*@ requires \valid(a);
  @ requires TAG(c) == TAG_CONS || c == MAKE_NUM(0);
  @ assigns \nothing;
  @*/
static word cdr(legacy_agent_t *a, word c)
{
    word idx;
    CBMC_ASSERT(TAG(c) == TAG_CONS || c == MAKE_NUM(0));
    if (TAG(c) != TAG_CONS) return MAKE_NUM(0);

    idx = PAYLOAD(c);
    CBMC_ASSERT(idx >= 1);
    CBMC_ASSERT(idx + 1 < a->heap_limit);
    return heap_base(a)[idx + 1];
}

/* ---------- Symbol table (per-agent, simple) ---------- */
static char sym_names[MAX_AGENTS][MAX_SYMBOLS][MAX_NAME];
static int sym_count[MAX_AGENTS];

static word intern(legacy_agent_t *a, const char *name)
{
    int i, id = (int)a->id;
    for (i = 0; i < sym_count[id]; i++) {
        if (strcmp(sym_names[id][i], name) == 0)
            return MAKE_SYM(i);
    }
    if (sym_count[id] >= MAX_SYMBOLS) return 0;
    strncpy(sym_names[id][sym_count[id]], name, MAX_NAME - 1);
    sym_names[id][sym_count[id]][MAX_NAME - 1] = '\0';
    i = sym_count[id]++;
    return MAKE_SYM(i);
}

static const char *sym_name(legacy_agent_t *a, word s)
{
    if (TAG(s) != TAG_SYM) return "?";
    return sym_names[a->id][PAYLOAD(s)];
}

/* ---------- Environment (association list) ---------- */
static word env_get(legacy_agent_t *a, word env, word sym)
{
    while (TAG(env) == TAG_CONS) {
        word pair = car(a, env);
        if (car(a, pair) == sym) return cdr(a, pair);
        env = cdr(a, env);
    }
    return S_NIL;
}

static word env_set(legacy_agent_t *a, word env, word sym, word val)
{
    return cons(a, cons(a, sym, val), env);
}

/* ---------- Journal (append-only, C89 file I/O) ---------- */
static void journal_write(unsigned long agent_id, const char *op, word val)
{
    char rec[JOURNAL_REC_SIZE];
    if (!journal) return;
    memset(rec, 0, JOURNAL_REC_SIZE);
    sprintf(rec, "%lu %lu %s %ld", global_clock, agent_id, op, (long)val);
    fwrite(rec, 1, JOURNAL_REC_SIZE, journal);
    fflush(journal);
}

/* ---------- Minimal Lisp printer ---------- */
static void print_val(legacy_agent_t *a, word v, FILE *out)
{
    if (TAG(v) == TAG_NUM) {
        fprintf(out, "%ld", (long)PAYLOAD(v));
    } else if (TAG(v) == TAG_SYM) {
        fprintf(out, "%s", sym_name(a, v));
    } else if (TAG(v) == TAG_CONS) {
        fprintf(out, "(");
        print_val(a, car(a, v), out);
        v = cdr(a, v);
        while (TAG(v) == TAG_CONS) {
            fprintf(out, " ");
            print_val(a, car(a, v), out);
            v = cdr(a, v);
        }
        if (v != S_NIL) {
            fprintf(out, " . ");
            print_val(a, v, out);
        }
        fprintf(out, ")");
    } else {
        fprintf(out, "#<func>");
    }
}

/* ---------- Macro expansion + Evaluator (the cognitive core) ---------- */
static word eval(legacy_agent_t *a, word expr);

static word expand_macro(legacy_agent_t *a, word macro, word args)
{
    word params, body, newenv, p, arg;
    params = car(a, cdr(a, macro));
    body = car(a, cdr(a, cdr(a, macro)));
    newenv = a->env;
    p = params;
    arg = args;
    while (TAG(p) == TAG_CONS && TAG(arg) == TAG_CONS) {
        newenv = env_set(a, newenv, car(a, p), car(a, arg));
        p = cdr(a, p);
        arg = cdr(a, arg);
    }
    {
        word old = a->env;
        word res;
        a->env = newenv;
        res = eval(a, body);
        a->env = old;
        return res;
    }
}

static word apply(legacy_agent_t *a, word fn, word args)
{
    if (TAG(fn) == TAG_FUNC) {
        word *p = (word *)((unsigned long)fn & ~3L);
        if (p[0] == MAKE_SYM(0)) {
            word params = p[1];
            word body = p[2];
            word newenv = a->env;
            word piter, aiter;
            piter = params;
            aiter = args;
            while (TAG(piter) == TAG_CONS && TAG(aiter) == TAG_CONS) {
                newenv = env_set(a, newenv, car(a, piter), car(a, aiter));
                piter = cdr(a, piter);
                aiter = cdr(a, aiter);
            }
            {
                word old = a->env;
                word res;
                a->env = newenv;
                res = eval(a, body);
                a->env = old;
                return res;
            }
        }
    }
    if (TAG(fn) == TAG_CONS && car(a, fn) == S_MACRO) {
        return expand_macro(a, fn, args);
    }
    return S_NIL;
}

static word eval_list(legacy_agent_t *a, word list)
{
    if (list == S_NIL || TAG(list) != TAG_CONS) return S_NIL;
    return cons(a, eval(a, car(a, list)), eval_list(a, cdr(a, list)));
}

/*@ requires valid_agent(a);
  @ assigns a->env, a->free_ptr, a->alive,
  @         heap_base(a)[0 .. a->heap_limit-1];
  @ ensures valid_agent(a);
  @*/
static word eval(legacy_agent_t *a, word expr)
{
    word op, args, res;

    if (!a->alive) return S_NIL;

    if (TAG(expr) == TAG_NUM || TAG(expr) == TAG_SYM) {
        if (TAG(expr) == TAG_SYM) {
            if (expr == S_NIL || expr == S_T) return expr;
            return env_get(a, a->env, expr);
        }
        return expr;
    }

    if (TAG(expr) != TAG_CONS) return S_NIL;

    op = car(a, expr);
    args = cdr(a, expr);

    if (op == S_QUOTE) {
        return car(a, args);
    }
    if (op == S_IF) {
        word cond = eval(a, car(a, args));
        if (cond != S_NIL)
            return eval(a, car(a, cdr(a, args)));
        else
            return eval(a, car(a, cdr(a, cdr(a, args))));
    }
    if (op == S_LAMBDA) {
        word p = alloc_words(a, 3);
        if (!p) return S_NIL;
        heap_base(a)[p] = MAKE_SYM(0);
        heap_base(a)[p + 1] = car(a, args);
        heap_base(a)[p + 2] = car(a, cdr(a, args));
        return MAKE_FUNC(p);
    }
    if (op == S_MACRO) {
        return expr;
    }
    if (op == S_SETQ) {
        word sym = car(a, args);
        word val = eval(a, car(a, cdr(a, args)));
        a->env = env_set(a, a->env, sym, val);
        journal_write(a->id, "SETQ", val);
        return val;
    }
    if (op == S_PROGN) {
        res = S_NIL;
        while (TAG(args) == TAG_CONS) {
            res = eval(a, car(a, args));
            args = cdr(a, args);
        }
        return res;
    }

    if (op == S_CAR) return car(a, eval(a, car(a, args)));
    if (op == S_CDR) return cdr(a, eval(a, car(a, args)));
    if (op == S_CONS) {
        word x = eval(a, car(a, args));
        word y = eval(a, car(a, cdr(a, args)));
        return cons(a, x, y);
    }
    if (op == S_EVAL) return eval(a, eval(a, car(a, args)));

    {
        word fn = eval(a, op);
        word evargs = eval_list(a, args);
        return apply(a, fn, evargs);
    }
}

static word agent_eval(legacy_agent_t *a, word expr)
{
    return eval(a, expr);
}

/* ---------- Message passing over pipe ---------- */
static int send_msg(legacy_agent_t *from, legacy_agent_t *to, const char *text)
{
    char buf[MSG_MAX];
    int n;
    if (to->pipe_fd[1] < 0) return -1;
    sprintf(buf, "%lu:%s", from->id, text);
    n = strlen(buf) + 1;
    if (write(to->pipe_fd[1], buf, n) != n) return -1;
    journal_write(from->id, "SEND", (word)to->id);
    return 0;
}

static int recv_msg(legacy_agent_t *a, char *buf, int buflen)
{
    int n;
    if (a->pipe_fd[0] < 0) return -1;
    n = read(a->pipe_fd[0], buf, buflen - 1);
    if (n <= 0) return -1;
    buf[n] = '\0';
    journal_write(a->id, "RECV", 0);
    return n;
}

/* ---------- Agent process body (runs after fork) ---------- */
static void agent_main(legacy_agent_t *a)
{
    char msg[MSG_MAX];
    word expr;

    close(a->pipe_fd[1]);

    /* Bootstrap a self-modifying handler macro */
    {
        word params = cons(a, intern(a, "m"), S_NIL);
        word body = cons(a, S_PROGN,
                        cons(a, cons(a, S_SETQ,
                                    cons(a, intern(a, "last"),
                                         cons(a, intern(a, "m"), S_NIL))),
                             cons(a, intern(a, "m"), S_NIL)));
        word macro = cons(a, S_MACRO, cons(a, params, cons(a, body, S_NIL)));
        a->env = env_set(a, a->env, intern(a, "handler"), macro);
    }

    while (a->alive) {
        global_clock++;
        if (recv_msg(a, msg, MSG_MAX) > 0) {
            word msym = intern(a, msg);
            word call = cons(a, intern(a, "handler"),
                             cons(a, cons(a, S_QUOTE, cons(a, msym, S_NIL)), S_NIL));
            expr = eval(a, call);
            journal_write(a->id, "EVAL", expr);
        }
        sleep(1);
    }
    _exit(0);
}

/* ---------- Substrate: create agent ---------- */
static legacy_agent_t *create_agent(const char *name)
{
    legacy_agent_t *a;
    word *heap;

    if (agent_count >= MAX_AGENTS) return NULL;
    a = &agents[agent_count];
    memset(a, 0, sizeof(*a));
    a->id = (unsigned long)agent_count;
    strncpy(a->name, name, MAX_NAME - 1);
    a->alive = 1;

    heap = (word *)malloc(AGENT_WORDS * sizeof(word));
    if (!heap) return NULL;
    memset(heap, 0, AGENT_WORDS * sizeof(word));
    a->lisp_heap = heap;
    a->free_ptr = 1;
    a->heap_limit = AGENT_WORDS;
    a->eval = agent_eval;
    a->env = S_NIL;

    if (pipe(a->pipe_fd) < 0) {
        free(heap);
        return NULL;
    }

    sym_count[a->id] = 0;
    S_NIL = intern(a, "nil");
    S_T = intern(a, "t");
    S_QUOTE = intern(a, "quote");
    S_IF = intern(a, "if");
    S_LAMBDA = intern(a, "lambda");
    S_MACRO = intern(a, "macro");
    S_SETQ = intern(a, "setq");
    S_PROGN = intern(a, "progn");
    S_CAR = intern(a, "car");
    S_CDR = intern(a, "cdr");
    S_CONS = intern(a, "cons");
    S_EVAL = intern(a, "eval");
    S_MSG = intern(a, "msg");

    a->env = env_set(a, a->env, S_NIL, S_NIL);
    a->env = env_set(a, a->env, S_T, S_T);

    agent_count++;
    return a;
}

/* ---------- CBMC bounded harness ---------- */
#ifdef __CPROVER__
void cbmc_harness(void)
{
    legacy_agent_t *a;
    word e, res;
    int steps;

    a = create_agent("verify");
    CBMC_ASSUME(a != 0);
    CBMC_ASSUME(a->heap_limit == AGENT_WORDS);
    CBMC_ASSUME(a->free_ptr == 1);

    for (steps = 0; steps < 8; steps++) {
        CBMC_ASSUME(a->alive == 1 || a->alive == 0);
        if (!a->alive) break;

        e = cons(a, S_QUOTE, cons(a, MAKE_SYM(0), MAKE_NUM(0)));
        res = a->eval(a, e);

        CBMC_ASSERT(a->free_ptr >= 1);
        CBMC_ASSERT(a->free_ptr <= a->heap_limit);
        CBMC_ASSERT(TAG(res) <= TAG_FUNC);
    }

    CBMC_ASSERT(a->free_ptr <= AGENT_WORDS);
}

void cbmc_array_bounds_harness(void)
{
    legacy_agent_t *a;
    word e, res, i;
    int steps;

    a = create_agent("bounds");
    CBMC_ASSUME(a != 0);
    CBMC_ASSUME(a->free_ptr == 1);
    CBMC_ASSUME(a->heap_limit == AGENT_WORDS);

    for (steps = 0; steps < 6; steps++) {
        CBMC_ASSUME(a->alive == 1 || a->alive == 0);
        if (!a->alive) break;

        e = MAKE_NUM(0);
        for (i = 0; i < 3; i++) {
            e = cons(a, MAKE_NUM(i), e);
            CBMC_ASSERT(a->free_ptr <= a->heap_limit);
        }

        res = a->eval(a, cons(a, S_QUOTE, cons(a, e, MAKE_NUM(0))));

        CBMC_ASSERT(a->free_ptr >= 1);
        CBMC_ASSERT(a->free_ptr <= AGENT_WORDS);
        if (TAG(res) == TAG_CONS) {
            word idx = PAYLOAD(res);
            CBMC_ASSERT(idx >= 1);
            CBMC_ASSERT(idx + 1 < a->heap_limit);
        }
    }

    CBMC_ASSERT(a->free_ptr <= AGENT_WORDS);
}
#endif

/* ---------- Orchestrator entry ---------- */
int main(int argc, char **argv)
{
    legacy_agent_t *a0, *a1;
    const char *jpath = (argc > 1) ? argv[1] : "smaoe.journal";
    int i, status;

    journal = fopen(jpath, "ab");
    if (!journal) {
        perror("journal");
        return 1;
    }

    printf("SMAOE Hybrid C89+Lisp starting (journal=%s)\n", jpath);

    a0 = create_agent("sensor");
    a1 = create_agent("reasoner");
    if (!a0 || !a1) {
        fprintf(stderr, "create_agent failed\n");
        return 1;
    }

    a0->pid = fork();
    if (a0->pid == 0) {
        close(a0->pipe_fd[1]);
        agent_main(a0);
    }
    a1->pid = fork();
    if (a1->pid == 0) {
        agent_main(a1);
    }

    sleep(1);
    send_msg(a0, a1, "TEMP=42");
    sleep(1);
    send_msg(a0, a1, "REWRITE-HANDLER");
    sleep(1);
    send_msg(a1, a0, "ACK");

    for (i = 0; i < 5; i++) {
        global_clock++;
        sleep(1);
    }

    a0->alive = 0;
    a1->alive = 0;
    kill(a0->pid, SIGTERM);
    kill(a1->pid, SIGTERM);
    waitpid(a0->pid, &status, 0);
    waitpid(a1->pid, &status, 0);

    fclose(journal);
    printf("SMAOE finished. Journal written to %s\n", jpath);
    return 0;
}
