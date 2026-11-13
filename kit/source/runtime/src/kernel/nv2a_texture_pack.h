#pragma once
#include "nv2a_gpu_d3d11.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Source fingerprints belong to an upload, never to a guest address alone.
 * The GPU texture cache clears this when the source bytes change. */
typedef struct {
    uint8_t digest[32];
    uint32_t valid, entry;
    uint64_t generation;
} Nv2aTexturePackKey;
struct ID3D11Device;
struct ID3D11ShaderResourceView;
/* Called on the render thread at a frame boundary. */
void nv2a_texture_pack_frame(void);
/* Release only device resources; retain indexes and the selected quality. */
void nv2a_texture_pack_device_lost(void);
void nv2a_texture_pack_draw(void);
int nv2a_texture_pack_fingerprint(const Nv2aGpuTexture *,Nv2aGpuTexel,uint32_t face_stride,uint8_t digest[32]);
/* Borrowed view, retained by the bounded pack cache. Logical texture sizes
 * and UVs remain guest-owned; only the GPU resource and mip range change. */
struct ID3D11ShaderResourceView *nv2a_texture_pack_view(struct ID3D11Device *,
    const Nv2aGpuTexture *,Nv2aGpuTexel,uint32_t face_stride,Nv2aTexturePackKey *,uint32_t *levels,uint32_t *scale);
typedef struct {
    uint64_t hits, misses, loads, failures, cache_bytes;
    uint32_t quality, entries;
} Nv2aTexturePackStats;
/* Renderer owner only, like frame/view/device_lost. CPU work diagnostics use
 * nv2a_gpu_cpu_take on that same thread; neither getter snapshots workers. */
Nv2aTexturePackStats nv2a_texture_pack_stats(void);
#ifdef __cplusplus
}
#endif
