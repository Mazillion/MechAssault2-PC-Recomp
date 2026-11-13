#ifndef NV2A_GPU_VSH_H
#define NV2A_GPU_VSH_H
#include "nv2a_vsh_interp.h"
/* Caller owns the HLSL string. NULL means a program needs CPU context writes. */
char *nv2a_gpu_vsh_source(const Nv2aVshProgram *program);
#endif
