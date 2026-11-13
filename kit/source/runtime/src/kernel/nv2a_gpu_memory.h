#ifndef NV2A_GPU_MEMORY_H
#define NV2A_GPU_MEMORY_H
#include <stddef.h>
#include <stdint.h>
/* Versioned texture sources. Both the physical and tiled CPU aliases are
 * observed, including ordinary guest stores outside resource-lock helpers. */
uint64_t nv2a_gpu_memory_serial(void);
uint64_t nv2a_gpu_memory_version(uint32_t va,size_t bytes);
uint64_t nv2a_gpu_memory_watch(uint32_t va,size_t bytes);
void nv2a_gpu_memory_changed(void *native,size_t bytes);
/* Physical DMA writes bypass CPU permissions while retaining cache coherence. */
int nv2a_gpu_memory_dma_write(void *native,const void *source,size_t bytes);
void nv2a_gpu_memory_reallocated(void *native,size_t bytes);
uint64_t nv2a_gpu_memory_allocation_serial(void);
uint64_t nv2a_gpu_memory_epoch(uint32_t va,size_t bytes);
uint32_t nv2a_gpu_memory_protection(void *native,uint32_t actual);
void nv2a_gpu_memory_shutdown(void);
#endif
