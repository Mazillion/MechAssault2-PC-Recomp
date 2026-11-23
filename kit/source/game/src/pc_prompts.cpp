#include "pc_settings.h"
#include "platform/pc_frontend.h"
#include "ma2_startup_policy.h"
#include <windows.h>
#include <string>
#include <unordered_map>
#include <mutex>
#include <cwctype>
extern "C" ptrdiff_t g_xbox_mem_offset;
extern "C" uint32_t xbox_HeapAlloc(uint32_t,uint32_t);

static const int defaults[PC_BINDING_COUNT]={VK_RETURN,VK_BACK,VK_SPACE,'F','R','Q','1','2','C','V','A','D','S','W',VK_SHIFT,VK_CONTROL,VK_LBUTTON,VK_RBUTTON};
extern "C" uint32_t ma2_pc_wide(const wchar_t *text)
{
    static std::mutex lock;
    static std::unordered_map<std::wstring,uint32_t> strings;
    std::lock_guard<std::mutex> guard(lock);
    auto it=strings.find(text);if(it!=strings.end())return it->second;
    size_t bytes=(wcslen(text)+1)*sizeof(wchar_t);
    uint32_t address=xbox_HeapAlloc((uint32_t)bytes,16);
    if(!address)return 0;
    memcpy((void *)(g_xbox_mem_offset+address),text,bytes);
    strings.emplace(text,address);return address;
}
static std::wstring key_name(PcBinding action)
{
    int key=pc_binding_get(action,defaults[action]);
    if(key==VK_LBUTTON)return L"Left mouse";
    if(key==VK_RBUTTON)return L"Right mouse";
    if(key==VK_MBUTTON)return L"Middle mouse";
    if(key==VK_XBUTTON1)return L"Mouse 4";
    if(key==VK_XBUTTON2)return L"Mouse 5";
    if(key==VK_RETURN)return L"Enter";
    if(key==VK_ESCAPE)return L"Esc";
    wchar_t name[48];UINT scan=ma2_key_name_scan(key);
    if(!GetKeyNameTextW(scan<<16,name,48))swprintf_s(name,L"Key %u",key);
    return name;
}
static bool word_char(wchar_t c)
{
    WORD type=0;
    return c==L'_' || (GetStringTypeW(CT_CTYPE1,&c,1,&type) && (type&(C1_ALPHA|C1_DIGIT)));
}
static std::wstring button_label(PcBinding action,std::wstring following)
{
    /* A tutorial viewed from Pause still describes its gameplay binding.
     * Only explicit menu commands use the fixed menu confirm/cancel keys. */
    for(auto &c:following)c=(wchar_t)towlower(c);
    size_t first=following.find_first_not_of(L" \t\r\n");
    if(first!=std::wstring::npos)following.erase(0,first);
    if(action==PC_START && (pc_input_context()==PC_INPUT_GAMEPLAY ||
        following.compare(0,31,L"to review the current objective")==0 ||
        following.compare(0,25,L"to view mission objective")==0))return L"Esc";
    if(pc_input_context()==PC_INPUT_MENU && (action==PC_A || action==PC_B)) {
        static const wchar_t *commands[]={L"to continue",L"to confirm",L"to select mission",L"to join",
            L"to update",L"to try again",L"to read this message",L"to start the troubleshooter",L"to cancel"};
        for(const wchar_t *command:commands) {
            size_t n=wcslen(command);
            if(following.compare(0,n,command)==0 && (n==following.size() || !word_char(following[n])))
                return action==PC_A?L"Enter":L"Esc";
        }
    }
    return key_name(action);
}
static void replace(std::wstring &text,const wchar_t *phrase,const std::wstring &replacement,bool bounded=false,
                    bool preserve_case=false,PcBinding context_button=PC_BINDING_COUNT)
{
    std::wstring lower=text,needle=phrase;
    for(auto &c:lower)c=(wchar_t)towlower(c);
    for(auto &c:needle)c=(wchar_t)towlower(c);
    size_t from=0;
    while((from=lower.find(needle,from))!=std::wstring::npos) {
        if(bounded && ((from && word_char(lower[from-1])) ||
            (from+needle.size()<lower.size() && word_char(lower[from+needle.size()])))) {
            from+=needle.size();continue;
        }
        std::wstring value=context_button==PC_BINDING_COUNT?replacement:button_label(context_button,lower.substr(from+needle.size()));
        if(context_button==PC_START && needle==L"press start")value=L"Press "+value;
        if(preserve_case && !value.empty() && iswlower(text[from]))value[0]=(wchar_t)towlower(value[0]);
        text.replace(from,needle.size(),value);
        for(auto &c:value)c=(wchar_t)towlower(c);
        lower.replace(from,needle.size(),value);from+=value.size();
    }
}
static void replace_bare_buttons(std::wstring &text)
{
    std::wstring lower=text;
    for(auto &c:lower)c=(wchar_t)towlower(c);
    size_t from=0;
    while((from=lower.find(L"press",from))!=std::wstring::npos) {
        size_t start=from;from+=5;
        if((start && word_char(lower[start-1])) || from==lower.size() || !iswspace(lower[from]))continue;
        size_t button=from;
        while(button<lower.size() && iswspace(lower[button]))button++;
        /* Consume the complete authored whitespace-separated code, not just
         * its first letter, retaining the existing standalone-token boundary. */
        while(button<lower.size()) {
            if(button+1<lower.size() && word_char(lower[button+1]))break;
            PcBinding action=PC_BINDING_COUNT;
            if(!iswupper(text[button]))break; /* W-05: real button letters are upper case ("press a key" is not A) */
            switch(lower[button]) {
                case L'a':action=PC_A;break;case L'b':action=PC_B;break;
                case L'x':action=PC_X;break;case L'y':action=PC_Y;break;
                default:break;
            }
            if(action==PC_BINDING_COUNT)break;
            size_t next=button+1;
            while(next<lower.size() && iswspace(lower[next]))next++;
            // Full button prose is handled by the existing tokens below.
            if(next>button+1 && lower.compare(next,6,L"button")==0 &&
                (next+6==lower.size() || !word_char(lower[next+6])))break;
            std::wstring label=button_label(action,lower.substr(next));
            text.replace(button,1,label);lower.replace(button,1,label);
            from=button+label.size();
            button=from;
            while(button<lower.size() && iswspace(lower[button]))button++;
            if(button==from)break;
            /* An authored dialog can omit the second Press: "Press A to
             * update or B to cancel". Stay within this instruction clause,
             * and accept only the explicit standalone cancel alternative. */
            size_t cancel=lower.find(L" or b to cancel",button);
            size_t end=lower.find_first_of(L".!?;\r\n",button);
            if(cancel!=std::wstring::npos && (end==std::wstring::npos || cancel<end) &&
                (cancel+15==lower.size() || !word_char(lower[cancel+15]))) {
                std::wstring value=button_label(PC_B,lower.substr(cancel+6));
                text.replace(cancel+4,1,value);lower.replace(cancel+4,1,value);
            }
        }
    }
}
extern "C" uint32_t ma2_pc_prompt_text(uint32_t original)
{
    if(original<0x10000 || original>=0x04000000)return original;
    const wchar_t *p=(const wchar_t *)(g_xbox_mem_offset+original);
    size_t limit=(0x04000000-original)/sizeof(wchar_t);
    if(limit>2048)limit=2048;
    size_t length=wcsnlen(p,limit);if(length==limit)return original;
    std::wstring text(p,length),before=text;
    struct PromptCache {
        unsigned revision;
        HKL layout;
        std::unordered_map<std::wstring,uint32_t> results;
    };
    static thread_local PromptCache cache={};
    unsigned revision=pc_input_revision();
    HKL layout=GetKeyboardLayout(0);
    if(cache.revision!=revision || cache.layout!=layout) {
        cache.results.clear();cache.revision=revision;cache.layout=layout;
    }
    auto previous=cache.results.find(text);
    if(previous!=cache.results.end())return previous->second?previous->second:original;
    bool pad=pc_input_device()==PC_INPUT_GAMEPAD;
    /* Add Guest (split screen): the new player joins on a controller, never on the keyboard, so
     * "Press START on the new player's controller" keeps the pad's name for the button (Menu). */
    if(text.find(L"new player's controller")!=std::wstring::npos)pad=true;
    if(!pad) {
        replace_bare_buttons(text);
        /* Pull describes a trigger action, not arbitrary lore prose. */
        replace(text,L"Pull the Right trigger",L"Press "+key_name(PC_MOUSE_FIRE),true,true);
        replace(text,L"Pull Right trigger",L"Press "+key_name(PC_MOUSE_FIRE),true,true);
        replace(text,L"Pull the Left trigger",L"Press "+key_name(PC_MOUSE_ALT_FIRE),true,true);
        replace(text,L"Pull Left trigger",L"Press "+key_name(PC_MOUSE_ALT_FIRE),true,true);
    }
    struct Token {const wchar_t *text;PcBinding action;const wchar_t *pad;};
    static const Token tokens[]={
        {L"the White button",PC_WHITE,L"RB"},{L"White button",PC_WHITE,L"RB"},
        {L"the Black button",PC_BLACK,L"LB"},{L"Black button",PC_BLACK,L"LB"},
        {L"the Right trigger",PC_MOUSE_FIRE,L"RT"},{L"Right trigger",PC_MOUSE_FIRE,L"RT"},
        {L"the Left trigger",PC_MOUSE_ALT_FIRE,L"LT"},{L"Left trigger",PC_MOUSE_ALT_FIRE,L"LT"},
        {L"the A button",PC_A,L"A"},{L"A button",PC_A,L"A"},
        {L"the B button",PC_B,L"B"},{L"B button",PC_B,L"B"},
        {L"the X button",PC_X,L"X"},{L"X button",PC_X,L"X"},
        {L"the Y button",PC_Y,L"Y"},{L"Y button",PC_Y,L"Y"},
        {L"the START button",PC_START,L"Menu"},{L"START button",PC_START,L"Menu"},
        {L"Press START",PC_START,L"Menu"}};
    for(const auto &t:tokens) {
        std::wstring label=pad?t.pad:key_name(t.action);
        if(!wcscmp(t.text,L"Press START"))label=L"Press "+label;
        replace(text,t.text,label,true,!wcscmp(t.text,L"Press START"),
            !pad && (t.action==PC_A || t.action==PC_B || t.action==PC_START)?t.action:PC_BINDING_COUNT);
    }
    if(!pad) {
        /* Stick clicks are buttons; resolve them before generic stick movement. */
        const std::wstring left_click=L"Press "+key_name(PC_LEFT_THUMB);
        const std::wstring right_click=L"Press "+key_name(PC_RIGHT_THUMB);
        replace(text,L"Click the Left thumbstick",left_click,true,true);
        replace(text,L"Click Left thumbstick",left_click,true,true);
        replace(text,L"Click the Right thumbstick",right_click,true,true);
        replace(text,L"Click Right thumbstick",right_click,true,true);
        replace(text,L"Press the Left thumbstick",left_click,true,true);
        replace(text,L"Press Left thumbstick",left_click,true,true);
        replace(text,L"Press the Right thumbstick",right_click,true,true);
        replace(text,L"Press Right thumbstick",right_click,true,true);
        static const struct {const wchar_t *word;PcBinding action;} directions[]={
            {L"up",PC_MOVE_FORWARD},{L"down",PC_MOVE_BACK},{L"left",PC_MOVE_LEFT},{L"right",PC_MOVE_RIGHT}};
        for(const auto &direction:directions) {
            std::wstring label=L"Press "+key_name(direction.action);
            replace(text,(std::wstring(L"Move the Left thumbstick ")+direction.word).c_str(),label,true,true);
            replace(text,(std::wstring(L"Move Left thumbstick ")+direction.word).c_str(),label,true,true);
        }
        replace(text,L"D-pad",L"arrow keys",true);
        replace(text,L"left thumbstick",key_name(PC_MOVE_FORWARD)+L" / "+key_name(PC_MOVE_LEFT)+L" / "+key_name(PC_MOVE_BACK)+L" / "+key_name(PC_MOVE_RIGHT));
        replace(text,L"right thumbstick",L"mouse");
    }
    uint32_t result=before==text?0:ma2_pc_wide(text.c_str());
    /* Match by text rather than guest address: temporary strings and level
     * reloads reuse addresses. A zero entry means return this caller's own
     * unchanged pointer. Input revisions keep live tutorials responsive. */
    if(cache.results.size()>=128)cache.results.clear();
    cache.results.emplace(std::move(before),result);
    return result?result:original;
}
extern "C" uint32_t ma2_scn_text(uint32_t id,uint32_t original); /* scenario/ma2_scenario.c */
extern "C" int ma2_scn_editor_test_level(void);                  /* scenario/ma2_scenario.c */
extern "C" uint32_t ma2_pc_localized(uint32_t id,uint32_t original)
{
    if(id>=0x00F00000 && id<0x01000000)return ma2_scn_text(id,original);
    if(id>=0x70020000 && id<0x70030000)return ma2_bots_localized(id,original);
    if(id>=0x70000000 && id<0x70001000)return ma2_pc_page_text(id);
    bool pad=pc_input_device()==PC_INPUT_GAMEPAD;
    if(id==94)return ma2_pc_wide(pad?L"Press Menu to start":L"Press Enter or click to start");
    if(id>=0x70010000&&id<0x70010006) {
        static const wchar_t *pad_names[]={L"A",L"B",L"X",L"Y",L"RB",L"LB"};
        static const PcBinding binding[]={PC_A,PC_B,PC_X,PC_Y,PC_WHITE,PC_BLACK};
        unsigned n=id-0x70010000;
        if(pad)return ma2_pc_wide(pad_names[n]);
        std::wstring label=n==0?L"Enter":n==1?L"Esc":key_name(binding[n]);
        int key=pc_binding_get(binding[n],defaults[binding[n]]);
        if(n>1)switch(key){
            case VK_LBUTTON:label=L"LMB";break;case VK_RBUTTON:label=L"RMB";break;
            case VK_MBUTTON:label=L"MMB";break;case VK_XBUTTON1:label=L"M4";break;case VK_XBUTTON2:label=L"M5";break;
            case VK_BACK:label=L"Bksp";break;case VK_CAPITAL:label=L"Caps";break;
            case VK_PRIOR:label=L"PgUp";break;case VK_NEXT:label=L"PgDn";break;
            case VK_INSERT:label=L"Ins";break;case VK_DELETE:label=L"Del";break;
            case VK_SCROLL:label=L"ScrLk";break;case VK_SNAPSHOT:label=L"PrtSc";break;
        }
        /* W-17: the footer draws the key right before its caption ("1 Player Options"); brackets set it
         * apart. The glyph slot is narrow: "[Enter]" ran into the caption before it ("Back[Enter]", seen live),
         * so only names of up to 4 characters get brackets. */
        if(label.size()<=4)label=L"["+label+L"]";
        return ma2_pc_wide(label.c_str());
    }
    // W-04: the one working Multiplayer entry is the offline bot match. Compare with the English original so
    // other languages keep their own text.
    if(id==0x7CEB&&original&&!wcscmp((const wchar_t*)(g_xbox_mem_offset+original),L"LOCAL XBOX CONSOLE"))return ma2_pc_wide(L"BOT MATCH");
    if(id==0x92D&&original&&!wcsncmp((const wchar_t*)(g_xbox_mem_offset+original),L"Go head-to-head",15))
        return ma2_pc_wide(L"Play offline against computer-controlled pilots.");
    // Playtest 11-17: in a mission or match started by the Scenario Editor's Test, the pause menu's "Quit to Main Hub"
    // (text 0xE) goes back to the editor, so it says so. Retail and Custom Scenarios played from the menu keep the game's text.
    if(id==0xE&&ma2_scn_editor_test_level())return ma2_pc_wide(L"Return to Editor");
    // Keep the stored/localized message intact. Resolve its prompts when it
    // is measured/drawn so an already-visible tutorial can change devices.
    return original;
}

