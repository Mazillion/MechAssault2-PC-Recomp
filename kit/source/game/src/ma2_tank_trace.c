/* Private shoreline3 diagnostic. Read-only guest observations; no gameplay correction. */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ma2_tank_trace.h"
extern ptrdiff_t g_xbox_mem_offset;
#define TANKS 3
#define RECORDS_PER_TANK 512
typedef struct TankRecord {
 uint32_t site,uid,actor,root,argument,value,ordinal,elapsed_ms,frame,step_ms;
 uint32_t actor_flags,root_flags,root_update_flags,root_vtable,virtual_normal,virtual_attitude;
 uint32_t root_branch_bytes,provider_vector[3];
 double dt,raw_dt,sim_time;
 uint8_t actor_pose[24],root_transforms[120],root_normal[12],tank_states[8];
 uint32_t wheel_heights[4];double time_scale;
} TankRecord;
typedef struct TankSlot {uint32_t uid,actor,root,events,records,updates,queries,attitudes,detail_frame,last_detail_ms,have_detail,primary_springs,secondary_springs,contact_updates,suspension_updates;} TankSlot;
static LONG mode,guard;
static unsigned count,used;
static ULONGLONG started;
static char destination[MAX_PATH];
static TankSlot slots[TANKS];
static TankRecord records[TANKS*RECORDS_PER_TANK];
#define TREE_NODES 4096
#define TREE_DEPTH 64
#define PREARM_ATTEMPTS 16
typedef struct TreeWork {uint32_t node,parent,depth;} TreeWork;
static TreeWork work[TREE_NODES];
static uint32_t visited[TREE_NODES];
static uint32_t mission_root,trigger,attempt_frame,prearm_frame;
static unsigned have_prearm_frame;
static unsigned attempts,lookup_nodes,have_attempt_frame,seen_unfired;
static const char *pending_reason;
static uint32_t ancestry_actor,ancestry_node,ancestry_depth,ancestry_uid,ancestry_vtable,ancestry_parent;
static const char *ancestry_reason;
/* mode=-2 defers a rejected observation's diagnostic until its original
   measurement deadline. mode=-1 is the allocation/clock/IO-free fast path. */
static int valid(uint32_t p,size_t n){return p>=0x10000 && n<=0x04000000 && p<=0x04000000-n;}
static void read_guest(void *out,uint32_t p,size_t n){if(valid(p,n))memcpy(out,(const void*)(g_xbox_mem_offset+p),n);else memset(out,0,n);}
static uint32_t word(uint32_t p){uint32_t v;read_guest(&v,p,4);return v;}
static void diagnostic(const char *status,const char *reason){
 fprintf(stderr,"[TANK-TRACE] status=%s reason=%s attempts=%u nodes=%u mission_root=%08X trigger=%08X records=%u\n",
  status,reason,attempts,lookup_nodes,mission_root,trigger,used);
 if(ancestry_reason)fprintf(stderr,"[TANK-TRACE-ANCESTRY] reason=%s actor=%08X node=%08X depth=%u uid=%u vtable=%08X parent=%08X\n",
  ancestry_reason,ancestry_actor,ancestry_node,ancestry_depth,ancestry_uid,ancestry_vtable,ancestry_parent);
}
static void reject(const char *reason){
 pending_reason=reason;
 if(started){mode=-2;return;} /* No IO inside a measurement window. */
 diagnostic("rejected",reason);mode=-1;
}
static void initialize(void){
 const char *scene=getenv("RECOMP_TANK_TRACE_SCENE"),*ids=getenv("RECOMP_TANK_TRACE_UIDS"),*file=getenv("RECOMP_TANK_TRACE_FILE");
 mode=-1;
 if(!scene||!*scene)return;
 if(strcmp(scene,"Mission_01b")||!ids||!file){reject("invalid-configuration");return;}
 unsigned n=0;const char *cursor=ids;
 while(*cursor && n<TANKS){char *end;if(*cursor<'0'||*cursor>'9'){reject("invalid-uids");return;}
  unsigned long uid=strtoul(cursor,&end,10);
  if(end==cursor||(uid!=132500 && uid!=132555 && uid!=132557)){reject("foreign-uid");return;}
  for(unsigned i=0;i<n;i++)if(slots[i].uid==(uint32_t)uid){reject("duplicate-uid");return;}
  slots[n++].uid=(uint32_t)uid;
  if(!*end){cursor=end;break;}
  if(*end!=','||!end[1]){reject("invalid-uids");return;}cursor=end+1;
 }
 if(*cursor||n!=TANKS){reject("not-exactly-three-uids");return;}
 DWORD length=GetFullPathNameA(file,MAX_PATH,destination,NULL);
 if(!length||length>=MAX_PATH||!strstr(destination,"\\bringup\\logs\\worker2-tank-rate\\")){
  reject("outside-private-output-folder");return;
 }
 count=n;mode=1;diagnostic("waiting","awaiting-selected-actor");
}
static int runtime_scene(void){
 uint32_t manager=word(0x524A60);if(!valid(manager,8))return 0;
 uint32_t path=word(manager+4);if(!valid(path,256))return 0;
 char scene[256];read_guest(scene,path,sizeof scene);
 if(!memchr(scene,0,sizeof scene))return 0;
 for(unsigned i=0;scene[i];i++)if(scene[i]=='/')scene[i]='\\';
 return !_stricmp(scene,"missions\\Mission_01b.wdf");
}
/* Runtime parent links are checked against the original MARoot identity.
   Authored grouping UIDs do not establish a live ownership contract. */
