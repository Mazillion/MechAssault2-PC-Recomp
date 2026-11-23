#include "pc_settings.h"
#include "ma2_startup_policy.h"
#include "platform/pc_frontend.h"
#include <windows.h>
#include <windowsx.h>
#include <d3d11.h>
#include <xinput.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include <algorithm>
#include <atomic>
#include <cwctype>
#include "imgui.h"
#include "backends/imgui_impl_win32.h"
#include "backends/imgui_impl_dx11.h"
#include "ma2_release.h"
#include "pc_update_check.h"     // 1.1.7: the update line on the main menu
#include "pc_report_replies.h"   // 1.2.0: the developer's replies to the player's reports
#include "pc_messages.h"         // 1.2.0: Messages (updates, what's new, replies)
#include <shellapi.h>
extern "C" ptrdiff_t g_xbox_mem_offset;
// The active host pad (xinput_device.c): slot 0 if connected, else the lowest connected slot, switching on a new
// button press, with empty slots probed once a second. The settings page and the Editor button read it too.
extern "C" DWORD xbox_InputPollHostState(XINPUT_STATE *state);
extern "C" BOOL xbox_InputAnyPadConnected(void);
static uint32_t &guest(uint32_t address){return *(uint32_t *)(g_xbox_mem_offset+address);}
static char ini_path[MAX_PATH];
static std::atomic<DWORD> menu_time;
static std::atomic<uint32_t> menu_kind,menu_object;
static std::atomic<HWND> game_window;
static std::atomic<bool> opened,request_exit;
static std::atomic<uint32_t> exit_dialog;
static DWORD opened_at;
static uint32_t page_object,page_form;
static int page,selection,binding_page,waiting_binding=-1;
static unsigned last_revision;
static bool page_dirty,settings_write_failed,fullscreen;
static bool distance_dragging;
static bool fps_context_initialized,fps_platform_initialized,fps_renderer_initialized;
static bool renderer_error_handled;
static int output_width=1280,output_height=960;
static std::vector<std::pair<int,int>> resolutions;
static const char *binding_names[PC_BINDING_COUNT]={"Start","Back","A","B","X","Y","White","Black","LeftTrigger","RightTrigger","MoveLeft","MoveRight","MoveBack","MoveForward","LeftThumb","RightThumb","MouseFire","MouseAltFire"};
// W-05: rows are named by action; the pad button follows in brackets. The labels go through the prompt
// rewriter (pc_prompts.cpp), so they must not contain "B button", "White button" and the like: those
// would be replaced by the bound key.
static const wchar_t *binding_labels[PC_BINDING_COUNT]={L"Pause / objectives (Start)",L"Back button",L"Jump jets / neurohack (A)",L"Pad B",L"Pad X",L"Enter / exit vehicle (Y)",L"White (RB)",L"Black (LB)",L"Next weapon, key (LT)",L"Fire, key (RT)",L"Move left",L"Move right",L"Move backward",L"Move forward",L"Left stick click",L"Right stick click",L"Fire, mouse (RT)",L"Next weapon, mouse (LT)"};
static int default_keys[PC_BINDING_COUNT]={VK_RETURN,VK_BACK,VK_SPACE,'F','R','Q','1','2','C','V','A','D','S','W',VK_SHIFT,VK_CONTROL,VK_LBUTTON,VK_RBUTTON};
// Default letter and digit bindings follow the physical keys of the QWERTY layout (W is scan 0x11), so an AZERTY or
// Dvorak player gets WASD where WASD sits on their keyboard (easy-wins B1.13, D6). Mouse, Enter, Space etc. are fixed.
static void init_default_keys(ma2_scan_mapper mapper){
    static const struct{int index;unsigned scan;}letters[]={{4,0x13},{5,0x10},{6,0x02},{7,0x03},{3,0x21},{8,0x2E},{9,0x2F},{10,0x1E},{11,0x20},{12,0x1F},{13,0x11}};
    for(const auto &l:letters)default_keys[l.index]=ma2_default_key_from_scan(l.scan,default_keys[l.index],mapper);
}
static int env_int(const char *name,int fallback){const char *s=getenv(name);return s&&*s?atoi(s):fallback;}
static bool menu_available(){uint32_t kind=menu_kind.load();return kind&&kind!=0x481EF8&&GetTickCount()-menu_time.load()<250;}
// Last menu/HUD pulse (every frame in play and in menus): the bots' freeze watchdog (ma2_bots_core.c).
extern "C" DWORD ma2_pc_menu_pulse_ms(void){return menu_time.load();}
static bool modal_available(){return g_xbox_mem_offset&&guest(0x5240D4)>guest(0x5240D0);}
struct Event {UINT type;int key,x,y;uint32_t text_generation=0;};
static SRWLOCK events_lock=SRWLOCK_INIT;
static std::vector<Event> events;
static void queue(Event e){AcquireSRWLockExclusive(&events_lock);if(events.size()<256)events.push_back(e);ReleaseSRWLockExclusive(&events_lock);}
/* Guest-thread modal identity, published with the window event queue. The
 * window thread never dereferences a guest object or edits the name vector. */
static uint32_t text_record,text_object,text_generation=1;
static bool text_focused=true,text_cancel_pending;
static std::atomic<bool> text_closed;
struct TextDecoder {
    UINT code_page=0;char bytes[4]={};unsigned used=0,expected=0;
    void clear(){used=expected=0;}
    uint32_t decode(UINT cp,bool unicode,uintptr_t value){
        if(unicode){clear();return value<=0xFFFF?(uint32_t)value:0;}
        if(code_page!=cp){clear();code_page=cp;}
        if(value>255){
            clear();
            if(value>0xFFFF||!IsDBCSLeadByteEx(cp,(BYTE)(value>>8)))return 0;
            char packed[2]={(char)(value>>8),(char)value};wchar_t wide;
            return MultiByteToWideChar(cp,MB_ERR_INVALID_CHARS,packed,2,&wide,1)==1?wide:0;
        }
        BYTE b=(BYTE)value;
        if(used&&cp==CP_UTF8&&(b&0xC0)!=0x80)clear();
        if(!used){
            if(cp==CP_UTF8){
                expected=b<0x80?1:b>=0xC2&&b<=0xDF?2:b>=0xE0&&b<=0xEF?3:b>=0xF0&&b<=0xF4?4:0;
                if(!expected)return 0;
            }else expected=IsDBCSLeadByteEx(cp,b)?2:1;
        }
        bytes[used++]=(char)b;
        if(used<expected)return 0;
        wchar_t wide[2]={};int count=MultiByteToWideChar(cp,MB_ERR_INVALID_CHARS,bytes,used,wide,2);
        clear();return count==1?wide[0]:0; // The retail name editor stores BMP characters.
    }
};
static TextDecoder text_decoder;
static void invalidate_text_events_locked(){
    if(!++text_generation)++text_generation;
    events.erase(std::remove_if(events.begin(),events.end(),[](const Event &e){return e.text_generation!=0;}),events.end());
    text_decoder.clear();
}
static void sync_text_dialog(){
    uint32_t object=0,record=0;
    if(g_xbox_mem_offset&&!opened&&!text_closed&&!pc_display_movie_active())record=ma2_gui_keyboard_dialog(&object);
    AcquireSRWLockExclusive(&events_lock);
    if(record!=text_record||object!=text_object){
        invalidate_text_events_locked();text_record=record;text_object=object;text_cancel_pending=false;
    }
    bool changed=pc_input_text_captured()!=(record!=0);
    pc_input_text_capture(record!=0);
    ReleaseSRWLockExclusive(&events_lock);
    if(changed){pc_input_camera_capture(0);PostMessageA(game_window,PC_WINDOW_CAPTURE_MESSAGE,0,0);}
}
static void text_focus_or_close(UINT message){
    AcquireSRWLockExclusive(&events_lock);
    invalidate_text_events_locked();
    if(message==WM_SETFOCUS)text_focused=true;
    else if(message==WM_KILLFOCUS)text_focused=false;
    else {text_closed=true;text_record=text_object=0;text_cancel_pending=false;pc_input_text_capture(0);}
    ReleaseSRWLockExclusive(&events_lock);
}
static int text_message(HWND window,UINT message,uintptr_t wparam,intptr_t lparam){
    if(!pc_input_text_captured())return 0;
    bool keyboard=false;int handled=0;
    AcquireSRWLockExclusive(&events_lock);
    auto add=[&](UINT type,int key,int x=0,int y=0,unsigned repeat=1){
        if(!text_record||!text_focused||text_cancel_pending)return;
        for(unsigned i=0;i<repeat&&events.size()<256;i++)events.push_back({type,key,x,y,text_generation});
    };
    unsigned repeat=std::clamp((unsigned)(lparam&0xFFFF),1u,32u);
    if(message==WM_CHAR){
        uint32_t code=text_decoder.decode(GetACP(),IsWindowUnicode(window)!=FALSE,wparam);
        if(code>=0x20&&!(code>=0x7F&&code<0xA0)&&code<0xFFFE&&!(code>=0xD800&&code<=0xDFFF))
            add(WM_CHAR,(int)code,0,0,repeat);
        keyboard=true;handled=1;
    }else if(message==WM_KEYDOWN){
        if(wparam==VK_BACK||wparam==VK_LEFT||wparam==VK_RIGHT)add(WM_KEYDOWN,(int)wparam,0,0,repeat);
        else if((wparam==VK_RETURN||wparam==VK_ESCAPE)&&!(lparam&(1LL<<30)))add(WM_KEYDOWN,(int)wparam);
        keyboard=true;handled=1;
    }else if(message==WM_MOUSEMOVE||message==WM_LBUTTONDOWN){
        add(message,VK_LBUTTON,GET_X_LPARAM(lparam),GET_Y_LPARAM(lparam));handled=1;
    }else if(message==WM_KEYUP||message==WM_INPUT||(message>=WM_MOUSEFIRST&&message<=WM_MOUSELAST))handled=1;
    ReleaseSRWLockExclusive(&events_lock);
    if(keyboard)pc_input_note_keyboard();
    return handled;
}
static bool current_text_event(const Event &e,uint32_t &record,uint32_t &object){
    AcquireSRWLockShared(&events_lock);
    bool valid=e.text_generation==text_generation&&text_record&&text_focused&&!text_cancel_pending;
    record=text_record;object=text_object;
    ReleaseSRWLockShared(&events_lock);
    return valid;
}
static void text_cancel(){
    AcquireSRWLockExclusive(&events_lock);invalidate_text_events_locked();text_cancel_pending=true;
    ReleaseSRWLockExclusive(&events_lock);pc_input_menu_button(1);
}
static void menu_point(HWND window,int px,int py,float &x,float &y){
    RECT r={};GetClientRect(window,&r);
    float aspect=pc_display_aspect(4.f/3.f),height=std::min((float)r.bottom,r.right/aspect),width=height*aspect;
    x=(px-(r.right-width)*.5f)/width;y=(py-(r.bottom-height)*.5f)/height;
}
/* Editor button on Custom Scenarios (ux\spec-ingame-editor.md): a square left of the scenario
 * (the mission picker's barrel moves right for it, ma2_bots_ui.c), in the picker's 640x480 units.
 * Click, E or pad Y opens the Scenario Editor (scenario/ma2_scenario.c ma2_scn_editor_open). */
extern "C" int ma2_scn_custom_active(void);
extern "C" int ma2_scn_editor_available(void);
extern "C" int ma2_scn_editor_running(void);
extern "C" void ma2_scn_editor_open(void);
extern "C" const wchar_t *ma2_scn_editor_note(void);
static const float EDITOR_X0=65,EDITOR_X1=113,EDITOR_Y0=330,EDITOR_Y1=378;
static bool editor_button_shown(){return !opened&&!exit_dialog&&menu_available()&&menu_kind==0x48F3E0&&ma2_scn_custom_active()&&!ma2_scn_editor_running();}
/* Pad / keyboard focus on the Editor button (spec section 0): Up, LB or Tab from the scenario row
 * focuses it, A / Enter opens the editor, Right / Down / B / Esc / Tab return to the row; Y and E
 * open it from anywhere on the screen. While the button has focus the guest reads an empty pad
 * (pc_frontend_capture), so the A or Enter that opens the editor cannot also start the mission;
 * the capture ends once the focus is gone and the pad buttons are up. */
static std::atomic<bool> editor_focus{false},editor_capture{false};
static void editor_focus_set(bool on){
    if(editor_focus.exchange(on)==on)return;
    fprintf(stderr,"[SCN] editor button focus %s\n",on?"on":"off");
    if(on&&!editor_capture.exchange(true)){pc_frontend_capture(1);PostMessageA(game_window,PC_WINDOW_CAPTURE_MESSAGE,0,0);}
}
static void editor_capture_release(){if(editor_capture.exchange(false)){pc_frontend_capture(0);PostMessageA(game_window,PC_WINDOW_CAPTURE_MESSAGE,0,0);}}
/* Tests (RECOMP_UI_LIVE): a scripted pad for the Editor button. The live channel's key=<vk> with the
 * Windows VK_GAMEPAD_* codes (0xC3 A ... 0xCE D-pad right) holds / taps that pad button; a tap is kept
 * until the next menu frame reads it. Only the native button logic sees it, never the guest. */
static std::atomic<unsigned> test_pad_held{0},test_pad_tapped{0};
static unsigned test_pad_bit(uintptr_t vk){
    switch(vk){case 0xC3:return XINPUT_GAMEPAD_A;case 0xC4:return XINPUT_GAMEPAD_B;case 0xC5:return XINPUT_GAMEPAD_X;
    case 0xC6:return XINPUT_GAMEPAD_Y;case 0xC7:return XINPUT_GAMEPAD_RIGHT_SHOULDER;case 0xC8:return XINPUT_GAMEPAD_LEFT_SHOULDER;
    case 0xCB:return XINPUT_GAMEPAD_DPAD_UP;case 0xCC:return XINPUT_GAMEPAD_DPAD_DOWN;case 0xCD:return XINPUT_GAMEPAD_DPAD_LEFT;
    case 0xCE:return XINPUT_GAMEPAD_DPAD_RIGHT;case 0xD0:return XINPUT_GAMEPAD_BACK;}   // 0xD0 VK_GAMEPAD_VIEW: Messages
    return 0;
}
// ma2_scenario.c, back from the editor: focus returns to the scenario row before it posts any key.
extern "C" void ma2_pc_editor_focus_clear(void){editor_focus_set(false);editor_capture_release();}
static bool menu_widescreen(){return pc_display_aspect(4.f/3.f)>4.f/3.f+.01f;}
// A normalised menu point (menu_point) in the picker's 640x480 units: widescreen menus are
// 480*aspect units across with the 4:3 panel centred (ma2_bots_ui.c LAYOUT_*).
static float menu_layout_w(){return menu_widescreen()?480.f*pc_display_aspect(4.f/3.f):640.f;}
static void menu_units(float nx,float ny,float &x,float &y){float lw=menu_layout_w();x=nx*lw-(lw-640.f)*.5f;y=ny*480.f;}
static bool editor_button_hit(float nx,float ny){float x,y;menu_units(nx,ny,x,y);return x>=EDITOR_X0-4&&x<EDITOR_X1+4&&y>=EDITOR_Y0-4&&y<EDITOR_Y1+20;}
// A box in the picker's 640x480 units, in client pixels.
static void menu_box(HWND window,float x0,float y0,float x1,float y1,float box[4]){
    RECT r={};GetClientRect(window,&r);
    float aspect=pc_display_aspect(4.f/3.f),height=std::min((float)r.bottom,r.right/aspect),width=height*aspect;
    float ox=(r.right-width)*.5f,oy=(r.bottom-height)*.5f;
    auto nx=[&](float x){float lw=menu_layout_w();return (x+(lw-640.f)*.5f)/lw;};
    box[0]=ox+nx(x0)*width;box[2]=ox+nx(x1)*width;box[1]=oy+y0/480.f*height;box[3]=oy+y1/480.f*height;
}
static void editor_button_box(HWND window,float box[4]){menu_box(window,EDITOR_X0,EDITOR_Y0,EDITOR_X1,EDITOR_Y1,box);}
// Footer hint between the retail footer's left ('1 Player Options') and right groups.
static const float EDITOR_HINT_X0=198,EDITOR_HINT_X1=274,EDITOR_HINT_Y0=412,EDITOR_HINT_Y1=433;
/* Editor button art for Custom Scenarios (4B, ux\spec-ingame-editor.md section 0).
 * Drop-in for pc_native_settings.cpp: replaces the body of draw_editor_button(); same arguments,
 * plus `pressed` (one frame of #326378 fill after the click / A / Enter). Needs <algorithm>, <cmath>
 * and imgui.h (1.92.5); only ImDrawList primitives, no texture.
 * Box b = {x0,y0,x1,y1} in back-buffer pixels (96x96 at 1280x960); everything scales with its width.
 * Look: dark fill, white L corner brackets (editor theme::bracket_frame math), a 3x3 blueprint grid,
 * a yellow pencil lying across it with the line it just drew. Focus = cyan 2 px outline + soft fade. */
static void draw_editor_button(ImDrawList *list,const float b[4],bool hover,bool enabled,bool pressed=false){
    const float w=b[2]-b[0],h=b[3]-b[1],s=w/96.f;const int a=enabled?255:128;
    auto col=[&](int r,int g,int bl,float alpha=1.f){return IM_COL32(r,g,bl,(int)(alpha*a));};
    auto P=[&](float fx,float fy){return ImVec2(b[0]+fx*w,b[1]+fy*h);};
    // focus glow: 2 px cyan outline, then a #2F586D fade over ~6 px
    if(hover&&enabled){
        for(int i=3;i>=1;i--){float o=2.f*s+2.f*i*s;list->AddRect(ImVec2(b[0]-o,b[1]-o),ImVec2(b[2]+o,b[3]+o),col(47,88,109,.75f/i),3*s,0,2.f*s);}
        list->AddRect(ImVec2(b[0]-2*s,b[1]-2*s),ImVec2(b[2]+2*s,b[3]+2*s),col(73,151,179),2*s,0,2.f*s);
    }
    // fill (70 % navy, so the room shows through like the panel)
    list->AddRectFilled(ImVec2(b[0],b[1]),ImVec2(b[2],b[3]),pressed?col(50,99,120,.9f):col(15,26,38,.7f));
    // corner brackets: 3 px, arms a third of the side
    {const ImU32 white=col(252,251,248);const float th=3*s,ax=w/3,ay=h/3;
        auto L=[&](float cx,float cy,float sx,float sy){
            list->AddRectFilled(ImVec2(std::min(cx,cx+sx*ax),std::min(cy,cy+sy*th)),ImVec2(std::max(cx,cx+sx*ax),std::max(cy,cy+sy*th)),white,1.5f*s);
            list->AddRectFilled(ImVec2(std::min(cx,cx+sx*th),std::min(cy,cy+sy*ay)),ImVec2(std::max(cx,cx+sx*th),std::max(cy,cy+sy*ay)),white,1.5f*s);};
        L(b[0],b[1],1,1);L(b[2],b[1],-1,1);L(b[0],b[3],1,-1);L(b[2],b[3],-1,-1);}
    // blueprint grid, 3x3 cells
    {const ImU32 grid=col(70,118,147);const float g0=.2f,g1=.8f,t=std::max(1.f,1.5f*s);
        for(int i=0;i<4;i++){float f=g0+(g1-g0)*i/3.f;list->AddLine(P(f,g0),P(f,g1),grid,t);list->AddLine(P(g0,f),P(g1,f),grid,t);}}
    // the line the pencil just drew, along the bottom grid row
    const ImU32 yellow=hover?col(220,220,106):col(206,206,72);
    list->AddLine(P(.29f,.71f),P(.76f,.71f),yellow,std::max(2.f,3.f*s));
    // pencil: tip at (.29,.71), lying up-right at 45 degrees, length .62 of the side
    {const ImVec2 T=P(.29f,.71f);const float len=.62f*w,r=.068f*w,k=.70710678f;
        const ImVec2 d(k,-k),n(k,k);  // along the pencil, across it
        auto Q=[&](float along,float across){return ImVec2(T.x+d.x*along+n.x*across,T.y+d.y*along+n.y*across);};
        const float cone=.22f*len,lead=.08f*len,ferrule=.86f*len,eraser=.90f*len;
        list->AddTriangleFilled(T,Q(cone,-r),Q(cone,r),col(233,221,180));                       // sharpened wood
        list->AddTriangleFilled(T,Q(lead,-r*lead/cone),Q(lead,r*lead/cone),col(15,26,38));      // lead
        ImVec2 body[4]={Q(cone,-r),Q(ferrule,-r),Q(ferrule,r),Q(cone,r)};
        list->AddConvexPolyFilled(body,4,yellow);
        ImVec2 facet[4]={Q(cone,r*.35f),Q(ferrule,r*.35f),Q(ferrule,r),Q(cone,r)};            // shaded side
        list->AddConvexPolyFilled(facet,4,col(150,150,48));
        ImVec2 band[4]={Q(ferrule,-r),Q(eraser,-r),Q(eraser,r),Q(ferrule,r)};
        list->AddConvexPolyFilled(band,4,col(215,216,217));
        ImVec2 rub[4]={Q(eraser,-r),Q(len,-r),Q(len,r),Q(eraser,r)};
        list->AddConvexPolyFilled(rub,4,col(98,174,206));}
    // caption under the button: yellow when focused (the footer carries the key hint)
    const char *caption=enabled?"Editor":"Editor not installed";
    float size=std::max(13.f,h*.22f);ImFont *font=ImGui::GetFont();
    ImVec2 ts=font->CalcTextSizeA(size,FLT_MAX,0,caption);
    ImVec2 at((b[0]+b[2]-ts.x)*.5f,b[3]+h*.12f);
    list->AddText(font,size,ImVec2(at.x+1.5f*s,at.y+1.5f*s),IM_COL32(0,0,0,(int)(.63f*a)),caption);
    list->AddText(font,size,at,hover&&enabled?col(206,206,72):col(252,251,248),caption);
}
// 'E  Editor' with the keyboard and mouse, the yellow Y button + 'Editor' with a pad (like the
// retail footer legends, which also follow the last device used). Box b in back-buffer pixels.
static void draw_editor_hint(ImDrawList *list,const float b[4],bool pad){
    const float h=b[3]-b[1],size=h*.66f;ImFont *font=ImGui::GetFont();
    const ImU32 text=IM_COL32(160,190,214,255),shadow=IM_COL32(0,0,0,160);
    const char *label="Editor";ImVec2 ls=font->CalcTextSizeA(size,FLT_MAX,0,label);
    float key_w=pad?h*.68f:font->CalcTextSizeA(size,FLT_MAX,0,"E").x,gap=h*.45f,total=key_w+gap+ls.x;
    float x=(b[0]+b[2]-total)*.5f,y=(b[1]+b[3]-ls.y)*.5f;
    if(pad){ImVec2 c(x+key_w*.5f,(b[1]+b[3])*.5f);float r=key_w*.5f;
        list->AddCircleFilled(c,r,IM_COL32(206,190,40,255),24);
        ImVec2 ys=font->CalcTextSizeA(size*.8f,FLT_MAX,0,"Y");
        list->AddText(font,size*.8f,ImVec2(c.x-ys.x*.5f,c.y-ys.y*.5f),IM_COL32(15,26,38,255),"Y");}
    else {list->AddText(font,size,ImVec2(x+1,y+1),shadow,"E");list->AddText(font,size,ImVec2(x,y),text,"E");}
    list->AddText(font,size,ImVec2(x+key_w+gap+1,y+1),shadow,label);list->AddText(font,size,ImVec2(x+key_w+gap,y),text,label);
}
static void draw_editor_note(ImDrawList *list,float width,float height,const wchar_t *note){
    char text[400];WideCharToMultiByte(CP_UTF8,0,note,-1,text,sizeof(text),nullptr,nullptr);
    // wrapped to 70% of the screen, centred near the top
    float size=std::max(16.f,height/48.f),wrap=width*.7f;ImFont *font=ImGui::GetFont();ImVec2 ts=font->CalcTextSizeA(size,FLT_MAX,wrap,text);
    float x=(width-ts.x)*.5f,y=height*.08f;
    list->AddRectFilled(ImVec2(x-14,y-8),ImVec2(x+ts.x+14,y+ts.y+8),IM_COL32(15,26,38,220));
    list->AddText(font,size,ImVec2(x,y),IM_COL32(252,251,248,255),text,nullptr,wrap);
}
/* 1.1.7's update line under the main menu ("Version X is available. Run the installer to update.") is replaced in 1.2.0 by
 * Messages: the envelope's count and an update message with the list of changes and Update now (see Messages below). */
