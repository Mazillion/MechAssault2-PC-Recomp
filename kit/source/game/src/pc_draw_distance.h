#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* 50 preserves the authored scene. Endpoints are half / twice its distance. */
float ma2_draw_distance_scale(int value);
float ma2_draw_distance_far(float near_plane, float far_plane, int value);
void ma2_draw_distance_fog(float *start, float *end, int value);
float ma2_pc_camera_distance(uint32_t camera, uint32_t caller);
void ma2_pc_fog_distance(uint32_t environment, volatile float *start, volatile float *end);
void ma2_pc_lightmap_parameters(uint32_t camera, volatile float *far_plane, volatile float *horizontal_fov);
float ma2_pc_lightmap_extent(uint32_t camera, float far_plane, float authored_extent);
float ma2_pc_lod_distance_squared(float distance_squared);
#ifdef __cplusplus
}
#endif
