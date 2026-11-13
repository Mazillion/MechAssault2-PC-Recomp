#include "pc_settings.h"
#include "platform/pc_frontend.h"
#include <windows.h>
#include <windowsx.h>
#include <d3d11.h>
#include <xinput.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <vector>
#include <atomic>
#include "imgui.h"
#include "backends/imgui_impl_win32.h"
#include "backends/imgui_impl_dx11.h"
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND,UINT,WPARAM,LPARAM);
extern "C" ptrdiff_t g_xbox_mem_offset;

static char ini_path[MAX_PATH];
static std::atomic<DWORD> menu_time;
static std::atomic<uint32_t> menu_kind;
static std::atomic<bool> request_open,request_close,request_exit;
static std::atomic<uint32_t> routed_video_menu;
static std::atomic<bool> routed_video_closed;
static std::atomic<uint32_t> original_submenu;
static std::atomic<int> requested_tab{-1};
static std::atomic<bool> title_form;
static std::atomic<int> waiting_binding{-1},binding_key;
static std::atomic<int> anchor_left,anchor_top,anchor_right,anchor_bottom;
struct WindowMessage { HWND window; UINT message; WPARAM wparam; LPARAM lparam; };
static SRWLOCK messages_lock=SRWLOCK_INIT;
static std::vector<WindowMessage> messages;
static bool initialized,renderer_initialized,opened;
static ImGuiStyle base_style;
static float style_scale;
static DWORD opened_at;
static int output_width=1280,output_height=960;
static bool fullscreen;
static bool settings_write_failed;
static DWORD settings_write_error;
static const char *action_names[PC_BINDING_COUNT]={
    "Start / Pause","Back","Confirm / A","Back / B","X button","Y button",
    "White / Right shoulder","Black / Left shoulder","Left trigger","Fire / Right trigger",
    "Move left","Move right","Move backward","Move forward","Left stick press","Right stick press",
    "Mouse fire","Mouse left trigger"
};
static const char *binding_names[PC_BINDING_COUNT]={
    "Start","Back","A","B","X","Y","White","Black","LeftTrigger","RightTrigger",
    "MoveLeft","MoveRight","MoveBack","MoveForward","LeftThumb","RightThumb","MouseFire","MouseAltFire"
};
static const int default_keys[PC_BINDING_COUNT]={
    VK_RETURN,VK_BACK,VK_SPACE,'F','R','Q','1','2','C','V','A','D','S','W',VK_SHIFT,VK_CONTROL,VK_LBUTTON,VK_RBUTTON
};
static int env_int(const char *name,int fallback)
{ const char *s=getenv(name);return s&&*s?atoi(s):fallback; }
static bool menu_available()
{ return menu_kind.load(std::memory_order_relaxed)!=0 && GetTickCount()-menu_time.load(std::memory_order_relaxed)<250; }

extern "C" void ma2_pc_input_context_update(void)
{
    pc_input_set_context(!menu_available()?PC_INPUT_GAMEPLAY:
        menu_kind.load()==0x481B70 && title_form.load()?PC_INPUT_TITLE:PC_INPUT_MENU);
}
extern "C" void ma2_pc_form_loading(uint32_t object,uint32_t filename)
{
    if(filename<0x10000 || filename>=0x04000000)return;
    const char *path=(const char *)(g_xbox_mem_offset+filename);
    if(strstr(path,"hub_start.ggf"))title_form=true;
    else if(strstr(path,"hub_main"))title_form=false;
    if(getenv("RECOMP_MENU_TRACE"))fprintf(stderr,"[PC-MENU] form owner=%08X path=%s\n",object,path);
    ma2_pc_input_context_update();
}

extern "C" void ma2_pc_menu_selection(uint32_t object,uint32_t selected,int activated)
{
    static const bool trace=getenv("RECOMP_MENU_TRACE")!=nullptr;
    static uint32_t last_object,last_selected;
    if(trace && (activated || object!=last_object || selected!=last_selected)) {
        fprintf(stderr,"[PC-MENU] selection object=%08X id=%u activated=%d\n",object,selected,activated);
        last_object=object;last_selected=selected;
    }
}

