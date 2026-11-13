#ifndef NV2A_VERTEX_LAYOUT_H
#define NV2A_VERTEX_LAYOUT_H
#include <stdint.h>
/* NV097 array formats align inline attributes to their component width.
 * S1/S32K contain 16-bit components; CMP is one packed 32-bit normal. */
static inline uint32_t nv2a_vertex_component_bytes(uint32_t type)
{
    switch(type) {
    case 0:case 4:return 1;
    case 1:case 5:return 2;
    case 2:case 6:return 4;
    default:return 0;
    }
}
static inline uint32_t nv2a_vertex_value_bytes(uint32_t type,uint32_t count)
{
    if(!count)return 0;
    return type==0 || type==6?4:nv2a_vertex_component_bytes(type)*count;
}
static inline uint32_t nv2a_vertex_inline_attribute(uint32_t cursor,uint32_t type,
                                                  uint32_t count,uint32_t *offset)
{
    uint32_t component=nv2a_vertex_component_bytes(type);
    if(!component || !count){*offset=cursor;return cursor;}
    *offset=(cursor+component-1)&~(component-1);
    return *offset+nv2a_vertex_value_bytes(type,count);
}
#endif
