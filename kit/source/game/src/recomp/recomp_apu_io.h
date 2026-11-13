#ifndef MA2_RECOMP_APU_IO_H
#define MA2_RECOMP_APU_IO_H
/* Selected lifted DirectSound functions perform ordinary memory operations
 * and register operations. Keep both routes exact, while avoiding a Windows
 * access violation and host instruction decode for every hardware access. */
uint32_t apu_guest_read32(uint32_t address);
void apu_guest_write32(uint32_t address,uint32_t value);
static __forceinline int ma2_apu_register(uint32_t address)
{
    uint32_t offset=address-0xFE800000u;
    return offset<0x30000 || offset-0x3F000u<0x1000 || offset-0x5F000u<0x1000;
}
static __forceinline void ma2_store32(uint32_t address,uint32_t value)
{
    if(ma2_apu_register(address))apu_guest_write32(address,value);
    else MEM32(address)=value;
}
static __forceinline uint32_t ma2_load32(uint32_t address)
{
    if(ma2_apu_register(address))return apu_guest_read32(address);
    return MEM32(address);
}
#endif
