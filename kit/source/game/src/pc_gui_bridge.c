/* Calls into the original GoGui implementation, always on its guest thread. */
#include "recomp/gen/recomp_types.h"
#include "recomp/gen/recomp_funcs.h"
#include "pc_settings.h"
#include <stdio.h>
#include <stdlib.h>
#include <windows.h>
#include <wchar.h>
#define eax g_eax
#define ecx g_ecx
#define edx g_edx
#define ebx g_ebx
#define esi g_esi
#define edi g_edi
#define esp g_esp

typedef struct GuiRegisters {
    uint32_t a,c,d,b,s,i,sp,bp,seh;
    RecompXmm x[8];double fp[8];int top;
    uint16_t fp_control,fp_cc;int fp_cmp,df;
} GuiRegisters;
static GuiRegisters save_registers(void) {
    GuiRegisters r={eax,ecx,edx,ebx,esi,edi,esp,g_ebp,g_seh_ebp,
        {g_xmm0,g_xmm1,g_xmm2,g_xmm3,g_xmm4,g_xmm5,g_xmm6,g_xmm7},{0},g_fp_top};
    memcpy(r.fp,g_fp_stack,sizeof(r.fp));r.fp_control=g_fp_control_word;r.fp_cc=g_fp_cc;r.fp_cmp=g_fp_cmp;r.df=g_df;return r;
}
static void restore_registers(GuiRegisters *r) {
    eax=r->a;ecx=r->c;edx=r->d;ebx=r->b;esi=r->s;edi=r->i;esp=r->sp;
    g_ebp=r->bp;g_seh_ebp=r->seh;
    g_xmm0=r->x[0];g_xmm1=r->x[1];g_xmm2=r->x[2];g_xmm3=r->x[3];
    g_xmm4=r->x[4];g_xmm5=r->x[5];g_xmm6=r->x[6];g_xmm7=r->x[7];
    memcpy(g_fp_stack,r->fp,sizeof(r->fp));g_fp_top=r->top;
    g_fp_control_word=r->fp_control;g_fp_cc=r->fp_cc;g_fp_cmp=r->fp_cmp;g_df=r->df;
}
uint32_t ma2_gui_find(uint32_t form,uint32_t action) {
    if(!form)return 0;
    GuiRegisters r=save_registers();ecx=form;edx=action;
    PUSH32(esp,0x001FBBDB);RECOMP_ABI_CALL(0x001CAA80,sub_001CAA80);
    uint32_t result=eax;restore_registers(&r);return result;
}
static uint32_t dialog_form(uint32_t handle){
    uint32_t begin=MEM32(0x5240D0),end=MEM32(0x5240D4);
    if(end<begin||end-begin>4096)return 0;
    if(!handle&&end>begin)return MEM32(MEM32(end-4)+4);
    for(uint32_t p=begin;p<end;p+=4){uint32_t d=MEM32(p);if(d&&MEM32(d)==handle)return MEM32(d+4);}
    return 0;
}
void ma2_gui_text(uint32_t widget,uint32_t text) {
    if(!widget||!text)return;
    GuiRegisters r=save_registers();esi=widget;PUSH32(esp,text);
    PUSH32(esp,0x001FBBE0);RECOMP_ABI_CALL(0x001FF864,sub_001FF864);
    restore_registers(&r);
}
void ma2_gui_slider(uint32_t widget,float value) {
    if(!widget)return;
    GuiRegisters r=save_registers();ecx=widget;PUSH32(esp,0);MEMF(esp)=value;
    PUSH32(esp,0x002201C8);RECOMP_ABI_CALL(0x00217B30,sub_00217B30);
    restore_registers(&r);
}
uint32_t ma2_gui_dialog(uint32_t title,uint32_t message) {
    GuiRegisters r=save_registers();PUSH32(esp,0);PUSH32(esp,0x4010);
    eax=title;ecx=message;
    PUSH32(esp,0x001F35F5);RECOMP_ABI_CALL(0x001FB975,sub_001FB975);
    uint32_t result=eax;restore_registers(&r);
    uint32_t form=dialog_form(result);
    ma2_gui_text(ma2_gui_find(form,0x466),ma2_pc_wide(L"Exit game"));
    return result;
}
uint32_t ma2_gui_dialog_result(uint32_t dialog) {
    GuiRegisters r=save_registers();PUSH32(esp,dialog);
    PUSH32(esp,0x001F4ED1);RECOMP_ABI_CALL(0x001FBB86,sub_001FBB86);
    uint32_t result=eax;restore_registers(&r);return result;
}
void ma2_gui_volume(uint32_t object,unsigned channel,unsigned value) {
    static const uint32_t channels[]={0x1FFFD,0x1FFFF,0x1FFFE};
    if(channel>=3)return;
    GuiRegisters r=save_registers();esi=value;
    PUSH32(esp,channels[channel]);PUSH32(esp,MEM32(object+0x64));
    PUSH32(esp,0x0022031C);RECOMP_ABI_CALL(0x000BC270,sub_000BC270);
    restore_registers(&r);
}
static int valid_widget(uint32_t w){return w>=0x10000&&w<0x03FFF000;}
/* Native rows are inversion, vibration. Retail +76 controls pitch (profile
 * bit 0); +75 controls vibration (bit 1). Keep the guest's axis convention. */
