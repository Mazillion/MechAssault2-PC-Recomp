/* getenv, cached. The bot code asks for the same RECOMP_* switches every tick (env_on / env_int and
 * plain getenv in the controller, brain and world view), and the CRT's getenv takes its lock and
 * walks the whole environment block each time: a profile of a 6v6 match (2026-10-14) found a large
 * share of the game thread inside those calls. The environment does not change while the game runs,
 * so each name is looked up once, keyed by the address of its string literal. Include after
 * <stdlib.h>; every later getenv(...) in the unit goes through the cache. */
#ifndef MA2_BOT_ENV_H
#define MA2_BOT_ENV_H
#include <stdlib.h>
#include <stdint.h>
#include <intrin.h>

static const char *ma2_bot_getenv_cached(const char *name)
{
    static struct { const char *volatile key; const char *val; } tab[512];
    uint32_t h = (uint32_t)(((uintptr_t)name >> 3) * 2654435761u) & 511u;
    for (int k = 0; k < 512; k++, h = (h + 1u) & 511u) {
        const char *key = tab[h].key;
        if (key == name) return tab[h].val;
        if (!key) {
            const char *v = getenv(name);
            tab[h].val = v; _ReadWriteBarrier(); tab[h].key = name;   /* the value is in place before the key shows */
            return v;
        }
    }
    return getenv(name);                                               /* table full: no caching */
}
#define getenv(n) ma2_bot_getenv_cached(n)
#endif
