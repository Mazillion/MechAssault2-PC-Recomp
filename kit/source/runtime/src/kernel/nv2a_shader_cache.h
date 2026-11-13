#ifndef NV2A_SHADER_CACHE_H
#define NV2A_SHADER_CACHE_H
#if defined(_WIN32)
#include <windows.h>
#include <d3dcompiler.h>
#include <stdint.h>
#include <stddef.h>
typedef struct Nv2aShaderJob Nv2aShaderJob;
#define NV2A_SHADER_PENDING HRESULT_FROM_WIN32(ERROR_IO_PENDING)
Nv2aShaderJob *nv2a_shader_find(uint64_t key,unsigned kind);
HRESULT nv2a_shader_poll(Nv2aShaderJob *,ID3DBlob **code,ID3DBlob **errors);
HRESULT nv2a_shader_compile(const char *source,size_t length,const char *entry,const char *profile,
                            uint64_t key,unsigned kind,ID3DBlob **code,ID3DBlob **errors);
HRESULT nv2a_shader_compile_blocking(const char *source,size_t length,const char *entry,
                                     const char *profile,ID3DBlob **code,ID3DBlob **errors);

/* Shader preloading (PC setting "Pre-create shaders"; see nv2a_shader_cache.c). A key names a
 * compiled shader as the disk cache does: source hash, source length, kind (0 vertex, 1 pixel). */
#include <d3d11.h>
typedef struct Nv2aShaderKey { uint64_t hash; uint32_t length,kind; } Nv2aShaderKey;
extern volatile LONG nv2a_shader_level_epoch;   /* changes when a level starts loading */
void nv2a_shader_key(const char *source,size_t length,const char *entry,const char *profile,Nv2aShaderKey *key);
void nv2a_shader_job_key(const Nv2aShaderJob *job,Nv2aShaderKey *key);
void nv2a_shader_preload_device(ID3D11Device *device);        /* NULL: device released */
void nv2a_shader_level_begin(uint32_t level);                  /* any thread */
void nv2a_shader_note_level(const Nv2aShaderKey *key);         /* render thread: used in this level */
void nv2a_shader_resident(const Nv2aShaderKey *key,int delta); /* render thread: object in its tables */
ID3D11VertexShader *nv2a_shader_take_vertex(const Nv2aShaderKey *key,ID3D11InputLayout **layout);
ID3D11PixelShader *nv2a_shader_take_pixel(const Nv2aShaderKey *key);
#endif
#endif