// "Esc: Back" at the end of the settings pages' hint line (keyboard text): its
// glyphs in 640x480 units (x), and relative to the hint row top (y). It is the
// clickable part of the line, with a small margin.
static const float ESC_BACK_X0=392,ESC_BACK_X1=466,ESC_BACK_Y0=3,ESC_BACK_Y1=24;
static bool esc_back_hit(float x,float y){return x>=ESC_BACK_X0-8&&x<ESC_BACK_X1+8&&y>=(page==0?412:400)&&y<448;}
/* Hover glow behind the footer prompt (Esc Back, Enter Select...) under the
 * mouse. The guest thread publishes its box in client pixels every menu frame;
 * render() fades it in and out. */
static SRWLOCK glow_lock=SRWLOCK_INIT;
// Last WM_MOUSEMOVE (client lparam): the pointer of a hidden test window.
static std::atomic<uint32_t> mouse_position{0xFFFFFFFFu};
static float glow_box[4];static DWORD glow_time;static bool glow_hover;
static void publish_glow(bool hover,const float box[4]){
    static int trace=-1;if(trace<0)trace=getenv("RECOMP_MENU_TRACE")!=nullptr;
    if(trace&&hover!=glow_hover){
        if(hover)fprintf(stderr,"[PC-GLOW] footer %.0f,%.0f - %.0f,%.0f\n",box[0],box[1],box[2],box[3]);
        else fprintf(stderr,"[PC-GLOW] none (device=%d camera=%d text=%d movie=%d)\n",(int)pc_input_device(),pc_input_camera_captured(),pc_input_text_captured(),(int)pc_display_movie_active());
    }
    AcquireSRWLockExclusive(&glow_lock);
    glow_hover=hover;glow_time=GetTickCount();if(hover)memcpy(glow_box,box,sizeof(glow_box));
    ReleaseSRWLockExclusive(&glow_lock);
}
static void update_glow(uint32_t object){
    HWND window=game_window.load();POINT p={};RECT r={};float box[4];
    bool pointer=false;
    static const bool scripted=getenv("RECOMP_UI_LIVE")!=nullptr;
    if(window&&scripted){uint32_t m=mouse_position.load();p.x=GET_X_LPARAM(m);p.y=GET_Y_LPARAM(m);pointer=m!=0xFFFFFFFFu;}
    else if(window&&GetForegroundWindow()==window)pointer=GetCursorPos(&p)&&ScreenToClient(window,&p);
    // The name keyboard (text capture) keeps its own form: its footer prompts glow too.
    uint32_t kb_owner=0,kb_record=pc_input_text_captured()&&!opened?ma2_gui_keyboard_dialog(&kb_owner):0;
    if(!pointer||pc_input_device()!=PC_INPUT_KEYBOARD_MOUSE||pc_input_camera_captured()||(pc_input_text_captured()&&!kb_record)||
       pc_display_movie_active()||!GetClientRect(window,&r)||p.x<0||p.y<0||p.x>=r.right||p.y>=r.bottom){publish_glow(false,nullptr);return;}
    if(opened){
        // Settings pages: the clickable "Esc: Back" end of the hint line (see ma2_pc_submenu_update).
        float scale=std::min(r.right/640.0f,r.bottom/480.0f),ox=(r.right-640*scale)*.5f,oy=(r.bottom-480*scale)*.5f;
        float x=(p.x-ox)/scale,y=(p.y-oy)/scale;
        bool hover=esc_back_hit(x,y);
        float text_y=page==0?416.f:406.f;
        box[0]=ox+ESC_BACK_X0*scale;box[1]=oy+(text_y+ESC_BACK_Y0)*scale;box[2]=ox+ESC_BACK_X1*scale;box[3]=oy+(text_y+ESC_BACK_Y1)*scale;
        publish_glow(hover,box);return;
    }
    float aspect=pc_display_aspect(4.f/3.f),height=std::min((float)r.bottom,r.right/aspect),width=height*aspect;
    float ox=(r.right-width)*.5f,oy=(r.bottom-height)*.5f,n[4];
    bool hover=kb_record?ma2_gui_footer_in_form(guest(kb_owner+0x23C),(p.x-ox)/width,(p.y-oy)/height,aspect,n)!=0:
                         ma2_gui_footer_at(modal_available()?0:object,(p.x-ox)/width,(p.y-oy)/height,aspect,n)!=0;
    if(!hover&&!kb_record&&!modal_available()&&!getenv("RECOMP_NO_BARREL_ARROWS")){
        // A vertical barrel's neighbour rows and arrows glow like a footer prompt (the middle row is
        // the game's own selection highlight).
        int step=0;float centre[2];
        if(ma2_vbarrel_pointer(object,aspect>4.f/3.f+.01f,(p.x-ox)/width,(p.y-oy)/height,&step,n,centre)&&step){
            box[0]=ox+n[0]*width;box[1]=oy+n[1]*height;box[2]=ox+(n[0]+n[2])*width;box[3]=oy+(n[1]+n[3])*height;
            publish_glow(true,box);return;
        }
    }
    if(getenv("RECOMP_MENU_TRACE")){static LONG lx=-1,ly=-1;if(p.x!=lx||p.y!=ly){lx=p.x;ly=p.y;
        fprintf(stderr,"[PC-GLOW] pointer %ld,%ld -> %.3f,%.3f owner=%08X modal=%d hover=%d\n",p.x,p.y,(p.x-ox)/width,(p.y-oy)/height,object,(int)modal_available(),(int)hover);}}
    if(hover){
        // Legend rows are taller than their text: keep the glow on the glyphs (22 of 480 lines).
        float unit=height/480.f,cy=oy+(n[1]+n[3]*.5f)*height,half=std::min(n[3]*height,22.f*unit)*.5f;
        box[0]=ox+n[0]*width;box[1]=cy-half;box[2]=ox+(n[0]+n[2])*width;box[3]=cy+half;
    }
    publish_glow(hover,box);
}
static float glow_alpha(float box[4]){
    static float alpha,shown[4];static LARGE_INTEGER last;LARGE_INTEGER now,frequency;
    QueryPerformanceCounter(&now);QueryPerformanceFrequency(&frequency);
    float dt=last.QuadPart?std::min((float)(now.QuadPart-last.QuadPart)/frequency.QuadPart,.1f):0.f;last=now;
    AcquireSRWLockShared(&glow_lock);
    // A menu that stopped pulsing (closed, loading, in play) drops its glow.
    bool hover=glow_hover&&GetTickCount()-glow_time<200;
    if(hover){if(alpha<=0.f)memcpy(shown,glow_box,sizeof(shown));
        else for(int i=0;i<4;i++)shown[i]+=(glow_box[i]-shown[i])*std::min(1.f,dt*30.f);}
    ReleaseSRWLockShared(&glow_lock);
    alpha=std::clamp(alpha+(hover?dt/.10f:-dt/.16f),0.f,1.f);
    memcpy(box,shown,sizeof(shown));return alpha;
}
static void draw_glow(ImDrawList *list,const float b[4],float alpha){
    // Soft blue-white halo: stacked rounded boxes fading outwards, densest on the text.
    // Reach and opacity halved after playtesting (2026-10-15); the vertical reach is half again.
    float h=b[3]-b[1],spread=h*.375f;const int layers=14;
    for(int i=layers;i>=0;i--){
        float t=(float)i/layers,ex=h*.15f+spread*t,ey=spread*t*.3f,a=alpha*(i?.0225f:.03f);
        list->AddRectFilled(ImVec2(b[0]-ex,b[1]-ey),ImVec2(b[2]+ex,b[3]+ey),
            IM_COL32(165,210,255,(int)(a*255.f+.5f)),(h*.5f+ey));
    }
}
/* The report page's text box around rows 1-4 (640x480 units x 70..570, y 124..241), in the name
 * keyboard's text-field style: a thin blue rule with a dark line on each side (a recessed field; the
 * overlay draws over the game's text, so the inside stays clear); with focus the rule turns the hover
 * glow's blue-white and the same soft halo surrounds it. The settings pages are 4:3, centred. */
static void draw_report_box(ImDrawList *list,float width,float height,bool focus){
    float scale=std::min(width/640.f,height/480.f),ox=(width-640*scale)*.5f,oy=(height-480*scale)*.5f,t=std::max(1.f,1.25f*scale);
    ImVec2 a(ox+70*scale,oy+124*scale),b(ox+570*scale,oy+241*scale);
    if(focus){float spread=6*scale;
        for(int i=8;i>=1;i--){float k=i/8.f;list->AddRect(ImVec2(a.x-spread*k,a.y-spread*k),ImVec2(b.x+spread*k,b.y+spread*k),IM_COL32(165,210,255,(int)(28*(1-k)+6)),3*scale,0,1.5f*scale);}}
    list->AddRect(ImVec2(a.x-t,a.y-t),ImVec2(b.x+t,b.y+t),IM_COL32(4,14,28,170),2*scale,0,t);
    list->AddRect(ImVec2(a.x+t,a.y+t),ImVec2(b.x-t,b.y-t),IM_COL32(4,14,28,120),2*scale,0,t);
    list->AddRect(a,b,focus?IM_COL32(170,214,250,235):IM_COL32(74,128,178,230),2*scale,0,t);
}
static DWORD settings_write_error;
static void write_int(const char *section,const char *name,int value){char text[32];snprintf(text,sizeof(text),"%d",value);
    if(!WritePrivateProfileStringA(section,name,text,ini_path)){DWORD error=GetLastError();if(!settings_write_failed)settings_write_error=error;settings_write_failed=true;
        fprintf(stderr,"[PC-MENU] cannot save [%s] %s (error %lu)\n",section,name,error);}}
extern "C" int ma2_pc_ini_get_int(const char *section,const char *key,int fallback){return (int)GetPrivateProfileIntA(section,key,fallback,ini_path);}
extern "C" void ma2_pc_ini_set_int(const char *section,const char *key,int value){char text[32];snprintf(text,sizeof(text),"%d",value);WritePrivateProfileStringA(section,key,text,ini_path);}
static void read_window_state(){
    HWND window=game_window.load();if(!window)return;
    fullscreen=(GetWindowLongPtrA(window,GWL_STYLE)&WS_OVERLAPPEDWINDOW)==0;
    RECT client={};GetClientRect(window,&client);
    unsigned width,height;pc_render_size_get(&width,&height);output_width=width;output_height=height;
    page_dirty=true;
}
static void save_settings(bool apply_window)
{
    settings_write_failed=false;
    write_int("Display","WindowWidth",output_width);write_int("Display","WindowHeight",output_height);
    write_int("Display","Fullscreen",fullscreen);write_int("Display","VSync",pc_setting_get(PC_VSYNC,0));
    write_int("Display","FrameLimit",pc_setting_get(PC_FRAME_LIMIT,0));WritePrivateProfileStringA("Display","RenderScale",nullptr,ini_path);
    write_int("Display","PreserveAspect",pc_setting_get(PC_ASPECT,1));write_int("Display","SmoothScaling",pc_setting_get(PC_FILTER,1));
    write_int("Display","ShowFPS",pc_setting_get(PC_SHOW_FPS,0));write_int("Audio","Volume",pc_setting_get(PC_VOLUME,100));
    write_int("Display","DrawDistance",pc_setting_get(PC_DRAW_DISTANCE,50));
    write_int("Display","TextureQuality",pc_setting_get(PC_TEXTURE_QUALITY,0));
    write_int("Display","FMVQuality",pc_setting_get(PC_FMV_QUALITY,0));
    write_int("Display","PreloadShaders",pc_setting_get(PC_SHADER_PRELOAD,1));
    write_int("Input","Keyboard",pc_setting_get(PC_KEYBOARD,1));write_int("Input","MouseAim",pc_setting_get(PC_MOUSE_AIM,1));
    write_int("Input","MouseSensitivity",pc_setting_get(PC_MOUSE_SENSITIVITY,100));write_int("Input","InvertY",pc_setting_get(PC_INVERT_Y,0));
    write_int("Input","InvertFlight",pc_setting_get(PC_INVERT_FLIGHT,0));
    write_int("Input","PauseOnFocusLoss",ma2_pc_ini_get_int("Input","PauseOnFocusLoss",1)!=0);   /* W-10: keep the keys visible */
    write_int("Audio","MuteInBackground",ma2_pc_ini_get_int("Audio","MuteInBackground",1)!=0);
    for(int i=0;i<PC_BINDING_COUNT;i++)write_int("Bindings",binding_names[i],pc_binding_get((PcBinding)i,default_keys[i]));
    if(apply_window)PostMessageA(game_window,PC_WINDOW_CONFIG_MESSAGE,fullscreen,MAKELPARAM(output_width,output_height));
    fprintf(stderr,"[PC-MENU] %s settings %dx%d fullscreen=%d",settings_write_failed?"failed to save":"saved",output_width,output_height,fullscreen);
    if(settings_write_failed)fprintf(stderr," (error %lu)",settings_write_error);
    fprintf(stderr,"\n");
}
static std::wstring key_name(int key)
{
    switch(key){case VK_LBUTTON:return L"Left mouse";case VK_RBUTTON:return L"Right mouse";case VK_MBUTTON:return L"Middle mouse";
    case VK_XBUTTON1:return L"Mouse 4";case VK_XBUTTON2:return L"Mouse 5";case VK_RETURN:return L"Enter";case VK_ESCAPE:return L"Esc";}
    wchar_t name[48];UINT scan=ma2_key_name_scan(key);
    if(!GetKeyNameTextW(scan<<16,name,48))swprintf_s(name,L"Key %d",key);return name;
}
static bool gamepad_connected(){
    return xbox_InputAnyPadConnected()!=FALSE;
}
static const wchar_t *yesno(PcSetting s,int fallback){return pc_setting_get(s,fallback)?L"On":L"Off";}
/* Optional content the installer may not have produced: the Enhancer's 2x/4x texture packs and
 * its 2x cutscene videos. Only installed choices are offered (same paths as the loaders:
 * nv2a_texture_pack.cpp load_index, bink_native.c enhanced_name). Checked when the page opens. */
extern "C" BOOL xbox_translate_path(const char *xbox_path,WCHAR *host_path_buf,DWORD buf_size);
static bool texture_installed[3]={true,false,false},videos_installed;
/* ---- 1.1.6: "Report a problem" inside the game ----
 * Two ways in: "Report a Problem" on the pause menu (the retail form's hidden dev item, relabelled, moved
 * under the last item and shown; it opens the Audio settings submenu straight on this page), and the row
 * "Report a problem..." on Audio settings. The page is page 5 of the native settings pages, drawn in the
 * Audio settings form (its ten rows, title and hint line, build_pc_assets.py), so it has the game's own
 * font, colours, panel and footer. The text box is drawn around rows 1-4 in the colours of the name
 * keyboard's text field. Typing goes into the box only (the page owns the keyboard: pc_frontend_capture);
 * on a controller, Left / Right on the box picks a ready-made description. Send runs "MA2 Report.exe
 * --game" on a worker thread (no window; the game thread only reads the outcome), shows "Sending..." and
 * then the report number, which goes to the clipboard. Nothing appears unless the player opens the page;
 * with no helper beside the game neither entry is shown (1.1.5 menus). */
extern "C" int ma2_report_helper_path(wchar_t *out,DWORD size);
extern "C" int ma2_report_log_path(wchar_t *out,DWORD size);
extern "C" void apu_health_request(void);
static int report_available=-1;   // the helper exists (checked once)
static bool report_helper_ok(){if(report_available<0){wchar_t p[MAX_PATH];report_available=ma2_report_helper_path(p,MAX_PATH)?1:0;
    fprintf(stderr,"[REPORT] in-game report %s\n",report_available?"available":"off (no MA2 Report.exe beside the game)");}return report_available==1;}
static int report_from=1;          // page to go back to (1 = Audio settings), or -1: close the submenu (opened from the pause menu)
static std::atomic<DWORD> report_pending{0};   // the pause item was chosen at this tick: the Audio submenu opens on the report page
static std::wstring report_text;   // the player's words (game thread only)
static int report_preset=-1;       // the ready-made description in the box (-1 = the player's own words)
static const wchar_t *report_presets[]={L"The game crashed.",L"The game froze or stopped responding.",
    L"Sound problem (crackling, missing or cutting out).",L"Graphics problem.",L"The game runs slowly or stutters.",
    L"Controls or controller problem.",L"Bots behaved strangely.",L"Something else went wrong."};
static const int REPORT_PRESETS=(int)(sizeof report_presets/sizeof *report_presets);
static const size_t REPORT_MAX_TEXT=1000;
static const int REPORT_BOX=1,REPORT_SEND=7,REPORT_BACK=8;
struct ReportOutcome{bool saved=false,ok=false;std::wstring id,cause,message,folder;};
static std::atomic<int> report_state{0};        // 0 typing, 1 sending, 2 outcome ready (report_outcome)
static std::atomic<int> report_can_send{-1};    // the helper has a relay address (1), saves only (0), not known yet (-1)
static ReportOutcome report_outcome;           // written by the worker before report_state=2, read by the game thread after
static bool report_shown_outcome;
static std::wstring report_from_utf8(const std::string &u){if(u.empty())return L"";int n=MultiByteToWideChar(CP_UTF8,0,u.c_str(),(int)u.size(),nullptr,0);
    std::wstring w(n,L'\0');MultiByteToWideChar(CP_UTF8,0,u.c_str(),(int)u.size(),&w[0],n);return w;}
static std::string report_to_utf8(const std::wstring &w){if(w.empty())return "";int n=WideCharToMultiByte(CP_UTF8,0,w.c_str(),(int)w.size(),nullptr,0,nullptr,nullptr);
    std::string u(n,'\0');WideCharToMultiByte(CP_UTF8,0,w.c_str(),(int)w.size(),&u[0],n,nullptr,nullptr);return u;}
static DWORD report_run(const std::wstring &args,DWORD timeout_ms,DWORD *exit_code){
    wchar_t helper[MAX_PATH];if(!ma2_report_helper_path(helper,MAX_PATH))return 0;
    std::wstring cmd=L"\""+std::wstring(helper)+L"\" "+args;
    STARTUPINFOW si={};si.cb=sizeof si;si.dwFlags=STARTF_USESHOWWINDOW;si.wShowWindow=SW_HIDE;PROCESS_INFORMATION pi={};
    std::vector<wchar_t> line(cmd.begin(),cmd.end());line.push_back(0);
    if(!CreateProcessW(helper,line.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW|BELOW_NORMAL_PRIORITY_CLASS,nullptr,nullptr,&si,&pi))return 0;
    CloseHandle(pi.hThread);
    DWORD w=WaitForSingleObject(pi.hProcess,timeout_ms);
    if(w!=WAIT_OBJECT_0)TerminateProcess(pi.hProcess,4);   // our own child only
    GetExitCodeProcess(pi.hProcess,exit_code);CloseHandle(pi.hProcess);
    return w==WAIT_OBJECT_0?1:2;
}
static DWORD WINAPI report_probe_thread(LPVOID){
    DWORD code=3;report_can_send=report_run(L"--can-send",15000,&code)==1&&code==0?1:0;
    fprintf(stderr,"[REPORT] sending %s\n",report_can_send?"set up":"not set up in this build: Save only");
    return 0;
}
static void report_copy(const std::wstring &text){
    if(text.empty()||!OpenClipboard(nullptr))return;
    EmptyClipboard();HGLOBAL h=GlobalAlloc(GMEM_MOVEABLE,(text.size()+1)*sizeof(wchar_t));
    if(h){memcpy(GlobalLock(h),text.c_str(),(text.size()+1)*sizeof(wchar_t));GlobalUnlock(h);if(!SetClipboardData(CF_UNICODETEXT,h))GlobalFree(h);}
    CloseClipboard();
}
static std::wstring report_words;   // handed to the worker (written before it starts)
static DWORD WINAPI report_send_thread(LPVOID){
    ReportOutcome o;
    // The newest [HEALTH] line goes into the log the report carries (printed by the APU thread within a frame).
    apu_health_request();Sleep(100);
    wchar_t tmp[MAX_PATH];DWORD n=GetTempPathW(MAX_PATH,tmp);
    std::wstring base=n&&n<MAX_PATH-40?std::wstring(tmp):std::wstring(L".\\");
    wchar_t name[64];swprintf_s(name,L"ma2-report-%lu-%lu",GetCurrentProcessId(),GetTickCount());
    std::wstring text_file=base+name+L".txt",result_file=base+name+L"-result.txt";
    std::string words=report_to_utf8(report_words);
    HANDLE f=CreateFileW(text_file.c_str(),GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_TEMPORARY,nullptr);
    if(f!=INVALID_HANDLE_VALUE){DWORD w=0;WriteFile(f,words.data(),(DWORD)words.size(),&w,nullptr);CloseHandle(f);}
    wchar_t log[MAX_PATH]=L"";ma2_report_log_path(log,MAX_PATH);
    wchar_t ini[MAX_PATH]=L"";MultiByteToWideChar(CP_ACP,0,ini_path,-1,ini,MAX_PATH);
    std::wstring args=L"--game --kind manual --text-file \""+text_file+L"\" --result \""+result_file+L"\"";
    if(log[0])args+=L" --log \""+std::wstring(log)+L"\"";
    if(ini[0])args+=L" --ini \""+std::wstring(ini)+L"\"";
    DWORD code=2,ran=report_run(args,180000,&code);
    std::string r;
    HANDLE h=CreateFileW(result_file.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);
    if(h!=INVALID_HANDLE_VALUE){char buf[4096];DWORD got=0;if(ReadFile(h,buf,sizeof buf-1,&got,nullptr)){buf[got]=0;r=buf;}CloseHandle(h);}
    DeleteFileW(text_file.c_str());DeleteFileW(result_file.c_str());
    auto value=[&](const char *key){std::string k=std::string("\n")+key+"=";std::string all="\n"+r;size_t a=all.find(k);
        if(a==std::string::npos)return std::wstring();a+=k.size();size_t b=all.find('\n',a);return report_from_utf8(all.substr(a,b==std::string::npos?std::string::npos:b-a));};
    o.saved=value("saved")==L"1";o.ok=value("ok")==L"1";o.id=value("id");o.cause=value("cause_text");o.message=value("message");o.folder=value("folder");
    if(o.ok&&(o.id.size()!=10||o.id.compare(0,4,L"MA2-")))o.ok=false;
    if(!ran)o.cause=L"the report helper could not start";
    else if(ran==2)o.cause=L"the report helper took too long";
    else if(o.cause.empty())o.cause=L"the report could not be made";
    // The number goes to the clipboard (not in harness runs: a test must not change the desktop's clipboard).
    if(o.ok&&!getenv("RECOMP_UI_LIVE"))report_copy(o.id);
    fprintf(stderr,"[REPORT] in-game report: %s%s%s (exit %lu)\n",o.ok?"sent ":o.saved?"saved, not sent: ":"not saved: ",
        o.ok?report_to_utf8(o.id).c_str():"",o.ok?"":report_to_utf8(o.cause).c_str(),code);
    report_outcome=o;report_state=2;
    return 0;
}
static void report_send(){
    if(report_state==1)return;
    report_words=report_text;
    while(!report_words.empty()&&iswspace(report_words.back()))report_words.pop_back();
    report_state=1;report_shown_outcome=false;
    fprintf(stderr,"[REPORT] in-game report: sending (%zu characters typed)\n",report_words.size());
    HANDLE t=CreateThread(nullptr,0,report_send_thread,nullptr,0,nullptr);
    if(t)CloseHandle(t);else{report_outcome=ReportOutcome();report_outcome.cause=L"the report could not be started";report_state=2;}
}
/* ---- 1.2.0: Messages on the main menu (pc_messages.h, pc_report_replies.h) ----
 * An envelope in the main menu's top-right corner with the count of unread messages, opened with M, the controller's
 * View (Back) button or a click. With unread messages a short "1 new message" line fades in beside it at launch; nothing
 * opens by itself. The inbox: a list (newest first, unread marked) and a reader. Three kinds of message:
 *   update    "Version X is available" + that release's list of changes (/v1/latest "notes"); "Update now" closes the
 *             game and starts the installer copy in the install folder (RecompLauncher: update there instead; no copy:
 *             the releases page). It replaces the 1.1.7 update line on the main menu.
 *   whatsnew  "Updated to X" (or "What's new in X" when no earlier version was recorded) with that version's section of
 *             the install's Docs\Changelog.txt, once per version.
 *   reply     the developer's reply to a report sent from this PC (MA2 Report.exe --replies, only when
 *             report-conversations.txt exists) with the answer box, "Attach my latest logs" and Send (--answer).
 * Stored in messages.txt next to pc_settings.ini (format: pc_messages.h); updates and what's new kept 6 months, replies
 * 60 days. Reading a reply tells the relay it was seen (--seen), so the installer does not show it again; a reply read or
 * answered in the installer comes back as read / answered here ("state=" lines). */
