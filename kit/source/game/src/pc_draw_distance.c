#include "pc_draw_distance.h"
#include "platform/pc_frontend.h"
#include <math.h>
#include <float.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
extern ptrdiff_t g_xbox_mem_offset;
#define F32(a) (*(float *)(g_xbox_mem_offset+(a)))
#define U8(a) (*(uint8_t *)(g_xbox_mem_offset+(a)))

float ma2_draw_distance_scale(int value)
{
    if(value<0)value=0;
    if(value>100)value=100;
    return exp2f((value-50)/50.f);
}
float ma2_draw_distance_far(float near_plane,float far_plane,int value)
{
    if(value==50 || !isfinite(near_plane) || !isfinite(far_plane) || near_plane<=0 || far_plane<=near_plane)return far_plane;
    double scaled=(double)far_plane*ma2_draw_distance_scale(value);
    if(scaled>FLT_MAX)return far_plane;
    return fmaxf((float)scaled,nextafterf(near_plane,FLT_MAX));
}
void ma2_draw_distance_fog(float *start,float *end,int value)
{
    if(value==50 || !isfinite(*start) || !isfinite(*end) || *end<=*start || *end<=0)return;
    float scale=ma2_draw_distance_scale(value);
    if(fabs((double)*start*scale)>FLT_MAX || (double)*end*scale>FLT_MAX)return;
    *start*=scale;*end*=scale;
}
float ma2_pc_lod_distance_squared(float distance_squared)
{
    int value=pc_setting_get(PC_DRAW_DISTANCE,50);
    if(value==50 || !isfinite(distance_squared) || distance_squared<0)return distance_squared;
    /* NodeSwitchLod uses one squared distance for mesh selection, hysteresis
     * and fading. Adjust that input together, leaving authored thresholds,
     * world positions and collision bounds untouched. Cache the multiplier:
     * this callback visits many scene nodes every frame. */
    static int previous=-1;
    static float inverse_square=1;
    if(value!=previous){
        float scale=ma2_draw_distance_scale(value);
        inverse_square=1.f/(scale*scale);previous=value;
    }
    double result=(double)distance_squared*inverse_square;
    return result<=FLT_MAX?(float)result:distance_squared;
}
void ma2_pc_lightmap_parameters(uint32_t camera,volatile float *far_plane,volatile float *horizontal_fov)
{
    if(U8(camera+0x88))return;
    /* The terrain lightmap takes its origin from the real camera frustum,
     * but sizes its coverage using these separate camera queries. Use the
     * same far plane and horizontal view as the rendered projection. Keep
     * the authored FOV margin if it was already more conservative. */
    *far_plane=ma2_draw_distance_far(F32(camera+0x7C),*far_plane,pc_setting_get(PC_DRAW_DISTANCE,50));
    float projection_x=F32(camera+0x194);
    if(isfinite(projection_x) && projection_x>0 && F32(camera+0x1C0)==1.f &&
       isfinite(*horizontal_fov) && *horizontal_fov>0 && *horizontal_fov<180) {
        float rendered=atanf(1.f/projection_x)*(360.f/3.14159265358979323846f);
        *horizontal_fov=fmaxf(*horizontal_fov,rendered);
    }
}
float ma2_pc_lightmap_extent(uint32_t camera,float far_plane,float authored_extent)
{
    float px=F32(camera+0x194),py=F32(camera+0x1A8),near_plane=F32(camera+0x7C);
    if(U8(camera+0x88) || F32(camera+0x1C0)!=1.f || !isfinite(px) || !isfinite(py) ||
       px<=0 || py<=0 || !isfinite(far_plane) || !isfinite(near_plane) ||
       near_plane<=0 || far_plane<=near_plane || !isfinite(authored_extent))return authored_extent;
    /* The retail bound assumes a narrow frustum: its apex-to-far-corner
     * distance can be smaller than a wide far plane. Its diameter bounds
     * every world-axis span as the camera rotates. Use it before the original
     * level clamp and texel snapping, keeping that update logic intact. */
    double slope2=1.0/((double)px*px)+1.0/((double)py*py);
    double depth=far_plane-near_plane,sum=(double)far_plane+near_plane;
    double diameter=fmax(2.0*far_plane*sqrt(slope2),sqrt(depth*depth+sum*sum*slope2));
    return diameter<=FLT_MAX?fmaxf(authored_extent,(float)diameter):authored_extent;
}
float ma2_pc_camera_distance(uint32_t camera,uint32_t caller)
{
    float original=F32(camera+0x80),near_plane=F32(camera+0x7C);
    int value=pc_setting_get(PC_DRAW_DISTANCE,50);
    /* The bridge restores this authored value after the original builder has
     * produced its projection, inverse and CPU frustum. Never feed a scaled
     * value back into camera animation or level state. Orthographic cameras
     * include shadow maps and must keep their volume. */
    if(!U8(camera+0x88))F32(camera+0x80)=ma2_draw_distance_far(near_plane,original,value);
    static int tracing=-1;
    if(tracing<0)tracing=getenv("RECOMP_DRAW_DISTANCE_TRACE")!=NULL;
    if(tracing){
        static unsigned count;static int last_value=-1;
        if(value!=last_value){count=0;last_value=value;}
        if(count++<40 || count%2400==0)fprintf(stderr,"[DRAW-DISTANCE] camera=%08X caller=%08X active=%u ortho=%u value=%d near=%.6g far=%.6g effective=%.6g\n",camera,caller,U8(camera+0x10),U8(camera+0x88),value,near_plane,original,F32(camera+0x80));
    }
    return original;
}
void ma2_pc_fog_distance(uint32_t environment,volatile float *start,volatile float *end)
{
    int value=pc_setting_get(PC_DRAW_DISTANCE,50);
    float original_start=*start,original_end=*end;
    /* Stack arguments to the environment's render call, after interpolation;
     * source fog, color, height and transition timing remain game-owned. */
    float scaled_start=original_start,scaled_end=original_end;
    ma2_draw_distance_fog(&scaled_start,&scaled_end,value);
    *start=scaled_start;*end=scaled_end;
    static int tracing=-1;
    if(tracing<0)tracing=getenv("RECOMP_DRAW_DISTANCE_TRACE")!=NULL;
    if(tracing){
        static unsigned count;static int last_value=-1;
        if(value!=last_value){count=0;last_value=value;}
        if(count++<12 || count%600==0)fprintf(stderr,"[DRAW-DISTANCE] environment=%08X value=%d fog=%.6g,%.6g effective=%.6g,%.6g\n",environment,value,original_start,original_end,*start,*end);
    }
}