static const uint8_t controller_option_offsets[2]={0x76,0x75};
int ma2_gui_controller_option(uint32_t object,unsigned option){
    return valid_widget(object)&&option<2 ? MEM8(object+controller_option_offsets[option])!=0 : 0;
}
void ma2_gui_controller_set(uint32_t object,unsigned option,int enabled){
    if(valid_widget(object)&&option<2)MEM8(object+controller_option_offsets[option])=enabled!=0;
}
void ma2_gui_controller_commit(uint32_t object){
    /* In-mission Controls applies +76 to the active camera before its base
     * handler saves the profile. Lobby Controls uses only the base handler. */
    if(!valid_widget(object))return;
    GuiRegisters r=save_registers();ecx=object;PUSH32(esp,0);
    PUSH32(esp,0x001FCD00);
    if(MEM32(object)==0x48BF20)RECOMP_ABI_CALL(0x00223DBC,sub_00223DBC);
    else RECOMP_ABI_CALL(0x0021F989,sub_0021F989);
    restore_registers(&r);
}
void ma2_gui_volume_commit(uint32_t object){
    /* Preserve the retail profile serialization and its rollback snapshot. */
    GuiRegisters r=save_registers();ecx=object;PUSH32(esp,0);
    PUSH32(esp,0x001FCD00);RECOMP_ABI_CALL(0x0022021C,sub_0022021C);
    restore_registers(&r);
}
static void widget_rect(uint32_t widget,float rect[4]) {
    GuiRegisters r=save_registers();esp-=16;uint32_t address=esp;
    PUSH32(esp,address);PUSH32(esp,widget);
    PUSH32(esp,0x001FF971);RECOMP_ABI_CALL(0x00201B61,sub_00201B61);
    memcpy(rect,(void *)(g_xbox_mem_offset+address),16);restore_registers(&r);
}
static void inspect_widget(uint32_t w,int depth){
    if(!valid_widget(w)||depth>4)return;
    float r[4];widget_rect(w,r);
    fprintf(stderr,"[PC-RECT] depth=%d id=%u action=%u xywh=%d,%d,%d,%d rect=%.4f,%.4f,%.4f,%.4f policy=%u\n",depth,
        MEM32(w+0x6C),MEM32(w+0x70),SMEM32(w+0x4C),SMEM32(w+0x50),SMEM32(w+0x54),SMEM32(w+0x58),r[0],r[1],r[2],r[3],MEM32(w+0x80));
    for(uint32_t c=MEM32(w+0x1C);valid_widget(c);c=MEM32(c+0x18))inspect_widget(c,depth+1);
}
void ma2_gui_inspect(uint32_t form){if(form&&getenv("RECOMP_MENU_TRACE"))inspect_widget(MEM32(form+0x38),0);}
static uint32_t widget_hit(uint32_t widget,float x,float y,int depth) {
    if(!valid_widget(widget)||depth>40)return 0;
    unsigned flags=MEM32(widget+0x74);
    if(!(flags&2))return 0;
    uint32_t text_id=MEM32(widget+0xE8);
    if(text_id>=0x70010000&&text_id<0x70010006){
        /* A footer's text is a child of its key label. Both rectangles are
         * clickable, without replacing the menu's selected campaign/item. */
        static const unsigned buttons[]={0,1,2,3,5,4};
        float r[4];widget_rect(widget,r);
        if(x>=r[0]&&x<r[0]+r[2]&&y>=r[1]&&y<r[1]+r[3])return 0x80000000u|buttons[text_id-0x70010000];
        for(uint32_t child=MEM32(widget+0x1C);valid_widget(child);child=MEM32(child+0x18)){
            widget_rect(child,r);
            if(x>=r[0]&&x<r[0]+r[2]&&y>=r[1]&&y<r[1]+r[3])return 0x80000000u|buttons[text_id-0x70010000];
        }
    }
    for(uint32_t child=MEM32(widget+0x1C);valid_widget(child);child=MEM32(child+0x18)) {
        uint32_t hit=widget_hit(child,x,y,depth+1);if(hit)return hit;
    }
    if((flags&0x20)&&MEM32(widget+0x48)!=5){
        float r[4];widget_rect(widget,r);
        if(x>=r[0]&&x<r[0]+r[2]&&y>=r[1]&&y<r[1]+r[3])return widget;
    }
    return 0;
}
uint32_t ma2_gui_dialog_pointer(uint32_t dialog,float x,float y){
    uint32_t form=dialog_form(dialog);
    return form?widget_hit(MEM32(form+0x38),x,y,0):0;
}
uint32_t ma2_gui_hover(uint32_t owner,float x,float y) {
    if(!valid_widget(owner))return 0;
    uint32_t begin=MEM32(owner+4),end=MEM32(owner+8);
    if(end<begin||end-begin>1024)return 0;
    for(uint32_t entry=begin;entry<end;entry+=4){
        uint32_t form=MEM32(entry);if(!valid_widget(form)||(MEM32(form+0x4C)&3)!=3)continue;
        uint32_t hit=widget_hit(MEM32(form+0x38),x,y,0);
        if(hit){
            if(hit&0x80000000u)return hit;
            GuiRegisters r=save_registers();edi=owner;esi=hit;ebx=0;
            PUSH32(esp,0x001CAEA0);RECOMP_ABI_CALL(0x001CAD40,sub_001CAD40);
            restore_registers(&r);return hit;
        }
    }
    return 0;
}

