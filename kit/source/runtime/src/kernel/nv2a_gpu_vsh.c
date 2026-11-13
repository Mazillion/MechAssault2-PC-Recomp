/* Specialise each uploaded Xbox vertex program into native GPU instructions.
 * Register sources, paired MAC/ILU reads, masks, relative addressing and
 * output defaults match nv2a_vsh_interp.c, which remains the reference. */
#include "nv2a_gpu_vsh.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

typedef struct { char *data; size_t size, capacity; int failed; } Source;
static void emit(Source *s,const char *format,...)
{
    if(s->failed)return;
    va_list args;va_start(args,format);
    int n=vsnprintf(s->data+s->size,s->capacity-s->size,format,args);
    va_end(args);
    if(n<0 || (size_t)n>=s->capacity-s->size){s->failed=1;return;}
    s->size+=(size_t)n;
}
static uint32_t f(const uint32_t *i,int dw,int bit,int size){return(i[dw]>>bit)&((1u<<size)-1);}
static void source(char *out,size_t size,const uint32_t *i,int which)
{
    uint32_t neg,swz,mux,reg;
    if(which==0){neg=f(i,1,8,1);swz=f(i,1,0,8);mux=f(i,2,26,2);reg=f(i,2,28,4);}
    else if(which==1){neg=f(i,2,25,1);swz=f(i,2,17,8);mux=f(i,2,11,2);reg=f(i,2,13,4);}
    else{neg=f(i,2,10,1);swz=f(i,2,2,8);mux=f(i,3,28,2);reg=(f(i,2,0,2)<<2)|f(i,3,30,2);}
    char base[96];
    if(mux==1){if(reg==12)snprintf(base,sizeof(base),"opos");else snprintf(base,sizeof(base),"R[%u]",reg<12?reg:0);}
    else if(mux==2)snprintf(base,sizeof(base),"input.a%u",f(i,1,9,4));
    else {
        int ci=(int)f(i,1,13,8);
        if(f(i,3,1,1))snprintf(base,sizeof(base),"vertex_constants[((%d+a0)>=0 && (%d+a0)<192)?(%d+a0):0]",ci,ci,ci);
        else snprintf(base,sizeof(base),"vertex_constants[%d]",ci<192?ci:0);
    }
    const char *sw="xyzw";
    snprintf(out,size,"%s(%s.%c%c%c%c)",neg?"-":"",base,
             sw[(swz>>6)&3],sw[(swz>>4)&3],sw[(swz>>2)&3],sw[swz&3]);
}
static void masked(Source *s,const char *dest,const char *value,uint32_t mask)
{
    const char *sw="xyzw";
    for(int j=0;j<4;j++)if(mask&(8u>>j))emit(s,"%s.%c=%s.%c;\n",dest,sw[j],value,sw[j]);
}
char *nv2a_gpu_vsh_source(const Nv2aVshProgram *program)
{
    if(!program->count || program->count>NV2A_VSH_SLOTS)return NULL;
    Source s={0};s.capacity=256*1024;s.data=malloc(s.capacity);
    if(!s.data)return NULL;
    s.data[0]=0;
    emit(&s,"cbuffer VertexConstants:register(b1){float4 vertex_constants[192];float4 vsh_fog;uint4 vsh_flags;};\n");
    emit(&s,"struct RawVertex {\n");
    for(int j=0;j<16;j++)emit(&s,"float4 a%d:ATTR%d;\n",j,j);
    emit(&s,"};\nfloat4 nv_mul(float4 a,float4 b){float4 r=a*b;return float4(a.x==0||b.x==0?0:r.x,a.y==0||b.y==0?0:r.y,a.z==0||b.z==0?0:r.z,a.w==0||b.w==0?0:r.w);}\nPixel program_vertex_main(RawVertex input){\nfloat4 R[12];float4 O[13];float4 opos=float4(0,0,0,1);int a0=0;\n");
    emit(&s,"[unroll]for(int j=0;j<12;j++)R[j]=0;[unroll]for(int j=0;j<13;j++)O[j]=0;O[3].w=1;O[4].w=1;O[9].w=1;O[10].w=1;O[11].w=1;O[12].w=1;\n");
    for(uint32_t slot=0;slot<program->count;slot++) {
        const uint32_t *i=program->words[slot];
        uint32_t mac=f(i,1,21,4),ilu=f(i,1,25,3),td=f(i,3,20,4);
        uint32_t om=f(i,3,12,4),oa=f(i,3,3,8);
        if(om && !f(i,3,11,1) && program->context_write){free(s.data);return NULL;}
        char a[160],b[160],c[160],dest[32];
        source(a,sizeof(a),i,0);source(b,sizeof(b),i,1);source(c,sizeof(c),i,2);
        emit(&s,"{float4 a=%s,b=%s,c=%s;float4 m=0,u=0;\n",a,b,c);
        switch(mac) {
        case 1:emit(&s,"m=a;\n");break;
        case 2:emit(&s,"m=nv_mul(a,b);\n");break;
        case 3:emit(&s,"m=a+c;\n");break;
        case 4:emit(&s,"m=nv_mul(a,b)+c;\n");break;
        case 5:emit(&s,"m=dot(a.xyz,b.xyz).xxxx;\n");break;
        case 6:emit(&s,"m=(dot(a.xyz,b.xyz)+b.w).xxxx;\n");break;
        case 7:emit(&s,"m=dot(a,b).xxxx;\n");break;
        case 8:emit(&s,"m=float4(1,a.y*b.y,a.z,b.w);\n");break;
        case 9:emit(&s,"m=min(a,b);\n");break;
        case 10:emit(&s,"m=max(a,b);\n");break;
        case 11:emit(&s,"m=float4(a.x<b.x,a.y<b.y,a.z<b.z,a.w<b.w);\n");break;
        case 12:emit(&s,"m=float4(a.x>=b.x,a.y>=b.y,a.z>=b.z,a.w>=b.w);\n");break;
        case 13:emit(&s,"a0=(int)floor(a.x+.001);m=a;\n");break;
        default:if(mac)emit(&s,"m=a;\n");break;
        }
        switch(ilu) {
        case 1:emit(&s,"u=c;\n");break;
        case 2:emit(&s,"u=(1/c.x).xxxx;\n");break;
        case 3:emit(&s,"float r=1/c.x;r=(r<0?-1:1)*clamp(abs(r),5.42101e-20,1.8446743e19);u=r.xxxx;\n");break;
        case 4:emit(&s,"u=rsqrt(abs(c.x)).xxxx;\n");break;
        case 5:emit(&s,"u=float4(exp2(floor(c.x)),frac(c.x),exp2(c.x),1);\n");break;
        case 6:emit(&s,"float ax=abs(c.x),lg=log2(ax),e=floor(lg);u=float4(e,ax==0?1:ax/exp2(e),lg,1);\n");break;
        case 7:emit(&s,"u=float4(1,max(c.x,0),c.x>0?pow(max(c.y,0),clamp(c.w,-127.9961,127.9961)):0,1);\n");break;
        }
        if(mac && mac!=13 && td<=12 && !(ilu && td==1)) {
            if(td==12)snprintf(dest,sizeof(dest),"opos");else snprintf(dest,sizeof(dest),"R[%u]",td);
            masked(&s,dest,"m",f(i,3,24,4));
        }
        uint32_t it=mac?1:td;
        if(ilu && it<=12) {
            if(it==12)snprintf(dest,sizeof(dest),"opos");else snprintf(dest,sizeof(dest),"R[%u]",it);
            masked(&s,dest,"u",f(i,3,16,4));
        }
        if(om && f(i,3,11,1) && oa<13) {
            if(oa==0)snprintf(dest,sizeof(dest),"opos");else snprintf(dest,sizeof(dest),"O[%u]",oa);
            if(oa==5) {
                /* The fog output is scalar: choose the first enabled lane. */
                int lane=(om&8)?0:(om&4)?1:(om&2)?2:3;
                emit(&s,"O[5].x=%s.%c;\n",f(i,3,2,1)?"u":"m","xyzw"[lane]);
            } else masked(&s,dest,f(i,3,2,1)?"u":"m",om);
        }
        emit(&s,"}\n");
    }
    emit(&s,"Vertex v;v.pos=opos;v.d0=O[3];v.d1=O[4];v.fog=O[5];v.t0=O[9];v.t1=O[10];v.t2=O[11];v.t3=O[12];\n");
    emit(&s,"float distance=v.fog.x,f=distance;float exceptional=(vsh_flags.y==0x800||vsh_flags.y==0x2601)?1:0;if(!vsh_flags.x)f=1;else if(vsh_flags.y==0x800||vsh_flags.y==0x802)f=vsh_fog.x+exp2(f*vsh_fog.y*16)-1.5;else if(vsh_flags.y==0x801||vsh_flags.y==0x803)f=vsh_fog.x+exp2(-f*f*vsh_fog.y*vsh_fog.y*32)-1.5;else f=vsh_fog.x+f*vsh_fog.y-1;if(vsh_flags.y>=0x802&&vsh_flags.y<=0x804)f=abs(f);if(vsh_flags.x&&(isinf(distance)||nv_isnan(f)))f=exceptional;v.fog.x=clamp(f,-asfloat(0x7F7FFFFFu),asfloat(0x7F7FFFFFu));\nreturn vertex_main(v);}\n");
    if(s.failed){free(s.data);return NULL;}
    return s.data;
}
