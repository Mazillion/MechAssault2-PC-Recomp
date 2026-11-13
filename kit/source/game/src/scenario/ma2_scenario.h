/* Custom scenarios (.ma2map), see ma2_scenario.c and docs/editor.md. */
#ifndef MA2_SCENARIO_H
#define MA2_SCENARIO_H
#include <stdint.h>
#include <wchar.h>
#ifdef __cplusplus
extern "C" {
#endif
void     ma2_scn_mount(uint32_t fs);              /* recomp_0000.c, after the Common.mgf mount */
void     ma2_scn_resource_path(uint32_t buf);     /* recomp_0037.c, level resource "<root><name>.mgf" */
void     ma2_scn_level_load(void);                /* recomp_0000.c, end of the level mount (loc_000138A0) */
void     ma2_scn_level_unload(void);              /* recomp_0000.c, after the level unmount (loc_000178C9) */
uint32_t ma2_scn_text(uint32_t id, uint32_t original);   /* pc_prompts.cpp, ids 0x00F00000..0x00FFFFFF */
int      ma2_scn_count(void);
int      ma2_scn_mission_count(void);             /* custom missions found at boot */
int      ma2_scn_custom_active(void);
int      ma2_scn_level_custom(void);              /* the level being played is a scenario (.ma2map) */
int      ma2_scn_custom_enter(void);              /* recomp_0051.c, hub action 8 (Custom Scenarios); 0 = no picker */
void     ma2_scn_custom_leave(void);
void     ma2_scn_hub_action(uint32_t action);     /* recomp_0051.c, hub item activation */
/* The Scenario Editor opened from Custom Scenarios (pc_native_settings.cpp button, E, pad Y) */
int      ma2_scn_editor_available(void);          /* MA2 Scenario Editor.exe next to the game */
int      ma2_scn_editor_running(void);
void     ma2_scn_editor_open(void);               /* any thread; acted on by the next picker frame */
const wchar_t *ma2_scn_editor_note(void);         /* a note to show over the menu, or NULL */
#ifdef __cplusplus
}
#endif
#endif