// The player-facing words (REPLIES-STRINGS.md). The head line of a reply is ma2_reply_head() in pc_report_replies.h.
static const char *const msg_text_title="Messages";
static const char *const msg_text_new_one="1 new message";
static const char *const msg_text_new_many="%d new messages";
static const char *const msg_text_none="No messages yet.";
static const char *const msg_text_close="Close";
static const char *const msg_text_update_title="Version %s is available";
static const char *const msg_text_update_now="Update now";
static const char *const msg_text_update_now_note="The game closes and the installer opens to update it.";
static const char *const msg_text_update_launcher="The game closes and RecompLauncher opens on this game: press Update there.";
static const char *const msg_text_update_page="Open the releases page";
static const char *const msg_text_update_page_note="Get the new installer from the project's releases page.";
static const char *const msg_text_update_have="This version is installed.";
static const char *const msg_text_update_failed="The installer could not be started.";
static const char *const msg_text_no_notes="The list of changes is on the project's releases page.";
static const char *const msg_text_updated="Updated to %s";
static const char *const msg_text_whats_new="What's new in %s";
static const char *const msg_text_answered="You answered this message.";
static const char *const msg_text_older="Only the newest message of a conversation can be answered.";
static const char *const msg_text_kind_update="Update";
static const char *const msg_text_kind_whatsnew="What's new";
static const char *const msg_text_kind_reply="Reply";
static const char *const msg_text_hint_keys="Up/Down: Choose    Enter: Open    Tab: Next    PgUp/PgDn: Scroll    Esc: Back";
static const char *const msg_text_hint_pad="D-pad: Choose    A: Select    LB/RB: Scroll    B: Back";
static const char *const msg_text_key_keys="M";
static const char *const msg_text_key_pad="View";
static const char *const msg_releases_url="https://github.com/Mazillion/MechAssault2-PC-Recomp/releases";
static const char *const reply_text_answer="Your answer:";
static const char *const reply_text_type="(type here)";
static const char *const reply_text_pad="(Left / Right picks an answer)";
static const wchar_t *const reply_presets[]={L"Thanks, that fixed it.",L"It still happens.",L"I tried that, but nothing changed.",
    L"I'll try that and tell you how it went."};
static const char *const reply_text_attach="Attach my latest logs";
static const char *const reply_text_send="Send";
static const char *const reply_text_private="Please don't type personal details. You're anonymous if you don't.";
static const char *const reply_text_sending="Sending...";
static const char *const reply_text_sent="Answer sent.";
static const char *const reply_text_sent_logs="Answer sent with your latest logs.";
static const char *const reply_text_failed="Could not send your answer (%s).";
static const char *const reply_text_empty="Type an answer first.";
static const char *const reply_text_closed="The developer can't take an answer to this message any more.";
static const int REPLY_PRESETS=(int)(sizeof reply_presets/sizeof *reply_presets);
static bool report_cursor_on();
enum {F_LIST,F_BODY,F_BOX,F_TICK,F_SEND,F_BUTTON,F_CLOSE,F_COUNT};
enum {REPLY_NONE,REPLY_SENT,REPLY_SENT_LOGS,REPLY_FAILED,REPLY_EMPTY,REPLY_CLOSED};
struct InboxMsg{std::string kind,key,date,title,body,sent;bool read=false,can_answer=false,answered=false;};
static SRWLOCK reply_lock=SRWLOCK_INIT;                // guards everything below (window, render, guest and worker threads)
struct ReplyLock{ReplyLock(){AcquireSRWLockExclusive(&reply_lock);}~ReplyLock(){ReleaseSRWLockExclusive(&reply_lock);}};
static std::vector<InboxMsg> inbox;                     // newest first
static std::atomic<int> reply_state{0};                // 0 inbox closed, 1 open, 2 open and sending an answer
static int inbox_sel=0,inbox_focus=F_LIST,inbox_list_top=0,inbox_launch_unread=-1;static DWORD inbox_launch_at;
static float inbox_scroll=0,inbox_scroll_max=0,inbox_scroll_step=40;
static std::wstring reply_answer;static int reply_preset=-1;
static bool reply_attach=true;static int reply_outcome=REPLY_NONE;static std::string reply_reason,update_note;
static float inbox_hit[F_COUNT][4],inbox_item_hit[16][4],envelope_hit[4];static int inbox_items_shown;static bool inbox_hit_valid,envelope_shown;
static TextDecoder reply_decoder;
static std::atomic<int> inbox_pad{-1};                // -1 follow the game's device, 0 keyboard / mouse, 1 controller
static std::string msg_own_version;                     // this install's version (recomp-install.json)
static bool update_merged;
// ---- storage
static std::wstring msg_file(){std::wstring p;wchar_t w[MAX_PATH];MultiByteToWideChar(CP_ACP,0,ini_path,-1,w,MAX_PATH);p=w;
    size_t s=p.find_last_of(L"\\/");return (s==std::wstring::npos?std::wstring(L"."):p.substr(0,s))+L"\\messages.txt";}
static std::string msg_today(){SYSTEMTIME st;GetSystemTime(&st);char d[11];snprintf(d,sizeof d,"%04u-%02u-%02u",st.wYear,st.wMonth,st.wDay);return d;}
static std::string msg_esc(const std::string &s){std::vector<char> b(s.size()*2+2);ma2_msg_escape(s.c_str(),b.data(),b.size());return b.data();}
static std::string msg_reflow(const std::string &t){std::vector<char> b(t.begin(),t.end());b.push_back(0);ma2_msg_reflow(b.data());return b.data();}
static std::string msg_unesc(const std::string &s){std::vector<char> b(s.size()+2);ma2_msg_unescape(s.c_str(),b.data(),b.size());return b.data();}
static bool msg_kept(const InboxMsg &m,int today){int d=ma2_msg_parse_date(m.date.c_str());if(d<0)return false;
    return today-d<(m.kind=="reply"?MA2_MSG_REPLY_DAYS:MA2_MSG_KEEP_DAYS);}
static void msg_sort(){std::stable_sort(inbox.begin(),inbox.end(),[](const InboxMsg &a,const InboxMsg &b){return a.date>b.date;});}
static void msg_load_locked(){
    inbox.clear();std::string all;
    HANDLE h=CreateFileW(msg_file().c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);
    if(h!=INVALID_HANDLE_VALUE){std::vector<char> buf(1<<20);DWORD got=0;if(ReadFile(h,buf.data(),(DWORD)buf.size()-1,&got,nullptr))all.assign(buf.data(),got);CloseHandle(h);}
    const int today=ma2_msg_parse_date(msg_today().c_str());
    size_t p=0;
    while(p<all.size()){
        size_t e=all.find('\n',p);std::string line=all.substr(p,e==std::string::npos?std::string::npos:e-p);p=e==std::string::npos?all.size():e+1;
        if(!line.empty()&&line.back()=='\r')line.pop_back();
        if(line.empty()||line[0]=='#')continue;
        std::vector<std::string> f;size_t a=0;for(;;){size_t t=line.find('\t',a);f.push_back(line.substr(a,t==std::string::npos?std::string::npos:t-a));if(t==std::string::npos)break;a=t+1;}
        if(f.size()!=7||(f[0]!="update"&&f[0]!="whatsnew"&&f[0]!="reply"))continue;
        InboxMsg m;m.kind=f[0];m.key=f[1];m.date=f[2];m.read=f[3]=="1";m.title=msg_unesc(f[5]);m.body=msg_unesc(f[6]);if(m.kind!="reply")m.body=msg_reflow(m.body);
        if(m.kind=="reply"){char sent[16]="";int can=0,ans=0;sscanf(f[4].c_str(),"sent=%15s can=%d answered=%d",sent,&can,&ans);m.sent=sent;m.can_answer=can!=0;m.answered=ans!=0;
            if(m.key.size()<12||!ma2_reply_id_ok(m.key.substr(0,10).c_str()))continue;}
        if(msg_kept(m,today))inbox.push_back(m);
    }
    msg_sort();
}
static void msg_save_locked(){
    std::string out="# MechAssault 2 messages: kind, key, date, read, extra, title, text (kept 6 months; replies 60 days). Never sent anywhere.\n";
    for(auto &m:inbox){
        std::string extra=m.kind=="reply"?"sent="+m.sent+" can="+(m.can_answer?"1":"0")+" answered="+(m.answered?"1":"0"):"";
        out+=m.kind+"\t"+msg_esc(m.key)+"\t"+m.date+"\t"+(m.read?"1":"0")+"\t"+extra+"\t"+msg_esc(m.title)+"\t"+msg_esc(m.body)+"\n";
    }
    std::wstring f=msg_file(),tmp=f+L".part";
    HANDLE h=CreateFileW(tmp.c_str(),GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,0,nullptr);
    if(h==INVALID_HANDLE_VALUE)return;
    DWORD w=0;BOOL ok=WriteFile(h,out.data(),(DWORD)out.size(),&w,nullptr);CloseHandle(h);
    if(ok&&w==out.size())MoveFileExW(tmp.c_str(),f.c_str(),MOVEFILE_REPLACE_EXISTING);else DeleteFileW(tmp.c_str());
}
static InboxMsg *msg_find_locked(const std::string &kind,const std::string &key){for(auto &m:inbox)if(m.kind==kind&&m.key==key)return &m;return nullptr;}
static int msg_unread_locked(){int n=0;for(auto &m:inbox)if(!m.read)n++;return n;}
static std::string msg_read_text(const std::wstring &path,size_t cap){
    std::string r;HANDLE h=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);
    if(h!=INVALID_HANDLE_VALUE){std::vector<char> buf(cap);DWORD got=0;if(ReadFile(h,buf.data(),(DWORD)buf.size()-1,&got,nullptr))r.assign(buf.data(),got);CloseHandle(h);}
    return r;
}
static std::wstring msg_exe_dir(){wchar_t p[MAX_PATH];DWORD n=GetModuleFileNameW(nullptr,p,MAX_PATH);std::wstring s(p,n);size_t k=s.find_last_of(L"\\/");return k==std::wstring::npos?L".":s.substr(0,k);}
static bool msg_newer(const std::string &v){return ma2_update_is_newer(v.c_str(),msg_own_version.c_str())!=0;}
// What's new: once per version, from the install's own Changelog (Docs\Changelog.txt).
static void msg_whats_new_locked(){
    char last[32]="";GetPrivateProfileStringA("Messages","LastVersion","",last,sizeof last,ini_path);
    if(msg_own_version.empty()||msg_own_version==last)return;
    std::string text=msg_read_text(msg_exe_dir()+L"\\Docs\\Changelog.txt",256*1024);
    std::vector<char> sec(8192);ma2_changelog_section(text.c_str(),msg_own_version.c_str(),sec.data(),sec.size());
    if(!msg_find_locked("whatsnew",msg_own_version)){
        InboxMsg m;m.kind="whatsnew";m.key=msg_own_version;m.date=msg_today();
        char t[96];snprintf(t,sizeof t,last[0]?msg_text_updated:msg_text_whats_new,msg_own_version.c_str());m.title=t;
        m.body=sec[0]?msg_reflow(sec.data()):msg_text_no_notes;inbox.insert(inbox.begin(),m);
        fprintf(stderr,"[MSG] what's new in %s added (%s, changes %s)\n",msg_own_version.c_str(),last[0]?"updated":"first record",sec[0]?"from Changelog.txt":"not found");
    }
    WritePrivateProfileStringA("Messages","LastVersion",msg_own_version.c_str(),ini_path);
}
static void msg_initialize(){
    char own[32];ma2_update_own_version(own,sizeof own,MA2_PUBLIC_VERSION);msg_own_version=own;
    ReplyLock l;msg_load_locked();msg_whats_new_locked();msg_save_locked();
    fprintf(stderr,"[MSG] %zu messages, %d unread\n",inbox.size(),msg_unread_locked());
}
// An update announced by the start-up check (pc_update_check.h): one message per version; it replaces the 1.1.7 line.
static void msg_merge_update(){
    if(update_merged)return;
    const char *v=ma2_update_available();if(!v)return;
    update_merged=true;
    ReplyLock l;std::string notes=msg_reflow(ma2_update_notes());
    InboxMsg *m=msg_find_locked("update",v);
    if(!m){InboxMsg n;n.kind="update";n.key=v;n.date=msg_today();char t[96];snprintf(t,sizeof t,msg_text_update_title,v);n.title=t;
        n.body=notes.empty()?msg_text_no_notes:notes;inbox.insert(inbox.begin(),n);fprintf(stderr,"[MSG] update %s added (%s)\n",v,notes.empty()?"no notes":"with notes");}
    else if(!notes.empty()&&m->body!=notes){m->body=notes;}
    msg_save_locked();
}
// ---- replies (MA2 Report.exe)
static std::wstring reply_temp_base(const wchar_t *what){
    wchar_t tmp[MAX_PATH];DWORD n=GetTempPathW(MAX_PATH,tmp);
    wchar_t name[64];swprintf_s(name,L"ma2-%s-%lu-%lu",what,GetCurrentProcessId(),GetTickCount());
    return (n&&n<MAX_PATH-40?std::wstring(tmp):std::wstring(L".\\"))+name;
}
static DWORD WINAPI reply_check_thread(LPVOID){
    std::wstring result=reply_temp_base(L"replies")+L".txt";
    DWORD code=2,ran=report_run(L"--replies --result \""+result+L"\"",60000,&code);
    std::string text=msg_read_text(result,64*1024);DeleteFileW(result.c_str());
    std::vector<Ma2Reply> got(16);int complete=0;
    int n=ma2_reply_parse_result(text.c_str(),got.data(),(int)got.size(),&complete);got.resize(n);
    int added=0,synced=0;
    {ReplyLock l;
        for(auto &r:got){
            char key[24];snprintf(key,sizeof key,"%s/%d",r.id,r.n);
            InboxMsg *m=msg_find_locked("reply",key);
            if(!m){InboxMsg x;x.kind="reply";x.key=key;x.date=msg_today();char d[16];snprintf(d,sizeof d,"%04d-%02d-%02d",r.year,r.month,r.day);x.sent=d;
                char head[96];ma2_reply_head(&r,head,sizeof head);x.title=head;x.body=r.text;x.can_answer=r.can_answer!=0;inbox.insert(inbox.begin(),x);added++;}
            else if(!m->read){m->body=r.text;m->can_answer=r.can_answer!=0;}          // edited by the developer before it was read
        }
        // what the installer (or an earlier run) did: "state=<id> <seen up to> <newest n> <newest is the player's>"
        for(size_t p=0;p<text.size();){
            size_t e=text.find('\n',p);std::string line=text.substr(p,e==std::string::npos?std::string::npos:e-p);p=e==std::string::npos?text.size():e+1;
            char id[16];int seen=0,newest=0,player=0;
            if(sscanf(line.c_str(),"state=%15s %d %d %d",id,&seen,&newest,&player)!=4||!ma2_reply_id_ok(id))continue;
            for(auto &m:inbox){
                if(m.kind!="reply"||m.key.compare(0,10,id))continue;
                int n2=atoi(m.key.c_str()+11);
                if(n2<=seen&&!m.read){m.read=true;synced++;}
                bool can=!player&&n2==newest&&!m.answered;
                if(m.can_answer!=can){m.can_answer=can;if(player&&n2<newest)m.answered=true;synced++;}
            }
        }
        if(added||synced)msg_save_locked();
    }
    fprintf(stderr,"[REPLY] check: %d new, %d updated from the other client (helper %s, exit %lu)\n",added,synced,ran==1?"ran":ran==2?"timed out":"did not start",code);
    return 0;
}
static void reply_data_dir(wchar_t *out,DWORD size){
    DWORD n=GetEnvironmentVariableW(L"MA2_REPORT_DATA_DIR",out,size);
    if(!n||n>=size)n=GetEnvironmentVariableW(L"RECOMP_REPORT_DATA_DIR",out,size);
    if(n&&n<size)return;
    n=GetEnvironmentVariableW(L"LOCALAPPDATA",out,size);
    if(!n||n+20>=size){out[0]=0;return;}
    wcscat_s(out,size,L"\\MechAssault2");
}
static void reply_check_start(){
    // Harness runs (RECOMP_SAVE_DIR) never ask the compiled-in relay: only with a test relay (as the update check).
    if(getenv("RECOMP_SAVE_DIR")&&!getenv("RECOMP_REPORT_RELAY")&&!getenv("MA2_REPORT_RELAY")){fprintf(stderr,"[REPLY] check skipped (test run without a test relay)\n");return;}
    wchar_t dir[MAX_PATH];reply_data_dir(dir,MAX_PATH);
    std::wstring store=std::wstring(dir)+L"\\report-conversations.txt";
    if(!dir[0]||GetFileAttributesW(store.c_str())==INVALID_FILE_ATTRIBUTES){fprintf(stderr,"[REPLY] no report sent from this PC: nothing to check\n");return;}
    if(!report_helper_ok())return;
    HANDLE t=CreateThread(nullptr,64*1024,reply_check_thread,nullptr,STACK_SIZE_PARAM_IS_A_RESERVATION,nullptr);
    if(t){SetThreadPriority(t,THREAD_PRIORITY_BELOW_NORMAL);CloseHandle(t);}
}
static DWORD WINAPI reply_seen_thread(LPVOID p){
    std::string *key=(std::string *)p;char id[16]="";int n=0;sscanf(key->c_str(),"%10s",id);n=atoi(key->c_str()+11);
    wchar_t args[64];swprintf_s(args,L"--seen %hs %d",id,n);
    DWORD code=2,ran=report_run(args,30000,&code);
    fprintf(stderr,"[REPLY] %s %d marked seen: %s\n",id,n,ran==1&&code==0?"yes":"no (the relay is asked again next time)");
    delete key;return 0;
}
static void reply_capture(bool on){pc_frontend_capture(on?1:0);if(game_window)PostMessageA(game_window,PC_WINDOW_CAPTURE_MESSAGE,0,0);}
static InboxMsg *inbox_cur_locked(){return inbox_sel>=0&&inbox_sel<(int)inbox.size()?&inbox[inbox_sel]:nullptr;}
static bool reply_answering_locked(){InboxMsg *m=inbox_cur_locked();
    return m&&m->kind=="reply"&&m->can_answer&&!m->answered&&reply_outcome!=REPLY_SENT&&reply_outcome!=REPLY_SENT_LOGS&&reply_outcome!=REPLY_CLOSED;}
