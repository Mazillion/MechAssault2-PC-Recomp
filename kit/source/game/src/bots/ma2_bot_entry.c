/* MechAssault 2 offline bots - vehicle entry ("Press Y to enter") zones.
 *
 * NOT a standalone translation unit: #include it from ma2_bots_core.c after
 * the helpers it uses (gva_ok, vcall0, class_id, class_id_of, is_kind,
 * uid_lookup, actor_pos, read_vec3) and after Ma2Vec3 (ma2_bot_api.h).
 *
 * How the game defines the zones (ma2.xbe, all offsets verified in code):
 *   - Every enterable vehicle owns 1..3 child entities of class MACrateGenerator
 *     ("gen", id at MEM32(0x4EB188); ctor sub_152C6C; the name is misleading,
 *     it is the entry-point definition).  Per gen it stores three (radius,
 *     offset) sets: +0x90/+0x94, +0xA4/+0xA8, +0xB4/+0xB8 (offset = vehicle
 *     LOCAL x,y,z).  sub_154CAD picks one: set B (+0xA4) normally, set C
 *     (+0xB4) for a MAMech whose flags (veh+0x26C) bit 11, 12 or 13 is set.
 *   - sub_154AD8 (eax=gen, from sub_EB188 which the vehicle calls when it
 *     becomes free) sets gen+0x88 = vehicle UID and sub_154C75/154CAD create
 *       zone = MATriggerPoint (id at MEM32(0x4EB0E4)), gen+0x7C = zone UID,
 *              zone+0xC0 = vehicle UID, zone+0xD4 = radius,
 *              zone+0xC8..0xD0 = local offset,
 *              zone+0xB0..0xB8 = WORLD centre (rewritten every tick by
 *              sub_145D77/145A14: vehicle position + offset rotated by the
 *              vehicle body's euler matrix; no scene node involved),
 *       and under it the use-trigger (vtable 0x479FC0, id MEM32(0x4EBAE8),
 *       +0x68 = gen UID).
 *   - Each tick the zone tests every MACorporeal in its list (sub_146xxx):
 *       hypot(dx, dz) <= zone+0xD4  (HORIZONTAL distance; height ignored)
 *     against the entity position (sub_DD8B9), then fires the use-trigger's
 *     slot 0x58 (sub_1999C6) -> sub_154678(gen) -> sub_C86EC offers the
 *     candidate to the player (nearest wins).  Anything in the circle works.
 *   - sub_154678 additionally requires: vehicle isA MACorporeal, vehicle
 *     +0x84 bit 9 (enterable), and unless gen+0xD4 bit 7 ("allow enemy") the
 *     vehicle nation (vt+0xA8) is neutral(0/6) or equals the player's.
 */
typedef struct Ma2EntryZone {
    Ma2Vec3  center;     /* world-space circle centre (y = zone height, ignored by the game) */
    float    radius;     /* horizontal radius */
    uint32_t gen;        /* MACrateGenerator child (entry definition) */
    uint32_t zone;       /* live MATriggerPoint, 0 if computed from the gen */
} Ma2EntryZone;

/* Local offset (vehicle frame: x right, y up, z forward) -> world, using the
 * vehicle scene node rows (+0xC0 right, +0xD0 up, +0xE0 forward, +0xF0 pos);
 * this is the same row-vector * matrix product as sub_145A14. */
static int entry_local_to_world(uint32_t veh, Ma2Vec3 l, Ma2Vec3 *out)
{
    uint32_t node = MEM32(veh + 0x94);
    if (!gva_ok(node, 0x100)) return 0;
    Ma2Vec3 p = read_vec3(node + 0xF0);
    out->x = p.x + l.x * MEMF(node + 0xC0) + l.y * MEMF(node + 0xD0) + l.z * MEMF(node + 0xE0);
    out->y = p.y + l.x * MEMF(node + 0xC4) + l.y * MEMF(node + 0xD4) + l.z * MEMF(node + 0xE4);
    out->z = p.z + l.x * MEMF(node + 0xC8) + l.y * MEMF(node + 0xD8) + l.z * MEMF(node + 0xE8);
    return 1;
}

