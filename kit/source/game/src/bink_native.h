#pragma once
#include <stdint.h>
/* Guest timers, file I/O and audio remain owned by the retail movie player.
 * The native codec consumes the same compressed video packet at each frame. */
int ma2_bink_decode(uint32_t movie, uint32_t video);
int ma2_bink_copy(uint32_t movie,uint32_t destination,int pitch,int height,
                  int x,int y,uint32_t flags);
void ma2_bink_release(uint32_t movie);
/* Associate only a successful retail BinkOpen with its original filename. */
void ma2_bink_opened(uint32_t movie,uint32_t name,uint32_t flags);
