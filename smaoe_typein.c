/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

/* SMAOE Magazine Type-In v1.2 - Pipe Macro Expansion + Error Handling
 * gcc -std=c89 -pedantic -Wall -Wextra -o smaoe_typein smaoe_typein.c
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/wait.h>
#include <sys/types.h>

#define ARENA_WORDS 4096
#define AGENTS 2
#define MSG_MAX 128

typedef unsigned long word;
static word arena[ARENA_WORDS];
static word *fp;
static int pfd[AGENTS][2];

/* ----- Magazine Hex Image ----- */
static const char *heximg[] = {
"4C495350",
"00010000",
"00000020",
"90010000",
"91020000",
"A2010000",
"C1000000",
"FF000000",
"00000000"
};
#define HEX_LINES (sizeof(heximg)/sizeof(heximg[0]))
#define EXPECTED_CS 0x3E8B2F1AUL

static unsigned long checksum(const char **lines, int n)
{
    unsigned long c = 0xA5A5A5A5UL;
    int i, j;
    for (i = 0; i < n; i++) {
        const char *s = lines[i];
        for (j = 0; s[j]; j++)
            c = (c << 3) ^ (c >> 29) ^ (unsigned char)s[j];
    }
    return c;
}

static word hexword(const char *s)
{
    word v = 0;
    int i;
    for (i = 0; i < 8; i++) {
        char c = s[i];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= c - '0';
        else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
        else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
    }
    return v;
}

static word *load_image(void)
{
    unsigned long cs = checksum(heximg, HEX_LINES);
    int i, pos = 0;
    if (cs != EXPECTED_CS) {
        fprintf(stderr, "CHECKSUM FAIL %08lX\n", (unsigned long)cs);
        exit(1);
    }
    for (i = 0; i < HEX_LINES && pos < ARENA_WORDS-16; i++)
        arena[pos++] = hexword(heximg[i]);
    fp = &arena[pos];
    return &arena[2];
}

static word *alloc(int n)
{
    word *p;
    if (fp + n > arena + ARENA_WORDS) return 0;
    p = fp;
    fp += n;
    return p;
}

/* Pipe message -> macro expansion */
static void macro_expand_from_pipe(int id, const char *msg)
{
    word *cell;
    unsigned long h = 0;
    int i;
    for (i = 0; msg[i] && msg[i] != '\n'; i++)
        h = (h * 33) + (unsigned char)msg[i];
    h &= 0xFFFF;
    cell = alloc(3);
    if (!cell) {
        fprintf(stderr, "agent%d: arena full\n", id);
        return;
    }
    cell[0] = 0x4D414352UL;
    cell[1] = h;
    cell[2] = (word)id;
    printf("agent%d: macro-expanded '%s' -> sym %04lX @ %p\n",
           id, msg, h, (void*)cell);
}

static void run_kernel(word *entry)
{
    word *pc = entry;
    word r[4];
    memset(r, 0, sizeof(r));
    for (;;) {
        word op = *pc++;
        word a = (op >> 24) & 0xFF;
        word b = (op >> 16) & 0xFF;
        word c = op & 0xFFFF;
        switch (a) {
        case 0x90: r[b] = c; break;
        case 0xA2: r[1] = r[1] + r[2]; break;
        case 0xC1: printf("kernel: %lu\n", r[1]); break;
        case 0xFF: return;
        default: return;
        }
    }
}

/* ---------- Robust pipe-read agent ---------- */
static void agent(int id)
{
    char buf[MSG_MAX];
    ssize_t n;

    if (close(pfd[id][1]) < 0) {
        fprintf(stderr, "agent%d: close write-end failed: %s\n",
                id, strerror(errno));
        _exit(1);
    }

    for (;;) {
        n = read(pfd[id][0], buf, sizeof(buf) - 1);

        if (n > 0) {
            buf[n] = '\0';
            macro_expand_from_pipe(id, buf);
            continue;
        }

        if (n == 0) {
            printf("agent%d: pipe EOF (writer closed)\n", id);
            break;
        }

        if (errno == EINTR) {
            continue;
        }

        fprintf(stderr, "agent%d: read error: %s\n", id, strerror(errno));
        break;
    }

    if (close(pfd[id][0]) < 0) {
        fprintf(stderr, "agent%d: close read-end failed: %s\n",
                id, strerror(errno));
    }
    _exit(0);
}

/* Parent-side write with error checking */
static int safe_write(int fd, const char *msg, size_t len)
{
    ssize_t n;
    size_t off = 0;

    while (off < len) {
        n = write(fd, msg + off, len - off);
        if (n > 0) {
            off += n;
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        fprintf(stderr, "parent: write error: %s\n", strerror(errno));
        return -1;
    }
    return 0;
}

int main(void)
{
    word *entry;
    pid_t kids[AGENTS];
    int i;
    char *msgs[] = { "TEMP=42\n", "REWRITE\n" };

    printf("SMAOE Type-In + Pipe Macro Expansion + Error Handling\n");
    entry = load_image();
    printf("Image OK. Entry %p Free %p\n", (void*)entry, (void*)fp);

    for (i = 0; i < AGENTS; i++) {
        if (pipe(pfd[i]) < 0) {
            fprintf(stderr, "pipe create failed: %s\n", strerror(errno));
            exit(1);
        }
        kids[i] = fork();
        if (kids[i] < 0) {
            fprintf(stderr, "fork failed: %s\n", strerror(errno));
            exit(1);
        }
        if (kids[i] == 0)
            agent(i);
        if (close(pfd[i][0]) < 0)
            fprintf(stderr, "parent: close read-end %d: %s\n",
                    i, strerror(errno));
    }

    for (i = 0; i < AGENTS; i++) {
        if (safe_write(pfd[i][1], msgs[i % 2],
                       strlen(msgs[i % 2]) + 1) < 0) {
            fprintf(stderr, "parent: failed to send to agent %d\n", i);
        }
    }

    run_kernel(entry);

    for (i = 0; i < AGENTS; i++) {
        if (close(pfd[i][1]) < 0)
            fprintf(stderr, "parent: close write-end %d: %s\n",
                    i, strerror(errno));
        waitpid(kids[i], 0, 0);
    }

    printf("Arena used: %ld words\n", (long)(fp - arena));
    return 0;
}
