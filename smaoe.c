/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

/* smaoe.c - Sovereign Multi-Agent Orchestration Engine
 * Pure C11, zero external dependencies beyond libc.
 * Compile: gcc -std=c11 -O2 -pthread -o smaoe smaoe.c
 * Or freestanding-ish: gcc -std=c11 -O2 -ffreestanding -nostdlib ... (with minimal stubs)
 */

#define _POSIX_C_SOURCE 200809L
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <pthread.h>
#include <time.h>
#include <errno.h>
#include <stdatomic.h>

/* Compile-time bounds (formal verification friendly) */
#define MAX_AGENTS 32
#define MSG_SLOT_SIZE 256
#define RING_SLOTS 256
#define AGENT_ARENA_SIZE (64 * 1024)
#define JOURNAL_SIZE (4 * 1024 * 1024)
#define CONTROL_PAGE_SIZE 4096
#define MAGIC 0x534D414F45ULL /* "SMAOE" */

typedef enum {
    ST_IDLE = 0,
    ST_READY,
    ST_RUNNING,
    ST_WAITING,
    ST_HALTED,
    ST_ERROR
} agent_state_t;

typedef struct {
    uint32_t src;
    uint32_t dst;
    uint32_t type;
    uint32_t len;
    uint8_t payload[MSG_SLOT_SIZE - 16];
} message_t;

typedef struct {
    uint32_t id;
    agent_state_t state;
    uint32_t priority;
    uint64_t last_tick;
    uint8_t *arena;
    size_t arena_used;
    size_t arena_limit;
    atomic_uint inbox_head;
    atomic_uint inbox_tail;
    uint32_t flags;
} agent_t;

typedef struct {
    uint64_t magic;
    uint32_t version;
    atomic_ullong logical_clock;
    uint32_t agent_count;
    uint32_t max_agents;
    atomic_uint ring_head;
    atomic_uint ring_tail;
    uint64_t journal_offset;
    agent_t agents[MAX_AGENTS];
    message_t ring[RING_SLOTS];
    uint8_t journal[JOURNAL_SIZE];
} global_t;

static global_t *G = NULL;
static int journal_fd = -1;
static void *map_base = NULL;
static size_t map_size = 0;

/* ---------- Journal (write-ahead, local only) ---------- */
static void journal_append(uint32_t agent_id, agent_state_t from, agent_state_t to) {
    if (G->journal_offset + 32 > JOURNAL_SIZE) {
        G->journal_offset = 0;
    }
    uint8_t *p = G->journal + G->journal_offset;
    uint64_t ts = atomic_load(&G->logical_clock);
    memcpy(p + 0, &ts, 8);
    memcpy(p + 8, &agent_id, 4);
    memcpy(p + 12, &from, 4);
    memcpy(p + 16, &to, 4);
    uint32_t csum = (uint32_t)(ts ^ agent_id ^ from ^ to);
    memcpy(p + 20, &csum, 4);
    G->journal_offset += 32;

    if (journal_fd >= 0) {
        pwrite(journal_fd, p, 32, (off_t)(G->journal_offset - 32));
        fdatasync(journal_fd);
    }
}

/* ---------- Deterministic transition function ---------- */
static agent_state_t transition(agent_t *a, const message_t *msg) {
    agent_state_t cur = a->state;
    agent_state_t next = cur;

    switch (cur) {
    case ST_IDLE:
        if (msg && msg->type == 1) next = ST_READY;
        break;
    case ST_READY:
        next = ST_RUNNING;
        break;
    case ST_RUNNING:
        if (msg && msg->type == 2) next = ST_WAITING;
        else if (a->arena_used > a->arena_limit * 3 / 4) next = ST_ERROR;
        else next = ST_READY;
        break;
    case ST_WAITING:
        if (msg && msg->type == 3) next = ST_READY;
        break;
    case ST_ERROR:
        next = ST_HALTED;
        break;
    case ST_HALTED:
        break;
    }
    return next;
}

/* ---------- Message ring (lock-free SPSC-style) ---------- */
static int ring_push(const message_t *m) {
    uint32_t head = atomic_load(&G->ring_head);
    uint32_t next = (head + 1) % RING_SLOTS;
    if (next == atomic_load(&G->ring_tail)) return -1;
    G->ring[head] = *m;
    atomic_store(&G->ring_head, next);
    return 0;
}

static int ring_pop(message_t *out) {
    uint32_t tail = atomic_load(&G->ring_tail);
    if (tail == atomic_load(&G->ring_head)) return -1;
    *out = G->ring[tail];
    atomic_store(&G->ring_tail, (tail + 1) % RING_SLOTS);
    return 0;
}