uint32_t ma2_gui_keyboard_dialog(uint32_t *object) {
    uint32_t begin=MEM32(0x5240D0),end=MEM32(0x5240D4);
    if(object)*object=0;
    if(!valid_widget(begin)||end<=begin||end-begin>4096||((begin|end)&3)||
       end>0x03FFF000)return 0;
    uint32_t record=MEM32(end-4);
    if(!valid_widget(record)||MEM32(record+0x10)||MEM32(0x4EC344)!=record)return 0;
    uint32_t owner=MEM32(record+0x1C);
    if(!valid_widget(owner)||MEM32(owner)!=0x48F7D0||!valid_widget(MEM32(owner+0x23C)))return 0;
    if(object)*object=owner;
    return record;
}
static int keyboard_matches(uint32_t record,uint32_t object) {
    uint32_t current;
    return record&&ma2_gui_keyboard_dialog(&current)==record&&current==object;
}
int ma2_gui_keyboard_action(uint32_t record,uint32_t object,uint32_t code,int typed) {
    if(!keyboard_matches(record,object)||MEM8(object+0x250))return 0;
    if(typed) {
        if(code<0x20||(code>=0x7F&&code<0xA0)||code>=0xFFFE||
           (code>=0xD800&&code<=0xDFFF))return 0;
    } else if(code>=0x10000&&
              code!=0x10001&&code!=0x10002&&(code<0x10004||code>0x1000B))return 0;
    if(!code)return 0;
    GuiRegisters r=save_registers();
    uint32_t selected=MEM32(object+0x254);
    uint8_t caps=MEM8(object+0x64),shift=MEM8(object+0x65);
    if(typed) {
        /* WM_CHAR already applies the host's shift/caps state. Ask the retail
         * converter which setting preserves that character, then let the
         * original action enforce length, cursor and name validation. */
        MEM8(object+0x64)=MEM8(object+0x65)=0;
        ecx=object;eax=code;PUSH32(esp,0x0024A4AB);
        RECOMP_ABI_CALL(0x0024A071,sub_0024A071);
        MEM8(object+0x64)=LO16(eax)!=(uint16_t)code;
    }
    MEM32(object+0x254)=code;PUSH32(esp,object);PUSH32(esp,0x0024A2E1);
    RECOMP_ABI_CALL(0x0024A2E6,sub_0024A2E6);
    if(keyboard_matches(record,object)) {
        MEM32(object+0x254)=selected;
        if(typed) {
            int refresh=MEM8(object+0x64)!=caps||MEM8(object+0x65)!=shift;
            MEM8(object+0x64)=caps;MEM8(object+0x65)=shift;
            if(refresh){PUSH32(esp,object);PUSH32(esp,0x0024A425);RECOMP_ABI_CALL(0x0024A0F4,sub_0024A0F4);}
        }
    }
    restore_registers(&r);return 1;
}
uint32_t ma2_gui_keyboard_pointer(uint32_t record,uint32_t object,float x,float y,int activate) {
    if(!keyboard_matches(record,object)||MEM8(object+0x250))return 0;
    /* MAKeyboardDlg keeps its form at +23C; the modal record's +4 is empty. */
    uint32_t form=MEM32(object+0x23C);
    uint32_t hit=widget_hit(MEM32(form+0x38),x,y,0);
    if(hit&0x80000000u)return hit;
    if(!hit)return 0;
    for(unsigned slot=0;slot<55;slot++) {
        uint32_t id=MEM32(object+0x84+slot*4),code=MEM32(object+0x160+slot*4);
        if(!id||!code)continue;
        uint32_t w=ma2_gui_find(form,id);
        /* Focus the logical grid entry even when its text child was hit. */
        if(!valid_widget(w)||!widget_hit(w,x,y,0))continue;
        if(!keyboard_matches(record,object))return 0;
        GuiRegisters r=save_registers();edi=object;esi=w;ebx=0;
        MEMF(object+0x6C)=(float)(slot%11);MEMF(object+0x70)=(float)(slot/11);
        PUSH32(esp,0x0024A6F0);RECOMP_ABI_CALL(0x001CAD40,sub_001CAD40);
        restore_registers(&r);
        if(activate&&!ma2_gui_keyboard_action(record,object,code,0))return 0;
        return w;
    }
    return 0;
}

/* Private QTE implementation candidate; no resources survive a draw. */
#include <math.h>
#include "platform/pc_frontend.h"
static RECOMP_TLS int qte_busy;
static int qte_range(uint32_t p,unsigned n){return p>=0x10000&&p<0x03FFF000&&n<=0x03FFF000-p;}
/* The prompt resolver retains full host key names. If its ASCII font/fit
 * contract rejects one, use the exact native binding with a compact name. */
