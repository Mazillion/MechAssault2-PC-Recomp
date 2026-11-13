#ifndef MA2_TANK_TRACE_H
#define MA2_TANK_TRACE_H
#include <stdint.h>
/* Disabled unless all three explicit diagnostic environment settings validate.
   Reads guest state only; does not change guest registers, flags or memory. */
void ma2_tank_trace(uint32_t site,uint32_t actor,uint32_t root,uint32_t argument,uint32_t value);
#endif