static uint32_t mission_ancestor(uint32_t actor){
 uint32_t seen[TREE_DEPTH],node=actor;
 ancestry_actor=actor;ancestry_reason=NULL;
 for(unsigned depth=0;depth<TREE_DEPTH;depth++){
  uint32_t parent=word(node+4);node=parent;
  ancestry_node=node;ancestry_depth=depth;ancestry_uid=ancestry_vtable=ancestry_parent=0;
  if(!valid(node,0x30)){ancestry_reason="parent-bounds";return 0;}
  ancestry_uid=word(node+0x2C);ancestry_vtable=word(node);ancestry_parent=word(node+4);
  for(unsigned i=0;i<depth;i++)if(seen[i]==node){ancestry_reason="parent-cycle";return 0;}
  seen[depth]=node;
  if(!ancestry_parent){
   if(ancestry_uid==1 && ancestry_vtable==0x468798)return node;
   ancestry_reason="terminal-root-identity";return 0;
  }
 }
 ancestry_reason="parent-depth-limit";return 0;
}
static uint32_t actor_mission(uint32_t actor){return mission_ancestor(actor);}
static int find_trigger(uint32_t owner,const char **reason){
 unsigned pending=1;lookup_nodes=0;uint32_t found=0;
 work[0]=(TreeWork){owner,0,0};
 while(pending){
  TreeWork item=work[--pending];uint32_t node=item.node;
  if(!valid(node,0x30)){*reason="invalid-tree-pointer";return -1;}
  if(item.depth>TREE_DEPTH){*reason="tree-depth-limit";return -1;}
  for(unsigned i=0;i<lookup_nodes;i++)if(visited[i]==node){*reason="tree-cycle-or-alias";return -1;}
  if(lookup_nodes==TREE_NODES){*reason="tree-node-limit";return -1;}
  visited[lookup_nodes++]=node;
  if(word(node+4)!=item.parent){*reason="tree-parent-mismatch";return -1;}
  if(word(node+0x2C)==130832){
   if(found){*reason="duplicate-trigger-uid";return -1;}
   if(!valid(node,0xB8)||word(node)!=0x474430){*reason="wrong-trigger-vtable";return -1;}
   found=node;
  }
  uint32_t sibling=word(node+8),child=word(node+0xC);
  if(!item.parent && sibling){*reason="mission-root-sibling";return -1;}
  if(sibling){if(pending==TREE_NODES){*reason="tree-work-limit";return -1;}work[pending++]=(TreeWork){sibling,item.parent,item.depth};}
  if(child){if(pending==TREE_NODES){*reason="tree-work-limit";return -1;}work[pending++]=(TreeWork){child,node,item.depth+1};}
 }
 if(!found){*reason="trigger-not-found";return 0;}
 trigger=found;return 1;
}
static int trigger_valid(void){
 if(!valid(trigger,0xB8)||word(trigger)!=0x474430||word(trigger+0x2C)!=130832||word(trigger+0xB0)!=1)return 0;
 uint8_t enabled,inverse;read_guest(&enabled,trigger+0x89,1);read_guest(&inverse,trigger+0xB4,1);
 return enabled!=0 && !inverse;
}
static int trigger_owned(void){return mission_ancestor(trigger)==mission_root;}
static int activation_gate(uint32_t actor,uint32_t site){
 if(!runtime_scene()){reject("runtime-scene-mismatch");return 0;}
 uint32_t owner=actor_mission(actor);
 if(!owner){reject("invalid-mission-ancestry");return 0;}
 if(mission_root && owner!=mission_root){reject("mission-root-changed");return 0;}
 mission_root=owner;
 if(!trigger){
  uint32_t frame=word(0x4E2420);
  if(have_attempt_frame && frame==attempt_frame)return 0;
  if(attempts==PREARM_ATTEMPTS){reject("trigger-lookup-attempt-limit");return 0;}
  have_attempt_frame=1;attempt_frame=frame;attempts++;
  const char *reason=NULL;int found=find_trigger(owner,&reason);
  if(found<0){reject(reason);return 0;}
  if(!found){if(attempts==PREARM_ATTEMPTS)reject("trigger-lookup-attempt-limit");return 0;}
 }
 if(!trigger_valid()){reject("trigger-identity-or-settings-changed");return 0;}
 if(!trigger_owned()){reject("trigger-mission-binding-changed");return 0;}
 uint8_t fired;read_guest(&fired,trigger+0x88,1);
 if(!started){
  if(!fired){if(!seen_unfired)diagnostic("waiting","trigger-unfired");seen_unfired=1;return 0;}
  if(!seen_unfired){reject("late-first-callback-trigger-already-fired");return 0;}
  if(site!=0x109536)return 0;
 }else if(!fired){reject("trigger-reset-during-window");return 0;}
 return 1;
}
static void finish(void){
 /* One batch write after observation ends; no IO in the measurement window.
    File format: magic8/version/count/records/bytes, slot records, event records. */
 for(unsigned i=0;i<count;i++)if(!slots[i].actor||!slots[i].root||!slots[i].records){
  diagnostic("rejected","missing-selected-tank-records");mode=-1;return;
 }
 uint32_t header[4]={2,count,used,sizeof(TankRecord)};
 FILE *out=NULL;
 if(fopen_s(&out,destination,"wbx")||!out){diagnostic("rejected","exclusive-output-create-failed");mode=-1;return;}
 int ok=fwrite("MA2TANK\0",8,1,out)==1;
 ok=(fwrite(header,sizeof header,1,out)==1)&&ok;
 ok=(fwrite(slots,sizeof slots,1,out)==1)&&ok;
 ok=(fwrite(records,sizeof(TankRecord),used,out)==used)&&ok;
 ok=(fclose(out)==0)&&ok;
 diagnostic(ok?"complete":"rejected",ok?"three-tanks-recorded":"output-write-failed");
 mode=-1;
}
void ma2_tank_trace(uint32_t site,uint32_t actor,uint32_t root,uint32_t argument,uint32_t value){
 if(mode==-1)return;
 if(InterlockedCompareExchange(&guard,1,0))return;
 if(mode==-2){
  if(GetTickCount64()-started>=10000){diagnostic("rejected",pending_reason);mode=-1;}
  InterlockedExchange(&guard,0);return;
 }
 if(!mode)initialize();
 if(mode!=1){InterlockedExchange(&guard,0);return;}
 if(started && GetTickCount64()-started>=10000){finish();InterlockedExchange(&guard,0);return;}
 if(!started && (!actor || site!=0x109536)){InterlockedExchange(&guard,0);return;}
 unsigned i;
 if(actor){
  for(i=0;i<count && slots[i].actor!=actor;i++);
  if(i<count && (!valid(actor,0x984)||word(actor)!=0x46AE48||word(actor+0x2C)!=slots[i].uid)){
   reject("cached-actor-identity-changed");InterlockedExchange(&guard,0);return;
  }
  if(!valid(actor,0x984)||word(actor)!=0x46AE48){InterlockedExchange(&guard,0);return;}
  uint32_t uid=word(actor+0x2C);for(i=0;i<count && slots[i].uid!=uid;i++);
  if(i==count){
   /* Observe the unchanged trigger through existing earlier MATank updates.
      Only the three selected actors may bind slots or start recording. */
   if(!started && site==0x109536 && runtime_scene()){
    uint32_t frame=word(0x4E2420);
    if(!have_prearm_frame || frame!=prearm_frame){
     have_prearm_frame=1;prearm_frame=frame;(void)activation_gate(actor,site);
    }
   }
   InterlockedExchange(&guard,0);return;
  }
  root=word(actor+0x8C);
  if(!valid(root,0x208)){reject("invalid-selected-physics-root");InterlockedExchange(&guard,0);return;}
  if(slots[i].actor && (slots[i].actor!=actor||slots[i].root!=root)){
   reject("selected-actor-or-physics-root-changed");InterlockedExchange(&guard,0);return;
  }
  slots[i].actor=actor;slots[i].root=root;
 }else {
  for(i=0;i<count && (!root || (slots[i].root!=root && !(site==0x1718ED && slots[i].actor && slots[i].actor+0x5D0==root)));i++);
  if(i==count){InterlockedExchange(&guard,0);return;}actor=slots[i].actor;
  if(!valid(actor,0x984)||word(actor)!=0x46AE48||word(actor+0x2C)!=slots[i].uid||word(actor+0x8C)!=slots[i].root || (root!=slots[i].root && (site!=0x1718ED || root!=actor+0x5D0 || word(root)!=0x4770B8)) || !valid(root,0x208)){
   reject("cached-root-binding-changed");InterlockedExchange(&guard,0);return;
  }
 }
 if(!activation_gate(actor,site)){InterlockedExchange(&guard,0);return;}
 ULONGLONG now=GetTickCount64();if(!started)started=now;
 TankSlot *slot=slots+i;slot->events++;if(site==0x109536)slot->updates++;
 if(site==0x170E48)slot->queries++;if(site==0x170E9E)slot->attitudes++;
 if(site==0x1718ED){if(root==slot->root)slot->primary_springs++;else slot->secondary_springs++;}
 if(site==0x1713E5)slot->contact_updates++;if(site==0x170FD8)slot->suspension_updates++;
 uint32_t frame=word(0x4E2420),elapsed=(uint32_t)(now-started);
 if(site==0x109536 && (!slot->have_detail || elapsed-slot->last_detail_ms>=500)){slot->have_detail=1;slot->detail_frame=frame;slot->last_detail_ms=elapsed;}
 if(slot->have_detail && slot->detail_frame==frame && slot->records<RECORDS_PER_TANK && used<TANKS*RECORDS_PER_TANK){
  TankRecord *r=records+used++;memset(r,0,sizeof *r);
  r->site=site;r->uid=slot->uid;r->actor=actor;r->root=root;r->argument=argument;r->value=value;r->ordinal=slot->events;r->elapsed_ms=(uint32_t)(now-started);
  r->frame=word(0x4E2420);r->step_ms=word(0x4E2424);r->actor_flags=word(actor+0x80);
  read_guest(&r->dt,0x4E2428,8);read_guest(&r->raw_dt,0x4E2418,8);read_guest(&r->sim_time,0x4E2440,8);
  read_guest(r->actor_pose,actor+0x4C0,sizeof r->actor_pose);read_guest(r->tank_states,actor+0x97C,8);
  if(valid(root,0x208)){
   r->root_flags=word(root+0x160);r->root_update_flags=word(root+0x144);r->root_branch_bytes=word(root+0x1FC);
   read_guest(r->root_transforms,root+4,sizeof r->root_transforms);read_guest(r->root_normal,root+0x154,12);
   for(unsigned wheel=0;wheel<4;wheel++)r->wheel_heights[wheel]=word(root+0x17C+wheel*0x24);
   uint32_t vt=word(root);r->root_vtable=vt;if(valid(vt,0x70)){r->virtual_normal=word(vt+0x2C);r->virtual_attitude=word(vt+0x6C);}
  }
  if(site==0x170E5A)read_guest(r->provider_vector,value,12);
  if(site==0x1718ED)read_guest(r->provider_vector,argument,12);
  read_guest(&r->time_scale,0x4A14F0,8);
  slot->records++;
 }
 unsigned full=0;for(unsigned j=0;j<count;j++)full+=slots[j].records==RECORDS_PER_TANK;
 if(full==count)finish();
 InterlockedExchange(&guard,0);
}
