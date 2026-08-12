#include "buddy.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define NULL_PTR ((void *)0)

#define MIN_RANK 1
#define MAX_RANK 16
#define PAGE_SIZE 4096UL
/* largest block: rank 16 => 2^(16-1) = 32768 pages (128 MiB) */
#define MAX_BLOCK_PAGES_LOG (MAX_RANK - 1)

/*
 * Buddy allocator over a pool of `pgcount` 4K pages.
 *
 * The pool is decomposed into power-of-two regions (each at most
 * 2^15 pages, i.e. rank 16).  A block of rank r covers 2^(r-1) pages.
 *
 * Per page index we store:
 *   g_rank[i]  - rank of the block if page i is a block head, else 0
 *   g_alloc[i] - 1 if page i is the head of an allocated block
 * Free block heads are tracked in one bitmap per rank so that the
 * lowest-address free block can be found quickly (keeps allocations
 * ascending in address order).
 */

static char *g_base;                          /* pool base address       */
static long g_npages;                         /* total number of pages   */
static long g_nwords;                         /* bitmap words            */
static unsigned char *g_rank;                 /* per-page head rank      */
static unsigned char *g_alloc;                /* per-page alloc flag     */
static uint64_t *g_freebits[MAX_RANK + 1];    /* free-head bitmaps       */
static int g_freecnt[MAX_RANK + 1];           /* free block counters     */

static void set_free_bit(int rank, long page)
{
    g_freebits[rank][page >> 6] |= (uint64_t)1 << (page & 63);
}

static void clear_free_bit(int rank, long page)
{
    g_freebits[rank][page >> 6] &= ~((uint64_t)1 << (page & 63));
}

/* find lowest-address free block head of given rank, -1 if none */
static long find_first_free(int rank)
{
    long w;
    for (w = 0; w < g_nwords; ++w) {
        uint64_t v = g_freebits[rank][w];
        if (v)
            return (w << 6) + __builtin_ctzll(v);
    }
    return -1;
}

/* insert a free block head into the structures */
static void add_free_block(long page, int rank)
{
    g_rank[page] = (unsigned char)rank;
    g_alloc[page] = 0;
    set_free_bit(rank, page);
    g_freecnt[rank]++;
}

/* remove a free block head from the structures */
static void remove_free_block(long page, int rank)
{
    clear_free_bit(rank, page);
    g_freecnt[rank]--;
}

int init_page(void *p, int pgcount)
{
    int r;
    long off, rem;

    if (!p || pgcount <= 0)
        return -EINVAL;

    /* reset any previous state */
    if (g_rank) {
        free(g_rank);
        g_rank = NULL_PTR;
    }
    if (g_alloc) {
        free(g_alloc);
        g_alloc = NULL_PTR;
    }
    for (r = MIN_RANK; r <= MAX_RANK; ++r) {
        if (g_freebits[r]) {
            free(g_freebits[r]);
            g_freebits[r] = NULL_PTR;
        }
        g_freecnt[r] = 0;
    }

    g_base = (char *)p;
    g_npages = pgcount;
    g_nwords = (pgcount + 63) / 64;

    g_rank = (unsigned char *)calloc((size_t)pgcount, 1);
    g_alloc = (unsigned char *)calloc((size_t)pgcount, 1);
    if (!g_rank || !g_alloc)
        return -ENOSPC;
    for (r = MIN_RANK; r <= MAX_RANK; ++r) {
        g_freebits[r] = (uint64_t *)calloc((size_t)g_nwords, sizeof(uint64_t));
        if (!g_freebits[r])
            return -ENOSPC;
    }

    /* decompose the pool into power-of-two regions, biggest first */
    off = 0;
    rem = pgcount;
    while (rem > 0) {
        int k = 0;
        long sz;
        while ((1L << (k + 1)) <= rem)
            k++;
        if (k > MAX_BLOCK_PAGES_LOG)
            k = MAX_BLOCK_PAGES_LOG;
        sz = 1L << k;
        add_free_block(off, k + 1);
        off += sz;
        rem -= sz;
    }

    return OK;
}

void *alloc_pages(int rank)
{
    int s;
    long h;

    if (rank < MIN_RANK || rank > MAX_RANK)
        return ERR_PTR(-EINVAL);
    if (!g_base)
        return ERR_PTR(-ENOSPC);

    s = rank;
    while (s <= MAX_RANK && g_freecnt[s] == 0)
        s++;
    if (s > MAX_RANK)
        return ERR_PTR(-ENOSPC);

    h = find_first_free(s);
    remove_free_block(h, s);

    /* split down to the requested rank, always keeping the left half */
    while (s > rank) {
        long right;
        s--;
        right = h + (1L << (s - 1));
        add_free_block(right, s);
        g_rank[h] = (unsigned char)s;
    }

    g_alloc[h] = 1;
    return (void *)(g_base + h * PAGE_SIZE);
}

int return_pages(void *p)
{
    long diff, i;
    int r;

    if (!p || !g_base)
        return -EINVAL;
    diff = (char *)p - g_base;
    if (diff < 0 || diff >= g_npages * (long)PAGE_SIZE)
        return -EINVAL;
    if (diff % (long)PAGE_SIZE)
        return -EINVAL;
    i = diff / (long)PAGE_SIZE;

    r = g_rank[i];
    if (r < MIN_RANK || r > MAX_RANK || !g_alloc[i])
        return -EINVAL;

    g_alloc[i] = 0;

    /* coalesce with buddies as far as possible */
    while (r < MAX_RANK) {
        long sz = 1L << (r - 1);
        long b = i ^ sz;
        if (b + sz > g_npages)
            break;                       /* buddy does not exist in pool */
        if (g_rank[b] != r || g_alloc[b])
            break;                       /* buddy not a free block of rank r */
        remove_free_block(b, r);
        g_rank[b] = 0;
        g_rank[i] = 0;
        if (b < i)
            i = b;
        r++;
    }

    add_free_block(i, r);
    return OK;
}

int query_ranks(void *p)
{
    long diff, i;
    int r;

    if (!p || !g_base)
        return -EINVAL;
    diff = (char *)p - g_base;
    if (diff < 0 || diff >= g_npages * (long)PAGE_SIZE)
        return -EINVAL;
    if (diff % (long)PAGE_SIZE)
        return -EINVAL;
    i = diff / (long)PAGE_SIZE;

    for (r = MIN_RANK; r <= MAX_RANK; ++r) {
        long sz = 1L << (r - 1);
        long h = i & ~(sz - 1);
        if (g_rank[h] == r)
            return r;
    }
    return -EINVAL;
}

int query_page_counts(int rank)
{
    if (rank < MIN_RANK || rank > MAX_RANK)
        return -EINVAL;
    return g_freecnt[rank];
}
