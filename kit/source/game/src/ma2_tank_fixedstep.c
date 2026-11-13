#define RECOMP_GENERATED_CODE
#include "recomp/gen/recomp_types.h"
#include <stdlib.h>
#include <math.h>
RECOMP_TLS Ma2TankStepScope ma2_tank_step_scope;
RECOMP_TLS unsigned ma2_tank_step_swapped;
#define SLOTS 2048u
typedef struct Phase {
    uint32_t valid,actor,uid,parent,root,model,key,vt,frame;
    double remainder,time;
} Phase;
static RECOMP_TLS Phase phases[SLOTS];
static RECOMP_TLS uint32_t context_actor;
static RECOMP_TLS int enable_cache,epoch_valid;
static RECOMP_TLS uint32_t epoch_frame;
static RECOMP_TLS double epoch_time;
static int enabled(void){if(!enable_cache){const char*v=getenv("RECOMP_TANK_FIXEDSTEP");enable_cache=v&&!strcmp(v,"0")?1:2;}return enable_cache==2;}
static int span(uint32_t p,size_t n){return p>=0x10000u&&n<=0x4000000u&&p<=0x4000000u-n;}
static uint32_t u32(uint32_t p){uint32_t v;memcpy(&v,(void*)((uintptr_t)p+g_xbox_mem_offset),4);return v;}
static double f64(uint32_t p){double v;memcpy(&v,(void*)((uintptr_t)p+g_xbox_mem_offset),8);return v;}
static int live(uint32_t a){return span(a,0x9A4)&&u32(a)==0x46AE48&&u32(0x46AE64)==0x109536&&(int32_t)u32(a+0x184)>0&&span(u32(a+0x8C),4)&&span(u32(a+0x94),4)&&u32(a+0x488)==0x4930B8&&u32(a+0x504)==0x4934B8;}
/* T1-2: the FastCode swap restores only the 40 bytes the scoped accessor redirects
 * (0x4E2420..0x4E2447), so it discards exactly the writes the TLS copy discards. */
static void swap_restore(void){memcpy((void*)((uintptr_t)0x4E2420+g_xbox_mem_offset),ma2_tank_step_scope.clock.bytes,40);ma2_tank_step_swapped=0;}
void ma2_tank_fixed_reset(void){if(ma2_tank_step_swapped)swap_restore();memset(phases,0,sizeof phases);epoch_valid=0;context_actor=0;ma2_tank_step_scope.active=0;}
void ma2_tank_fixed_forget(uint32_t a){for(unsigned i=0;i<SLOTS;i++)if(phases[i].valid&&phases[i].actor==a)phases[i].valid=0;}
void ma2_tank_fixed_input_select(uint32_t object){(void)object;/* Input remains at host cadence; no buffered overwrite. */}
/* All original actor work is called once, with its original registers, stack,
 * clocks and dt. Context only restricts the narrow contact/spring experiment. */
void ma2_tank_fixed_update(void(*body)(void)){
    uint32_t saved=context_actor,a=ecx;
    context_actor=enabled()&&live(a)?a:0;
    body();context_actor=saved;
}
/* -1 original integration, 0 no tick, 1 one canonical tick. Excess whole
 * steps are discarded, retaining fractional phase. At15Hz this deliberately
 * advances15 attitude steps/sec; movement/input/gameplay retain real dt. */
int ma2_tank_response_tick(uint32_t key,uint32_t vt){
    uint32_t a=context_actor;
    if(!a||!live(a)||!span(key,4))return -1;
    double dt=f64(0x4E2428),time=f64(0x4E2440);uint32_t frame=u32(0x4E2420);
    if(!(dt>0&&dt<=.2)||!isfinite(time)||time<dt)return -1;
    if(epoch_valid&&(time<epoch_time||frame<epoch_frame))memset(phases,0,sizeof phases);
    epoch_valid=1;epoch_time=time;epoch_frame=frame;
    Phase*s=NULL,*free_slot=NULL;unsigned start=((key>>4)^(a>>4))&(SLOTS-1);
    for(unsigned n=0;n<SLOTS;n++){Phase*p=phases+((start+n)&(SLOTS-1));if(p->valid&&p->actor==a&&p->key==key){s=p;break;}if(!free_slot&&(!p->valid||time-p->time>1))free_slot=p;}
    if(!s)s=free_slot;if(!s)return -1;
    uint32_t uid=u32(a+0x2C),parent=u32(a+4),root=u32(a+0x8C),model=u32(a+0x94);
    int bound=s->valid&&s->actor==a&&s->key==key&&s->vt==vt&&s->uid==uid&&s->parent==parent&&s->root==root&&s->model==model&&time-s->time<=1;
    if(bound&&s->frame==frame&&s->time==time)return 0;
    if(!bound)memset(s,0,sizeof *s);
    s->valid=1;s->actor=a;s->key=key;s->vt=vt;s->uid=uid;s->parent=parent;s->root=root;s->model=model;s->frame=frame;s->time=time;
    const double h=1.0/30;double elapsed=s->remainder+dt;unsigned ticks=(unsigned)floor(elapsed/h+1e-10);s->remainder=elapsed-ticks*h;if(s->remainder<0)s->remainder=0;
    if(dt==h)return -1;return ticks?1:0;
}
int ma2_tank_contact_begin(uint32_t root){
    if(!context_actor||u32(context_actor+0x8C)!=root||ma2_tank_step_scope.active||ma2_tank_step_swapped)return -1;
    int tick=ma2_tank_response_tick(root,u32(root));
    if(tick==1){const double h=1.0/30;unsigned char*g=(unsigned char*)((uintptr_t)0x4E2420+g_xbox_mem_offset);
        memcpy(ma2_tank_step_scope.clock.bytes,g,48);
        /* T1-2 FastCode: swap in guest memory (saved bytes restored at end, so writes
         * made during the step are discarded exactly as the TLS copy discards them). */
        if(g_recomp_fastcode){memcpy(g+8,&h,8);ma2_tank_step_swapped=1;}
        else{memcpy(ma2_tank_step_scope.clock.bytes+8,&h,8);ma2_tank_step_scope.active=1;}}
    return tick;
}
void ma2_tank_contact_end(void){
    if(ma2_tank_step_swapped)swap_restore();
    ma2_tank_step_scope.active=0;
}
