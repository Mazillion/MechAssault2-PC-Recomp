#pragma once
#include <stdint.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
void ma2_pc_settings_initialize(const char *ini);
void ma2_pc_menu_pulse(uint32_t object, uint32_t vtable);
void ma2_pc_form_loading(uint32_t object,uint32_t filename);
void ma2_pc_input_context_update(void);
void ma2_pc_modal_pulse(void);
void ma2_pc_menu_selection(uint32_t object, uint32_t selected, int activated);
int ma2_pc_video_menu_update(uint32_t object);
int ma2_pc_submenu_update(uint32_t object,uint32_t vtable);
void ma2_pc_submenu_destroyed(uint32_t object);
void ma2_pc_submenu_result(uint32_t object,int result);
uint32_t ma2_gui_find(uint32_t form,uint32_t action);
void ma2_barrel_arrows_pulse(uint32_t object,int ws_mode);
void ma2_barrel_offset(int dx);   /* Custom Scenarios: barrels move right for the Editor button */
int ma2_barrel_arrow_click(uint32_t object,int ws_mode,float nx,float ny,float *row_x,float *row_y);
int ma2_barrel_row_hit(uint32_t object,int ws_mode,float nx,float ny);
uint32_t ma2_gui_footer_at(uint32_t owner,float x,float y,float aspect,float rect[4]);
void ma2_gui_dump(uint32_t owner,const char *why);
void ma2_menu_policy_pulse(uint32_t object,uint32_t vtable);
void ma2_gui_text(uint32_t widget,uint32_t text);
void ma2_gui_slider(uint32_t widget,float value);
uint32_t ma2_gui_dialog(uint32_t title,uint32_t message);
uint32_t ma2_gui_dialog_result(uint32_t dialog);
uint32_t ma2_pc_wide(const wchar_t *text);
uint32_t ma2_pc_localized(uint32_t id,uint32_t original);
int  ma2_pc_ini_get_int(const char *section,const char *key,int fallback);
void ma2_pc_ini_set_int(const char *section,const char *key,int value);
uint32_t ma2_bots_localized(uint32_t id,uint32_t original);
uint32_t ma2_pc_prompt_text(uint32_t original);
uint32_t ma2_pc_page_text(uint32_t id);
void ma2_gui_volume(uint32_t object,unsigned channel,unsigned value);
void ma2_gui_volume_commit(uint32_t object);
int ma2_gui_controller_option(uint32_t object,unsigned option);
void ma2_gui_controller_set(uint32_t object,unsigned option,int enabled);
void ma2_gui_controller_commit(uint32_t object);
uint32_t ma2_gui_hover(uint32_t owner,float x,float y);
uint32_t ma2_gui_dialog_pointer(uint32_t dialog,float x,float y);
/* Guest-thread-only helpers. The modal record is the session identity. */
uint32_t ma2_gui_keyboard_dialog(uint32_t *object);
int ma2_gui_keyboard_action(uint32_t record,uint32_t object,uint32_t code,int typed);
uint32_t ma2_gui_keyboard_pointer(uint32_t record,uint32_t object,float x,float y,int activate);
void ma2_gui_inspect(uint32_t form);
/* Private QTE candidate declarations. */
typedef struct Ma2QteCaption {
    uint32_t label,alternative,legend,revision;
    uint16_t advance,legend_advance;
} Ma2QteCaption;
int ma2_pc_qte_caption(unsigned mask,Ma2QteCaption *out);
int ma2_gui_qte_draw(uint32_t widget);
#ifdef __cplusplus
}
#endif
