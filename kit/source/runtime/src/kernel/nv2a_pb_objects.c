/* PFIFO object handles refer to RAMHT entries, then PRAMIN contexts. The
 * formats are defined by NV2A (see src/nv2a/nv2a_regs.h and xemu pfifo.c).
 * Resolve at object binding time, not for every submitted vertex. */
#include "nv2a_pb_objects.h"
#include <stddef.h>
extern ptrdiff_t xbox_GetMemoryOffset(void);
#define REG(o) (*(volatile uint32_t *)(xbox_GetMemoryOffset() + (uintptr_t)0xFD000000u + (o)))

static int object_instance(uint32_t handle, uint32_t *instance)
{
    uint32_t config = REG(0x2210);
    uint32_t base = (config & 0x1F0u) << 8;
    uint32_t bytes = 4096u << ((config >> 16) & 3);
    if (base + bytes > 0x100000) return 0;
    for (uint32_t at = base; at < base + bytes; at += 8) {
        uint32_t context = REG(0x700000 + at + 4);
        if ((context & 0x80000000u) && REG(0x700000 + at) == handle) {
            *instance = (context & 0xFFFFu) << 4;
            return *instance <= 0x100000u - 16;
        }
    }
    return 0;
}
uint32_t nv2a_pb_object_class(uint32_t handle)
{
    uint32_t instance;
    return object_instance(handle, &instance) ? REG(0x700000 + instance) & 0xFFFu : 0;
}
int nv2a_pb_dma_address(uint32_t handle, uint32_t offset, uint32_t *va)
{
    uint32_t instance;
    if (!object_instance(handle, &instance)) return 0;
    uint32_t flags = REG(0x700000 + instance);
    uint32_t limit = REG(0x700000 + instance + 4);
    uint32_t frame = REG(0x700000 + instance + 8);
    uint32_t base = ((frame & 0xFFFFF000u) | (flags >> 20)) & 0x07FFFFFFu;
    if (offset > limit || limit - offset < 3 || base >= 0x04000000u ||
        offset > 0x04000000u - base - 4) return 0;
    *va = 0x80000000u + base + offset;
    return 1;
}
void nv2a_pb_pattern_color(uint32_t value)
{
    REG(0x400B10) = value;
}
static void (*software_method)(uint32_t);
static uint32_t scanout_va, scanout_pitch;
void nv2a_pb_set_software_method(void (*callback)(uint32_t))
{
    software_method = callback;
    scanout_va = scanout_pitch = 0;
}
void nv2a_pb_software_method(uint32_t value)
{
    if (value && software_method) software_method(value);
}
void nv2a_pb_set_scanout(uint32_t va, uint32_t pitch)
{
    if (va && pitch) { scanout_va = va; scanout_pitch = pitch; }
}
int nv2a_pb_get_scanout(uint32_t *va, uint32_t *pitch)
{
    if (!scanout_va || !scanout_pitch) return 0;
    *va = scanout_va; *pitch = scanout_pitch;
    return 1;
}