static int update_action_locked(const InboxMsg &m){
    static int managed=-1;   // who updates this install: recomp-install.json "managed_by", RECOMPLAUNCHER only as the fallback
    if(managed<0){std::string stamp=msg_read_text(msg_exe_dir()+L"\\recomp-install.json",65536);managed=ma2_managed_by_launcher(stamp.c_str(),getenv("RECOMPLAUNCHER")!=nullptr);
        fprintf(stderr,"[MSG] updates for this install come from %s\n",managed?"RecompLauncher":"the installer");}
    return ma2_update_action(msg_newer(m.key),managed,
        GetFileAttributesW((msg_exe_dir()+L"\\MechAssault 2 Recomp Installer.exe").c_str())!=INVALID_FILE_ATTRIBUTES);
}
static bool inbox_focus_ok_locked(int f){
    InboxMsg *m=inbox_cur_locked();
    if(f==F_LIST||f==F_CLOSE)return true;
    if(!m)return false;
    if(f==F_BODY)return true;
    if(f==F_BOX||f==F_TICK||f==F_SEND)return reply_answering_locked();
    if(f==F_BUTTON){if(m->kind!="update")return false;int a=update_action_locked(*m);return a==MA2_UPDATE_RUN_INSTALLER||a==MA2_UPDATE_RELEASES_PAGE||a==MA2_UPDATE_IN_LAUNCHER;}
    return false;
}
// The message under the cursor is shown: it is read now (a reply: the relay is told, so the installer does not show it).
static void inbox_select_locked(int i){
    if(inbox.empty()){inbox_sel=0;return;}
    i=std::clamp(i,0,(int)inbox.size()-1);
    if(i!=inbox_sel){reply_answer.clear();reply_preset=-1;reply_attach=true;reply_outcome=REPLY_NONE;reply_reason.clear();update_note.clear();}
    inbox_sel=i;inbox_scroll=0;
    InboxMsg &m=inbox[i];
    if(!m.read){
        m.read=true;msg_save_locked();
        if(m.kind=="reply"){HANDLE t=CreateThread(nullptr,64*1024,reply_seen_thread,new std::string(m.key),STACK_SIZE_PARAM_IS_A_RESERVATION,nullptr);if(t)CloseHandle(t);}
        fprintf(stderr,"[MSG] read: %s %s\n",m.kind.c_str(),m.key.c_str());
    }
}
static void inbox_open_locked(const char *how){
    if(reply_state)return;
    inbox_pad=strcmp(how,"View button")?0:1;
    reply_state=1;inbox_focus=F_LIST;inbox_list_top=0;inbox_hit_valid=false;
    int first=0;for(int i=0;i<(int)inbox.size();i++)if(!inbox[i].read){first=i;break;}
    inbox_sel=-1;inbox_select_locked(first);
    reply_capture(true);
    fprintf(stderr,"[MSG] inbox opened (%s): %zu messages\n",how,inbox.size());
}
static void inbox_close_locked(){
    if(reply_state!=1)return;
    reply_state=0;inbox_hit_valid=false;reply_capture(false);
    fprintf(stderr,"[MSG] inbox closed\n");
}
static DWORD WINAPI reply_send_thread(LPVOID){
    std::string id,key,words;bool attach;
    {ReplyLock l;InboxMsg *m=inbox_cur_locked();if(!m){reply_state=1;return 0;}key=m->key;id=key.substr(0,10);words=report_to_utf8(reply_answer);attach=reply_attach;}
    if(attach){apu_health_request();Sleep(100);}
    std::wstring base=reply_temp_base(L"answer"),text_file=base+L".txt",result_file=base+L"-result.txt";
    HANDLE f=CreateFileW(text_file.c_str(),GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_TEMPORARY,nullptr);
    if(f!=INVALID_HANDLE_VALUE){DWORD w=0;WriteFile(f,words.data(),(DWORD)words.size(),&w,nullptr);CloseHandle(f);}
    wchar_t log[MAX_PATH]=L"";ma2_report_log_path(log,MAX_PATH);
    wchar_t ini[MAX_PATH]=L"";MultiByteToWideChar(CP_ACP,0,ini_path,-1,ini,MAX_PATH);
    std::wstring args=L"--answer "+report_from_utf8(id)+L" --text-file \""+text_file+L"\" --result \""+result_file+L"\"";
    if(attach)args+=L" --attach";
    if(log[0])args+=L" --log \""+std::wstring(log)+L"\"";
    if(ini[0])args+=L" --ini \""+std::wstring(ini)+L"\"";
    DWORD code=2,ran=report_run(args,180000,&code);
    std::string r="\n"+msg_read_text(result_file,8192);
    DeleteFileW(text_file.c_str());DeleteFileW(result_file.c_str());
    auto value=[&](const char *k){std::string kk=std::string("\n")+k+"=";size_t a=r.find(kk);if(a==std::string::npos)return std::string();
        a+=kk.size();size_t b=r.find('\n',a);return r.substr(a,b==std::string::npos?std::string::npos:b-a);};
    std::string ok=value("ok"),cause=value("cause"),why=value("cause_text"),logs=value("logs");
    if(!ran)why="the report helper could not start";else if(ran==2)why="the report helper took too long";else if(why.empty())why="the answer could not be sent";
    {ReplyLock l;
        reply_outcome=ok=="1"?(logs=="1"?REPLY_SENT_LOGS:REPLY_SENT):cause=="closed"?REPLY_CLOSED:cause=="empty"?REPLY_EMPTY:REPLY_FAILED;
        reply_reason=why;reply_state=1;
        InboxMsg *m=msg_find_locked("reply",key);
        if(m&&(ok=="1"||cause=="closed")){m->answered=ok=="1";m->can_answer=false;msg_save_locked();}
        inbox_focus=reply_outcome==REPLY_FAILED?F_SEND:F_LIST;}
    fprintf(stderr,"[REPLY] answer to %s: %s%s (exit %lu)\n",id.c_str(),ok=="1"?"sent":"not sent: ",ok=="1"?(logs=="1"?" with logs":""):why.c_str(),code);
    return 0;
}
// RecompLauncher installs: never our installer. Its only deep link opens a game's page (recomplauncher://game/<id>, see
// INBOX-PROGRESS.md for the sources); the player presses Update there. The id is the launcher's folder for this game: the
// parent of the install folder ({install}/MechAssault 2). Tests: RECOMP_LAUNCHER_LINK_TEST=<file> writes the link there.
static std::string launcher_game_id(){
    std::wstring dir=msg_exe_dir();size_t s=dir.find_last_of(L"\\/");std::wstring parent=s==std::wstring::npos?L"":dir.substr(0,s);
    size_t t=parent.find_last_of(L"\\/");std::wstring name=t==std::wstring::npos?parent:parent.substr(t+1);
    std::string id;for(wchar_t c:name){if(c<128&&(iswalnum(c)||c==L'-'))id+=(char)c;else{id.clear();break;}}
    if(id.empty()||id.size()>80)id="gh-mazillion-mechassault2-pc-recomp";
    return id;
}
static void update_in_launcher_locked(const InboxMsg &m){
    std::string url="recomplauncher://game/"+launcher_game_id();
    const char *test=getenv("RECOMP_LAUNCHER_LINK_TEST");
    if(test&&*test){FILE *f=fopen(test,"w");if(f){fprintf(f,"%s\n",url.c_str());fclose(f);}}
    else if((INT_PTR)ShellExecuteA(nullptr,"open",url.c_str(),nullptr,nullptr,SW_SHOWNORMAL)<=32){
        update_note=msg_text_update_failed;fprintf(stderr,"[MSG] update %s: RecompLauncher could not be opened\n",m.key.c_str());return;}
    fprintf(stderr,"[MSG] update %s: RecompLauncher opened on %s%s, closing the game\n",m.key.c_str(),url.c_str(),test&&*test?" (test hook: written, not opened)":"");fflush(stderr);
    inbox_close_locked();
    PostMessageA(game_window,WM_CLOSE,0,0);
}
// "Update now": start the installer copy in the install folder (it opens on this install and offers the new version),
// then close the game the normal way. No copy: the releases page in the browser.
static void update_now_locked(const InboxMsg &m){
    int a=update_action_locked(m);
    if(a==MA2_UPDATE_IN_LAUNCHER){update_in_launcher_locked(m);return;}
    if(a==MA2_UPDATE_RELEASES_PAGE){ShellExecuteA(nullptr,"open",msg_releases_url,nullptr,nullptr,SW_SHOWNORMAL);fprintf(stderr,"[MSG] update %s: releases page opened\n",m.key.c_str());return;}
    if(a!=MA2_UPDATE_RUN_INSTALLER)return;
    std::wstring dir=msg_exe_dir(),exe=dir+L"\\MechAssault 2 Recomp Installer.exe",cmd=L"\""+exe+L"\"";
    std::vector<wchar_t> line(cmd.begin(),cmd.end());line.push_back(0);
    STARTUPINFOW si={};si.cb=sizeof si;PROCESS_INFORMATION pi={};
    SetEnvironmentVariableW(L"MA2_GAME_UPDATE",dir.c_str());   // the installer opens on this folder and starts its self-update
    if(!CreateProcessW(exe.c_str(),line.data(),nullptr,nullptr,FALSE,0,nullptr,dir.c_str(),&si,&pi)){
        update_note=msg_text_update_failed;fprintf(stderr,"[MSG] update %s: the installer could not be started (error %lu)\n",m.key.c_str(),GetLastError());return;}
    CloseHandle(pi.hThread);CloseHandle(pi.hProcess);
    fprintf(stderr,"[MSG] update %s: installer started (pid %lu), closing the game\n",m.key.c_str(),pi.dwProcessId);fflush(stderr);
    inbox_close_locked();
    PostMessageA(game_window,WM_CLOSE,0,0);
}
static void inbox_activate_locked(int f){
    if(reply_state!=1||!inbox_focus_ok_locked(f))return;
    InboxMsg *m=inbox_cur_locked();
    inbox_focus=f;
    if(f==F_CLOSE){inbox_close_locked();return;}
    if(f==F_LIST){int first=F_BODY;for(int g:{F_BOX,F_BUTTON})if(inbox_focus_ok_locked(g)){first=g;break;}inbox_focus=first;return;}
    if(f==F_BODY){for(int g:{F_BOX,F_BUTTON})if(inbox_focus_ok_locked(g)){inbox_focus=g;break;}return;}
    if(f==F_BOX){inbox_focus=F_SEND;return;}
    if(f==F_TICK){reply_attach=!reply_attach;return;}
    if(f==F_BUTTON&&m){update_now_locked(*m);return;}
    if(f==F_SEND){
        std::wstring t=reply_answer;while(!t.empty()&&iswspace(t.back()))t.pop_back();
        if(t.empty()){reply_outcome=REPLY_EMPTY;inbox_focus=F_BOX;return;}
        reply_state=2;reply_outcome=REPLY_NONE;
        fprintf(stderr,"[REPLY] sending the answer (%zu characters, logs %s)\n",t.size(),reply_attach?"on":"off");
        HANDLE h=CreateThread(nullptr,0,reply_send_thread,nullptr,0,nullptr);
        if(h)CloseHandle(h);else{reply_state=1;reply_outcome=REPLY_FAILED;reply_reason="the answer could not be started";}
    }
}
static void inbox_move_locked(int step){   // Tab / D-pad in the reader: the next control that is there
    static const int order[]={F_LIST,F_BODY,F_BOX,F_TICK,F_SEND,F_BUTTON};
    int at=0;for(int i=0;i<6;i++)if(order[i]==inbox_focus)at=i;
    for(int k=0;k<6;k++){at=(at+step+6)%6;if(inbox_focus_ok_locked(order[at]))break;}
    inbox_focus=order[at];
}
static void inbox_scroll_locked(float d){inbox_scroll=std::clamp(inbox_scroll+d,0.f,inbox_scroll_max);}
static void reply_preset_step_locked(int direction){
    if(!reply_answer.empty()&&reply_preset<0)return;              // the player's own words stay
    reply_preset=reply_preset<0?(direction>0?0:REPLY_PRESETS-1):(reply_preset+direction+REPLY_PRESETS+1)%(REPLY_PRESETS+1);
    if(reply_preset==REPLY_PRESETS){reply_preset=-1;reply_answer.clear();}else reply_answer=reply_presets[reply_preset];
}
static void reply_type_locked(uint32_t code){
    if(reply_state!=1||!reply_answering_locked())return;
    inbox_focus=F_BOX;if(reply_outcome==REPLY_EMPTY)reply_outcome=REPLY_NONE;
    if(reply_preset>=0){reply_answer.clear();reply_preset=-1;}
    if(code==0x16){   // Ctrl+V
        if(OpenClipboard(nullptr)){HANDLE h=GetClipboardData(CF_UNICODETEXT);
            if(h){const wchar_t *w=(const wchar_t *)GlobalLock(h);if(w){for(;*w&&reply_answer.size()<REPORT_MAX_TEXT;w++){wchar_t c=*w;if(c==L'\r')continue;if(c==L'\t')c=L' ';if(c==L'\n'||c>=0x20)reply_answer+=c;}GlobalUnlock(h);}}
            CloseClipboard();}
    }else if(code>=0x20&&code<0xFFFE&&reply_answer.size()<REPORT_MAX_TEXT)reply_answer+=(wchar_t)code;
}
// One key of the inbox (keyboard, or the pad mapped onto keys).
static void inbox_key_locked(int vk,bool shift){
    if(reply_state!=1)return;
    const bool box=inbox_focus==F_BOX&&reply_answering_locked();
    if(vk==VK_ESCAPE){if(inbox_focus==F_LIST)inbox_close_locked();else inbox_focus=F_LIST;}
    else if(vk==VK_BACK&&box){if(!reply_answer.empty()){reply_answer.pop_back();if(reply_answer.empty())reply_preset=-1;}}
    else if(vk==VK_TAB)inbox_move_locked(shift?-1:1);
    else if(vk==VK_UP||vk==VK_DOWN){int d=vk==VK_UP?-1:1;
        if(inbox_focus==F_LIST)inbox_select_locked(inbox_sel+d);else if(inbox_focus==F_BODY)inbox_scroll_locked(d*inbox_scroll_step);else inbox_move_locked(d);}
    else if(vk==VK_PRIOR||vk==VK_NEXT)inbox_scroll_locked((vk==VK_PRIOR?-1:1)*inbox_scroll_step*6);
    else if(vk==VK_HOME&&inbox_focus==F_LIST)inbox_select_locked(0);
    else if(vk==VK_END&&inbox_focus==F_LIST)inbox_select_locked((int)inbox.size()-1);
    else if(vk==VK_LEFT||vk==VK_RIGHT){int d=vk==VK_LEFT?-1:1;
        if(box)reply_preset_step_locked(d);
        else if(inbox_focus==F_TICK)reply_attach=!reply_attach;
        else if(d<0)inbox_focus=F_LIST;
        else if(inbox_focus==F_LIST)inbox_activate_locked(F_LIST);}
    else if(vk==VK_RETURN)inbox_activate_locked(inbox_focus);
    else if(vk==VK_SPACE&&!box)inbox_activate_locked(inbox_focus);
}
// Window thread: the inbox owns the keyboard and the mouse while it is open. Returns 1 when the message was used.
static int reply_input(HWND window,UINT message,uintptr_t wparam,intptr_t lparam){
    // A real key, click or wheel turns the hints to the keyboard's (the VK_GAMEPAD_* range 0xC3-0xDA is the controller).
    if((message==WM_KEYDOWN&&!(wparam>=0xC3&&wparam<=0xDA))||message==WM_LBUTTONDOWN||message==WM_MOUSEWHEEL)inbox_pad=0;
    if(message==WM_CHAR){
        uint32_t code=wparam==0x16?0x16:reply_decoder.decode(GetACP(),IsWindowUnicode(window)!=FALSE,wparam);
        if(code>=0x20&&code<0xFFFE)inbox_pad=0;
        if(code==0x16||(code>=0x20&&!(code>=0x7F&&code<0xA0)&&code<0xFFFE&&!(code>=0xD800&&code<=0xDFFF))){ReplyLock l;if(inbox_focus==F_BOX)reply_type_locked(code);}
        return 1;
    }
    if(message==WM_KEYDOWN){
        ReplyLock l;const bool first=!(lparam&(1LL<<30));
        if(first||wparam==VK_BACK||wparam==VK_UP||wparam==VK_DOWN||wparam==VK_PRIOR||wparam==VK_NEXT)inbox_key_locked((int)wparam,GetKeyState(VK_SHIFT)<0);
        return 1;
    }
    if(message==WM_MOUSEWHEEL){ReplyLock l;inbox_scroll_locked(-GET_WHEEL_DELTA_WPARAM(wparam)/120.f*inbox_scroll_step*2);return 1;}
    if(message==WM_LBUTTONDOWN){
        ReplyLock l;float x=(float)GET_X_LPARAM(lparam),y=(float)GET_Y_LPARAM(lparam);
        auto in=[&](const float *b){return b[2]>b[0]&&x>=b[0]&&x<b[2]&&y>=b[1]&&y<b[3];};
        if(!inbox_hit_valid||reply_state!=1)return 1;
        for(int i=0;i<inbox_items_shown;i++)if(in(inbox_item_hit[i])){inbox_select_locked(inbox_list_top+i);inbox_focus=F_LIST;return 1;}
        for(int f=F_BODY;f<F_COUNT;f++)if(in(inbox_hit[f])){if(f==F_BODY||f==F_BOX){if(inbox_focus_ok_locked(f))inbox_focus=f;}else inbox_activate_locked(f);return 1;}
        return 1;
    }
    if(message==WM_KEYUP||message==WM_INPUT||(message>=WM_MOUSEFIRST&&message<=WM_MOUSELAST))return 1;   // Alt+F4 still closes the game
    return 0;
}
/* The main menu screen starts the attract movie after 45 s without input: its update (sub_001F34BE) compares the game
 * clock 0x4E2430 with its last-input time at +0x88 against 0xAFC8 ms (another front-end screen, sub_001F864C, uses +0x74),
 * and resets that time only on a pad button, a key or a stick. The inbox takes all input from the menu, so while it is
 * open it keeps that time current. The screens are found once per opening by their vtable (searched in the XBE's vtable
 * area) and a plausible time stamp. Nothing found = attract after 45 s; the inbox stays open behind it. */
static const struct{uint32_t update,offset;}reply_idle_screens[]={{0x001F34BEu,0x88},{0x001F864Cu,0x74}};
static uint32_t reply_find_screen(uint32_t update,uint32_t offset){
    uint32_t slot=0;
    for(uint32_t a=0x480000;a<0x4A0000&&!slot;a+=4)if(guest(a)==update)slot=a;
    if(!slot)return 0;
    const uint32_t now=guest(0x4E2430);
    for(uint32_t a=0x10000;a<0x03FFF000-0x100;a+=4){
        uint32_t d=guest(a);
        if(d>slot||slot-d>0x100||(slot-d)&3)continue;          // the object's first dword is that vtable
        if(now-guest(a+offset)<=0xAFC8u+10000u)return a;
    }
    return 0;
}
static void reply_keep_menu_awake(){
    static uint32_t screen[2],vt[2];static int tried;
    if(reply_state==0){tried=0;screen[0]=screen[1]=0;return;}
    const uint32_t now=guest(0x4E2430);
    for(int i=0;i<2;i++){
        if(screen[i]&&guest(screen[i])!=vt[i])screen[i]=0;
        if(!screen[i]&&tried<3){screen[i]=reply_find_screen(reply_idle_screens[i].update,reply_idle_screens[i].offset);vt[i]=screen[i]?guest(screen[i]):0;
            fprintf(stderr,"[MSG] front-end screen %08X: %s%08X (its attract timer waits while the inbox is open)\n",reply_idle_screens[i].update,screen[i]?"":"not found ",screen[i]);}
        if(screen[i]){uint32_t &last=guest(screen[i]+reply_idle_screens[i].offset);if(now-last<=0xAFC8u+10000u)last=now;}
    }
    if(tried<3&&(!screen[0]||!screen[1]))tried++;
}
static bool envelope_visible(){
    return !opened&&!exit_dialog&&menu_available()&&menu_kind==0x481B70&&!pc_display_movie_active()&&!ma2_scn_editor_note()&&!modal_available();
}
// Guest thread, every menu frame: the update message, the View button on the main menu, the pad while the inbox is open.
static void reply_pulse(uint32_t vtable){
    static WORD last_pad;
    msg_merge_update();
    if(vtable==0x481B70)reply_keep_menu_awake();
    XINPUT_STATE pad={};WORD buttons=(WORD)(test_pad_held|test_pad_tapped.load());
    if(xbox_InputPollHostState(&pad)==ERROR_SUCCESS)buttons|=pad.Gamepad.wButtons;
    WORD pressed=buttons&~last_pad;last_pad=buttons;
    if(reply_state==0){
        if(vtable!=0x481B70)return;
        {ReplyLock l;if(inbox_launch_unread<0&&envelope_visible()){inbox_launch_unread=msg_unread_locked();inbox_launch_at=GetTickCount();
            if(inbox_launch_unread)fprintf(stderr,"[MSG] main menu: %d unread, the launch line shows\n",inbox_launch_unread);}}
        if((pressed&XINPUT_GAMEPAD_BACK)&&envelope_visible()&&!pc_input_text_captured()){test_pad_tapped&=~(unsigned)XINPUT_GAMEPAD_BACK;ReplyLock l;inbox_open_locked("View button");}
        return;
    }
    test_pad_tapped.exchange(0);
    if(pad.Gamepad.wButtons)pc_input_note_gamepad(pad.Gamepad.wButtons,pad.Gamepad.bLeftTrigger,pad.Gamepad.bRightTrigger,pad.Gamepad.sThumbLX,pad.Gamepad.sThumbLY,pad.Gamepad.sThumbRX,pad.Gamepad.sThumbRY);
    if(!pressed)return;
    inbox_pad=1;
    ReplyLock l;const bool box=inbox_focus==F_BOX&&reply_answering_locked();
    if(pressed&XINPUT_GAMEPAD_B)inbox_key_locked(VK_ESCAPE,false);
    else if(pressed&XINPUT_GAMEPAD_A){if(box)reply_preset_step_locked(1);else inbox_activate_locked(inbox_focus);}
    else if(pressed&XINPUT_GAMEPAD_DPAD_DOWN)inbox_key_locked(VK_DOWN,false);
    else if(pressed&XINPUT_GAMEPAD_DPAD_UP)inbox_key_locked(VK_UP,false);
    else if(pressed&XINPUT_GAMEPAD_DPAD_LEFT)inbox_key_locked(VK_LEFT,false);
    else if(pressed&XINPUT_GAMEPAD_DPAD_RIGHT)inbox_key_locked(VK_RIGHT,false);
    else if(pressed&XINPUT_GAMEPAD_LEFT_SHOULDER)inbox_scroll_locked(-inbox_scroll_step*4);
    else if(pressed&XINPUT_GAMEPAD_RIGHT_SHOULDER)inbox_scroll_locked(inbox_scroll_step*4);
    else if(pressed&XINPUT_GAMEPAD_BACK)inbox_close_locked();
}
// ---- drawing (render thread, back-buffer pixels; hit boxes go to the window thread in client pixels)
static const ImU32 C_WHITE=IM_COL32(252,251,248,255),C_MUTED=IM_COL32(160,190,214,255),C_YELLOW=IM_COL32(206,206,72,255),C_DIM=IM_COL32(120,140,160,255),
    C_RED=IM_COL32(255,140,120,255),C_GREEN=IM_COL32(150,220,150,255),C_GLOW=IM_COL32(170,214,250,235),C_LINE=IM_COL32(74,128,178,230),C_FILL=IM_COL32(15,26,38,240);
static void draw_focus_ring(ImDrawList *list,ImVec2 a,ImVec2 b,float u){for(int i=6;i>=1;i--){float k=i/6.f,s=5*u*k;list->AddRect(ImVec2(a.x-s,a.y-s),ImVec2(b.x+s,b.y+s),IM_COL32(165,210,255,(int)(26*(1-k)+6)),3*u,0,1.5f*u);}}
static std::string elide(ImFont *font,float size,const std::string &t,float w){
    if(font->CalcTextSizeA(size,FLT_MAX,0,t.c_str()).x<=w)return t;
    std::string s=t;while(!s.empty()&&font->CalcTextSizeA(size,FLT_MAX,0,(s+"...").c_str()).x>w){s.pop_back();while(!s.empty()&&((unsigned char)s.back()&0xC0)==0x80)s.pop_back();}
    return s+"...";
}
/* The mouse-over light of the envelope and of everything clickable in the inbox: the footer prompts' hover glow
 * (draw_glow), faded in over 0.1 s. The pointer is in back-buffer pixels (x<0 = none: a controller, another window).
 * Each frame the drawing reports what is under the pointer (item); begin() of the next frame fades that one in. */