static const signed char qte_caption_advances[128]={-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,11,4,4,5,-1,7,10,8,3,4,4,5,7,4,4,4,4,7,7,7,7,7,7,7,7,7,7,4,4,7,-1,7,7,11,8,8,8,8,8,7,9,8,4,6,8,7,9,8,9,8,9,8,8,7,8,8,10,8,8,7,4,4,4,-1,6,-1,7,7,6,7,7,4,7,7,4,4,6,4,10,7,7,7,7,4,6,4,7,6,8,6,6,6,-1,-1,-1,-1,-1};
static unsigned qte_caption_advance(const wchar_t *s){
    unsigned n=0;for(;*s;s++){if(*s>=128||qte_caption_advances[*s]<0)return 0;n+=qte_caption_advances[*s];}return n;
}
static int qte_binding_name(enum PcBinding action,int fallback,wchar_t short_name[8],wchar_t detail[24]){
    int key=pc_binding_get(action,fallback);const wchar_t *name=NULL;detail[0]=0;
    if(key<0||key>255){wcscpy_s(short_name,8,L"?");wcscpy_s(detail,24,L"Rebind");return 2;}
    static const struct {int key;const wchar_t *name;} names[]={
        {VK_LBUTTON,L"M1"},{VK_RBUTTON,L"M2"},{VK_MBUTTON,L"M3"},{VK_XBUTTON1,L"M4"},{VK_XBUTTON2,L"M5"},
        {VK_SPACE,L"Spc"},{VK_RETURN,L"Ent"},{VK_BACK,L"Bksp"},{VK_PAUSE,L"Pau"},
        {VK_TAB,L"Tab"},{VK_ESCAPE,L"Esc"},{VK_CONTROL,L"Ctrl"},{VK_SHIFT,L"Shft"},{VK_MENU,L"Alt"},
        {VK_LCONTROL,L"LCtl"},{VK_RCONTROL,L"RCtl"},{VK_LSHIFT,L"LSh"},{VK_RSHIFT,L"RSh"},
        {VK_LMENU,L"LAlt"},{VK_RMENU,L"RAlt"},{VK_LWIN,L"LWin"},{VK_RWIN,L"RWi"},{VK_APPS,L"App"},
        {VK_PRIOR,L"PgU"},{VK_NEXT,L"PgD"},{VK_HOME,L"Hom"},{VK_END,L"End"},
        {VK_LEFT,L"Left"},{VK_RIGHT,L"Rgt"},{VK_UP,L"Up"},{VK_DOWN,L"Dn"},
        {VK_CAPITAL,L"Caps"},{VK_SNAPSHOT,L"Prt"},{VK_SCROLL,L"Scr"},{VK_NUMLOCK,L"Num"},
        {VK_INSERT,L"Ins"},{VK_DELETE,L"Del"},{VK_MULTIPLY,L"N*"},{VK_ADD,L"N+"},
        {VK_SUBTRACT,L"N-"},{VK_DIVIDE,L"N/"},{VK_DECIMAL,L"N."},{VK_SEPARATOR,L"N,"},
        {VK_VOLUME_MUTE,L"Mute"},{VK_VOLUME_UP,L"Vol+"},{VK_VOLUME_DOWN,L"Vol-"},
        {VK_MEDIA_NEXT_TRACK,L"Next"},{VK_MEDIA_PREV_TRACK,L"Prev"},{VK_MEDIA_STOP,L"Stop"},{VK_MEDIA_PLAY_PAUSE,L"Play"}};
    if((key>='A'&&key<='Z')||(key>='0'&&key<='9')){short_name[0]=(wchar_t)key;short_name[1]=0;return 0;}
    if(key>=VK_F1&&key<=VK_F24){swprintf_s(short_name,8,L"F%d",key-VK_F1+1);return 0;}
    if(key>=VK_NUMPAD0&&key<=VK_NUMPAD9){swprintf_s(short_name,8,L"N%d",key-VK_NUMPAD0);return 0;}
    for(unsigned i=0;i<sizeof(names)/sizeof(names[0]);i++)if(key==names[i].key){name=names[i].name;break;}
    if(name){wcscpy_s(short_name,8,name);return 0;}
    static const int oem[]={VK_OEM_1,VK_OEM_2,VK_OEM_3,VK_OEM_4,VK_OEM_5,VK_OEM_6,VK_OEM_7,VK_OEM_8,VK_OEM_102};
    for(unsigned i=0;i<sizeof(oem)/sizeof(oem[0]);i++)if(key==oem[i]){
        if(i==8)wcscpy_s(short_name,8,L"O<");else swprintf_s(short_name,8,L"O%d",i+1);
        swprintf_s(detail,24,L"OEM %d",i==8?102:i+1);return 1;
    }
    /* An unnamed/nonstandard VK has no font-supported physical key name.
     * Show its exact stored identity and an explicit rebind instruction. */
    swprintf_s(short_name,8,L"K%02X",(unsigned)key);wcscpy_s(detail,24,L"Rebind");return 2;
}
static int qte_fitted_caption(unsigned mask,Ma2QteCaption *out,int *side){
    *side=0;if(ma2_pc_qte_caption(mask,out))return 1;
    if(pc_input_device()==PC_INPUT_GAMEPAD)return 0;
    enum PcBinding primary,secondary=PC_A;int first,second=0,alternate=0;
    switch(mask){
    case 4:primary=PC_A;first=VK_SPACE;break;case 1:primary=PC_X;first='R';break;
    case 2:primary=PC_Y;first='Q';break;case 0x40:primary=PC_B;first='F';break;
    case 0x10:primary=PC_MOUSE_ALT_FIRE;first=VK_RBUTTON;secondary=PC_LEFT_TRIGGER;second='C';alternate=1;break;
    case 0x20:primary=PC_MOUSE_FIRE;first=VK_LBUTTON;secondary=PC_RIGHT_TRIGGER;second='V';alternate=1;break;
    default:return 0;
    }
    wchar_t label[8],other[8],detail[24],other_detail[24],legend[64];
    int kind=qte_binding_name(primary,first,label,detail),other_kind=0;
    *out=(Ma2QteCaption){0};out->advance=(uint16_t)qte_caption_advance(label);
    if(!out->advance||out->advance>28)return 0;
    if(alternate&&pc_binding_get(primary,first)!=pc_binding_get(secondary,second)){
        other_kind=qte_binding_name(secondary,second,other,other_detail);
        if(kind==2||other_kind==2)swprintf_s(legend,64,L"Rebind %s/%s",label,other);
        else swprintf_s(legend,64,L"%s / %s",kind==1?detail:label,other_kind==1?other_detail:other);
        out->alternative=ma2_pc_wide(other);if(!out->alternative)return 0;
    }else if(kind){
        swprintf_s(legend,64,L"%s: %s",label,detail);*side=!alternate;
    }else legend[0]=0;
    if(legend[0]){
        out->legend_advance=(uint16_t)qte_caption_advance(legend);
        if(!out->legend_advance||out->legend_advance>(*side?84:108))return 0;
        out->legend=ma2_pc_wide(legend);if(!out->legend)return 0;
    }
    out->label=ma2_pc_wide(label);out->revision=pc_input_revision();return out->label!=0;
}
static recomp_func_t qte_method(uint32_t object,unsigned slot){
    if(!qte_range(object,4))return NULL;
    uint32_t vt=MEM32(object);
    if(!qte_range(vt,slot+4))return NULL;
    uint32_t target=MEM32(vt+slot);
    return target>=g_xbox_code_lo&&target<g_xbox_code_hi?recomp_lookup(target):NULL;
}
/* Opt-in host log; two observations per reason/mask, 128 total per guest thread. */
enum QteReason {
    QTE_OK,QTE_REENTRY,QTE_WIDGET,QTE_CLASS,QTE_STATE,QTE_CAPTION,QTE_UV,
    QTE_ORIGINAL_RELEASE,QTE_DEVICE_LOAD,QTE_FONT_SYSTEM,QTE_FONT_REGISTRY,QTE_STACK,
    QTE_RECT,QTE_SIZE,QTE_LEGEND_KIND,QTE_LEGEND_BOX,QTE_FORM,QTE_ROOT,
    QTE_TREE_RANGE,QTE_TREE_LIMIT,QTE_TREE_CYCLE,QTE_TREE_OWNER,QTE_TREE_BOX,
    QTE_TREE_OVERLAP,QTE_TREE_MISSING,QTE_PATH,QTE_TEXTURE_RELEASE,QTE_TEXTURE_CLASS,
    QTE_TEXTURE_SIZE_METHOD,QTE_TEXTURE_SIZE,QTE_FONT_LOOKUP,QTE_FONT_RELEASE,QTE_COMPACT,QTE_REASON_COUNT
};
static uint32_t qte_member(uint32_t object,unsigned offset){
    return qte_range(object,offset+4)?MEM32(object+offset):0;
}
static void qte_trace(enum QteReason reason,uint32_t w,const float *rect,uint32_t texture,uint32_t font){
    static RECOMP_TLS int enabled=-1;
    static RECOMP_TLS unsigned total;
    static RECOMP_TLS unsigned char seen[QTE_REASON_COUNT][7];
    if(enabled<0){const char *value=getenv("RECOMP_QTE_TRACE");enabled=value&&value[0]=='1';}
    if(!enabled||total>=128)return;
    unsigned mask=qte_range(w,0xED)?MEM8(w+0xEC):0,role=6;
    switch(mask){case 4:role=0;break;case 1:role=1;break;case 2:role=2;break;case 0x40:role=3;break;case 0x10:role=4;break;case 0x20:role=5;break;}
    if(seen[reason][role]>=2)return;
    seen[reason][role]++;total++;
    static const char *names[]={"replaced","reentry","widget_range","foreign_class","state","caption","sub_uv",
        "original_release_method","device_load_method","font_system","font_registry_method","scratch_stack",
        "rect_bounds","symbol_size","legend_kind","legend_bounds","form_range","root_range",
        "tree_range","tree_limit","tree_cycle","tree_owner","tree_rect","legend_overlap","tree_missing_qte_or_root",
        "path","texture_release_method","texture_class","texture_size_method","texture_size","font_lookup","font_release_method","compact_in_icon"};
    uint32_t original=qte_member(w,0xB8),device=MEM32(0x50C704),system=MEM32(0x50C958);
    uint32_t form=qte_member(w,0x8C),registry=qte_member(system,0x30);
    fprintf(stderr,"[PC-QTE] reason=%s w=%08X authored_id=%u runtime_id=%u tab_id=%u mask=%02X state=%u input=%u revision=%u class=%08X mode=%u form=%08X root=%08X original=%08X original_vt=%08X original_release=%08X device=%08X load=%08X system=%08X registry=%08X registry_method=%08X texture=%08X texture_vt=%08X font=%08X font_release=%08X uv=%08X,%08X,%08X,%08X rect=%.7g,%.7g,%.7g,%.7g gui_width=%u\n",
        names[reason],w,qte_member(w,8),qte_member(w,4),qte_member(w,0x6C),mask,qte_member(w,0xF0),pc_input_device(),pc_input_revision(),qte_member(w,0),qte_member(w,0x80),form,qte_member(form,0x38),
        original,qte_member(original,0),qte_member(qte_member(original,0),0x30),device,qte_member(qte_member(device,0),0x60),system,registry,qte_member(qte_member(registry,0),8),
        texture,qte_member(texture,0),font,qte_member(qte_member(font,0),0xC),qte_member(w,0xBC),qte_member(w,0xC0),qte_member(w,0xC4),qte_member(w,0xC8),
        rect?rect[0]:0,rect?rect[1]:0,rect?rect[2]:0,rect?rect[3]:0,pc_display_render_width(640,1));
}
static int qte_symbol_size(const float rect[4]){
    /* Original C1CCC10 mode 4 scales x/width by (4:3)/current aspect.
     * Match C1640's existing virtual viewport, preserving authored geometry. */
    unsigned width=pc_display_render_width(640,1);
    return rect[2]*width>=28&&rect[2]*width<=36&&rect[3]*480>=28&&rect[3]*480<=36;
}
static int qte_box(const float r[4]){
    return isfinite(r[0])&&isfinite(r[1])&&isfinite(r[2])&&isfinite(r[3])&&
        r[0]>=0&&r[1]>=0&&r[2]>0&&r[3]>0&&r[0]+r[2]<=1.001f&&r[1]+r[3]<=1.001f;
}
static int qte_empty_untextured_bitmap(uint32_t widget,const float r[4]){
    /* Both pinned original bitmap vtables draw through 001FE68D, which
     * returns before painting when +B8 has no texture. Ignore only bounded
     * empty geometry; unknown classes and malformed rectangles still fail. */
    if(!qte_range(widget,0xBC)||(MEM32(widget)!=0x482C68&&MEM32(widget)!=0x48A828)||MEM32(widget+0xB8))return 0;
    return isfinite(r[0])&&isfinite(r[1])&&isfinite(r[2])&&isfinite(r[3])&&
        r[0]>=0&&r[1]>=0&&r[2]>=0&&r[3]>=0&&(r[2]==0||r[3]==0)&&
        r[0]+r[2]<=1.001f&&r[1]+r[3]<=1.001f;
}
static int qte_overlap(const float a[4],const float b[4]){
    return a[0]<b[0]+b[2]&&b[0]<a[0]+a[2]&&a[1]<b[1]+b[3]&&b[1]<a[1]+a[3];
}
static unsigned qte_tint_channel(unsigned color,unsigned tint){
    unsigned value=color*tint/128;return value>255?255:value;
}
/* Read-only opt-in rejection evidence, bounded for the guest thread lifetime. */
static void qte_tree_rect_trace(uint32_t w,uint32_t other,const float rect[4],uint32_t form,uint32_t root){
    static RECOMP_TLS int enabled=-1;
    static RECOMP_TLS uint32_t offenders[16];static RECOMP_TLS unsigned count;
    if(enabled<0){const char *value=getenv("RECOMP_QTE_TRACE");enabled=value&&value[0]=='1';}
    if(!enabled||count==16)return;
    for(unsigned i=0;i<count;i++)if(offenders[i]==other)return;
    offenders[count++]=other;
    fprintf(stderr,"[PC-QTE-TREE] target=%08X target_id=%u mask=%02X aspect=%.7g gui_width=%u other=%08X other_id=%u class=%08X visibility=%08X mode=%u rect=%.7g,%.7g,%.7g,%.7g owner=%08X form=%08X root=%08X record=%u\n",
        w,MEM32(w+8),MEM8(w+0xEC),pc_display_aspect(4.f/3),pc_display_render_width(640,1),other,MEM32(other+8),MEM32(other),MEM32(other+0x74),MEM32(other+0x80),
        rect[0],rect[1],rect[2],rect[3],MEM32(other+0x8C),form,root,count);
}
static int qte_runtime_root(uint32_t node){
    /* Actual loaded GuiRootComponent: both original draw and post-draw
     * callbacks are no-ops. Its children still undergo every tree check. */
    return qte_range(node,0x90)&&MEM32(node)==0x47FD90&&MEM32(node+8)==1;
}
static enum QteReason qte_caption_box_reason(uint32_t w,const float legend[4],unsigned kind){
    if(!qte_box(legend))return QTE_LEGEND_BOX;
    uint32_t form=MEM32(w+0x8C);
    if(!qte_range(form,0x5C))return QTE_FORM;
    uint32_t pending[512],seen[512];unsigned count=0,nseen=0;int found=0,root_seen=0;
    uint32_t root=MEM32(form+0x38);if(!qte_range(root,0x90))return QTE_ROOT;
    pending[count++]=root;
    while(count){
        uint32_t p=pending[--count];
        if(!qte_range(p,0x90))return QTE_TREE_RANGE;
        if(nseen==512)return QTE_TREE_LIMIT;
        for(unsigned i=0;i<nseen;i++)if(seen[i]==p)return QTE_TREE_CYCLE;
        seen[nseen++]=p;found|=p==w;
        if(MEM32(p+0x8C)!=form)return QTE_TREE_OWNER;
        unsigned pid=MEM32(p+8);
        static const unsigned roots[]={2,34,2},first[]={189,96,8},second[]={190,97,9},last[]={215,114,26};
        root_seen|=pid==roots[kind]||(p==root&&qte_runtime_root(p));
        /* The authored full-screen container and three neurohack background
         * layers intentionally contain this legend, not another control. */
        int background=((p==root&&qte_runtime_root(p))||pid==roots[kind]||pid==first[kind]||pid==second[kind]||pid==last[kind]||
            (kind==2&&pid==3)); /* Mech's second authored full-screen container. */
        if(!background&&(MEM32(p+0x74)&2)){
            float other[4];widget_rect(p,other);
            if(!qte_box(other)){
                if(!qte_empty_untextured_bitmap(p,other)){qte_tree_rect_trace(w,p,other,form,root);return QTE_TREE_BOX;}
            }else if(qte_overlap(legend,other))return QTE_TREE_OVERLAP;
        }
        uint32_t next=MEM32(p+0x18),child=MEM32(p+0x1C);
        if(next){if(count==512)return QTE_TREE_LIMIT;pending[count++]=next;}
        if(child){if(count==512)return QTE_TREE_LIMIT;pending[count++]=child;}
    }
    return found&&root_seen?QTE_OK:QTE_TREE_MISSING;
}
static enum QteReason qte_legend_reason(uint32_t w,const float r[4],float legend[4]){
    /* Xbox 330C0 retains authored ID at +8; +4 may be remapped, +6C is tab_id. */
    unsigned id=MEM32(w+8),mask=MEM8(w+0xEC);int x,y,start;
    if(mask==0x10&&id>=207&&id<=210){x=id<209?341:343;y=id<209?402:403;start=323;}
    else if(mask==0x20&&id>=211&&id<=214){x=id<213?265:263;y=id<213?402:403;start=207;}
    else return QTE_LEGEND_KIND;
    float ux=r[2]/32.0f,uy=r[3]/32.0f;
    legend[0]=r[0]+(start-x)*ux;legend[1]=r[1]+(444-y)*uy;legend[2]=112*ux;legend[3]=16*uy;
    return qte_caption_box_reason(w,legend,0);
}
static enum QteReason qte_side_reason(uint32_t w,const float r[4],float legend[4]){
    unsigned id=MEM32(w+8),mask=MEM8(w+0xEC),kind,offset;int x,y,row,column;
    if(id>=191&&id<=206){kind=0;offset=id-191;}
    else if(id>=98&&id<=113){kind=1;offset=id-98;}
    else if(id>=10&&id<=25){kind=2;offset=id-10;}
    else return QTE_LEGEND_KIND;
    static const unsigned masks[]={4,1,2,0x40};if(mask!=masks[offset/4])return QTE_LEGEND_KIND;
    int shifted=offset%4>=2;
    switch(mask){
    case 4:x=303;y=shifted?375:374;row=360;column=385;break;
    case 1:x=shifted?288:289;y=360;row=360;column=193;break;
    case 2:x=303;y=shifted?345:346;row=340;column=193;break;
    default:x=shifted?318:317;y=360;row=340;column=385;break;
    }
    float ux=r[2]/32.0f,uy=r[3]/32.0f;
    legend[0]=r[0]+(column-x)*ux;legend[1]=r[1]+(row-y)*uy;legend[2]=88*ux;legend[3]=16*uy;
    return qte_caption_box_reason(w,legend,kind);
}
/* A compact trigger caption occupies only the original icon's pixels.
 * Validate every link/owner independently of unrelated HUD geometry. */