/* ---------- Arena (bump only) ---------- */
static void *arena_alloc(agent_t *a, size_t n) {
    n = (n + 7) & ~7ULL;
    if (a->arena_used + n > a->arena_limit) return NULL;
    void *p = a->arena + a->arena_used;
    a->arena_used += n;
    return p;
}

/* ---------- Scheduler (single-threaded deterministic core) ---------- */
static void scheduler_tick(void) {
    uint64_t clock = atomic_fetch_add(&G->logical_clock, 1) + 1;

    message_t msg;
    while (ring_pop(&msg) == 0) {
        if (msg.dst < G->agent_count) {
            agent_t *a = &G->agents[msg.dst];
            agent_state_t old = a->state;
            agent_state_t neu = transition(a, &msg);
            if (neu != old) {
                journal_append(a->id, old, neu);
                a->state = neu;
            }
            a->last_tick = clock;
        }
    }

    for (uint32_t i = 0; i < G->agent_count; i++) {
        agent_t *a = &G->agents[i];
        if (a->state == ST_READY || a->state == ST_RUNNING) {
            agent_state_t old = a->state;
            agent_state_t neu = transition(a, NULL);
            if (neu != old) {
                journal_append(a->id, old, neu);
                a->state = neu;
            }
            a->last_tick = clock;
        }
    }
}

/* ---------- Agent worker (optional multi-thread) ---------- */
static void *agent_thread(void *arg) {
    uint32_t id = (uint32_t)(uintptr_t)arg;
    agent_t *a = &G->agents[id];

    while (a->state != ST_HALTED) {
        struct timespec ts = {0, 5 * 1000 * 1000};
        nanosleep(&ts, NULL);
    }
    return NULL;
}

/* ---------- Initialization ---------- */
static int smaoe_init(const char *journal_path) {
    map_size = sizeof(global_t);
    map_base = mmap(NULL, map_size, PROT_READ | PROT_WRITE,
                    MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (map_base == MAP_FAILED) return -1;
    G = (global_t *)map_base;
    memset(G, 0, sizeof(*G));

    G->magic = MAGIC;
    G->version = 1;
    G->max_agents = MAX_AGENTS;
    atomic_store(&G->logical_clock, 0);

    if (journal_path) {
        journal_fd = open(journal_path, O_RDWR | O_CREAT, 0600);
        if (journal_fd >= 0) {
            ftruncate(journal_fd, JOURNAL_SIZE);
        }
    }

    for (uint32_t i = 0; i < 4; i++) {
        agent_t *a = &G->agents[i];
        a->id = i;
        a->state = ST_IDLE;
        a->priority = 10 - i;
        a->arena = (uint8_t *)malloc(AGENT_ARENA_SIZE);
        a->arena_limit = AGENT_ARENA_SIZE;
        a->arena_used = 0;
        atomic_store(&a->inbox_head, 0);
        atomic_store(&a->inbox_tail, 0);
        G->agent_count++;
    }

    return 0;
}

/* ---------- Public API ---------- */
int smaoe_send(uint32_t src, uint32_t dst, uint32_t type,
               const void *payload, uint32_t len) {
    if (len > sizeof(((message_t *)0)->payload)) return -1;
    message_t m = {0};
    m.src = src;
    m.dst = dst;
    m.type = type;
    m.len = len;
    if (payload && len) memcpy(m.payload, payload, len);
    return ring_push(&m);
}

void smaoe_run(uint64_t ticks) {
    for (uint64_t t = 0; t < ticks; t++) {
        scheduler_tick();
    }
}

/* ---------- Standalone main (demo) ---------- */
int main(int argc, char **argv) {
    const char *jpath = (argc > 1) ? argv[1] : "smaoe.journal";
    if (smaoe_init(jpath) != 0) {
        perror("init");
        return 1;
    }

    printf("SMAOE started. Agents: %u Clock: %llu\n",
           G->agent_count, (unsigned long long)atomic_load(&G->logical_clock));

    smaoe_send(999, 0, 1, "WAKE", 4);

    for (int i = 0; i < 20; i++) {
        smaoe_run(1);
        printf("tick %d clock=%llu agent0.state=%d\n",
               i, (unsigned long long)atomic_load(&G->logical_clock),
               G->agents[0].state);
        usleep(10000);
    }

    for (uint32_t i = 0; i < G->agent_count; i++) {
        G->agents[i].state = ST_HALTED;
    }

    if (journal_fd >= 0) close(journal_fd);
    munmap(map_base, map_size);
    return 0;
}