static struct{
    float x=-1,y=-1,alpha=0;int shown=-1,seen=-1;LARGE_INTEGER last{};
    void begin(float px,float py){
        LARGE_INTEGER now,frequency;QueryPerformanceCounter(&now);QueryPerformanceFrequency(&frequency);
        float dt=last.QuadPart?std::min((float)(now.QuadPart-last.QuadPart)/frequency.QuadPart,.1f):0.f;last=now;
        if(seen!=shown){shown=seen;alpha=0;}
        if(shown>=0)alpha=std::min(1.f,alpha+dt/.10f);
        seen=-1;x=px;y=py;
    }
    bool over(ImVec2 a,ImVec2 b)const{return x>=0&&x>=a.x&&x<b.x&&y>=a.y&&y<b.y;}
    // hit = the clickable area, a/b = what lights up (the same unless a label sits beside the control)
    void item(ImDrawList *list,int id,ImVec2 ha,ImVec2 hb,ImVec2 a,ImVec2 b){
        if(!over(ha,hb))return;
        seen=id;if(id==shown&&alpha>0){const float r[4]={a.x,a.y,b.x,b.y};draw_glow(list,r,alpha);}
    }
}hover_light;
enum{HL_ENVELOPE=100,HL_LIST=200};
static void draw_envelope(ImDrawList *list,float width,float height,float sx,float sy,bool hover){
    ReplyLock l;
    const float u=std::min(width/640.f,height/480.f),s=26*u,x1=width-18*u,x0=x1-s*1.35f,y0=16*u,y1=y0+s;
    const int unread=msg_unread_locked();const ImU32 c=hover?C_YELLOW:C_WHITE;
    list->AddRectFilled(ImVec2(x0-6*u,y0-6*u),ImVec2(x1+6*u,y1+5*u),IM_COL32(15,26,38,hover?220:170),4*u);
    if(hover){hover_light.seen=HL_ENVELOPE;
        if(hover_light.shown==HL_ENVELOPE&&hover_light.alpha>0){const float r[4]={x0-6*u,y0-6*u,x1+6*u,y1+5*u};draw_glow(list,r,hover_light.alpha);}}
    list->AddRect(ImVec2(x0,y0),ImVec2(x1,y1),c,2*u,0,std::max(1.5f,2*u));
    list->AddLine(ImVec2(x0+1*u,y0+1*u),ImVec2((x0+x1)*.5f,y0+s*.55f),c,std::max(1.5f,2*u));
    list->AddLine(ImVec2(x1-1*u,y0+1*u),ImVec2((x0+x1)*.5f,y0+s*.55f),c,std::max(1.5f,2*u));
    ImFont *font=ImGui::GetFont();float ks=std::max(11.f,8.5f*u);
    const char *key=pc_input_device()==PC_INPUT_GAMEPAD?msg_text_key_pad:msg_text_key_keys;
    ImVec2 kt=font->CalcTextSizeA(ks,FLT_MAX,0,key);ImVec2 kp((x0+x1-kt.x)*.5f,y1+6*u);
    list->AddRectFilled(ImVec2(kp.x-6*u,kp.y-3*u),ImVec2(kp.x+kt.x+6*u,kp.y+kt.y+3*u),IM_COL32(15,26,38,220),3*u);
    list->AddText(font,ks,kp,hover?C_YELLOW:C_WHITE,key);
    if(unread){char n[8];snprintf(n,sizeof n,"%d",std::min(unread,99));float r=7.5f*u;ImVec2 cc(x1,y0);
        list->AddCircleFilled(cc,r,IM_COL32(200,40,40,255),20);ImVec2 nt=font->CalcTextSizeA(ks,FLT_MAX,0,n);
        list->AddText(font,ks,ImVec2(cc.x-nt.x*.5f,cc.y-nt.y*.5f),C_WHITE,n);}
    envelope_hit[0]=(x0-6*u)/sx;envelope_hit[1]=(y0-8*u)/sy;envelope_hit[2]=(x1+8*u)/sx;envelope_hit[3]=(y1+22*u)/sy;envelope_shown=true;
    // the launch line: "1 new message", 6 s, then a fade of 1 s
    if(inbox_launch_unread>0){DWORD age=GetTickCount()-inbox_launch_at;
        if(age<7000){float a=age<6000?1.f:1.f-(age-6000)/1000.f;char t[48];
            if(inbox_launch_unread==1)snprintf(t,sizeof t,"%s",msg_text_new_one);else snprintf(t,sizeof t,msg_text_new_many,inbox_launch_unread);
            float ts=std::max(13.f,10.5f*u);ImVec2 tt=font->CalcTextSizeA(ts,FLT_MAX,0,t);
            ImVec2 p(x0-14*u-tt.x,(y0+y1-tt.y)*.5f);
            list->AddRectFilled(ImVec2(p.x-8*u,p.y-5*u),ImVec2(p.x+tt.x+8*u,p.y+tt.y+5*u),IM_COL32(15,26,38,(int)(200*a)),4*u);
            list->AddText(font,ts,p,IM_COL32(206,206,72,(int)(255*a)),t);}}
}
static void draw_inbox(ImDrawList *list,float width,float height,float sx,float sy){
    ReplyLock l;
    if(!reply_state)return;
    // The text grows a little more than the panel on tall screens (a 1440-1600 line screen shows it about a third larger).
    const float u=std::min(width/640.f,height/480.f),big=std::clamp(height/1200.f,1.f,1.35f),size=std::max(13.f,10.5f*u*big),pad=12*u;ImFont *font=ImGui::GetFont();
    const float w=std::min(width-24*u,620*u),h=std::min(height-24*u,440*u),x0=(width-w)*.5f,y0=(height-h)*.5f,x1=x0+w,y1=y0+h,t=std::max(1.f,1.25f*u);
    const bool gamepad=inbox_pad>=0?inbox_pad==1:pc_input_device()==PC_INPUT_GAMEPAD,busy=reply_state==2;
    auto px=[&](int f,float a,float b,float c,float d){inbox_hit[f][0]=a/sx;inbox_hit[f][1]=b/sy;inbox_hit[f][2]=c/sx;inbox_hit[f][3]=d/sy;};
    for(auto &b:inbox_hit)b[0]=b[1]=b[2]=b[3]=0;
    list->AddRectFilled(ImVec2(0,0),ImVec2(width,height),IM_COL32(0,0,0,110));
    list->AddRectFilled(ImVec2(x0,y0),ImVec2(x1,y1),C_FILL,4*u);
    list->AddRect(ImVec2(x0,y0),ImVec2(x1,y1),C_LINE,4*u,0,t);
    // title bar with Close
    list->AddText(font,size*1.25f,ImVec2(x0+pad,y0+pad*.8f),C_YELLOW,msg_text_title);
    ImVec2 cs=font->CalcTextSizeA(size,FLT_MAX,0,msg_text_close);ImVec2 ca(x1-pad-cs.x-16*u,y0+pad*.6f),cb(x1-pad,ca.y+size*1.6f);
    if(inbox_focus==F_CLOSE)draw_focus_ring(list,ca,cb,u);
    hover_light.item(list,F_CLOSE,ca,cb,ca,cb);
    list->AddRect(ca,cb,inbox_focus==F_CLOSE?C_GLOW:C_LINE,3*u,0,t);
    list->AddText(font,size,ImVec2(ca.x+8*u,(ca.y+cb.y-cs.y)*.5f),inbox_focus==F_CLOSE?C_YELLOW:C_WHITE,msg_text_close);px(F_CLOSE,ca.x,ca.y,cb.x,cb.y);
    const float top=y0+pad*.8f+size*1.25f+10*u,bottom=y1-pad-size*1.2f,lw=std::min(200*u*big,w*.36f);
    list->AddLine(ImVec2(x0+pad,top-4*u),ImVec2(x1-pad,top-4*u),C_LINE,t);
    list->AddText(font,size*.9f,ImVec2(x0+pad,bottom+4*u),C_MUTED,gamepad?msg_text_hint_pad:msg_text_hint_keys);
    // the list
    const float ih=size*3.0f;const int fit=std::max(1,(int)((bottom-top)/ih));
    if(inbox_sel<inbox_list_top)inbox_list_top=inbox_sel;
    if(inbox_sel>=inbox_list_top+fit)inbox_list_top=inbox_sel-fit+1;
    inbox_items_shown=0;
    if(inbox.empty())list->AddText(font,size,ImVec2(x0+pad,top+4*u),C_DIM,msg_text_none);
    for(int i=inbox_list_top;i<(int)inbox.size()&&inbox_items_shown<fit&&inbox_items_shown<16;i++){
        const InboxMsg &m=inbox[i];float iy=top+(i-inbox_list_top)*ih;ImVec2 a(x0+pad*.5f,iy),b(x0+pad*.5f+lw,iy+ih-3*u);
        bool sel=i==inbox_sel;
        if(sel)list->AddRectFilled(a,b,inbox_focus==F_LIST?IM_COL32(50,99,120,230):IM_COL32(30,52,72,230),3*u);
        if(sel&&inbox_focus==F_LIST)list->AddRect(a,b,C_GLOW,3*u,0,t);
        // the light stays inside the list column (the halo would cross the divider and the frame)
        list->PushClipRect(ImVec2(x0+t*2,top-4*u),ImVec2(x0+pad*.5f+lw+7*u,bottom),true);hover_light.item(list,HL_LIST+i,a,b,a,b);list->PopClipRect();
        if(!m.read)list->AddCircleFilled(ImVec2(a.x+7*u,iy+size*.65f),3.5f*u,C_YELLOW,12);
        list->AddText(font,size,ImVec2(a.x+14*u,iy+3*u),m.read?C_WHITE:C_YELLOW,elide(font,size,m.title,lw-18*u).c_str());
        char d[16];ma2_msg_short_date(m.date.c_str(),d,sizeof d);
        std::string sub=std::string(m.kind=="update"?msg_text_kind_update:m.kind=="whatsnew"?msg_text_kind_whatsnew:msg_text_kind_reply)+"  "+d;
        list->AddText(font,size*.85f,ImVec2(a.x+14*u,iy+4*u+size*1.25f),C_MUTED,sub.c_str());
        float *hb=inbox_item_hit[inbox_items_shown++];hb[0]=a.x/sx;hb[1]=a.y/sy;hb[2]=b.x/sx;hb[3]=b.y/sy;
    }
    list->AddLine(ImVec2(x0+pad*.5f+lw+8*u,top),ImVec2(x0+pad*.5f+lw+8*u,bottom),C_LINE,t);
    // the reader
    InboxMsg *m=inbox_cur_locked();
    if(m){
        const float rx=x0+pad*.5f+lw+18*u,rw=x1-pad-rx;float ry=top;
        ImVec2 ts=font->CalcTextSizeA(size*1.15f,FLT_MAX,rw,m->title.c_str());
        list->AddText(font,size*1.15f,ImVec2(rx,ry),C_YELLOW,m->title.c_str(),nullptr,rw);ry+=ts.y+4*u;
        char d[16];ma2_msg_short_date(m->date.c_str(),d,sizeof d);list->AddText(font,size*.85f,ImVec2(rx,ry),C_MUTED,d);ry+=size+6*u;
        // the controls under the text first: their height decides the text area
        const bool answering=m->kind=="reply"&&reply_answering_locked();
        const int act=m->kind=="update"?update_action_locked(*m):-1;
        char status[400]="";ImU32 sc=C_WHITE;
        if(m->kind=="reply"){
            if(busy)snprintf(status,sizeof status,"%s",reply_text_sending);
            else if(reply_outcome==REPLY_SENT){snprintf(status,sizeof status,"%s",reply_text_sent);sc=C_GREEN;}
            else if(reply_outcome==REPLY_SENT_LOGS){snprintf(status,sizeof status,"%s",reply_text_sent_logs);sc=C_GREEN;}
            else if(reply_outcome==REPLY_EMPTY){snprintf(status,sizeof status,"%s",reply_text_empty);sc=C_RED;}
            else if(reply_outcome==REPLY_CLOSED){snprintf(status,sizeof status,"%s",reply_text_closed);sc=C_RED;}
            else if(reply_outcome==REPLY_FAILED){snprintf(status,sizeof status,reply_text_failed,reply_reason.c_str());sc=C_RED;}
            else if(m->answered)snprintf(status,sizeof status,"%s",msg_text_answered);
            else if(!m->can_answer)snprintf(status,sizeof status,"%s",msg_text_older);
        }else if(m->kind=="update"){
            sc=C_MUTED;
            snprintf(status,sizeof status,"%s",!update_note.empty()?update_note.c_str():act==MA2_UPDATE_RUN_INSTALLER?msg_text_update_now_note:
                act==MA2_UPDATE_IN_LAUNCHER?msg_text_update_launcher:act==MA2_UPDATE_RELEASES_PAGE?msg_text_update_page_note:msg_text_update_have);
            if(!update_note.empty())sc=C_RED;
        }
        const float box_h=size*3.4f+8*u,btn_h=size*1.7f,status_h=status[0]?font->CalcTextSizeA(size,FLT_MAX,rw,status).y+6*u:0.f;
        float ctl=status_h;
        const float priv_h=font->CalcTextSizeA(size*.9f,FLT_MAX,rw,reply_text_private).y+2*u;   // wraps on a narrow panel
        if(answering)ctl+=size+4*u+box_h+6*u+size*1.3f+6*u+btn_h+6*u+priv_h;
        if(act==MA2_UPDATE_RUN_INSTALLER||act==MA2_UPDATE_RELEASES_PAGE||act==MA2_UPDATE_IN_LAUNCHER)ctl+=btn_h+6*u;
        float body_bottom=bottom-ctl-6*u;if(body_bottom<ry+size*3)body_bottom=ry+size*3;
        // the text, scrolled inside its area
        // the text by lines; in update notes and What's new each "- " item starts a little lower (a gap between the items)
        const bool items=m->kind=="update"||m->kind=="whatsnew";const float gap=items?size*.45f:0.f;
        std::vector<std::pair<std::string,float>> paras;float body_h=0;
        for(size_t p0=0;p0<=m->body.size();){size_t e=m->body.find('\n',p0);if(e==std::string::npos)e=m->body.size();
            std::string line=m->body.substr(p0,e-p0);float lh=line.empty()?size:font->CalcTextSizeA(size,FLT_MAX,rw-8*u,line.c_str()).y;
            float before=(!paras.empty()&&line.size()>1&&line[0]=='-'&&line[1]==' ')?gap:0.f;
            paras.push_back({line,before});body_h+=before+lh;p0=e+1;if(e==m->body.size())break;}
        ImVec2 bs(rw,body_h);
        inbox_scroll_max=std::max(0.f,bs.y-(body_bottom-ry));inbox_scroll=std::clamp(inbox_scroll,0.f,inbox_scroll_max);inbox_scroll_step=size*1.2f;
        if(inbox_focus==F_BODY)list->AddRect(ImVec2(rx-4*u,ry-3*u),ImVec2(x1-pad+2*u,body_bottom+3*u),C_GLOW,3*u,0,t);
        list->PushClipRect(ImVec2(rx-2*u,ry-2*u),ImVec2(x1-pad,body_bottom+2*u),true);
        {float ly=ry-inbox_scroll;for(auto &pr:paras){ly+=pr.second;
            if(!pr.first.empty())list->AddText(font,size,ImVec2(rx,ly),C_WHITE,pr.first.c_str(),nullptr,rw-8*u);
            ly+=pr.first.empty()?size:font->CalcTextSizeA(size,FLT_MAX,rw-8*u,pr.first.c_str()).y;}}
        list->PopClipRect();
        if(inbox_scroll_max>0){float frac=(body_bottom-ry)/(bs.y),pos=inbox_scroll/inbox_scroll_max,bh=std::max(12*u,(body_bottom-ry)*frac);
            float by=ry+(body_bottom-ry-bh)*pos;list->AddRectFilled(ImVec2(x1-pad-2*u,by),ImVec2(x1-pad+1*u,by+bh),C_MUTED,2*u);}
        px(F_BODY,rx-4*u,ry-3*u,x1-pad,body_bottom+3*u);
        float cy=body_bottom+8*u;
        auto button=[&](int id,float bx,const char *label){
            ImVec2 ls=font->CalcTextSizeA(size,FLT_MAX,0,label);float bw=std::max(ls.x+28*u,90*u);
            ImVec2 a(bx,cy),b(bx+bw,cy+btn_h);bool f=inbox_focus==id&&!busy;
            if(f)draw_focus_ring(list,a,b,u);
            list->AddRectFilled(a,b,f?IM_COL32(50,99,120,230):IM_COL32(26,44,62,230),3*u);
            if(!busy)hover_light.item(list,id,a,b,a,b);
            list->AddRect(a,b,f?C_GLOW:C_LINE,3*u,0,t);
            list->AddText(font,size,ImVec2(a.x+(bw-ls.x)*.5f,a.y+(btn_h-ls.y)*.5f),busy?C_DIM:f?C_YELLOW:C_WHITE,label);
            px(id,a.x,a.y,b.x,b.y);return bw;};
        if(answering){
            list->AddText(font,size,ImVec2(rx,cy),C_MUTED,reply_text_answer);cy+=size+4*u;
            ImVec2 a(rx,cy),b(rx+rw,cy+box_h);bool f=inbox_focus==F_BOX&&!busy;
            if(f)draw_focus_ring(list,a,b,u);
            list->AddRectFilled(a,b,IM_COL32(8,18,30,200),2*u);
            if(!busy)hover_light.item(list,F_BOX,a,b,a,b);
            list->AddRect(a,b,f?C_GLOW:C_LINE,2*u,0,t);
            std::string txt=report_to_utf8(reply_answer);const bool empty=txt.empty();
            const bool hint=empty&&(!f||gamepad);if(hint)txt=gamepad?reply_text_pad:reply_text_type;else if(f&&report_cursor_on())txt+="_";
            float tw=rw-8*u;ImVec2 tsz=font->CalcTextSizeA(size,FLT_MAX,tw,txt.c_str());float ty=std::min(a.y+4*u,b.y-4*u-tsz.y);
            list->PushClipRect(ImVec2(a.x+2,a.y+2),ImVec2(b.x-2,b.y-2),true);
            list->AddText(font,size,ImVec2(a.x+4*u,ty),hint?C_DIM:C_WHITE,txt.c_str(),nullptr,tw);
            list->PopClipRect();px(F_BOX,a.x,a.y,b.x,b.y);cy+=box_h+6*u;
            float bsz=size*.95f;ImVec2 ta(rx,cy),tb(rx+bsz,cy+bsz);bool tf=inbox_focus==F_TICK&&!busy;
            if(tf)draw_focus_ring(list,ta,tb,u);
            {ImVec2 ls=font->CalcTextSizeA(size,FLT_MAX,0,reply_text_attach);
             if(!busy)hover_light.item(list,F_TICK,ImVec2(ta.x-4*u,ta.y-4*u),ImVec2(tb.x+12*u+ls.x,tb.y+4*u),ta,ImVec2(tb.x+8*u+ls.x,tb.y));}
            list->AddRect(ta,tb,tf?C_GLOW:C_MUTED,2*u,0,t);
            if(reply_attach)list->AddRectFilled(ImVec2(ta.x+3*u,ta.y+3*u),ImVec2(tb.x-3*u,tb.y-3*u),tf?C_GLOW:C_YELLOW,1*u);
            list->AddText(font,size,ImVec2(tb.x+8*u,cy-1*u),tf?C_YELLOW:C_WHITE,reply_text_attach);
            ImVec2 ls=font->CalcTextSizeA(size,FLT_MAX,0,reply_text_attach);px(F_TICK,ta.x-4*u,ta.y-4*u,tb.x+8*u+ls.x+4*u,tb.y+4*u);
            cy+=size*1.3f+6*u;
            button(F_SEND,rx,reply_text_send);cy+=btn_h+6*u;
            list->AddText(font,size*.9f,ImVec2(rx,cy),C_DIM,reply_text_private,nullptr,rw);cy+=priv_h;
        }
        if(act==MA2_UPDATE_RUN_INSTALLER||act==MA2_UPDATE_RELEASES_PAGE||act==MA2_UPDATE_IN_LAUNCHER){button(F_BUTTON,rx,act==MA2_UPDATE_RELEASES_PAGE?msg_text_update_page:msg_text_update_now);cy+=btn_h+6*u;}
        if(status[0])list->AddText(font,size,ImVec2(rx,cy),sc,status,nullptr,rw);
    }
    inbox_hit_valid=true;
}