static enum QteReason qte_compact_trigger_identity(uint32_t w){
    unsigned id=MEM32(w+8),mask=MEM8(w+0xEC);
    if(!((mask==0x10&&id>=207&&id<=210)||(mask==0x20&&id>=211&&id<=214)))return QTE_LEGEND_KIND;
    uint32_t form=MEM32(w+0x8C);if(!qte_range(form,0x5C))return QTE_FORM;
    uint32_t root=MEM32(form+0x38);if(!qte_range(root,0x90)||(MEM32(root+8)!=2&&!qte_runtime_root(root)))return QTE_ROOT;
    uint32_t pending[512],seen[512];unsigned count=1,nseen=0;int found=0;pending[0]=root;
    while(count){
        uint32_t node=pending[--count];if(!qte_range(node,0x90))return QTE_TREE_RANGE;
        if(nseen==512)return QTE_TREE_LIMIT;
        for(unsigned i=0;i<nseen;i++)if(seen[i]==node)return QTE_TREE_CYCLE;
        seen[nseen++]=node;found|=node==w;
        if(MEM32(node+0x8C)!=form)return QTE_TREE_OWNER;
        uint32_t next=MEM32(node+0x18),child=MEM32(node+0x1C);
        if(next){if(count==512)return QTE_TREE_LIMIT;pending[count++]=next;}
        if(child){if(count==512)return QTE_TREE_LIMIT;pending[count++]=child;}
    }
    return found?QTE_OK:QTE_TREE_MISSING;
}
static int qte_compact_trigger_caption(uint32_t w,Ma2QteCaption *caption){
    if(pc_input_device()==PC_INPUT_GAMEPAD)return 0;
    enum PcBinding primary,secondary;int first,second;
    if(MEM8(w+0xEC)==0x10){primary=PC_MOUSE_ALT_FIRE;secondary=PC_LEFT_TRIGGER;first=VK_RBUTTON;second='C';}
    else if(MEM8(w+0xEC)==0x20){primary=PC_MOUSE_FIRE;secondary=PC_RIGHT_TRIGGER;first=VK_LBUTTON;second='V';}
    else return 0;
    wchar_t name[8],other[8],detail[24],other_detail[24];
    /* OEM/unnamed keys require the existing explanatory legend. Preserve
     * that refusal rather than hiding its OEM identity/rebind instruction. */
    if(qte_binding_name(primary,first,name,detail)||qte_binding_name(secondary,second,other,other_detail))return 0;
    unsigned advance=qte_caption_advance(name),alternate=qte_caption_advance(other);
    if(!advance||advance>28||!alternate||alternate>28)return 0;
    uint32_t label=ma2_pc_wide(name),alternative=0;
    if(pc_binding_get(primary,first)!=pc_binding_get(secondary,second)){alternative=ma2_pc_wide(other);if(!alternative)return 0;}
    if(!label)return 0;
    caption->label=label;caption->alternative=alternative;caption->legend=0;caption->advance=(uint16_t)advance;return 1;
}
static void qte_release(uint32_t resource,unsigned slot){
    recomp_func_t fn=qte_method(resource,slot);
    if(fn){ecx=resource;PUSH32(esp,0x001FE681);RECOMP_ABI_CALL(MEM32(MEM32(resource)+slot),fn);}
}
static void qte_font_draw(uint32_t style,uint32_t text,uint32_t rect,uint32_t clip,uint32_t color){
    ecx=style+4;eax=color;PUSH32(esp,0x001FF9E0);RECOMP_ABI_CALL(0x0011D75C,sub_0011D75C);
    edi=rect;esi=clip;PUSH32(esp,text);PUSH32(esp,style);
    /* C1640 scopes its own file-local virtual-font depth around glyph drawing,
     * preserving nested depth and the existing viewport-offset correction. */
    PUSH32(esp,0x001FFD75);RECOMP_ABI_CALL(0x000C1640,sub_000C1640);esp+=8;
}
int ma2_gui_qte_draw(uint32_t w){
    Ma2QteCaption caption;
    int side;
    enum QteReason reason=QTE_OK;
#define QTE_EARLY(code) do{qte_trace(code,w,NULL,0,0);return 0;}while(0)
    if(qte_busy)QTE_EARLY(QTE_REENTRY);
    if(!qte_range(w,0xF4))QTE_EARLY(QTE_WIDGET);
    if(MEM32(w)!=0x48A828)QTE_EARLY(QTE_CLASS);
    if(MEM32(w+0xF0)>3)QTE_EARLY(QTE_STATE);
    if(!qte_fitted_caption(MEM8(w+0xEC),&caption,&side))QTE_EARLY(QTE_CAPTION);
    /* Authored QTEs use the complete 32px sprite, never a sub-UV rectangle. */
    if(MEM32(w+0xBC)||MEM32(w+0xC0)||MEM32(w+0xC4)||MEM32(w+0xC8))QTE_EARLY(QTE_UV);
    uint32_t original=MEM32(w+0xB8),device=MEM32(0x50C704),system=MEM32(0x50C958);
    recomp_func_t texture_load=qte_method(device,0x60);
    if(!qte_method(original,0x30))QTE_EARLY(QTE_ORIGINAL_RELEASE);
    if(!texture_load)QTE_EARLY(QTE_DEVICE_LOAD);
    if(!qte_range(system,0x34))QTE_EARLY(QTE_FONT_SYSTEM);
    if(!qte_method(MEM32(system+0x30),8))QTE_EARLY(QTE_FONT_REGISTRY);
    if(!qte_range(esp-4096,4096))QTE_EARLY(QTE_STACK);
#undef QTE_EARLY
#define QTE_FINISH(code) do{reason=code;goto finish;}while(0)
    GuiRegisters saved=save_registers();
    qte_busy=1;esp-=1024;uint32_t scratch=esp,texture=0,font=0;int success=0,compact=0;
    float rect[4],legend[4];widget_rect(w,rect);
    if(!qte_box(rect))QTE_FINISH(QTE_RECT);
    if(!qte_symbol_size(rect))QTE_FINISH(QTE_SIZE);
    /* KBM mouse order: move the complete authored trigger state and its
     * current action captions together. No guest geometry/input is changed. */
    unsigned trigger_id=MEM32(w+8),trigger_mask=MEM8(w+0xEC);
    if((trigger_mask==0x10&&trigger_id>=207&&trigger_id<=210)||(trigger_mask==0x20&&trigger_id>=211&&trigger_id<=214)){
        if((reason=qte_compact_trigger_identity(w))!=QTE_OK)goto finish;
        int delta=trigger_mask==0x10?(trigger_id<209?76:80):-(trigger_id<213?76:80);
        rect[0]+=delta*(rect[2]/32.0f);
        if(!qte_box(rect))QTE_FINISH(QTE_RECT);
    }
    if(caption.legend&&(reason=side?qte_side_reason(w,rect,legend):qte_legend_reason(w,rect,legend))!=QTE_OK){
        if(side||(reason!=QTE_TREE_BOX&&reason!=QTE_TREE_OVERLAP))goto finish;
        if((reason=qte_compact_trigger_identity(w))!=QTE_OK)goto finish;
        if(!qte_compact_trigger_caption(w,&caption))QTE_FINISH(QTE_CAPTION);
        compact=1;
    }
    static const char *roles[]={"button_A","button_X","button_Y","button_B","trigger_L","trigger_R"};
    static const char *states[]={"idle","active","pressed","error"};
    unsigned role;
    switch(MEM8(w+0xEC)){case 4:role=0;break;case 1:role=1;break;case 2:role=2;break;
    case 0x40:role=3;break;case 0x10:role=4;break;case 0x20:role=5;break;default:QTE_FINISH(QTE_CAPTION);}
    unsigned state=MEM32(w+0xF0);
    char *path=(char *)(g_xbox_mem_offset+scratch+256);
    int length=snprintf(path,128,"\\Interface\\PC\\QTE\\%s_%s.tif",roles[role],states[state]);
    if(length<=0||length>=128)QTE_FINISH(QTE_PATH);
    MEM32(scratch+224)=scratch+256;MEM32(scratch+228)=scratch+256+length;MEM32(scratch+232)=scratch+384;
    ecx=device;PUSH32(esp,0);PUSH32(esp,0);PUSH32(esp,scratch+224);
    PUSH32(esp,0x001FE5FF);RECOMP_ABI_CALL(MEM32(MEM32(device)+0x60),texture_load);texture=eax;
    if(!qte_method(texture,0x30))QTE_FINISH(QTE_TEXTURE_RELEASE);
    if(MEM32(texture)!=MEM32(original))QTE_FINISH(QTE_TEXTURE_CLASS);
    recomp_func_t dimensions=qte_method(texture,8);
    if(!dimensions)QTE_FINISH(QTE_TEXTURE_SIZE_METHOD);
    ecx=texture;PUSH32(esp,scratch+212);PUSH32(esp,scratch+208);
    PUSH32(esp,0x001FE6C5);RECOMP_ABI_CALL(MEM32(MEM32(texture)+8),dimensions);
    if(MEM32(scratch+208)!=32||MEM32(scratch+212)!=32)QTE_FINISH(QTE_TEXTURE_SIZE);
    const char *font_path="\\Interface\\Fonts\\English\\ArialUnicodeMSBold10.mgFont";
    memcpy((void *)(g_xbox_mem_offset+scratch+384),font_path,strlen(font_path)+1);
    MEM32(scratch+216)=0;edi=MEM32(system+0x30);eax=scratch+384;
    PUSH32(esp,0);PUSH32(esp,scratch+216);PUSH32(esp,0x001FF779);
    RECOMP_ABI_CALL(0x002B46B0,sub_002B46B0);font=MEM32(scratch+216);
    if(!LO8(eax))QTE_FINISH(QTE_FONT_LOOKUP);
    if(!qte_method(font,0xC))QTE_FINISH(QTE_FONT_RELEASE);
    eax=scratch+64;PUSH32(esp,0x001FF75E);RECOMP_ABI_CALL(0x000C2470,sub_000C2470);
    MEM32(scratch+64)=font;MEM32(scratch+64+0x38)=1;MEM32(scratch+64+0x3C)=1;
    MEMF(scratch+64+0x34)=1.0f;
    memcpy((void *)(g_xbox_mem_offset+scratch),rect,16);
    float uv[4]={0,0,1,1};unsigned flags=MEM32(w+0xD8);
    if(flags&0x10)uv[3]=-1;if(flags&0x20)uv[2]=-1;
    memcpy((void *)(g_xbox_mem_offset+scratch+16),uv,16);
    uint32_t color=(MEM32(w+0xCC)&0xFFFFFF)|((uint32_t)MEM8(w+0xD4)<<24);
    PUSH32(esp,0x001FE810);RECOMP_ABI_CALL(0x00018DE0,sub_00018DE0);uint32_t context=eax;
    PUSH32(esp,MEM32(w+0xD0));PUSH32(esp,color);PUSH32(esp,scratch);PUSH32(esp,context);
    eax=texture;edi=scratch+16;PUSH32(esp,0x001FE81F);RECOMP_ABI_CALL(0x0007E4E0,sub_0007E4E0);
    static const uint32_t foreground[]={0xFFF0F0F0,0xFF101820,0xFFFFFFFF,0xFFFFB8C0};
    static const uint32_t edge[]={0xFF101820,0xFF40D0FF,0xFFFFFF60,0xFF500010};
    float clip[4]={0,0,rect[2],rect[3]};memcpy((void *)(g_xbox_mem_offset+scratch+32),clip,16);
    for(unsigned row=0;row<(compact&&caption.alternative?2u:1u);row++)for(int d=0;d<5;d++){
        float box[4];memcpy(box,rect,16);
        if(compact&&caption.alternative){box[3]=rect[3]/2;box[1]+=row*box[3];}
        clip[2]=box[2];clip[3]=box[3];memcpy((void *)(g_xbox_mem_offset+scratch+32),clip,16);
        if(d==0)box[0]-=rect[2]/32;if(d==1)box[0]+=rect[2]/32;
        if(d==2)box[1]-=rect[3]/32;if(d==3)box[1]+=rect[3]/32;
        if(compact){
            clip[0]=rect[0]-box[0];
            clip[1]=rect[1]+(caption.alternative?row*rect[3]/2:0)-box[1];
            memcpy((void *)(g_xbox_mem_offset+scratch+32),clip,16);
        }
        memcpy((void *)(g_xbox_mem_offset+scratch+128),box,16);
        uint32_t tint=(d==4?foreground[state]:edge[state]);
        /* Apply the authored per-widget RGB tint and separate opacity. */
        unsigned rgb=MEM32(w+0xCC);
        uint32_t tinted=qte_tint_channel(tint&255,rgb&255)|
            (qte_tint_channel((tint>>8)&255,(rgb>>8)&255)<<8)|
            (qte_tint_channel((tint>>16)&255,(rgb>>16)&255)<<16)|((uint32_t)MEM8(w+0xD4)<<24);
        qte_font_draw(scratch+64,row?caption.alternative:caption.label,scratch+128,scratch+32,tinted);
    }
    if(caption.legend){
        memcpy((void *)(g_xbox_mem_offset+scratch+128),legend,16);
        clip[2]=legend[2];clip[3]=legend[3];memcpy((void *)(g_xbox_mem_offset+scratch+32),clip,16);
        qte_font_draw(scratch+64,caption.legend,scratch+128,scratch+32,((uint32_t)MEM8(w+0xD4)<<24)|0xF0F0F0);
    }
    success=1;if(compact)reason=QTE_COMPACT;
finish:
    qte_trace(reason,w,rect,texture,font);
#undef QTE_FINISH
    if(font)qte_release(font,0xC);
    if(texture)qte_release(texture,0x30);
    restore_registers(&saved);qte_busy=0;return success;
}
