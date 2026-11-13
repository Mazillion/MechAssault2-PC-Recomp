#ifndef MA2_AUDIO_LIFETIME_H
#define MA2_AUDIO_LIFETIME_H
#include <stdint.h>
int ma2_audio_deferred_retain(uint32_t object);
uint32_t ma2_audio_ref_add(uint32_t object);
uint32_t ma2_audio_ref_drop(uint32_t object);
#endif