// The box text, word-wrapped (about the width of the box in the row font), the last four lines.
static std::vector<std::wstring> report_lines(const std::wstring &t,size_t width){
    std::vector<std::wstring> out;std::wstring line;size_t i=0;
    while(i<t.size()){
        if(t[i]==L'\n'){out.push_back(line);line.clear();i++;continue;}
        size_t e=i;while(e<t.size()&&t[e]!=L' '&&t[e]!=L'\n')e++;
        std::wstring word=t.substr(i,e-i);
        while(word.size()>width){if(!line.empty()){out.push_back(line);line.clear();}out.push_back(word.substr(0,width));word.erase(0,width);}
        if(line.size()+word.size()>width&&!line.empty()){out.push_back(line);line.clear();}
        line+=word;i=e;
        while(i<t.size()&&t[i]==L' '){if(line.size()<width)line+=L' ';i++;}
    }
    out.push_back(line);
    return out;
}
static int report_leave();
static bool report_cursor_on(){return (GetTickCount()/530)%2==0;}
static void report_enter(int from){
    page=5;report_from=from;selection=REPORT_BOX;page_dirty=true;
    if(report_state==2&&report_shown_outcome){report_state=0;}
    if(report_can_send<0){HANDLE t=CreateThread(nullptr,0,report_probe_thread,nullptr,0,nullptr);if(t)CloseHandle(t);else report_can_send=1;}
    fprintf(stderr,"[REPORT] in-game report page opened (from %s)\n",from<0?"the pause menu":"Audio settings");
}
static bool report_row_valid(int row){
    if(report_state==2)return row==REPORT_BACK||(row==REPORT_SEND&&!report_outcome.ok);
    if(report_state==1)return row==REPORT_BACK;
    return row==REPORT_BOX||row==REPORT_SEND||row==REPORT_BACK;
}
static std::wstring report_row_text(int row){
    const bool box=selection==REPORT_BOX&&report_state==0;
    auto marker=[&](int r){return std::wstring(r==selection?L">  ":L"   ");};
    if(report_state==2){
        report_shown_outcome=true;const ReportOutcome &o=report_outcome;
        std::vector<std::wstring> msg=o.message.empty()?std::vector<std::wstring>():report_lines(o.message,58);
        if(o.ok)switch(row){
            case 0:return L"Report sent.";
            case 1:return L"Your report number is "+o.id+L".";
            case 2:return L"It is copied to your clipboard, if you want to share it";
            case 3:return L"directly with the developer.";
            case 5:case 6:return (size_t)(row-5)<msg.size()?msg[row-5]:L"";
            case REPORT_BACK:return marker(row)+L"Back";
            default:return L"";}
        if(row==REPORT_SEND)return marker(row)+L"Try again";
        if(row==REPORT_BACK)return marker(row)+L"Back";
        // Rows 0-6, one short line each (a row's text box wraps a long line onto the next row).
        const bool unset=o.saved&&o.cause==L"sending is not set up in this version";
        std::vector<std::wstring> lines;
        lines.push_back(unset?L"Report saved.":o.saved?L"Could not send the report.":L"Could not make the report.");
        for(auto &l:report_lines(unset?L"Sending is not set up in this version.":L"Reason: "+o.cause+L".",56))lines.push_back(l);
        if(o.saved){
            lines.push_back(L"The report was saved in:");
            for(auto &l:report_lines(o.folder.empty()?std::wstring(L"%LOCALAPPDATA%\\MechAssault2\\Diagnostics"):o.folder,56))lines.push_back(l);
            lines.push_back(L"Attach it to a problem report on the project's GitHub page.");
        }
        for(auto &l:msg)lines.push_back(l);
        return row>=0&&row<7&&(size_t)row<lines.size()?lines[row]:L"";
    }
    switch(row){
    case 0:return L"What happened?";
    case 1:case 2:case 3:case 4:{
        std::wstring shown=report_text;
        if(shown.empty()&&!box)shown=pc_input_device()==PC_INPUT_GAMEPAD?L"(optional: Left / Right picks a description)":L"(optional: type here)";
        std::vector<std::wstring> lines=report_lines(shown,62);
        size_t first=lines.size()>4?lines.size()-4:0,i=first+(row-1);
        std::wstring t=i<lines.size()?lines[i]:L"";
        if(box&&report_state==0&&i==lines.size()-1)t+=report_cursor_on()?L"_":L" ";
        return L" "+t;}
    case 5:return L"Sends logs, settings and PC details to the developer.";
    case 6:return L"No names or personal files. Please don't type personal details.";
    case REPORT_SEND:
        if(report_state==1)return L"   Sending...";
        return marker(row)+(report_can_send==0?L"Save report":L"Send report");
    case REPORT_BACK:return marker(row)+L"Back";
    // 1.1.6 (owner): a report is most useful right after the problem (the logs cover the last 3 sessions, a bot match
    // recording only its last minutes: MA2 Report.exe keeps the newest blocks that fit under the 20 MB report limit).
    case 9:return report_state==1?L"Collecting the logs and sending them. You can keep playing.":
                  report_state==0?L"Best sent right after the problem: it covers recent play only.":L"";
    }
    return L"";
}
static void report_preset_step(int direction){
    if(!report_text.empty()&&report_preset<0)return;            // the player's own words stay
    report_preset=report_preset<0?(direction>0?0:REPORT_PRESETS-1):(report_preset+direction+REPORT_PRESETS+1)%(REPORT_PRESETS+1);
    if(report_preset==REPORT_PRESETS){report_preset=-1;report_text.clear();}else report_text=report_presets[report_preset];
    page_dirty=true;
}
static void report_type(uint32_t code){
    if(report_state!=0)return;
    if(selection!=REPORT_BOX){selection=REPORT_BOX;}
    if(report_preset>=0){report_text.clear();report_preset=-1;}
    if(code==0x16){   // Ctrl+V: paste text
        if(OpenClipboard(nullptr)){HANDLE h=GetClipboardData(CF_UNICODETEXT);
            if(h){const wchar_t *w=(const wchar_t *)GlobalLock(h);if(w){for(;*w&&report_text.size()<REPORT_MAX_TEXT;w++){wchar_t c=*w;if(c==L'\r')continue;if(c==L'\t')c=L' ';if(c==L'\n'||c>=0x20)report_text+=c;}GlobalUnlock(h);}}
            CloseClipboard();}
    }else if(code>=0x20&&code<0xFFFE&&report_text.size()<REPORT_MAX_TEXT)report_text+=(wchar_t)code;
    page_dirty=true;
}
static void check_optional_content(){
    const char *root=getenv("RECOMP_TEXTURE_PACK_DIR");
    for(int quality=1;quality<=2;quality++){
        char path[MAX_PATH];snprintf(path,sizeof(path),"%s\\%dx\\index.json",root&&*root?root:"game_files\\texture_packs",quality==1?2:4);
        texture_installed[quality]=GetFileAttributesA(path)!=INVALID_FILE_ATTRIBUTES;
    }
    WCHAR host[MAX_PATH];videos_installed=false;
    if(xbox_translate_path("D:\\Movie\\Enhanced\\2x",host,MAX_PATH)){
        DWORD a=GetFileAttributesW(host);videos_installed=a!=INVALID_FILE_ATTRIBUTES&&(a&FILE_ATTRIBUTE_DIRECTORY);
    }
    // A saved choice whose content is missing shows (and is) the original.
    if(!texture_installed[std::clamp(pc_setting_get(PC_TEXTURE_QUALITY,0),0,2)])pc_setting_set(PC_TEXTURE_QUALITY,0);
    if(!videos_installed&&pc_setting_get(PC_FMV_QUALITY,0))pc_setting_set(PC_FMV_QUALITY,0);
}
static int row_count(){return page==0?13:page==1?(report_helper_ok()?6:5):page==2?9:page==4?4:10;}
// Row geometry of the native pages (640x480 units), as laid out by build_pc_assets.py.
static float row_top(){return page==0?90.f:99.f;}
static float row_step(){return page==0?23.f:28.f;}
static bool valid_row(int row){if(page==5)return report_row_valid(row);return row>=0&&row<row_count()&&!(page==3&&row<8&&binding_page*8+row>=PC_BINDING_COUNT);}
static std::wstring row_text(int row)
{
    wchar_t text[180];text[0]=0;
    if(page==0) switch(row){
    case 0:swprintf_s(text,L"Window mode:  %s",fullscreen?L"Borderless fullscreen":L"Windowed");break;
    case 1:if(output_width==640&&output_height==480)wcscpy_s(text,L"Resolution:  640x480 (Xbox original)");
        else swprintf_s(text,L"Resolution:  %d x %d",output_width,output_height);break;
    case 2:{const wchar_t *values[]={L"Original",L"2x Enhanced",L"4x Enhanced"};
        swprintf_s(text,L"Textures:  %s%s",values[std::clamp(pc_setting_get(PC_TEXTURE_QUALITY,0),0,2)],
            texture_installed[1]||texture_installed[2]?L"":L"   (enhanced packs not installed)");break;}
    case 3:swprintf_s(text,L"VSync:  %s",yesno(PC_VSYNC,0));break;
    case 4:if(pc_setting_get(PC_FRAME_LIMIT,0))swprintf_s(text,L"Frame limit:  %d FPS",pc_setting_get(PC_FRAME_LIMIT,0));else wcscpy_s(text,L"Frame limit:  Unlimited");break;
    case 5:swprintf_s(text,L"Aspect ratio:  %s",pc_setting_get(PC_ASPECT,1)?L"Automatic":L"Original 4:3");break;
    case 6:swprintf_s(text,L"Smooth scaling:  %s",yesno(PC_FILTER,1));break;
    case 7:swprintf_s(text,L"Draw distance:  %d",pc_setting_get(PC_DRAW_DISTANCE,50));break;
    case 8:swprintf_s(text,L"Show frame rate:  %s",yesno(PC_SHOW_FPS,0));break;
    case 9:swprintf_s(text,L"Cutscene videos:  %s%s",pc_setting_get(PC_FMV_QUALITY,0)?L"Enhanced":L"Original",
        videos_installed?L"":L"   (enhanced not installed)");break;
    case 10:swprintf_s(text,L"Pre-load shaders on loading screens:  %s",yesno(PC_SHADER_PRELOAD,1));break;
    case 11:wcscpy_s(text,settings_write_failed?L"Save failed - try again":L"Apply display settings");break;
    case 12:wcscpy_s(text,L"Restore display defaults");break;
    }
    else if(page==1)switch(row){
    case 0:swprintf_s(text,L"Master volume:  %d%%",pc_setting_get(PC_VOLUME,100));break;
    case 1:case 2:case 3:swprintf_s(text,L"%s volume:  %u%%",row==1?L"Effects":row==2?L"Speech":L"Music",page_object?(guest(page_object+0x84+(row-1)*4)*100+32767)/65535:100);break;
    case 4:wcscpy_s(text,report_helper_ok()?L"Report a problem...":L"Back");break;
    case 5:if(report_helper_ok())wcscpy_s(text,L"Back");break;}
    else if(page==5)return report_row_text(row);
    else if(page==2)switch(row){
    case 0:swprintf_s(text,L"Keyboard controls:  %s%s",yesno(PC_KEYBOARD,1),pc_setting_get(PC_KEYBOARD,1)&&!gamepad_connected()?L" (no controller connected)":L"");break;
    case 1:swprintf_s(text,L"Mouse aiming:  %s",yesno(PC_MOUSE_AIM,1));break;
    case 2:swprintf_s(text,L"Mouse sensitivity:  %d%%",pc_setting_get(PC_MOUSE_SENSITIVITY,100));break;
    case 3:swprintf_s(text,L"Invert mouse Y:  %s",yesno(PC_INVERT_Y,0));break;
    case 4:swprintf_s(text,L"Invert flight controls (VTOL):  %s",yesno(PC_INVERT_FLIGHT,0));break;
    case 5:wcscpy_s(text,L"Keyboard and mouse bindings...");break;
    case 6:wcscpy_s(text,L"Restore control defaults");break;
    case 7:wcscpy_s(text,L"Controller settings...");break;
    case 8:wcscpy_s(text,L"Back");break;}
    else if(page==3){
        int b=binding_page*8+row;
        if(row<8&&b<PC_BINDING_COUNT)swprintf_s(text,L"%s:  %s",binding_labels[b],waiting_binding==b?L"Press any key or mouse button...":key_name(pc_binding_get((PcBinding)b,default_keys[b])).c_str());
        else if(row==8)swprintf_s(text,L"Next page  (%d / 3)",binding_page+1);
        else if(row==9)wcscpy_s(text,L"Back to controls");
    }
    else if(page==4)switch(row){
    case 0:swprintf_s(text,L"Invert right stick Y:  %s",ma2_gui_controller_option(page_object,0)?L"On":L"Off");break;
    case 1:swprintf_s(text,L"Vibration:  %s",ma2_gui_controller_option(page_object,1)?L"On":L"Off");break;
    case 2:wcscpy_s(text,L"Keyboard and mouse settings...");break;
    case 3:wcscpy_s(text,L"Back");break;}
    if(!*text)return L"";
    return std::wstring(row==selection?L">  ":L"   ")+text;
}
extern "C" uint32_t ma2_pc_page_text(uint32_t id)
{
    unsigned group=(id-0x70000000)/100,entry=(id-0x70000000)%100;
    if(entry==0)return ma2_pc_wide(group==1&&page==5?L"REPORT A PROBLEM":group==0?L"DISPLAY SETTINGS":group==1?L"AUDIO SETTINGS":L"CONTROLS");
    if(entry>=1&&entry<=13)return ma2_pc_wide(row_text(entry-1).c_str());
    if(entry==18)return ma2_pc_wide(L"Distance:  0 = 0.5x     50 = Original     100 = 2x");
    return ma2_pc_wide(pc_input_device()==PC_INPUT_GAMEPAD?L"D-pad: Navigate / Adjust     A: Select     B: Back":L"Arrows: Navigate / Adjust    Enter: Select    Esc: Back");
}
extern "C" void ma2_pc_input_context_update(void)
{
    auto context=pc_display_movie_active()?PC_INPUT_MOVIE:modal_available()?PC_INPUT_MENU:!menu_available()?PC_INPUT_GAMEPLAY:menu_kind.load()==0x482170?PC_INPUT_TITLE:PC_INPUT_MENU;
    auto previous=pc_input_context();pc_input_set_context(context);
    // Preserve early skip taps on entry. Clear input before the next menu.
    if(previous!=context && context!=PC_INPUT_GAMEPLAY && context!=PC_INPUT_MOVIE)PostMessageA(game_window,PC_WINDOW_CAPTURE_MESSAGE,0,0);
    // Back in play from a menu on keyboard/mouse: aim with the mouse straight away.
    if(previous!=context && context==PC_INPUT_GAMEPLAY && pc_input_device()==PC_INPUT_KEYBOARD_MOUSE &&
       pc_setting_get(PC_KEYBOARD,1) && pc_setting_get(PC_MOUSE_AIM,1))PostMessageA(game_window,PC_WINDOW_AIM_MESSAGE,0,0);
}
extern "C" void ma2_pc_form_loading(uint32_t object,uint32_t filename)
{
    if(filename<0x10000||filename>=0x04000000)return;
    static const char name_form[]="interface\\menus\\dialogs\\dlg_keyboard.ggf";
    if(g_xbox_mem_offset&&!text_closed&&object>=0x10000&&object<0x03FFF000&&guest(object)==0x48F7D0&&
       filename<=0x04000000-sizeof(name_form)&&!_strnicmp((char *)(g_xbox_mem_offset+filename),name_form,sizeof(name_form))){
        // Capture before form initialization finishes, so a held key cannot
        // leak through a USB poll before the top modal record is published.
        AcquireSRWLockExclusive(&events_lock);invalidate_text_events_locked();
        text_record=text_object=0;text_cancel_pending=false;pc_input_text_capture(1);
        ReleaseSRWLockExclusive(&events_lock);
        pc_input_camera_capture(0);PostMessageA(game_window,PC_WINDOW_CAPTURE_MESSAGE,0,0);
    }
    if(getenv("RECOMP_MENU_TRACE"))fprintf(stderr,"[PC-MENU] form owner=%08X path=%s\n",object,(char *)(g_xbox_mem_offset+filename));
}
/* The pause menu's Report a Problem: the retail form keeps a hidden dev item (vt 4830F0, action 800,
 * "Load Level (Dev)", flags 3D) after the footer legends. It is relabelled, put one step (29 lines) under
 * the lowest visible item, shown, and given the Volume action (300), so choosing it opens the Audio
 * submenu; report_pending sends that submenu to the report page. Only with the helper beside the game. */