static const signed char qte_font10_advances[128]={-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,11,4,4,5,-1,7,10,8,3,4,4,5,7,4,4,4,4,7,7,7,7,7,7,7,7,7,7,4,4,7,-1,7,7,11,8,8,8,8,8,7,9,8,4,6,8,7,9,8,9,8,9,8,8,7,8,8,10,8,8,7,4,4,4,-1,6,-1,7,7,6,7,7,4,7,7,4,4,6,4,10,7,7,7,7,4,6,4,7,6,8,6,6,6,-1,-1,-1,-1,-1};
/* Private staged candidate. Original font10 advance table is appended by stage_candidate.py. */
static int qte_advance(const std::wstring &text)
{
    int sum=0;
    for(wchar_t c:text) {
        if(c>=128 || qte_font10_advances[c]<0)return -1;
        sum+=qte_font10_advances[c];
    }
    return sum;
}
static std::wstring qte_key(PcBinding action)
{
    int key=pc_binding_get(action,defaults[action]);
    switch(key) {
    case VK_LBUTTON:return L"M1";case VK_RBUTTON:return L"M2";
    case VK_MBUTTON:return L"M3";case VK_XBUTTON1:return L"M4";case VK_XBUTTON2:return L"M5";
    case VK_SPACE:return L"Spc";case VK_RETURN:return L"Ent";case VK_BACK:return L"Bksp";
    case VK_CONTROL:return L"Ctrl";case VK_ESCAPE:return L"Esc";case VK_PRIOR:return L"PgU";
    case VK_NEXT:return L"PgD";case VK_CAPITAL:return L"Caps";case VK_SNAPSHOT:return L"Prt";
    case VK_SCROLL:return L"Scr";case VK_INSERT:return L"Ins";case VK_DELETE:return L"Del";
    default:return key_name(action);
    }
}
extern "C" int ma2_pc_qte_caption(unsigned mask,Ma2QteCaption *out)
{
    if(!out)return 0;
    *out={};
    if(pc_input_device()==PC_INPUT_GAMEPAD)return 0;
    PcBinding primary,secondary=PC_A;bool alternate=false;
    switch(mask) {
    case 4:primary=PC_A;break;case 1:primary=PC_X;break;
    case 2:primary=PC_Y;break;case 0x40:primary=PC_B;break;
    case 0x10:primary=PC_MOUSE_ALT_FIRE;secondary=PC_LEFT_TRIGGER;alternate=true;break;
    case 0x20:primary=PC_MOUSE_FIRE;secondary=PC_RIGHT_TRIGGER;alternate=true;break;
    default:return 0;
    }
    std::wstring label=qte_key(primary),legend,alternative;
    int advance=qte_advance(label);
    if(advance<0||advance>28)return 0;
    if(alternate && pc_binding_get(primary,defaults[primary])!=pc_binding_get(secondary,defaults[secondary])) {
        alternative=qte_key(secondary);
        legend=key_name(primary)+L" / "+key_name(secondary);
        int full=qte_advance(legend);
        if(qte_advance(alternative)<0||full<0||full>108)return 0;
        out->legend_advance=(uint16_t)full;
        out->alternative=ma2_pc_wide(alternative.c_str());
        out->legend=ma2_pc_wide(legend.c_str());
        if(!out->alternative||!out->legend){*out={};return 0;}
    }
    out->label=ma2_pc_wide(label.c_str());out->advance=(uint16_t)advance;
    out->revision=pc_input_revision();
    if(!out->label){*out={};return 0;}
    return 1;
}
