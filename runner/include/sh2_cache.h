#ifndef SATURN_SH2_CACHE_H
#define SATURN_SH2_CACHE_H

#include "saturn.h"

static inline void sh2_cache_touch(sh2 *c, unsigned set, unsigned way)
{
    static const uint8_t and_mask[4] = { 0x07u, 0x19u, 0x2Au, 0x34u };
    static const uint8_t or_mask [4] = { 0x00u, 0x20u, 0x14u, 0x0Bu };
    c->cache_lru[set] = (uint8_t)((c->cache_lru[set] & and_mask[way]) |
                                  or_mask[way]);
}

/* The SH7604 cache is write-through, without allocation on a write miss.
 * Update only the writing CPU's matching line. Burning Rangers patches its
 * cached transform instructions; the next fetch must see those local stores.
 * Other-core stores, DMA and cache-through aliases must leave the line private
 * (Sonic 3D Blast executes an old slave overlay while the master replaces RAM).
 * Call with an aligned address and a byte count of 1, 2 or 4. */
static inline void sh2_cache_write(sh2 *c, uint32_t a, uint32_t v, unsigned size)
{
    unsigned set, way;
    uint32_t tag;
    if ((a >> 29) != 0u || !(c->onchip[0x92] & 1u)) return;
    set = (a >> 4) & 63u;
    tag = (a >> 10) & 0x7FFFFu;
    for (way = 0; way < 4; way++) {
        if (c->cache_valid[set][way] && c->cache_tag[set][way] == tag) {
            uint8_t *p = &c->cache_data[(way << 10) | (set << 4) | (a & 15u)];
            for (unsigned i = 0; i < size; i++)
                p[i] = (uint8_t)(v >> ((size - 1u - i) * 8u));
            sh2_cache_touch(c, set, way);
            /* A store can touch another way in the instruction's current set.
             * Make the next fetch restore its own LRU position. */
            c->if_cache_base = 1u;
            return;
        }
    }
}

#endif