extern "C" void ma2_pc_menu_pulse(uint32_t object,uint32_t vtable)
{
    /* These are the retail hub, options and pause menu classes. Availability follows
     * the original menu lifecycle; the shortcut never steals a gameplay action. */
    if(vtable==0x481B70 || vtable==0x48AD00 || vtable==0x4827B0 || vtable==0x48B360 || vtable==0x48BF20 || vtable==0x48BE60) {
        uint32_t old=menu_kind.exchange(vtable,std::memory_order_relaxed);
        menu_time.store(GetTickCount(),std::memory_order_relaxed);
        if(old!=vtable)fprintf(stderr,"[PC-MENU] original hub/options/pause object=%08X class=%08X\n",object,vtable);
        ma2_pc_input_context_update();
    }
    static const bool trace=getenv("RECOMP_MENU_TRACE")!=nullptr;
    if(trace) {
        static uint32_t seen[96];static unsigned count;
        static DWORD reported;
        if((vtable==0x481B70 || vtable==0x48AD00 || vtable==0x4827B0) && GetTickCount()-reported>=1000) {
            reported=GetTickCount();fprintf(stderr,"[PC-MENU] pulse t=%lu object=%08X class=%08X\n",reported,object,vtable);
        }
        for(unsigned i=0;i<count;i++)if(seen[i]==vtable)return;
        if(count<96){seen[count++]=vtable;fprintf(stderr,"[PC-MENU] update object=%08X class=%08X\n",object,vtable);}
    }
}

extern "C" int ma2_pc_video_menu_update(uint32_t object)
{return ma2_pc_submenu_update(object,0x48B360);}

extern "C" void ma2_pc_submenu_destroyed(uint32_t object)
{
    if(original_submenu.load()==object)original_submenu=0;
    if(routed_video_menu.load()==object){routed_video_menu=0;routed_video_closed=false;}
}

extern "C" void ma2_pc_submenu_result(uint32_t object,int result)
{
    if(result==2 && original_submenu.load()==object) {
        original_submenu=0;
        fprintf(stderr,"[PC-MENU] returned from original game settings submenu\n");
    }
}

extern "C" int ma2_pc_submenu_update(uint32_t object,uint32_t vtable)
{
    int tab=vtable==0x48B360?0:vtable==0x48BF20?2:vtable==0x48BE60?1:-1;
    if(tab<0)return -1;
    ma2_pc_menu_pulse(object,vtable);
    if(original_submenu.load()==object)return -1;
    if(routed_video_closed.exchange(false)) {
        routed_video_menu=0;
        fprintf(stderr,"[PC-MENU] returned from native settings to original menu\n");
        return 2; /* Original menu manager's Back/completed result. */
    }
    if(routed_video_menu.exchange(object)!=object) {
        requested_tab=tab;request_open=true;
        fprintf(stderr,"[PC-MENU] original %s selected; opening native %s tab\n",
            tab==0?"Video Settings":tab==1?"Volume":"Controls",tab==0?"Display":tab==1?"Audio":"Controls");
    }
    return 0;
}

static int window_message(void *window,unsigned message,uintptr_t wparam,intptr_t lparam)
{
    if(message==PC_WINDOW_RENDERER_ERROR_MESSAGE) {
        // The render worker continues draining guest fences while the window
        // thread presents the error. Closing must not depend on another frame.
        pc_frontend_capture(0);
        wchar_t text[256];
        swprintf_s(text,L"MechAssault 2 cannot continue rendering (0x%08X).\n"
            L"The game will close. Try restarting the game.",
            (unsigned)wparam);
        if(!env_int("RECOMP_GPU_RECOVERY_TEST",0))
            MessageBoxW((HWND)window,text,L"MechAssault 2",MB_OK|MB_ICONERROR);
        PostMessageW((HWND)window,WM_CLOSE,0,0);
        return 1;
    }
    bool captured=pc_frontend_captured()!=0;
    if(captured && message==WM_CLOSE) {
        request_exit=true;return 1;
    }
    if(waiting_binding.load()>=0 && (message==WM_LBUTTONDOWN || message==WM_RBUTTONDOWN ||
       message==WM_MBUTTONDOWN || message==WM_XBUTTONDOWN)) {
        int key=message==WM_LBUTTONDOWN?VK_LBUTTON:message==WM_RBUTTONDOWN?VK_RBUTTON:
            message==WM_MBUTTONDOWN?VK_MBUTTON:GET_XBUTTON_WPARAM(wparam)==XBUTTON1?VK_XBUTTON1:VK_XBUTTON2;
        binding_key.store(key);return 1;
    }
    if(message==WM_KEYDOWN && !(lparam&(1LL<<30))) {
        if(waiting_binding.load()>=0) {
            binding_key.store((int)wparam);return 1;
        }
        if(!captured && menu_available() && wparam==(uintptr_t)pc_binding_get(PC_WHITE,'1')) {
            request_open=true;return 1;
        }
        if(captured && (wparam==VK_ESCAPE || wparam==(uintptr_t)pc_binding_get(PC_B,'F'))) {
            request_close=true;return 1;
        }
    }
    /* Queue only messages whose values outlive dispatch. WM_INPUT owns a
     * short-lived RAWINPUT handle and belongs to the game's mouse handler. */
    bool input=(message>=WM_MOUSEFIRST && message<=WM_MOUSELAST) ||
        (message>=WM_KEYFIRST && message<=WM_KEYLAST) || message==WM_SETFOCUS || message==WM_KILLFOCUS;
    if(input && (captured || menu_available())) {
        AcquireSRWLockExclusive(&messages_lock);
        if(messages.size()<1024)messages.push_back({(HWND)window,message,(WPARAM)wparam,(LPARAM)lparam});
        ReleaseSRWLockExclusive(&messages_lock);
    }
    if(!captured && menu_available() && message==WM_LBUTTONDOWN) {
        int x=GET_X_LPARAM(lparam),y=GET_Y_LPARAM(lparam);
        if(x>=anchor_left && x<anchor_right && y>=anchor_top && y<anchor_bottom) {
            request_open=true;return 1;
        }
    }
    if(captured && (input || message==WM_INPUT)) {
        /* Alt+Enter and Alt+F4 still reach our/Windows' window handlers. */
        if(message==WM_SYSKEYDOWN || message==WM_SYSKEYUP)return 0;
        return 1;
    }
    return 0;
}