static std::atomic<uint32_t> report_item{0};
static void report_pause_item(uint32_t object){
    if(!report_helper_ok()||object<0x10000||object>=0x03FFF000)return;
    uint32_t begin=guest(object+4),end=guest(object+8);
    if(end<begin||end-begin>1024)return;
    static uint32_t done_item,label;
    for(uint32_t e=begin;e<end;e+=4){
        uint32_t form=guest(e);if(form<0x10000||form>=0x03FFF000)continue;
        uint32_t root=guest(form+0x38);if(root<0x10000||root>=0x03FFF000)continue;
        // items are children of the root's first child (the panel)
        uint32_t panel=guest(root+0x1C);if(panel<0x10000||panel>=0x03FFF000)continue;
        uint32_t dev=0,lowest=0,volume=0,video=0;int lowest_y=-1;
        for(uint32_t w=guest(panel+0x1C);w>=0x10000&&w<0x03FFF000;w=guest(w+0x18)){
            if(guest(w)!=0x4830F0)continue;
            uint32_t action=guest(w+0x70),flags=guest(w+0x74);
            if(w==done_item||action==800){dev=w;continue;}
            if(action==300)volume=w;
            if(action==1500&&(flags&0x22)==0x22)video=w;
            if((flags&0x22)==0x22&&action>=100&&action<=1500&&(int)guest(w+0x50)>lowest_y){lowest=w;lowest_y=(int)guest(w+0x50);}
        }
        if(!dev||!lowest)continue;
        /* 1.1.6 (owner): the item sits right after Video Settings, with the settings entries; every item below Video
         * Settings moves one step (29) down, so the exit item (Quit / Return to Editor) stays the LAST item, as in 1.1.5
         * (navigation follows the on-screen order). Without a visible Video Settings item it goes under the lowest item as before. */
        if(done_item!=dev&&video){
            int vy=(int)guest(video+0x50);
            for(uint32_t w=guest(panel+0x1C);w>=0x10000&&w<0x03FFF000;w=guest(w+0x18)){
                if(guest(w)!=0x4830F0||w==dev||(int)guest(w+0x50)<=vy)continue;
                uint32_t action=guest(w+0x70);if(action<100||action>1500)continue;
                for(uint32_t a=w,k=0;k<3&&a>=0x10000&&a<0x03FFF000;k++){
                    if(k!=1){guest(a+0x50)+=29;guest(a+0x60)+=29;}
                    a=k==0?guest(w+0x1C):guest(a+0x18);
                }
            }
            lowest=video;lowest_y=vy;
        }
        if(done_item!=dev){
            if(!label)label=ma2_pc_wide(L"Report a Problem");
            // One step (29 lines) under the lowest item: the item's and its highlight bar's y, in the 4:3 (+50) and
            // widescreen (+60) rectangles. Their x and width stay the form's own (the bar is drawn from its
            // authored size: a copied width doubled it). Child 0 is item-relative and stays.
            uint32_t a=lowest,b=dev;
            for(int k=0;k<3&&a>=0x10000&&a<0x03FFF000&&b>=0x10000&&b<0x03FFF000;k++){
                if(k!=1){guest(b+0x50)=guest(a+0x50)+29;guest(b+0x60)=guest(a+0x60)+29;}
                if(k==2){
                    // The dev item's bar is authored 256 wide and drawn 0.9 px per unit at 640 wide, the others' 512 at 0.68:
                    // 391 units (centre kept) draws it as long as the other items' bars (measured on screen shots).
                    for(int r=0;r<2;r++){int x=(int)guest(b+0x4C+r*16),w=(int)guest(b+0x54+r*16);
                        if(w==256){guest(b+0x4C+r*16)=(uint32_t)(x+(w-391)/2);guest(b+0x54+r*16)=391;}}
                }
                a=k==0?guest(lowest+0x1C):guest(a+0x18);b=k==0?guest(dev+0x1C):guest(b+0x18);
            }
            guest(dev+0x70)=300;
            // +9C: the hash of the submenu an item opens (the dev item's is the level picker): the Volume item's, so the
            // game itself opens the Audio submenu (the activate hook below also swaps the item; either one is enough).
            if(volume)guest(dev+0x9C)=guest(volume+0x9C);
            ma2_gui_text(dev,label);
            done_item=dev;report_item=dev;
            fprintf(stderr,"[REPORT] pause menu item %08X under %08X (y %d)\n",dev,lowest,lowest_y+29);
        }
        if((guest(dev+0x74)&2)==0)guest(dev+0x74)|=2;
        if(guest(dev+0x48)==5)guest(dev+0x48)=1;
        return;
    }
}
extern "C" __declspec(thread) uint32_t g_esp;   // the guest stack pointer (game thread)
extern "C" void ma2_pc_menu_selection(uint32_t object,uint32_t selected,int activated)
{
    if(getenv("RECOMP_MENU_TRACE")&&activated)fprintf(stderr,"[PC-MENU] activate %08X id=%u\n",object,selected);
    /* Report a Problem chosen on the pause menu. This runs first in the pause menu's activate handler
     * (sub_001FCEB6, argument = the chosen item at esp+4), which tells its items apart by pointer: the
     * argument becomes the Volume item, so the game opens the Audio submenu its own way, and
     * report_pending sends that submenu to the report page. */
    if(activated&&report_item&&object>=0x10000&&object<0x03FFF000&&guest(object)==0x4827B0&&g_esp>=0x10000&&g_esp<0x03FFF000&&
       guest(g_esp+4)==report_item.load()){
        uint32_t volume=0,begin=guest(object+4),end=guest(object+8);
        for(uint32_t e=begin;e<end&&end-begin<=1024&&!volume;e+=4){
            uint32_t form=guest(e),root=form>=0x10000&&form<0x03FFF000?guest(form+0x38):0,panel=root>=0x10000&&root<0x03FFF000?guest(root+0x1C):0;
            for(uint32_t w=panel>=0x10000&&panel<0x03FFF000?guest(panel+0x1C):0;w>=0x10000&&w<0x03FFF000&&!volume;w=guest(w+0x18))
                if(guest(w)==0x4830F0&&guest(w+0x70)==300&&w!=report_item.load())volume=w;
        }
        if(volume){
            guest(g_esp+4)=volume;
            report_pending=GetTickCount();if(!report_pending)report_pending=1;
            fprintf(stderr,"[REPORT] pause menu: Report a Problem (through the Volume item %08X)\n",volume);
        }
    }
}
extern "C" void ma2_pc_menu_pulse(uint32_t object,uint32_t vtable)
{
    uint32_t old=menu_kind.exchange(vtable);menu_object=object;menu_time=GetTickCount();
    if(old!=vtable)fprintf(stderr,"[PC-MENU] menu=%08X object=%08X\n",vtable,object);
    // Every menu: barrel arrows follow the selected value (see ma2_bots_ui.c).
    ma2_barrel_offset(vtable==0x48F3E0&&ma2_scn_custom_active()?50:0);
    if(!opened&&object>=0x10000&&object<0x03FFF000&&!pc_display_movie_active()&&!getenv("RECOMP_NO_BARREL_ARROWS"))ma2_barrel_arrows_pulse(object,pc_display_aspect(4.f/3.f)>4.f/3.f+.01f);
    // Unavailable features (Xbox Live, System Link, guests) are shown disabled (ma2_bots_ui.c menu policy).
    if(object>=0x10000&&object<0x03FFF000)ma2_menu_policy_pulse(object,vtable);
    if(vtable==0x4827B0&&!opened)report_pause_item(object);
    reply_pulse(vtable);                     // 1.2.0: a reply waiting shows on the main menu; the pad while its box is open
    ma2_pc_input_context_update();
    sync_text_dialog();
    {   // Name keyboard footer: on keyboard/mouse the confirm prompt reads "Done" (Enter = Done there);
        // on a pad it keeps "Select" (A types the highlighted key).
        uint32_t kb=0;
        if(!opened&&pc_input_text_captured()&&ma2_gui_keyboard_dialog(&kb)&&kb){
            static uint32_t done_text;if(!done_text)done_text=ma2_pc_wide(L"Done");
            ma2_gui_legend_caption(guest(kb+0x23C),0,pc_input_device()==PC_INPUT_GAMEPAD?0:done_text);
        }
    }
    if(!opened){
        std::vector<Event> pending;AcquireSRWLockExclusive(&events_lock);pending.swap(events);ReleaseSRWLockExclusive(&events_lock);
        for(auto e:pending){
            if(e.text_generation){
                uint32_t record,owner,current;
                if(!current_text_event(e,record,owner)||ma2_gui_keyboard_dialog(&current)!=record||current!=owner)continue;
                if(e.type==WM_CHAR)ma2_gui_keyboard_action(record,owner,(uint32_t)e.key,1);
                else if(e.type==WM_KEYDOWN){
                    if(e.key==VK_ESCAPE)text_cancel();
                    else {uint32_t code=e.key==VK_BACK?0x10002:e.key==VK_LEFT?0x10004:e.key==VK_RIGHT?0x10005:0x1000B;
                        ma2_gui_keyboard_action(record,owner,code,0);}
                }else if(e.type==WM_MOUSEMOVE||e.type==WM_LBUTTONDOWN){
                    float x,y;menu_point(game_window,e.x,e.y,x,y);
                    // Footer prompts first, by their drawn text (as the hover glow shows them).
                    float footer_box[4];
                    uint32_t hit=ma2_gui_footer_in_form(guest(owner+0x23C),x,y,pc_display_aspect(4.f/3.f),footer_box);
                    if(!hit)hit=ma2_gui_keyboard_pointer(record,owner,x,y,e.type==WM_LBUTTONDOWN);
                    if((hit&0x80000000u)&&e.type==WM_LBUTTONDOWN){
                        // Back = cancel (Esc). The confirm prompt = the Done key, as Enter is on the
                        // keyboard; the pad's A (type the highlighted key) stays on the pad.
                        if((hit&7)==1)text_cancel();
                        else if((hit&7)==0)ma2_gui_keyboard_action(record,owner,0x1000B,0);
                        else pc_input_menu_button(hit&7);
                        if(getenv("RECOMP_MENU_TRACE"))fprintf(stderr,"[PC-KEYBOARD] footer click button %u\n",hit&7);
                    }
                }
                sync_text_dialog();continue;
            }
            if(pc_input_text_captured())continue;
            if(e.type==WM_MOUSEMOVE||e.type==WM_LBUTTONDOWN){
                float x,y;menu_point(game_window,e.x,e.y,x,y);
                if(vtable==0x48F3E0&&!modal_available()&&editor_button_shown()&&editor_button_hit(x,y)){
                    if(e.type==WM_LBUTTONDOWN&&ma2_scn_editor_available())ma2_scn_editor_open();
                    continue;
                }
                if(e.type==WM_LBUTTONDOWN)editor_focus_set(false);   // a click on the row, arrows or footer
                if(!modal_available()&&!getenv("RECOMP_NO_BARREL_ARROWS")){
                    // Vertical barrel (Select Difficulty): the items above and below the selection and the
                    // two arrows glow on hover (update_glow); a click focuses the barrel and steps it once
                    // like Up/Down, which brings the clicked item to the middle. The middle item falls
                    // through to the normal path: hover focuses, a click selects it like Enter.
                    int step=0;float vbox[4],centre[2];
                    int vb=ma2_vbarrel_pointer(object,pc_display_aspect(4.f/3.f)>4.f/3.f+.01f,x,y,&step,vbox,centre);
                    if(vb&&step){
                        if(e.type==WM_LBUTTONDOWN){
                            ma2_gui_hover(object,centre[0],centre[1]);
                            int key=step<0?VK_UP:VK_DOWN;
                            if(getenv("RECOMP_MENU_TRACE"))fprintf(stderr,"[PC-VBARREL] click %s: step %d\n",vb==2?"arrow":"row",step);
                            PostMessageA(game_window,WM_KEYDOWN,key,1);
                            PostMessageA(game_window,WM_KEYUP,key,(LPARAM)3<<30);
                        }
                        continue;
                    }
                }
                if(e.type==WM_LBUTTONDOWN&&!modal_available()&&!getenv("RECOMP_NO_BARREL_ARROWS")){
                    // New Game barrel arrows: focus the row, then step it like Left/Right.
                    float row_x,row_y;int dir=ma2_barrel_arrow_click(object,pc_display_aspect(4.f/3.f)>4.f/3.f+.01f,x,y,&row_x,&row_y);
                    if(dir){
                        uint32_t focused=ma2_gui_hover(object,row_x,row_y);
                        if(getenv("RECOMP_MENU_TRACE"))fprintf(stderr,"[PC-ARROWS] step %d: row %.3f,%.3f focused %08X\n",dir,row_x,row_y,focused);
                        int key=dir<0?VK_LEFT:VK_RIGHT;
                        PostMessageA(game_window,WM_KEYDOWN,key,1);
                        PostMessageA(game_window,WM_KEYUP,key,(LPARAM)3<<30);
                        continue;
                    }
                }
                float footer_box[4];
                uint32_t hit=ma2_gui_footer_at(modal_available()?0:object,x,y,pc_display_aspect(4.f/3.f),footer_box);
                if(!hit)hit=modal_available()?ma2_gui_dialog_pointer(0,x,y):ma2_gui_hover(object,x,y);
                // A click on a value row (map, mode, bots...) only focuses it: it must not act as
                // Enter and start the game -- that is the Enter key's and the select button's job.
                bool row=!modal_available()&&!(hit&0x80000000u)&&!getenv("RECOMP_NO_BARREL_ARROWS")&&
                         ma2_barrel_row_hit(object,pc_display_aspect(4.f/3.f)>4.f/3.f+.01f,x,y);
                if(row&&e.type==WM_LBUTTONDOWN&&getenv("RECOMP_MENU_TRACE"))fprintf(stderr,"[PC-MOUSE] click on a value row: focus only\n");
                if(hit&&e.type==WM_LBUTTONDOWN&&!row){
                    if(hit&0x80000000u)pc_input_menu_button(hit&7);else pc_input_menu_click();
                    if(getenv("RECOMP_MENU_TRACE"))fprintf(stderr,"[PC-MOUSE] click owner=%08X widget=%08X action=%u\n",object,hit,(hit&0x80000000u)?hit&7:guest(hit+0x70));
                }
            }
        }
    }
    {   // The Editor button by pad: Y opens it; Up / LB focus it, then A opens, Right / Down / B leave.
        static WORD last_pad;
        if(vtable!=0x48F3E0||opened||!ma2_scn_custom_active()){last_pad=0;editor_focus_set(false);editor_capture_release();}
        else {
            XINPUT_STATE pad={};WORD buttons=(WORD)(test_pad_held|test_pad_tapped.exchange(0));
            if(xbox_InputPollHostState(&pad)==ERROR_SUCCESS){const auto &g=pad.Gamepad;buttons|=g.wButtons;
                // the guest's pad read (which notes the device) is blanked while the button has focus
                if(editor_capture&&g.wButtons)pc_input_note_gamepad(g.wButtons,g.bLeftTrigger,g.bRightTrigger,g.sThumbLX,g.sThumbLY,g.sThumbRX,g.sThumbRY);}
            WORD pressed=buttons&~last_pad;last_pad=buttons;
            if(!modal_available()&&editor_button_shown()){
                if(pressed&XINPUT_GAMEPAD_Y){if(ma2_scn_editor_available())ma2_scn_editor_open();}
                else if(!editor_focus){if(pressed&(XINPUT_GAMEPAD_DPAD_UP|XINPUT_GAMEPAD_LEFT_SHOULDER))editor_focus_set(true);}
                else if(pressed&XINPUT_GAMEPAD_A){if(ma2_scn_editor_available())ma2_scn_editor_open();}
                else if(pressed&(XINPUT_GAMEPAD_DPAD_RIGHT|XINPUT_GAMEPAD_DPAD_DOWN|XINPUT_GAMEPAD_B))editor_focus_set(false);
            }
            if(!editor_focus&&!buttons&&!ma2_scn_editor_running())editor_capture_release();
        }
    }
    update_glow(object);
    {   // RECOMP_MENU_DUMP: each menu's widget tree once it has settled, and each modal dialog's.
        static int dump=-1;if(dump<0)dump=getenv("RECOMP_MENU_DUMP")!=nullptr;
        if(dump){
            static uint32_t dumped_object,dumped_modal;static DWORD menu_since,modal_since;DWORD now=GetTickCount();
            if(object!=dumped_object){if(!menu_since)menu_since=now;else if(now-menu_since>800){ma2_gui_dump(object,"menu");dumped_object=object;menu_since=0;}}
            uint32_t top=modal_available()?guest(guest(0x5240D4)-4):0;
            if(!top){dumped_modal=0;modal_since=0;}
            else if(top!=dumped_modal){if(!modal_since)modal_since=now;else if(now-modal_since>500){ma2_gui_dump(0,"modal");dumped_modal=top;modal_since=0;}}
        }
    }
    if(exit_dialog.load()) {
        uint32_t result=ma2_gui_dialog_result(exit_dialog);
        if(result){exit_dialog=0;fprintf(stderr,"[PC-MENU] exit confirmation result=%u\n",result);
            if(result==1||result==3) {
                LARGE_INTEGER now={},frequency={};
                QueryPerformanceCounter(&now);QueryPerformanceFrequency(&frequency);
                fprintf(stderr,"[EXIT] phase=post_request source=ma2_pc_menu_pulse reason=confirmed_quit pid=%lu tid=%lu qpc=%lld frequency=%lld message=0x%04X wparam=0 lparam=0 dialog_result=%u\n",
                    GetCurrentProcessId(),GetCurrentThreadId(),(long long)now.QuadPart,(long long)frequency.QuadPart,WM_CLOSE,result);
                fflush(stderr);
                PostMessageA(game_window,WM_CLOSE,0,0);
            }}
    } else if(request_exit.exchange(false)) {
        exit_dialog=ma2_gui_dialog(ma2_pc_wide(L"EXIT GAME"),ma2_pc_wide(L"Exit MechAssault 2 and return to the desktop?"));
        fprintf(stderr,"[PC-MENU] exit confirmation opened handle=%08X\n",exit_dialog.load());
    }
}
extern "C" void ma2_pc_modal_pulse(void){
    // A modal suspends the underlying menu update, but keeps its input context
    // and cursor alive. In particular, Escape must never become Start/confirm.
    sync_text_dialog();
    if(modal_available())ma2_pc_menu_pulse(menu_object,menu_kind);
}
static void refresh_page()
{
    if(!page_form)return;
    ma2_gui_text(ma2_gui_find(page_form,9000),ma2_pc_wide(page==0?L"DISPLAY SETTINGS":page==1?L"AUDIO SETTINGS":page==4?L"CONTROLLER":page==5?L"REPORT A PROBLEM":L"KEYBOARD AND MOUSE"));
    for(int i=0;i<(page==0?13:10);i++)ma2_gui_text(ma2_gui_find(page_form,9100+i),ma2_pc_wide(row_text(i).c_str()));
    if(page==0)ma2_gui_slider(ma2_gui_find(page_form,9302),pc_setting_get(PC_DRAW_DISTANCE,50)/100.f);
    ma2_gui_text(ma2_gui_find(page_form,9200),ma2_pc_page_text(0x70000011));
    page_dirty=false;last_revision=pc_input_revision();
}
static void change(PcSetting setting,int low,int high,int amount){pc_setting_set(setting,std::clamp(pc_setting_get(setting,low)+amount,low,high));}
static void toggle(PcSetting setting,int fallback){pc_setting_set(setting,!pc_setting_get(setting,fallback));}
static int activate(int direction,bool confirm)
{
    if(waiting_binding>=0)return 0;
    if(page==0)switch(selection){
    case 0:fullscreen=!fullscreen;break;
    case 1:{auto it=std::find(resolutions.begin(),resolutions.end(),std::make_pair(output_width,output_height));int i=it==resolutions.end()?0:(int)(it-resolutions.begin());
        i=(i+direction+(int)resolutions.size())%(int)resolutions.size();output_width=resolutions[i].first;output_height=resolutions[i].second;break;}
    case 2:{int q=std::clamp(pc_setting_get(PC_TEXTURE_QUALITY,0),0,2);   // next installed choice
        for(int step=0;step<3;step++){q=(q+direction+3)%3;if(texture_installed[q])break;}
        pc_setting_set(PC_TEXTURE_QUALITY,q);break;}
    case 3:toggle(PC_VSYNC,0);break;
    case 4:{int values[]={30,60,90,120,144,165,180,240,360};int index=0,value=pc_setting_get(PC_FRAME_LIMIT,144);while(index<9&&values[index]!=value)index++;if(index==9)index=4;index=(index+direction+9)%9;pc_setting_set(PC_FRAME_LIMIT,values[index]);break;}
    case 5:toggle(PC_ASPECT,1);break;
    case 6:toggle(PC_FILTER,1);break;
    case 7:change(PC_DRAW_DISTANCE,0,100,direction*5);break;
    case 8:toggle(PC_SHOW_FPS,0);break;
    case 9:if(videos_installed)toggle(PC_FMV_QUALITY,0);break;
    case 10:toggle(PC_SHADER_PRELOAD,1);break;
    case 11:pc_setting_set(PC_RENDER_SCALE,0);save_settings(true);break;
    case 12:pc_setting_set(PC_SHADER_PRELOAD,1);pc_setting_set(PC_VSYNC,0);pc_setting_set(PC_FRAME_LIMIT,144);pc_setting_set(PC_RENDER_SCALE,0);pc_setting_set(PC_ASPECT,1);pc_setting_set(PC_FILTER,1);pc_setting_set(PC_SHOW_FPS,0);pc_setting_set(PC_DRAW_DISTANCE,50);pc_setting_set(PC_TEXTURE_QUALITY,0);pc_setting_set(PC_FMV_QUALITY,0);break;}
    else if(page==1){if(selection==0)change(PC_VOLUME,0,100,direction*5);
        else if(selection==4&&report_helper_ok()){if(confirm)report_enter(1);}
        else if(selection==(report_helper_ok()?5:4))return 2;
        else if(selection<4){int &value=*(int *)(g_xbox_mem_offset+page_object+0x84+(selection-1)*4);value=std::clamp(value+direction*3277,0,65535);ma2_gui_volume(page_object,selection-1,value);}}
    else if(page==2)switch(selection){
    case 0:if(pc_setting_get(PC_KEYBOARD,1)&&!gamepad_connected())break;/* never leave the player with no input device */toggle(PC_KEYBOARD,1);break;case 1:toggle(PC_MOUSE_AIM,1);break;case 2:change(PC_MOUSE_SENSITIVITY,10,500,direction*10);break;case 3:toggle(PC_INVERT_Y,0);break;
    case 4:toggle(PC_INVERT_FLIGHT,0);break;
    case 5:if(confirm){page=3;selection=0;binding_page=0;}break;
    case 6:for(int i=0;i<PC_BINDING_COUNT;i++)pc_binding_set((PcBinding)i,default_keys[i]);pc_setting_set(PC_MOUSE_SENSITIVITY,100);pc_setting_set(PC_INVERT_Y,0);pc_setting_set(PC_INVERT_FLIGHT,0);pc_setting_set(PC_MOUSE_AIM,1);pc_setting_set(PC_KEYBOARD,1);break;
    case 7:if(confirm){page=4;selection=0;}break;
    case 8:return 2;}
    else if(page==5){
        if(selection==REPORT_BOX&&report_state==0){
            // A controller cannot type: Left / Right (and A) pick a ready-made description. Enter moves on to Send.
            if(!confirm||pc_input_device()==PC_INPUT_GAMEPAD)report_preset_step(confirm?1:direction);
            else selection=REPORT_SEND;
        }
        else if(selection==REPORT_SEND&&confirm){
            if(report_state==0)report_send();
            else if(report_state==2&&!report_outcome.ok){report_state=0;selection=REPORT_SEND;}
        }
        else if(selection==REPORT_BACK&&confirm)return report_leave();
    }
    else if(page==3&&confirm){if(selection==8){binding_page=(binding_page+1)%3;selection=0;}else if(selection==9){page=2;selection=4;}else if(binding_page*8+selection<PC_BINDING_COUNT)waiting_binding=binding_page*8+selection;}
    else if(page==4){
        if(selection<2)ma2_gui_controller_set(page_object,selection,!ma2_gui_controller_option(page_object,selection));
        else if(selection==2&&confirm){page=2;selection=0;}
        else if(selection==3)return 2;
    }
    page_dirty=true;return 0;
}
static int report_leave(){
    fprintf(stderr,"[REPORT] in-game report page closed\n");
    if(report_state==2&&report_shown_outcome)report_state=0;
    if(report_from<0)return 2;
    page=1;selection=4;page_dirty=true;return 0;
}
static int page_key(int key)
{
    if(page==5){
        // The box owns Backspace and Space; the rest works as on the other pages.
        if(key==VK_BACK&&selection==REPORT_BOX&&report_state==0){if(!report_text.empty()){report_text.pop_back();if(report_text.empty())report_preset=-1;}page_dirty=true;return 0;}
        if(key==VK_SPACE&&selection==REPORT_BOX&&report_state==0)return 0;   // typed through WM_CHAR
        if(key==VK_ESCAPE||key==VK_BACK)return report_leave();
        if(key==VK_UP||key==VK_DOWN){int order[3]={REPORT_BOX,REPORT_SEND,REPORT_BACK},at=0;for(int i=0;i<3;i++)if(order[i]==selection)at=i;
            for(int step=0;step<3;step++){at=(at+(key==VK_UP?2:1))%3;if(report_row_valid(order[at]))break;}
            selection=order[at];page_dirty=true;return 0;}
    }
    if(waiting_binding>=0){if(key!=VK_ESCAPE&&key>0&&key<256)pc_binding_set((PcBinding)waiting_binding,key);waiting_binding=-1;page_dirty=true;return 0;}
    if(key==VK_ESCAPE||key==VK_BACK){if(page==3){page=2;selection=4;page_dirty=true;return 0;}return 2;}
    if(key==VK_UP||key==VK_DOWN){do {selection=(selection+(key==VK_UP?-1:1)+row_count())%row_count();}while(!valid_row(selection));page_dirty=true;return 0;}
    if(key==VK_LEFT||key==VK_RIGHT||key==VK_RETURN||key==VK_SPACE)return activate(key==VK_LEFT?-1:1,key==VK_RETURN||key==VK_SPACE);
    return 0;
}
extern "C" int ma2_pc_submenu_update(uint32_t object,uint32_t vtable)
{
    int type=vtable==0x48B360?0:vtable==0x48BE60?1:vtable==0x48BF20?2:-1;if(type<0)return -1;
    ma2_pc_menu_pulse(object,vtable);
    bool entering=page_object!=object;
    if(entering){
        check_optional_content();
        page_object=object;page_form=guest(object+0x68);page=type==2&&pc_input_device()==PC_INPUT_GAMEPAD?4:type;selection=0;waiting_binding=-1;page_dirty=true;opened=true;opened_at=GetTickCount();distance_dragging=false;
        // The pause menu's Report a Problem opened this Audio submenu: straight to the report page.
        DWORD asked=report_pending.exchange(0);
        if(type==1&&asked&&GetTickCount()-asked<3000&&report_helper_ok())report_enter(-1);
        read_window_state();
        pc_frontend_capture(1);PostMessageA(game_window,PC_WINDOW_CAPTURE_MESSAGE,0,0);
        fprintf(stderr,"[PC-MENU] original settings page=%d form=%08X\n",page,page_form);
        ma2_gui_inspect(page_form);
        AcquireSRWLockExclusive(&events_lock);events.clear();ReleaseSRWLockExclusive(&events_lock);
    }
    std::vector<Event> pending;AcquireSRWLockExclusive(&events_lock);pending.swap(events);ReleaseSRWLockExclusive(&events_lock);
    int result=0;
    for(auto e:pending){
        if(e.type==WM_SIZE){read_window_state();continue;}
        if(e.type==WM_LBUTTONUP||e.type==WM_KILLFOCUS||e.type==WM_CAPTURECHANGED){distance_dragging=false;continue;}
        if(e.type==WM_CHAR){if(page==5)report_type((uint32_t)e.key);continue;}
        if(e.type==WM_KEYDOWN)result=page_key(e.key);
        else if(e.type>=WM_MOUSEFIRST&&e.type<=WM_MOUSELAST){
            if(waiting_binding>=0&&e.type!=WM_MOUSEMOVE)result=page_key(e.key);
            else {
                RECT r;GetClientRect(game_window,&r);float scale=std::min(r.right/640.0f,r.bottom/480.0f);
                float x=(e.x-(r.right-640*scale)*.5f)/scale,y=(e.y-(r.bottom-480*scale)*.5f)/scale;
                int row=y>=row_top()?(int)((y-row_top())/row_step()):-1;
                if(page==5&&row>=1&&row<=4&&report_state==0){if(selection!=REPORT_BOX){selection=REPORT_BOX;page_dirty=true;}continue;}
                if(page==0&&((e.type==WM_LBUTTONDOWN&&x>=315&&x<566&&row==7)||(distance_dragging&&e.type==WM_MOUSEMOVE))){
                    distance_dragging=true;selection=7;
                    pc_setting_set(PC_DRAW_DISTANCE,std::clamp((int)((x-325)*100/230+.5f),0,100));page_dirty=true;continue;
                }
                if(x>=70&&x<570&&valid_row(row)){
                    if(selection!=row){selection=row;page_dirty=true;}
                    if(e.type!=WM_MOUSEMOVE)result=activate(e.type==WM_RBUTTONDOWN?-1:1,true);
                }
                if(e.type==WM_LBUTTONDOWN&&esc_back_hit(x,y))result=page_key(VK_ESCAPE);
            }
        }
        if(result)break;
    }
    static WORD last_buttons;static int last_axis;static DWORD repeat_at;
    XINPUT_STATE pad={};
    // Tests (RECOMP_UI_LIVE): the scripted pad buttons (see test_pad_bit) work on these pages as well.
    const WORD scripted=(WORD)(test_pad_held|test_pad_tapped.exchange(0));
    if(xbox_InputPollHostState(&pad)==ERROR_SUCCESS||scripted){
        pad.Gamepad.wButtons|=scripted;
        const auto &g=pad.Gamepad;if(!scripted)pc_input_note_gamepad(g.wButtons,g.bLeftTrigger,g.bRightTrigger,g.sThumbLX,g.sThumbLY,g.sThumbRX,g.sThumbRY);
        // The A press that opened Controls must not toggle its first option.
        WORD pressed=entering?0:g.wButtons&~last_buttons;last_buttons=g.wButtons;
        // Physical menu navigation must not become a captured keyboard binding.
        const bool capturing_binding=waiting_binding>=0;
        int axis=g.sThumbLY>16000?VK_UP:g.sThumbLY<-16000?VK_DOWN:g.sThumbLX<-16000?VK_LEFT:g.sThumbLX>16000?VK_RIGHT:0;
        if(entering)repeat_at=GetTickCount()+350;
        else if(axis&&(axis!=last_axis||GetTickCount()>=repeat_at)){if(!capturing_binding)page_key(axis);repeat_at=GetTickCount()+(axis!=last_axis?350:100);}last_axis=axis;
        if(!capturing_binding){
                if(pressed&XINPUT_GAMEPAD_DPAD_UP)page_key(VK_UP);if(pressed&XINPUT_GAMEPAD_DPAD_DOWN)page_key(VK_DOWN);
                if(pressed&XINPUT_GAMEPAD_DPAD_LEFT)page_key(VK_LEFT);if(pressed&XINPUT_GAMEPAD_DPAD_RIGHT)page_key(VK_RIGHT);
                if(pressed&XINPUT_GAMEPAD_A)result=page_key(VK_RETURN);
        }
        if(pressed&XINPUT_GAMEPAD_B)result=page_key(VK_ESCAPE);
    }else last_buttons=0;
    if(result==2){
        if(type==1)ma2_gui_volume_commit(object);
        if(type==2)ma2_gui_controller_commit(object);
        save_settings(false);opened=false;page_object=page_form=0;pc_frontend_capture(0);PostMessageA(game_window,PC_WINDOW_CAPTURE_MESSAGE,0,0);return 2;
    }
    if(page==5){static int blink=-1,shown_state=-1;int b=report_cursor_on();int st=report_state;
        if(st==2&&shown_state!=2)selection=report_outcome.ok?REPORT_BACK:REPORT_SEND;   // the outcome: Back, or Try again
        if(b!=blink||st!=shown_state){blink=b;shown_state=st;page_dirty=true;}}
    if(page_dirty||last_revision!=pc_input_revision())refresh_page();return 0;
}
extern "C" int ma2_pc_video_menu_update(uint32_t object){return ma2_pc_submenu_update(object,0x48B360);}
extern "C" void ma2_pc_submenu_destroyed(uint32_t object){if(page_object==object){page_object=page_form=0;opened=false;pc_frontend_capture(0);}}
extern "C" void ma2_pc_submenu_result(uint32_t object,int result){if(result==2)ma2_pc_submenu_destroyed(object);}
// Sticky Keys hotkey (five Shift presses) is off only while the game window has focus (easy-wins D8). Follows Microsoft's
// "Disabling Shortcut Keys in Games": remember the user's setting, clear only the hotkey bits, put the remembered value
// back on focus loss, window destroy, exit (main.c atexit) and crash (main.c filter). Not written to the user's profile
// (no SPIF_UPDATEINIFILE), so even a hard kill is undone by the next sign-out.
static STICKYKEYS sticky_saved;
static std::atomic<int> sticky_suppressed,sticky_have_saved;
extern "C" void ma2_pc_sticky_keys(int suppress){
    if(suppress){
        int expected=0;if(!sticky_suppressed.compare_exchange_strong(expected,1))return;
        STICKYKEYS now={};now.cbSize=sizeof(now);
        if(!SystemParametersInfoA(SPI_GETSTICKYKEYS,sizeof(now),&now,0)){sticky_suppressed=0;return;}
        sticky_saved=now;sticky_have_saved=1;
        // Self-heal (W-02): a kill or power loss skips every restore path, and the next start would then save the
        // already-cleared flags as "the user's setting". Keep the original flags in the ini while suppressed.
        if(ini_path[0]){
            int left=(int)GetPrivateProfileIntA("Internal","StickyKeysSaved",-1,ini_path);
            if(left>=0){sticky_saved.dwFlags=(DWORD)left;
                fprintf(stderr,"[PC-INPUT] Sticky Keys: restoring flags 0x%lX left by an unclean exit\n",(unsigned long)sticky_saved.dwFlags);}
            else{char text[32];snprintf(text,sizeof(text),"%lu",(unsigned long)now.dwFlags);WritePrivateProfileStringA("Internal","StickyKeysSaved",text,ini_path);}
        }
        if(!(now.dwFlags&SKF_STICKYKEYSON)){
            now.dwFlags&=~(DWORD)(SKF_HOTKEYACTIVE|SKF_CONFIRMHOTKEY);
            SystemParametersInfoA(SPI_SETSTICKYKEYS,sizeof(now),&now,0);
        }
        fprintf(stderr,"[PC-INPUT] Sticky Keys hotkey suppressed (was flags 0x%lX)\n",(unsigned long)sticky_saved.dwFlags);
    }else{
        int expected=1;if(!sticky_suppressed.compare_exchange_strong(expected,0))return;
        int saved=1;if(sticky_have_saved.compare_exchange_strong(saved,0)){
            SystemParametersInfoA(SPI_SETSTICKYKEYS,sizeof(sticky_saved),&sticky_saved,0);
            if(ini_path[0])WritePrivateProfileStringA("Internal","StickyKeysSaved",nullptr,ini_path);
            fprintf(stderr,"[PC-INPUT] Sticky Keys hotkey restored (flags 0x%lX)\n",(unsigned long)sticky_saved.dwFlags);
        }
    }
}
static int window_message(void *window,unsigned message,uintptr_t wparam,intptr_t lparam)
{
    game_window=(HWND)window;
    if(message==WM_SETFOCUS&&!env_int("RECOMP_KEEP_STICKY_HOTKEY",0))ma2_pc_sticky_keys(1);
    else if(message==WM_KILLFOCUS||message==WM_CLOSE||message==WM_DESTROY)ma2_pc_sticky_keys(0);
    if(message==WM_MOUSEMOVE)mouse_position=(uint32_t)lparam;
    if(message==WM_SETFOCUS||message==WM_KILLFOCUS||message==WM_CLOSE||message==WM_DESTROY||message==PC_WINDOW_RENDERER_ERROR_MESSAGE)
        text_focus_or_close(message);
    if(message==PC_WINDOW_RENDERER_ERROR_MESSAGE) {
        // A terminal renderer cannot draw another guest dialog or settings
        // page. Handle this on the window thread even when a movie owns input.
        if(renderer_error_handled)return 1;
        renderer_error_handled=true;
        pc_frontend_capture(0);pc_input_camera_capture(0);distance_dragging=false;
        PostMessageA((HWND)window,PC_WINDOW_CAPTURE_MESSAGE,0,0);
        wchar_t text[256];
        if((unsigned)wparam==0x887A0004u)
            swprintf_s(text,L"MechAssault 2 needs a DirectX 11 (feature level 11.0) graphics card.\n"
                L"Update the graphics driver and start the game again (0x%08X).",(unsigned)wparam);
        else if((unsigned)wparam==0x8007000Eu)
            swprintf_s(text,L"MechAssault 2 ran out of graphics memory.\n"
                L"Lower Texture quality or Resolution in Display settings and start the game again (0x%08X).",(unsigned)wparam);
        else
            swprintf_s(text,L"MechAssault 2 cannot continue rendering (0x%08X).\n"
                L"The game will close. Try restarting the game.",(unsigned)wparam);
        if(!env_int("RECOMP_GPU_RECOVERY_TEST",0))
            MessageBoxW((HWND)window,text,L"MechAssault 2",MB_OK|MB_ICONERROR);
        LARGE_INTEGER now={},frequency={};
        QueryPerformanceCounter(&now);QueryPerformanceFrequency(&frequency);
        fprintf(stderr,"[EXIT] phase=post_request source=window_message reason=terminal_renderer_error pid=%lu tid=%lu qpc=%lld frequency=%lld message=0x%04X wparam=0 lparam=0 renderer_error=0x%08X\n",
            GetCurrentProcessId(),GetCurrentThreadId(),(long long)now.QuadPart,(long long)frequency.QuadPart,WM_CLOSE,(unsigned)wparam);
        fflush(stderr);
        PostMessageW((HWND)window,WM_CLOSE,0,0);
        return 1;
    }
    if((message==WM_KEYDOWN||message==WM_KEYUP)&&test_pad_bit(wparam)&&getenv("RECOMP_UI_LIVE")){
        unsigned bit=test_pad_bit(wparam);
        if(message==WM_KEYDOWN){test_pad_held|=bit;test_pad_tapped|=bit;pc_input_note_gamepad(bit,0,0,0,0,0,0);}
        else test_pad_held&=~bit;
        fprintf(stderr,"[PC-UI] test pad %s %04X\n",message==WM_KEYDOWN?"down":"up",bit);
        return 1;
    }
    // The launching menu may still have a recent pulse. The movie owns input.
    if(pc_display_movie_active())return 0;
    if(message==WM_SIZE&&opened&&wparam!=SIZE_MINIMIZED)queue({WM_SIZE,0,0,0});
    // Window shortcuts remain available while a settings page owns input.
    if((message==WM_KEYDOWN||message==WM_KEYUP)&&(wparam==VK_F11||wparam==VK_F8))return 0;
    // 1.2.0: Messages owns the keyboard and the mouse while it is open; M or a click on the envelope opens it on the main menu.
    if(reply_state.load()&&reply_input((HWND)window,message,wparam,lparam))return 1;
    if(!reply_state.load()&&envelope_visible()&&!pc_input_text_captured()&&!pc_input_camera_captured()){
        if(message==WM_KEYDOWN&&wparam=='M'&&!(lparam&(1LL<<30))){ReplyLock l;inbox_open_locked("M");return 1;}
        if(message==WM_LBUTTONDOWN&&envelope_shown){float x=(float)GET_X_LPARAM(lparam),y=(float)GET_Y_LPARAM(lparam);
            if(x>=envelope_hit[0]&&x<envelope_hit[2]&&y>=envelope_hit[1]&&y<envelope_hit[3]){ReplyLock l;inbox_open_locked("click");return 1;}}
    }
    if(text_message((HWND)window,message,wparam,lparam))return 1;
    if(message==WM_LBUTTONDOWN&&getenv("RECOMP_MENU_TRACE")){
        RECT r={};GetClientRect((HWND)window,&r);
        fprintf(stderr,"[PC-POINTER] menu=%08X age=%u opened=%d camera=%d point=%d,%d client=%ld,%ld\n",
            menu_kind.load(),GetTickCount()-menu_time.load(),(int)opened.load(),pc_input_camera_captured(),GET_X_LPARAM(lparam),GET_Y_LPARAM(lparam),r.right,r.bottom);
    }
    if(message==WM_KILLFOCUS)pc_input_camera_capture(0);
    if(message==WM_KEYDOWN&&wparam==VK_ESCAPE&&pc_input_camera_captured()){
        pc_input_camera_capture(0);PostMessageA((HWND)window,PC_WINDOW_CAPTURE_MESSAGE,0,0);return 1;
    }
    if(message==WM_KEYDOWN&&editor_button_shown()&&!modal_available()&&!pc_input_text_captured()){
        // Keyboard focus on the Editor button: Up / Tab from the scenario row; Enter / Space open it.
        const bool first=!(lparam&(1LL<<30));
        if(!editor_focus){if(wparam==VK_UP||wparam==VK_TAB){if(first)editor_focus_set(true);return 1;}}
        else if(wparam==VK_RETURN||wparam==VK_SPACE){if(first&&ma2_scn_editor_available())ma2_scn_editor_open();return 1;}
        else if(wparam==VK_RIGHT||wparam==VK_DOWN||wparam==VK_ESCAPE||wparam==VK_BACK||wparam==VK_TAB){if(first)editor_focus_set(false);return 1;}
        else if(wparam==VK_LEFT||wparam==VK_UP)return 1;
    }
    if(message==WM_KEYDOWN&&!(lparam&(1LL<<30))&&wparam=='E'&&editor_button_shown()&&!modal_available()&&!pc_input_text_captured()){
        if(ma2_scn_editor_available())ma2_scn_editor_open();
        return 1;
    }
    if(message==WM_KEYDOWN&&!(lparam&(1LL<<30))&&wparam==VK_ESCAPE&&!opened&&!exit_dialog&&menu_available()&&!modal_available()&&
        (menu_kind==0x481B70||menu_kind==0x482170)){request_exit=true;return 1;}
    if(opened){
        if(message==WM_CHAR){
            // The report page's text box (other pages ignore typed characters, as before).
            if(page==5){static TextDecoder decoder;uint32_t code=wparam==0x16?0x16:decoder.decode(GetACP(),IsWindowUnicode((HWND)window)!=FALSE,wparam);
                if(code==0x16||(code>=0x20&&!(code>=0x7F&&code<0xA0)&&code<0xFFFE&&!(code>=0xD800&&code<=0xDFFF)))queue({WM_CHAR,(int)code,0,0});}
            return page==5?1:0;
        }
        if(message==WM_KEYDOWN&&page==5&&wparam!=VK_ESCAPE&&wparam!=VK_RETURN&&(lparam&(1LL<<30))&&wparam!=VK_BACK&&wparam!=VK_UP&&wparam!=VK_DOWN)return 1;
        if(message==WM_KEYDOWN&&page==5&&wparam==VK_BACK){queue({WM_KEYDOWN,(int)wparam,0,0});return 1;}
        if(message==WM_KEYDOWN&&!(lparam&(1LL<<30))){queue({WM_KEYDOWN,(int)wparam,0,0});return 1;}
        if(message==WM_KILLFOCUS||message==WM_CAPTURECHANGED){queue({message,0,0,0});return 0;}
        if(message==WM_MOUSEMOVE||message==WM_LBUTTONDOWN||message==WM_LBUTTONUP||message==WM_RBUTTONDOWN||message==WM_MBUTTONDOWN||message==WM_XBUTTONDOWN){
            if(message==WM_LBUTTONDOWN)SetCapture((HWND)window);
            if(message==WM_LBUTTONUP&&GetCapture()==(HWND)window)ReleaseCapture();
            int key=message==WM_RBUTTONDOWN?VK_RBUTTON:message==WM_MBUTTONDOWN?VK_MBUTTON:message==WM_XBUTTONDOWN?(GET_XBUTTON_WPARAM(wparam)==XBUTTON1?VK_XBUTTON1:VK_XBUTTON2):VK_LBUTTON;
            queue({message,key,GET_X_LPARAM(lparam),GET_Y_LPARAM(lparam)});return 1;}
        if(message==WM_KEYUP||message==WM_INPUT||(message>=WM_MOUSEFIRST&&message<=WM_MOUSELAST))return 1;
    }
    else if(menu_available()&&(menu_kind!=0x482170||exit_dialog)&&!pc_input_camera_captured()&&
            (message==WM_MOUSEMOVE||message==WM_LBUTTONDOWN)){
        float x,y;menu_point((HWND)window,GET_X_LPARAM(lparam),GET_Y_LPARAM(lparam),x,y);
        if(message==WM_LBUTTONDOWN&&!exit_dialog&&menu_kind==0x481B70&&x>=0&&x<=1&&y>=0&&y<.64f){
            pc_input_camera_capture(1);PostMessageA((HWND)window,PC_WINDOW_CAMERA_MESSAGE,1,0);return 1;
        }
        queue({message,VK_LBUTTON,GET_X_LPARAM(lparam),GET_Y_LPARAM(lparam)});return 1;
    }
    return 0;
}
#include "pc_capture.inl"
static void device_lost()
{
    // Called by the immediate-context owner before it releases the device.
    // The CPU context/fonts/preferences and Win32 backend remain reusable.
    if(fps_renderer_initialized) {
        ImGui_ImplDX11_Shutdown();fps_renderer_initialized=false;
    }
}
static void render(void *device_ptr,void *context_ptr,void *window_ptr,unsigned width,unsigned height)
{
    game_window=(HWND)window_ptr;
    auto device=(ID3D11Device *)device_ptr;auto context=(ID3D11DeviceContext *)context_ptr;
    static unsigned frames;static DWORD started;static float fps;
    DWORD now=GetTickCount();frames++;if(!started)started=now;
    if(now-started>=500){fps=frames*1000.f/(now-started);frames=0;started=now;}
    float glow_rect[4];float glow=glow_alpha(glow_rect);
    bool show_fps=pc_setting_get(PC_SHOW_FPS,0)!=0;
    bool editor_button=editor_button_shown();const wchar_t *editor_note=ma2_scn_editor_note();
    const bool report_box=opened&&page==5&&page_form&&report_state!=2&&!pc_display_movie_active();
    const bool reply_box=reply_state.load()!=0&&!pc_display_movie_active();
    const bool envelope=envelope_visible();     // 1.2.0: Messages on the main menu
    if(show_fps||glow>0.f||editor_button||editor_note||report_box||envelope||reply_box){
        if(!fps_context_initialized){ImGui::CreateContext();ImGui::GetIO().IniFilename=nullptr;ImGui::GetIO().LogFilename=nullptr;
            // The overlay never owns the cursor: menus and mouse aim set it.
            ImGui::GetIO().ConfigFlags|=ImGuiConfigFlags_NoMouseCursorChange;fps_context_initialized=true;}
        if(!fps_platform_initialized)fps_platform_initialized=ImGui_ImplWin32_Init(window_ptr);
        if(fps_platform_initialized&&!fps_renderer_initialized)fps_renderer_initialized=ImGui_ImplDX11_Init(device,context);
        if(fps_renderer_initialized){
            ImGui_ImplDX11_NewFrame();ImGui_ImplWin32_NewFrame();
            // Draw in back-buffer pixels; the glow box is in client pixels.
            RECT client={};GetClientRect((HWND)window_ptr,&client);
            if(width&&height&&client.right>0&&client.bottom>0){
                ImGui::GetIO().DisplaySize=ImVec2((float)width,(float)height);
                float sx=(float)width/client.right,sy=(float)height/client.bottom;
                glow_rect[0]*=sx;glow_rect[2]*=sx;glow_rect[1]*=sy;glow_rect[3]*=sy;
            }
            ImGui::NewFrame();
            if(report_box&&client.right>0&&client.bottom>0)draw_report_box(ImGui::GetBackgroundDrawList(),(float)width,(float)height,selection==REPORT_BOX&&report_state==0);
            if(glow>0.f)draw_glow(ImGui::GetBackgroundDrawList(),glow_rect,glow);
            if(editor_button&&client.right>0&&client.bottom>0){
                float box[4];editor_button_box((HWND)window_ptr,box);
                POINT p={};static const bool scripted=getenv("RECOMP_UI_LIVE")!=nullptr;bool pointer;
                if(scripted){uint32_t m=mouse_position.load();p.x=GET_X_LPARAM(m);p.y=GET_Y_LPARAM(m);pointer=m!=0xFFFFFFFFu;}
                else pointer=GetForegroundWindow()==(HWND)window_ptr&&GetCursorPos(&p)&&ScreenToClient((HWND)window_ptr,&p);
                bool hover=editor_focus||(pointer&&p.x>=box[0]&&p.x<box[2]&&p.y>=box[1]&&p.y<box[3]);
                float sx=(float)width/client.right,sy=(float)height/client.bottom;
                box[0]*=sx;box[2]*=sx;box[1]*=sy;box[3]*=sy;
                draw_editor_button(ImGui::GetBackgroundDrawList(),box,hover,ma2_scn_editor_available()!=0);
                if(ma2_scn_editor_available()){
                    float hint[4];menu_box((HWND)window_ptr,EDITOR_HINT_X0,EDITOR_HINT_Y0,EDITOR_HINT_X1,EDITOR_HINT_Y1,hint);
                    hint[0]*=sx;hint[2]*=sx;hint[1]*=sy;hint[3]*=sy;
                    draw_editor_hint(ImGui::GetBackgroundDrawList(),hint,pc_input_device()==PC_INPUT_GAMEPAD);
                }
            }
            if(editor_note)draw_editor_note(ImGui::GetForegroundDrawList(),(float)width,(float)height,editor_note);
            if(client.right>0&&client.bottom>0){
                const float sx=(float)width/client.right,sy=(float)height/client.bottom;
                {   // the mouse-over light: the pointer in back-buffer pixels, none on a controller
                    POINT p={};static const bool scripted=getenv("RECOMP_UI_LIVE")!=nullptr;bool pointer;
                    if(scripted){uint32_t m=mouse_position.load();p.x=GET_X_LPARAM(m);p.y=GET_Y_LPARAM(m);pointer=m!=0xFFFFFFFFu;}
                    else pointer=GetForegroundWindow()==(HWND)window_ptr&&GetCursorPos(&p)&&ScreenToClient((HWND)window_ptr,&p);
                    const bool pad=reply_box?inbox_pad==1||(inbox_pad<0&&pc_input_device()==PC_INPUT_GAMEPAD):pc_input_device()==PC_INPUT_GAMEPAD;
                    if(!pointer||pad)hover_light.begin(-1,-1);else hover_light.begin(p.x*sx,p.y*sy);
                }
                if(envelope&&!reply_box){   // 1.2.0: Messages (it replaces the 1.1.7 update line)
                    POINT p={};static const bool scripted=getenv("RECOMP_UI_LIVE")!=nullptr;bool pointer;
                    if(scripted){uint32_t m=mouse_position.load();p.x=GET_X_LPARAM(m);p.y=GET_Y_LPARAM(m);pointer=m!=0xFFFFFFFFu;}
                    else pointer=GetForegroundWindow()==(HWND)window_ptr&&GetCursorPos(&p)&&ScreenToClient((HWND)window_ptr,&p);
                    // a controller in use: no mouse-over light (as the footer glow)
                    bool hover=pointer&&pc_input_device()!=PC_INPUT_GAMEPAD&&envelope_shown&&p.x>=envelope_hit[0]&&p.x<envelope_hit[2]&&p.y>=envelope_hit[1]&&p.y<envelope_hit[3];
                    draw_envelope(ImGui::GetForegroundDrawList(),(float)width,(float)height,sx,sy,hover);
                }else envelope_shown=false;
                if(reply_box){
                    static bool logged;if(!logged){logged=true;fprintf(stderr,"[MSG] inbox drawn\n");}
                    draw_inbox(ImGui::GetForegroundDrawList(),(float)width,(float)height,sx,sy);
                }
            }
            if(show_fps){
                ImGui::SetNextWindowPos(ImVec2(16,16));ImGui::SetNextWindowBgAlpha(.55f);
                ImGui::Begin("Frame rate",nullptr,ImGuiWindowFlags_NoDecoration|ImGuiWindowFlags_AlwaysAutoResize|ImGuiWindowFlags_NoInputs|ImGuiWindowFlags_NoSavedSettings);
                ImGui::Text("%.0f FPS",fps);ImGui::End();
            }
            ImGui::Render();ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        }
    }
    capture_panel(device,context);
}
extern "C" void ma2_pc_settings_initialize(const char *ini)
{
    GetFullPathNameA(ini,MAX_PATH,ini_path,nullptr);output_width=env_int("RECOMP_WINDOW_WIDTH",1280);output_height=env_int("RECOMP_WINDOW_HEIGHT",960);fullscreen=env_int("RECOMP_FULLSCREEN",0)!=0;pc_render_size(output_width,output_height);
    const char *env_names[PC_SETTING_COUNT]={"RECOMP_VSYNC","RECOMP_FRAME_LIMIT","RECOMP_PRESERVE_ASPECT","RECOMP_SCALE_FILTER","RECOMP_AUDIO_VOLUME","RECOMP_MOUSE_AIM","RECOMP_MOUSE_SENSITIVITY","RECOMP_INVERT_Y","RECOMP_KEYBOARD","RECOMP_RENDER_SCALE","RECOMP_SHOW_FPS","RECOMP_DRAW_DISTANCE","RECOMP_TEXTURE_QUALITY","RECOMP_FMV_QUALITY","RECOMP_INVERT_FLIGHT","RECOMP_SHADER_PRELOAD"};
    const int values[PC_SETTING_COUNT]={0,144,1,1,100,1,100,0,1,0,0,50,0,0,0,1};for(int i=0;i<PC_SETTING_COUNT;i++)pc_setting_set((PcSetting)i,env_int(env_names[i],values[i]));
    pc_setting_set(PC_DRAW_DISTANCE,std::clamp(pc_setting_get(PC_DRAW_DISTANCE,50),0,100));
    pc_setting_set(PC_TEXTURE_QUALITY,std::clamp(pc_setting_get(PC_TEXTURE_QUALITY,0),0,2));
    pc_setting_set(PC_FMV_QUALITY,std::clamp(pc_setting_get(PC_FMV_QUALITY,0),0,1));
    init_default_keys(nullptr);
    {int f=pc_setting_get(PC_FRAME_LIMIT,144);if(f<=0)pc_setting_set(PC_FRAME_LIMIT,144);else if(f<30)pc_setting_set(PC_FRAME_LIMIT,30);} // a typo in the ini must not run the game at 5 fps; Unlimited (0) is gone (playtest 11-04: high step rates broke VTOL thrust) -> 144
    for(int i=0;i<PC_BINDING_COUNT;i++){int key=GetPrivateProfileIntA("Bindings",binding_names[i],default_keys[i],ini_path);pc_binding_set((PcBinding)i,key>0&&key<256?key:default_keys[i]);}
    resolutions={{640,480},{960,720},{1280,960},{1280,720},{1920,1080},{2560,1440},{3440,1440},{3840,1600},{3840,2160},{5120,1440},{output_width,output_height}};
    DEVMODEA mode={};mode.dmSize=sizeof(mode);for(DWORD i=0;EnumDisplaySettingsA(nullptr,i,&mode);i++)if(mode.dmPelsWidth>=960&&mode.dmPelsHeight>=600)resolutions.emplace_back(mode.dmPelsWidth,mode.dmPelsHeight);
    std::sort(resolutions.begin(),resolutions.end());resolutions.erase(std::unique(resolutions.begin(),resolutions.end()),resolutions.end());
    pc_frontend_register(render,window_message);
    pc_frontend_register_device_lost(device_lost);
    ma2_update_check_start(MA2_PUBLIC_VERSION); // 1.1.7: background thread, off with [Update] CheckForUpdates=0
    msg_initialize();                           // 1.2.0: Messages (stored messages, what's new in this version)
    reply_check_start();                        // 1.2.0: only when a report was sent from this PC (MA2 Report.exe asks)
}
