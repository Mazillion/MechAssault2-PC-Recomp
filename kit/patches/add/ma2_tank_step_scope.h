#ifndef MA2_TANK_STEP_SCOPE_H
#define MA2_TANK_STEP_SCOPE_H
/* Process clock bytes stay in guest memory. Only this thread's translated
 * memory accessors see a temporary per-tick clock. Native wall clocks and
 * direct guest mappings are intentionally not changed. */
typedef struct Ma2TankStepScope {
    unsigned active;
    union { double aligned[6]; unsigned char bytes[48]; } clock;
} Ma2TankStepScope;
extern RECOMP_TLS Ma2TankStepScope ma2_tank_step_scope;
/* T1-2: with FastCode on, the tank step swaps the 48 clock bytes in guest
 * memory (fast copies use the plain pointer) and leaves .active at 0, so
 * prebuilt objects that inline the accessor above read guest memory too.
 * Logic that asks "inside a tank step?" must test both flags. */
extern RECOMP_TLS unsigned ma2_tank_step_swapped;
static __forceinline uintptr_t ma2_tank_scoped_pointer(uint32_t address) {
    if (address >= 0x4E2420u && address < 0x4E2448u && ma2_tank_step_scope.active)
        return (uintptr_t)(ma2_tank_step_scope.clock.bytes + address - 0x4E2420u);
    return (uintptr_t)address + g_xbox_mem_offset;
}
void ma2_tank_fixed_update(void (*body)(void));
void ma2_tank_fixed_forget(uint32_t actor);
void ma2_tank_fixed_reset(void);
void ma2_tank_fixed_input_select(uint32_t object);
int ma2_tank_response_tick(uint32_t key,uint32_t vt);
int ma2_tank_contact_begin(uint32_t root);
void ma2_tank_contact_end(void);
#endif