static void write_int(const char *section,const char *name,int value)
{
    char text[24];snprintf(text,sizeof(text),"%d",value);
    if(!WritePrivateProfileStringA(section,name,text,ini_path)) {
        settings_write_failed=true;settings_write_error=GetLastError();
    }
}
static void save_settings(HWND window)
{
    settings_write_failed=false;settings_write_error=0;
    write_int("Display","WindowWidth",output_width);write_int("Display","WindowHeight",output_height);
    write_int("Display","Fullscreen",fullscreen);write_int("Display","VSync",pc_setting_get(PC_VSYNC,0));
    write_int("Display","FrameLimit",pc_setting_get(PC_FRAME_LIMIT,0));
    WritePrivateProfileStringA("Display","RenderScale",nullptr,ini_path);
    write_int("Display","ShowFPS",pc_setting_get(PC_SHOW_FPS,0));
    write_int("Display","PreserveAspect",pc_setting_get(PC_ASPECT,1));
    write_int("Display","SmoothScaling",pc_setting_get(PC_FILTER,1));
    write_int("Audio","Volume",pc_setting_get(PC_VOLUME,100));
    write_int("Input","Keyboard",pc_setting_get(PC_KEYBOARD,1));
    write_int("Input","MouseAim",pc_setting_get(PC_MOUSE_AIM,1));
    write_int("Input","MouseSensitivity",pc_setting_get(PC_MOUSE_SENSITIVITY,100));
    write_int("Input","InvertY",pc_setting_get(PC_INVERT_Y,0));
    for(int i=0;i<PC_BINDING_COUNT;i++)write_int("Bindings",binding_names[i],pc_binding_get((PcBinding)i,default_keys[i]));
    PostMessageA(window,PC_WINDOW_CONFIG_MESSAGE,fullscreen,MAKELPARAM(output_width,output_height));
    if(settings_write_failed)fprintf(stderr,"[PC-MENU] Cannot save settings to %s (Windows error %lu)\n",ini_path,settings_write_error);
    else fprintf(stderr,"[PC-MENU] saved display=%dx%d fullscreen=%d vsync=%d limit=%d volume=%d\n",
        output_width,output_height,fullscreen,pc_setting_get(PC_VSYNC,0),pc_setting_get(PC_FRAME_LIMIT,0),pc_setting_get(PC_VOLUME,100));
}
static void close_settings(HWND window)
{
    save_settings(window);opened=false;pc_frontend_capture(0);waiting_binding=-1;
    if(routed_video_menu.load())routed_video_closed=true;
    PostMessageA(window,PC_WINDOW_CAPTURE_MESSAGE,0,0);
    ImGui::GetIO().ClearInputKeys();
    fprintf(stderr,"[PC-MENU] closed PC settings\n");
}
static void key_name(int key,char *name,size_t size)
{
    if(key==VK_LBUTTON){strcpy_s(name,size,"Left mouse");return;}
    if(key==VK_RBUTTON){strcpy_s(name,size,"Right mouse");return;}
    if(key==VK_MBUTTON){strcpy_s(name,size,"Middle mouse");return;}
    if(key==VK_XBUTTON1){strcpy_s(name,size,"Mouse 4");return;}
    if(key==VK_XBUTTON2){strcpy_s(name,size,"Mouse 5");return;}
    UINT scan=MapVirtualKeyA((UINT)key,MAPVK_VK_TO_VSC);
    if(key>=VK_PRIOR && key<=VK_DOWN)scan|=0x100;
    if(!GetKeyNameTextA((LONG)(scan<<16),name,(int)size))snprintf(name,size,"Key %d",key);
}
static void toggle(const char *label,PcSetting setting,int fallback)
{
    bool value=pc_setting_get(setting,fallback)!=0;
    if(ImGui::Checkbox(label,&value))pc_setting_set(setting,value);
}
static bool setting_combo(const char *label,int *value,const char *const items[],int count)
{
    ImGui::TextUnformatted(label);ImGui::SetNextItemWidth(-1);ImGui::PushID(label);
    bool changed=ImGui::Combo("##value",value,items,count);ImGui::PopID();return changed;
}
static bool setting_slider(const char *label,int *value,int low,int high,const char *format)
{
    ImGui::TextUnformatted(label);ImGui::SetNextItemWidth(-1);ImGui::PushID(label);
    bool changed=ImGui::SliderInt("##value",value,low,high,format);ImGui::PopID();return changed;
}
static void defaults()
{
    pc_setting_set(PC_VSYNC,0);pc_setting_set(PC_FRAME_LIMIT,0);pc_setting_set(PC_ASPECT,1);
    pc_setting_set(PC_FILTER,1);pc_setting_set(PC_VOLUME,100);pc_setting_set(PC_MOUSE_AIM,1);
    pc_setting_set(PC_RENDER_SCALE,0);pc_setting_set(PC_SHOW_FPS,0);
    pc_setting_set(PC_MOUSE_SENSITIVITY,100);pc_setting_set(PC_INVERT_Y,0);pc_setting_set(PC_KEYBOARD,1);
    for(int i=0;i<PC_BINDING_COUNT;i++)pc_binding_set((PcBinding)i,default_keys[i]);
    output_width=1280;output_height=960;fullscreen=false;
}
static void capture_panel(ID3D11Device *device,ID3D11DeviceContext *context)
{
    static bool captured;
    static DWORD captured_at;
    static unsigned capture_index;
    const char *path=getenv("RECOMP_PC_UI_DUMP");
    unsigned interval=(unsigned)env_int("RECOMP_PC_UI_DUMP_INTERVAL_MS",0);
    bool always=env_int("RECOMP_PC_UI_DUMP_ALWAYS",0)!=0;
    if(!path || (!opened && !always) || (!always && GetTickCount()-opened_at<200) ||
       (captured && (!interval || GetTickCount()-captured_at<interval)))return;
    ID3D11RenderTargetView *view=nullptr;ID3D11Resource *resource=nullptr;
    ID3D11Texture2D *image=nullptr,*staging=nullptr;
    context->OMGetRenderTargets(1,&view,nullptr);if(!view)return;
    view->GetResource(&resource);view->Release();
    resource->QueryInterface(__uuidof(ID3D11Texture2D),(void **)&image);resource->Release();
    if(!image)return;
    D3D11_TEXTURE2D_DESC desc;image->GetDesc(&desc);
    desc.Usage=D3D11_USAGE_STAGING;desc.BindFlags=0;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;desc.MiscFlags=0;
    if(SUCCEEDED(device->CreateTexture2D(&desc,nullptr,&staging))) {
        context->CopyResource(staging,image);D3D11_MAPPED_SUBRESOURCE mapped;
        if(SUCCEEDED(context->Map(staging,0,D3D11_MAP_READ,0,&mapped))) {
            char numbered[MAX_PATH];
            if(interval){snprintf(numbered,sizeof(numbered),"%s-%04u.bmp",path,capture_index++);path=numbered;}
            FILE *file=fopen(path,"wb");
            if(file) {
                BITMAPFILEHEADER header={0};BITMAPINFOHEADER info={0};
                header.bfType=0x4D42;header.bfOffBits=sizeof(header)+sizeof(info);
                header.bfSize=header.bfOffBits+desc.Width*desc.Height*4;
                info.biSize=sizeof(info);info.biWidth=desc.Width;info.biHeight=-(LONG)desc.Height;
                info.biPlanes=1;info.biBitCount=32;info.biCompression=BI_RGB;
                fwrite(&header,1,sizeof(header),file);fwrite(&info,1,sizeof(info),file);
                for(UINT y=0;y<desc.Height;y++)fwrite((char *)mapped.pData+y*mapped.RowPitch,4,desc.Width,file);
                fclose(file);captured=true;captured_at=GetTickCount();
                fprintf(stderr,"[PC-MENU] captured native settings: %s\n",path);
            }
            context->Unmap(staging,0);
        }
        staging->Release();
    }
    image->Release();
}
static void device_lost()
{
    if(renderer_initialized) {
        ImGui_ImplDX11_Shutdown();
        renderer_initialized=false;
    }
}
static void render(void *device_ptr,void *context_ptr,void *window_ptr,unsigned width,unsigned height)
{
    bool available=menu_available();
    static unsigned fps_frames;static DWORD fps_time;static float fps;
    DWORD now=GetTickCount();fps_frames++;
    if(!fps_time)fps_time=now;
    if(now-fps_time>=500){fps=(float)fps_frames*1000/(now-fps_time);fps_frames=0;fps_time=now;}
    bool show_fps=pc_setting_get(PC_SHOW_FPS,0)!=0;
    auto device=(ID3D11Device *)device_ptr;auto context=(ID3D11DeviceContext *)context_ptr;auto window=(HWND)window_ptr;
    if(!available && !opened && !show_fps) {
        if(env_int("RECOMP_PC_UI_DUMP_ALWAYS",0))capture_panel(device,context);
        return;
    }
    if(!initialized) {
        IMGUI_CHECKVERSION();ImGui::CreateContext();
        ImGuiIO &io=ImGui::GetIO();io.IniFilename=nullptr;io.LogFilename=nullptr;
        char font[MAX_PATH];GetWindowsDirectoryA(font,MAX_PATH);strcat_s(font,"\\Fonts\\segoeui.ttf");
        ImFont *ui_font=io.Fonts->AddFontFromFileTTF(font,18.0f);
        if(ui_font)io.FontDefault=ui_font;
        ImGui::StyleColorsDark();auto &style=ImGui::GetStyle();
        style.FontSizeBase=18.0f;
        style.WindowRounding=2;style.FrameRounding=1;style.FramePadding=ImVec2(10,7);
        style.WindowPadding=ImVec2(24,20);style.ItemSpacing=ImVec2(12,10);
        style.Colors[ImGuiCol_WindowBg]=ImVec4(.065f,.09f,.085f,.97f);
        style.Colors[ImGuiCol_Button]=ImVec4(.18f,.22f,.16f,1);
        style.Colors[ImGuiCol_ButtonHovered]=ImVec4(.37f,.40f,.20f,1);
        style.Colors[ImGuiCol_ButtonActive]=ImVec4(.44f,.45f,.20f,1);
        style.Colors[ImGuiCol_CheckMark]=ImVec4(.86f,.83f,.48f,1);
        style.Colors[ImGuiCol_SliderGrab]=ImVec4(.72f,.73f,.40f,1);
        style.Colors[ImGuiCol_SliderGrabActive]=ImVec4(.86f,.83f,.48f,1);
        style.Colors[ImGuiCol_TitleBg]=style.Colors[ImGuiCol_TitleBgActive]=ImVec4(.13f,.17f,.14f,1);
        style.Colors[ImGuiCol_Tab]=ImVec4(.18f,.22f,.16f,1);
        style.Colors[ImGuiCol_TabSelected]=ImVec4(.35f,.38f,.20f,1);
        style.Colors[ImGuiCol_TabHovered]=ImVec4(.42f,.44f,.25f,1);
        style.Colors[ImGuiCol_TabSelectedOverline]=ImVec4(.86f,.83f,.48f,1);
        style.Colors[ImGuiCol_FrameBg]=ImVec4(.13f,.18f,.15f,1);
        style.Colors[ImGuiCol_FrameBgHovered]=ImVec4(.23f,.28f,.18f,1);
        style.Colors[ImGuiCol_FrameBgActive]=ImVec4(.30f,.33f,.19f,1);
        style.Colors[ImGuiCol_Header]=ImVec4(.25f,.30f,.18f,1);
        style.Colors[ImGuiCol_HeaderHovered]=ImVec4(.35f,.40f,.22f,1);
        style.Colors[ImGuiCol_HeaderActive]=ImVec4(.42f,.44f,.25f,1);
        style.Colors[ImGuiCol_NavCursor]=ImVec4(.86f,.83f,.48f,1);
        base_style=style;
        ImGui_ImplWin32_Init(window);initialized=true;
    }
    if(!renderer_initialized) {
        if(!ImGui_ImplDX11_Init(device,context))return;
        renderer_initialized=true;
    }
    float ui_scale=ImGui_ImplWin32_GetDpiScaleForHwnd(window);
    const char *scale_override=getenv("RECOMP_UI_SCALE");
    if(scale_override)ui_scale=(float)atof(scale_override);
    ui_scale=std::clamp(ui_scale,1.0f,3.0f);
    if(ui_scale!=style_scale) {
        ImGui::GetStyle()=base_style;
        ImGui::GetStyle().ScaleAllSizes(ui_scale);
        ImGui::GetStyle().FontScaleDpi=ui_scale;
        style_scale=ui_scale;
        fprintf(stderr,"[PC-MENU] display scaling: %.0f%%\n",ui_scale*100);
    }
    std::vector<WindowMessage> pending;
    AcquireSRWLockExclusive(&messages_lock);pending.swap(messages);ReleaseSRWLockExclusive(&messages_lock);
    for(const auto &m:pending)ImGui_ImplWin32_WndProcHandler(m.window,m.message,m.wparam,m.lparam);
    XINPUT_STATE pad={0};static WORD previous_buttons;
    if(XInputGetState(0,&pad)==ERROR_SUCCESS && available &&
        (pad.Gamepad.wButtons&XINPUT_GAMEPAD_RIGHT_SHOULDER) && !(previous_buttons&XINPUT_GAMEPAD_RIGHT_SHOULDER))request_open=true;
    previous_buttons=pad.Gamepad.wButtons;
    if(request_open.exchange(false) && !opened) {
        opened=true;opened_at=GetTickCount();pc_frontend_capture(1);
        fullscreen=(GetWindowLongPtrA(window,GWL_STYLE)&WS_POPUP)!=0;
        PostMessageA(window,PC_WINDOW_CAPTURE_MESSAGE,0,0);
        ImGui::GetIO().ClearInputKeys();
        fprintf(stderr,"[PC-MENU] opened PC settings from original menu class=%08X\n",menu_kind.load());
    }
    if(request_close.exchange(false) && opened)close_settings(window);
    if(request_exit.exchange(false)) {
        if(opened)close_settings(window);
        PostMessageA(window,WM_CLOSE,0,0);
        return;
    }
    ImGuiIO &io=ImGui::GetIO();
    io.ConfigFlags=opened?(ImGuiConfigFlags_NavEnableKeyboard|ImGuiConfigFlags_NavEnableGamepad):0;
    ImGui_ImplDX11_NewFrame();ImGui_ImplWin32_NewFrame();ImGui::NewFrame();
    if(opened && (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight) || ImGui::IsKeyPressed(ImGuiKey_Escape)))close_settings(window);
    if(!opened && available) {
        float button_width=std::min(260*ui_scale,(float)width-24),button_height=42*ui_scale;
        float x=std::max(12.0f,(float)width-button_width-24*ui_scale),y=24*ui_scale;
        anchor_left=(int)x;anchor_top=(int)y;anchor_right=(int)(x+button_width);anchor_bottom=(int)(y+button_height);
        ImGui::SetNextWindowPos(ImVec2(x,y));ImGui::SetNextWindowSize(ImVec2(button_width,button_height));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,ImVec2(0,0));
        ImGui::Begin("PC settings entry",nullptr,ImGuiWindowFlags_NoDecoration|ImGuiWindowFlags_NoBackground|ImGuiWindowFlags_NoSavedSettings|ImGuiWindowFlags_NoNav);
        char key[32],label[96];key_name(pc_binding_get(PC_WHITE,'1'),key,sizeof(key));
        snprintf(label,sizeof(label),"PC SETTINGS   [%s / RB]",key);
        if(ImGui::Button(label,ImVec2(button_width,button_height)))request_open=true;
        ImGui::End();ImGui::PopStyleVar();
    } else if(opened) {
        float pane_width=std::min(760*ui_scale,(float)width-24*ui_scale),pane_height=std::min(640*ui_scale,(float)height-24*ui_scale);
        ImGui::SetNextWindowPos(ImVec2(width*.5f,height*.5f),ImGuiCond_Always,ImVec2(.5f,.5f));
        ImGui::SetNextWindowSize(ImVec2(pane_width,pane_height));
        ImGui::Begin("PC SETTINGS",nullptr,ImGuiWindowFlags_NoResize|ImGuiWindowFlags_NoMove|ImGuiWindowFlags_NoCollapse|ImGuiWindowFlags_NoSavedSettings);
        ImGui::TextColored(ImVec4(.85f,.83f,.54f,1),"MECHASSAULT 2  /  PC SETTINGS");
        ImGui::Separator();
        float footer_height=ImGui::GetFrameHeightWithSpacing()+ImGui::GetTextLineHeightWithSpacing()*2;
        if(settings_write_failed)footer_height+=ImGui::GetTextLineHeightWithSpacing()*2;
        ImGui::BeginChild("Settings content",ImVec2(0,-footer_height));
        ImGui::PushItemWidth(std::min(290*ui_scale,ImGui::GetContentRegionAvail().x*.52f));
        int tab=requested_tab.exchange(-1);
        if(ImGui::BeginTabBar("Settings categories")) {
            if(ImGui::BeginTabItem("Display",nullptr,tab==0?ImGuiTabItemFlags_SetSelected:0)) {
                ImGui::Spacing();ImGui::Checkbox("Borderless fullscreen",&fullscreen);
                if(!fullscreen) {
                    const int widths[]={800,960,1280,1280,1920,2560,3840};
                    const int heights[]={600,720,960,720,1080,1440,2160};
                    const char *labels[]={"800 x 600","960 x 720","1280 x 960","1280 x 720","1920 x 1080","2560 x 1440","3840 x 2160"};
                    int choice=1;for(int i=0;i<7;i++)if(output_width==widths[i] && output_height==heights[i])choice=i;
                    if(setting_combo("Window resolution",&choice,labels,7)){output_width=widths[choice];output_height=heights[choice];}
                } else { MONITORINFO mi={sizeof(mi)};GetMonitorInfoA(MonitorFromWindow(window,MONITOR_DEFAULTTONEAREST),&mi);
                    ImGui::Text("Display resolution: %ld x %ld",mi.rcMonitor.right-mi.rcMonitor.left,mi.rcMonitor.bottom-mi.rcMonitor.top); }
                toggle("Vertical sync",PC_VSYNC,0);
                const int limits[]={0,30,60,120,144,165,240};const char *names[]={"Unlimited","30 FPS","60 FPS","120 FPS","144 FPS","165 FPS","240 FPS"};
                int limit=pc_setting_get(PC_FRAME_LIMIT,0),choice=0;for(int i=0;i<7;i++)if(limit==limits[i])choice=i;
                if(setting_combo("Frame limit",&choice,names,7))pc_setting_set(PC_FRAME_LIMIT,limits[choice]);
                toggle("Preserve aspect ratio",PC_ASPECT,1);toggle("Smooth image scaling",PC_FILTER,1);
                toggle("Show frame rate",PC_SHOW_FPS,0);
                ImGui::EndTabItem();
            }
            if(ImGui::BeginTabItem("Audio",nullptr,tab==1?ImGuiTabItemFlags_SetSelected:0)) {
                ImGui::Spacing();int volume=pc_setting_get(PC_VOLUME,100);
                if(setting_slider("Master volume",&volume,0,100,"%d%%"))pc_setting_set(PC_VOLUME,volume);
                if(menu_kind.load()==0x48BE60 && routed_video_menu.load()) {
                    if(ImGui::Button("Music, dialogue and effects")) {
                        original_submenu=routed_video_menu.load();routed_video_menu=0;
                        close_settings(window);
                    }
                } else ImGui::TextWrapped("Music, dialogue and effects levels are available in the game's Volume menu.");
                ImGui::EndTabItem();
            }
            if(ImGui::BeginTabItem("Controls",nullptr,tab==2?ImGuiTabItemFlags_SetSelected:0)) {
                ImGui::Spacing();toggle("Keyboard and mouse input",PC_KEYBOARD,1);toggle("Mouse aiming",PC_MOUSE_AIM,1);
                int sensitivity=pc_setting_get(PC_MOUSE_SENSITIVITY,100);
                if(setting_slider("Mouse sensitivity",&sensitivity,1,500,"%d%%"))pc_setting_set(PC_MOUSE_SENSITIVITY,sensitivity);
                toggle("Invert mouse Y",PC_INVERT_Y,0);
                ImGui::TextWrapped("XInput controllers work automatically. F8 releases mouse capture; Alt+Enter switches fullscreen.");
                if(menu_kind.load()==0x48BF20 && routed_video_menu.load() &&
                   ImGui::Button("Game controller layout")) {
                    original_submenu=routed_video_menu.load();routed_video_menu=0;
                    close_settings(window);
                }
                ImGui::Separator();int key=binding_key.exchange(0),waiting=waiting_binding.load();
                if(waiting>=0 && key){if(key!=VK_ESCAPE && key!=VK_F8 && key!=VK_F11)pc_binding_set((PcBinding)waiting,key);waiting_binding=-1;}
                ImGui::TextDisabled("Select a binding, then press a key or mouse button.");
                for(int i=0;i<PC_BINDING_COUNT;i++) {
                    ImGui::PushID(i);char name[48];key_name(pc_binding_get((PcBinding)i,default_keys[i]),name,sizeof(name));
                    float bind_width=std::min(190*ui_scale,ImGui::GetContentRegionAvail().x*.46f);
                    ImGui::TextUnformatted(action_names[i]);
                    ImGui::SameLine(ImGui::GetWindowContentRegionMax().x-bind_width);
                    if(ImGui::Button(waiting_binding==i?"Press a key...":name,ImVec2(bind_width,0)))waiting_binding=i;
                    ImGui::PopID();
                }
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
        ImGui::PopItemWidth();ImGui::EndChild();ImGui::Separator();
        float footer_width=ImGui::GetContentRegionAvail().x-ImGui::GetStyle().ItemSpacing.x*2;
        if(ImGui::Button("Apply",ImVec2(footer_width*.37f,0)))save_settings(window);
        ImGui::SameLine();if(ImGui::Button("Defaults",ImVec2(footer_width*.37f,0)))defaults();
        ImGui::SameLine();if(ImGui::Button("Back",ImVec2(footer_width*.26f,0)))close_settings(window);
        ImGui::TextWrapped("Arrows / D-pad: navigate   Enter / A: select   Esc / B: back");
        if(settings_write_failed) {
            ImGui::PushStyleColor(ImGuiCol_Text,ImVec4(1,.55f,.35f,1));
            ImGui::TextWrapped("Could not save settings (Windows error %lu).",settings_write_error);ImGui::PopStyleColor();
        }
        ImGui::End();
    }
    if(show_fps && !opened) {
        ImGui::SetNextWindowPos(ImVec2(18*ui_scale,18*ui_scale));ImGui::SetNextWindowBgAlpha(.6f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,ImVec2(10*ui_scale,5*ui_scale));
        ImGui::Begin("Frame rate",nullptr,ImGuiWindowFlags_NoDecoration|ImGuiWindowFlags_AlwaysAutoResize|
            ImGuiWindowFlags_NoInputs|ImGuiWindowFlags_NoSavedSettings|ImGuiWindowFlags_NoNav);
        ImGui::Text("%.0f FPS",fps);ImGui::End();ImGui::PopStyleVar();
    }
    ImGui::Render();ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());capture_panel(device,context);
}
extern "C" void ma2_pc_settings_initialize(const char *ini)
{
    GetFullPathNameA(ini,MAX_PATH,ini_path,nullptr);
    output_width=env_int("RECOMP_WINDOW_WIDTH",1280);output_height=env_int("RECOMP_WINDOW_HEIGHT",960);
    const char *env_names[PC_SETTING_COUNT]={"RECOMP_VSYNC","RECOMP_FRAME_LIMIT","RECOMP_PRESERVE_ASPECT","RECOMP_SCALE_FILTER","RECOMP_AUDIO_VOLUME",
        "RECOMP_MOUSE_AIM","RECOMP_MOUSE_SENSITIVITY","RECOMP_INVERT_Y","RECOMP_KEYBOARD","RECOMP_RENDER_SCALE","RECOMP_SHOW_FPS","RECOMP_DRAW_DISTANCE","RECOMP_TEXTURE_QUALITY"};
    const int values[PC_SETTING_COUNT]={0,0,1,1,100,1,100,0,1,2,0,50,0};
    for(int i=0;i<PC_SETTING_COUNT;i++)pc_setting_set((PcSetting)i,env_int(env_names[i],values[i]));
    for(int i=0;i<PC_BINDING_COUNT;i++) {
        int key=(int)GetPrivateProfileIntA("Bindings",binding_names[i],default_keys[i],ini_path);
        pc_binding_set((PcBinding)i,key>0 && key<256?key:default_keys[i]);
    }
    pc_frontend_register(render,window_message);
    pc_frontend_register_device_lost(device_lost);
}
