// WDF reader (world files, template files and .mtb sub-object tables share it).
//   header: 5 x u32, first 0xFF002F00
//   object: 0xFF012F01, property count, class key
//   property: 0xFF022F02, type, key, size, payload[size]
// Keys are crc32(name) ^ 0xFFFFFFFF.
#include "internal.h"

#include <mutex>
#include <unordered_map>

namespace ma2m {

uint32_t WdfKey(const char* name) { return detail::Crc32(name, strlen(name)) ^ 0xFFFFFFFFu; }

namespace detail {

std::string WdfProperty::AsString() const {
    size_t n = 0;
    while (n < size && data[n]) n++;
    return std::string((const char*)data, n);
}

uint32_t WdfProperty::AsU32() const {
    uint32_t v = 0;
    if (size >= 4) memcpy(&v, data, 4);
    else if (size == 2) {
        uint16_t s;
        memcpy(&s, data, 2);
        v = (type == 2) ? (uint32_t)(int32_t)(int16_t)s : s;
    } else if (size == 1) v = (type == 1) ? (uint32_t)(int32_t)(int8_t)data[0] : data[0];
    return v;
}

int32_t WdfProperty::AsI32() const { return (int32_t)AsU32(); }

float WdfProperty::AsF32() const {
    if (type == 7 && size >= 4) {
        float f;
        memcpy(&f, data, 4);
        return f;
    }
    if (type == 8 && size >= 8) {
        double d;
        memcpy(&d, data, 8);
        return (float)d;
    }
    return (float)AsI32();
}

const WdfProperty* WdfObject::Find(uint32_t key) const {
    for (const auto& p : props)
        if (p.key == key) return &p;
    return nullptr;
}

bool ParseWdf(const uint8_t* data, size_t size, std::vector<WdfObject>& objects, std::string* error) {
    objects.clear();
    auto fail = [&](const char* msg, size_t at) {
        if (error) {
            char buf[96];
            snprintf(buf, sizeof(buf), "WDF: %s at 0x%zX", msg, at);
            *error = buf;
        }
        return false;
    };
    if (size < 20) return fail("short header", 0);
    uint32_t w;
    memcpy(&w, data, 4);
    if (w != 0xFF002F00u) return fail("bad header marker", 0);
    size_t pos = 20;
    while (pos < size) {
        if (pos + 12 > size) return fail("short object header", pos);
        uint32_t h[3];
        memcpy(h, data + pos, 12);
        if (h[0] != 0xFF012F01u) return fail("bad object marker", pos);
        if (h[1] > (size - pos) / 16) return fail("impossible property count", pos);
        pos += 12;
        objects.emplace_back();
        WdfObject& o = objects.back();
        o.class_key = h[2];
        o.props.reserve(h[1]);
        for (uint32_t i = 0; i < h[1]; i++) {
            if (pos + 16 > size) return fail("short property header", pos);
            uint32_t p[4];
            memcpy(p, data + pos, 16);
            if (p[0] != 0xFF022F02u || p[3] > size - pos - 16) return fail("bad property", pos);
            WdfProperty prop;
            prop.type = p[1];
            prop.key = p[2];
            prop.size = p[3];
            prop.data = data + pos + 16;
            o.props.push_back(prop);
            pos += 16 + p[3];
        }
    }
    return true;
}

namespace {
const char* const kNames[] = {
    // structure
    "UID", "parent", "child", "sibling", "TemplateID", "OverrideTemplate", "Description", "Name",
    "MARoot", "MoeEntity", "MoeEntityReference",
    // model/file properties
    "Corporal3DObj", "CorporalMTB", "AbandonedModel", "AbandonedMTB", "TransitionModel", "TransitionMTB",
    "CamouflageTexture", "HUDIconImage", "HUDEnemyIconImage", "HUDFriendIconImage", "MechFootprintDecal",
    "MechClawDecal", "EnvironmentMapName", "EffectFile", "SceneNode", "SubObjectName",
    // template links
    "DestroyedBuilding", "CorporalDestroyed", "DestroyedEffect", "HitEffect", "HoleEdgeEffect",
    "HoleTopEffect", "WeaponLoadout",
    // mtb visibility rules
    "MAModSubObjectShow", "MAModSubObjectState", "MAModSubObjectDiffuse", "MAModSubObjectParticleEffect",
    "MAModSubObjRotRecoil", "MASubObjRespondShow", "MASubObjRespondRotate", "MAModMechHeatSink",
    "CSubObjectNode", "Enable", "MinDamagePercent", "MaxDamagePercent", "Damage Level Min",
    "Damage Level Max", "Abandoned", "EnableTree", "Nation", "VisibleEntity",
    // template classes seen in GlobalEntities.wdf
    "MAWpnGrpRef", "MACorporeal", "MAGroup", "MAEffect", "MACorporealDestroyed", "MABuilding",
    "MAPowerupDrop", "MAWpnGrp", "MABarrel", "MABuildingDestroyed", "MAProjectile", "MAWeaponLoadout",
    "MAMech", "MAPlayerConfiguration", "MAPlayerConfigurationContainer", "MAOwnerController", "MAPerson",
    "MACameraThird", "MAObjectiveType", "MAPowerUp", "MATurret", "MATank", "MAFlyer", "MASky2",
    "MACameraProperty", "MAVehicle", "MAWater", "MAEnvironment", "DropPackManager", "DropPack",
    "MATriggerLogic", "MATriggerTimer", "MATriggerRadius", "MARubble", "MASnatchItBase", "MACheckpoint",
    "MAFreeCameraController", "MAPrecipitation", "MAFlagCTF", "MechAssaultSettings", "MACameraPan",
    "MACameraSpiral", "MACameraFirst", "MAEffectCluster", "MAEffectChain", "MAEffectShield",
    "MAKillReference", "MADirectionalLight", "MAPlayer", "MAUberBoss", "MAMiniSpider", "MASpider",
    "MASnatchItDataCore", "MACinema", "MACollectionArea", "MATerrain", "MATerrainInfo", "MALevelStart",
    "MAPathDB", "MATree", "MATreeGroup", "MADecal", "MALight", "MAPointLight",
};

struct NameTable {
    std::unordered_map<uint32_t, const char*> map;
    NameTable() {
        for (const char* n : kNames) map.emplace(WdfKey(n), n);
    }
};
}  // namespace

const char* KnownWdfName(uint32_t key) {
    static const NameTable table;
    auto it = table.map.find(key);
    return it == table.map.end() ? nullptr : it->second;
}

}  // namespace detail
}  // namespace ma2m