/* Zone computed from the gen's stored sets (what sub_154CAD copies into the
 * MATriggerPoint, then what sub_145A14 places). */
static int entry_from_gen(uint32_t veh, uint32_t gen, Ma2Vec3 *c, float *radius)
{
    uint32_t cmech = class_id(CLS_MAMECH_VA, "MAMech");
    uint32_t fl = MEM32(veh + 0x26C), ro, vo;
    if (MEM32(veh + 0x28) == cmech && ((fl >> 11) & 1 || (fl >> 12) & 1 || (fl >> 13) & 1)) { ro = 0xB4; vo = 0xB8; }
    else { ro = 0xA4; vo = 0xA8; }
    Ma2Vec3 l = { MEMF(gen + vo), MEMF(gen + vo + 4), MEMF(gen + vo + 8) };
    if (!entry_local_to_world(veh, l, c)) return 0;
    *radius = MEMF(gen + ro);
    return 1;
}

/* Returns the number of zones written (<= max). */
static int entry_zones(uint32_t veh, Ma2EntryZone *z, int max)
{
    int n = 0;
    if (!z || max <= 0 || !gva_ok(veh, 0x300)) return 0;
    uint32_t world = MEM32(veh + 0x60), uid = MEM32(veh + 0x2C);
    uint32_t cgen = class_id(0x4EB188u, "MACrateGenerator");
    uint32_t ctrg = class_id(0x4EB0E4u, "MATriggerPoint");
    uint32_t e = MEM32(veh + 0xC);
    for (int guard = 0; guard < 64 && n < max && gva_ok(e, 0x30); guard++, e = MEM32(e + 8)) {
        if (MEM32(e + 0x28) != cgen || !gva_ok(e, 0x114)) continue;
        Ma2EntryZone r; memset(&r, 0, sizeof(r));
        r.gen = e;
        uint32_t zone = uid_lookup(world, MEM32(e + 0x7C));
        if (gva_ok(zone, 0xE0) && MEM32(zone + 0x28) == ctrg && MEM32(zone + 0xC0) == uid) {
            if (!MEM8(zone + 0x89)) continue;                /* trigger disabled */
            r.zone = zone;
            r.center.x = MEMF(zone + 0xB0); r.center.y = MEMF(zone + 0xB4); r.center.z = MEMF(zone + 0xB8);
            r.radius = MEMF(zone + 0xD4);
        } else {
            /* No live zone.  gen+0xD4 bit 4 is set by sub_154AD8 when the entry is
             * armed (0x91/0x81+0x10); 0xC9/0x81 = inactive: the game offers no prompt. */
            if (!(MEM8(e + 0xD4) & 0x10) || !entry_from_gen(veh, e, &r.center, &r.radius)) continue;
        }
        if (!(r.radius > 0.f && r.radius < 100.f) || r.center.x != r.center.x || r.center.z != r.center.z) continue;
        z[n++] = r;
    }
    return n;
}

/* Circle (centre, radius) per active entry zone; the prompt is offered while
 * hypot(player.x - c.x, player.z - c.z) <= radius (height ignored).  Walk until
 * the distance is < radius - 0.3 to stay clear of float/tick jitter. */
int ma2_vehicle_entry_zones(uint32_t vehicle, Ma2EntryZone *out, int max)
{
    if (!gva_ok(vehicle, 0x300) || !((MEM32(vehicle + 0x84) >> 9) & 1)) return 0;   /* sub_154678: vehicle "enterable" bit */
    return entry_zones(vehicle, out, max);
}

/* World points where an on-foot pilot gets the enter prompt for `vehicle`
 * (MAMover actor address): one per active zone, the circle centre (always
 * inside the volume).  Returns the count; 0 = no prompt available now
 * (occupied/inactive vehicle).  Typically exactly 1 point per vehicle. */
int ma2_vehicle_entry_points(uint32_t vehicle, Ma2Vec3 *out, int max)
{
    Ma2EntryZone z[4];
    int n = ma2_vehicle_entry_zones(vehicle, z, 4), k = 0;
    for (int i = 0; i < n && k < max; i++) out[k++] = z[i].center;
    return k;
}
