#ifndef NV2A_GPU_WAIT_H
#define NV2A_GPU_WAIT_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Wait briefly while an observed completion word is unchanged. The caller
 * must recheck its original fence predicate, including after a spurious wake.
 * This never submits commands or changes the completion value. */
void nv2a_gpu_wait_word(volatile uint32_t *word,uint32_t observed);
/* Publish an actually executed semaphore, then wake its native waiters. */
void nv2a_gpu_publish_word(volatile uint32_t *word,uint32_t value);
#ifdef __cplusplus
}
#endif
#endif
