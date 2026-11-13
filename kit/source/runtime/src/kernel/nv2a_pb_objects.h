#pragma once
#include <stdint.h>
uint32_t nv2a_pb_object_class(uint32_t handle);
int nv2a_pb_dma_address(uint32_t handle, uint32_t offset, uint32_t *va);
void nv2a_pb_pattern_color(uint32_t value);
void nv2a_pb_set_software_method(void (*callback)(uint32_t));
void nv2a_pb_software_method(uint32_t value);
/* A title's miniport callback can publish the buffer queued for scanout.
 * Called on the FIFO consumer, after the rendering commands for that buffer. */
void nv2a_pb_set_scanout(uint32_t va, uint32_t pitch);
int nv2a_pb_get_scanout(uint32_t *va, uint32_t *pitch);
