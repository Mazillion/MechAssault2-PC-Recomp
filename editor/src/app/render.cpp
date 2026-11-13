#include "render.h"

#include <d3dcompiler.h>

#include "model_cache.h"

#include <cstdlib>
#include <algorithm>
#include <cmath>

namespace ed {

template <class T> static void release(T*& p) { if (p) { p->Release(); p = nullptr; } }

static constexpr size_t kModelLights = 64;  // LightsCB arrays (lp/lc/ld/lx[64] in the shader)

// ---------------------------------------------------------------------------------------------
// Camera

void Camera::update(float aspect) {
    float cp = std::cos(pitch), sp = std::sin(pitch);
    XMFLOAT3 target{tx, ty, tz};
    eye = {tx - std::sin(yaw) * cp * dist, ty + sp * dist, tz - std::cos(yaw) * cp * dist};
    XMVECTOR up = pitch > 1.5f ? XMVectorSet(std::sin(yaw), 0, std::cos(yaw), 0) : XMVectorSet(0, 1, 0, 0);
    XMMATRIX v = XMMatrixLookAtLH(XMLoadFloat3(&eye), XMLoadFloat3(&target), up);
    XMMATRIX p = XMMatrixPerspectiveFovLH(fov, std::max(0.1f, aspect), std::max(0.5f, dist * 0.002f), 30000.f);
    XMStoreFloat4x4(&view, v);
    XMStoreFloat4x4(&proj, p);
    XMStoreFloat4x4(&viewproj, v * p);
}

void Camera::ray(float px, float py, float w, float h, XMFLOAT3& o, XMFLOAT3& d) const {
    XMMATRIX vp = XMLoadFloat4x4(&viewproj);
    XMMATRIX inv = XMMatrixInverse(nullptr, vp);
    float nx = px / w * 2.f - 1.f, ny = 1.f - py / h * 2.f;
    XMVECTOR a = XMVector3TransformCoord(XMVectorSet(nx, ny, 0.f, 1.f), inv);
    XMVECTOR b = XMVector3TransformCoord(XMVectorSet(nx, ny, 1.f, 1.f), inv);
    XMStoreFloat3(&o, a);
    XMStoreFloat3(&d, XMVector3Normalize(XMVectorSubtract(b, a)));
}

bool Camera::project(const XMFLOAT3& p, float w, float h, float& sx, float& sy) const {
    XMVECTOR v = XMVector4Transform(XMVectorSet(p.x, p.y, p.z, 1.f), XMLoadFloat4x4(&viewproj));
    XMFLOAT4 c;
    XMStoreFloat4(&c, v);
    if (c.w <= 0.01f) return false;
    sx = (c.x / c.w * 0.5f + 0.5f) * w;
    sy = (0.5f - c.y / c.w * 0.5f) * h;
    return true;
}

// ---------------------------------------------------------------------------------------------
// Shaders

static const char* kShader = R"(
cbuffer CB : register(b0) {
    row_major float4x4 viewproj;
    float4 light;
    float4 brush;
    float4 flags;
    float4 tsize;
    float4 drep;     // detail repeats across the map: detail 0, detail 1, micro 0, micro 1
    float4 dpar;     // x detail on, y DetailBlend, z modulate, w time
    float4 eye;
    float4 amb;      // rgb ambient x2, w game lighting on
    float4 sun;      // rgb sun colour x2
    float4 fogc;     // rgb fog colour, w fog on
    float4 fogr;     // x start, y 1 / (end - start), zw orbit target xz (fog is measured from it)
    float4 fogh;     // FogMode 2 height band: x FogTop, y FogBottom, z 1 = on, w reference y (eye; < -1e8 = overview, horizontal distance)
    float4 grep0, grep1;  // P4 ground layers: repeats across the map per layer (layers 2..7)
    float4 ginfo;         // x layer count (0 = the retail pair), y brush ring colour (Renderer::Params::brush_tint)
};
SamplerState samp : register(s0);
Texture2D colormap : register(t0);
Texture2D heights : register(t11);
Texture2D lightpool : register(t12);
cbuffer LightsCB : register(b2) {
    float4 lcount;    // x lights
    float4 lp[64];    // position, outer radius
    float4 lc[64];    // colour, inner radius
    float4 ld[64];    // spot direction, cos outer angle (-2 = point light)
    float4 lx[64];    // x cos inner angle
};
float2 map_uv(float3 wp) { return float2(wp.x / tsize.x, 1 - wp.z / tsize.y); }
// Scene lights on the ground: the light map (already x2 like the game's modulate).
float3 scene_pool(float3 wp) { return flags.w > 0.5 ? lightpool.SampleLevel(samp, map_uv(wp), 0).rgb : float3(0, 0, 0); }
// Scene lights on models: point attenuation inner -> outer radius, spot cone, N.L (1 for translucent shading: nl1).
float3 point_lights(float3 wp, float3 n, bool nl1 = false) {
    float3 s = float3(0, 0, 0);
    for (int k = 0; k < (int)lcount.x; k++) {
        float3 d = lp[k].xyz - wp;
        float dist = length(d);
        if (dist >= lp[k].w) continue;
        float3 L = d / max(dist, 0.001);
        float att = saturate((lp[k].w - dist) / max(lp[k].w - lc[k].w, 0.01));
        float cone = ld[k].w < -1.5 ? 1 : smoothstep(ld[k].w, lx[k].x, dot(-L, ld[k].xyz));
        s += lc[k].rgb * att * cone * (nl1 ? 1.0 : saturate(dot(n, L)));
    }
    return s;
}
// MAEnvironment light: ambient + sun x lit factor (the fixed editor light when off).
float3 env_light(float lit) { return amb.w > 0.5 ? amb.rgb + sun.rgb * lit : float3(lit, lit, lit); }
// Fog amount 0..1. Mode 1: linear in horizontal distance. Mode 2 (Deliverance VS dump, error < 0.003): the vertex program adds the
// distance term max((d - Start) / (End - Start), 0), d the 3D distance to the eye, to a height term
// clamp((Top - y) / (Top - Bottom), 0, 1) x min(d / (Top - Bottom), 1), then clamps the sum to 0..1.
float fog_amt(float3 wp) {
    if (fogh.z < 0.5) return saturate((length(wp.xz - fogr.zw) - fogr.x) * fogr.y);
    float3 dv = wp - float3(fogr.z, fogh.w, fogr.w);
    if (fogh.w < -1e8) dv.y = 0;
    float d = length(dv), band = max(fogh.x - fogh.y, 0.01);
    return saturate(max((d - fogr.x) * fogr.y, 0) + saturate((fogh.x - wp.y) / band) * min(d / band, 1));
}
float3 env_fog(float3 col, float3 wp) {
    if (fogc.w < 0.5) return col;
    return lerp(col, fogc.rgb, fog_amt(wp));
}
Texture2D det0 : register(t4);
Texture2D det1 : register(t5);
Texture2D mic0 : register(t6);
Texture2D mic1 : register(t7);
SamplerState wrap : register(s1);
Texture2D lightmap : register(t1);
Texture2D overlay : register(t2);
// P4 ground layers (PC runtime maintainer tree, nv2a_gpu_terrain.hlsl, the same formula): the game's
// lerp(detail 1, detail 0, colour-map alpha) becomes sum w_k layer_k / sum w_k; layer 0/1 = the pair at
// their own repeats, layers 2.. an array at uv x repeats; weights at the colour-map UV (all 0 = the pair).
Texture2D gw0 : register(t13);
Texture2D gw1 : register(t14);
Texture2DArray glay : register(t15);
float3 ground_mix(float2 uv, float a) {
    float3 d0 = det0.Sample(wrap, uv * drep.x).rgb, d1 = det1.Sample(wrap, uv * drep.y).rgb;
    if (ginfo.x > 1.5) {
        float4 w0 = gw0.Sample(samp, uv), w1 = gw1.Sample(samp, uv);
        float s = dot(w0, 1) + dot(w1, 1);
        if (s > 0.002) {
            float3 acc = w0.x * d0 + w0.y * d1;
            float w[8] = {w0.x, w0.y, w0.z, w0.w, w1.x, w1.y, w1.z, w1.w};
            float r[8] = {grep0.x, grep0.y, grep0.z, grep0.w, grep1.x, grep1.y, grep1.z, grep1.w};
            [unroll] for (int k = 2; k < 8; k++)
                if (k < (int)ginfo.x && w[k] > 0) acc += w[k] * glay.Sample(wrap, float3(uv * r[k], k - 2)).rgb;
            return acc / s;
        }
    }
    return lerp(d1, d0, a);
}

struct TVIn { float3 p : POSITION; float3 n : NORMAL; float2 uv : TEXCOORD0; };
struct TVOut { float4 pos : SV_Position; float3 wp : TEXCOORD1; float3 n : NORMAL; float2 uv : TEXCOORD0; };
TVOut tvs(TVIn i) { TVOut o; o.pos = mul(float4(i.p, 1), viewproj); o.wp = i.p; o.n = i.n; o.uv = i.uv; return o; }
float4 tps(TVOut i) : SV_Target {
    float4 base = colormap.Sample(samp, i.uv);
    float3 c = base.rgb;
    float3 micro = float3(1, 1, 1);
    float nearblk = 0;
    if (dpar.x > 0.5) {
        // Decoded NV2A combiners (research desk t12 terrain-detail-combine): alpha weights
        // TerrainDetailMap (1) against TerrainDetailMap 1 (0) (also verified in game);
        // TerrainDetailModulate 1: 2 x colour x sat(2 x mix x DetailBlend), 0: lerp(colour, mix, DetailBlend).
        float3 mix = ground_mix(i.uv, base.a);
        // Game combiner (frame trace, spec R2-5): albedo = 4 x DetailBlend x detail x colour, no clamp
        // before the 2 x light (env_light) below.
        c = dpar.z > 0.5 ? 4 * dpar.y * mix * c : lerp(c, mix, dpar.y);
        // Micro detail: its own pass over the lit terrain (depth EQUAL), dst x src x 2 with
        // src = lerp(M0, 2 x M0 x M1, 0.5) (KillingField trace line 10034); the fade toward 0.5 with
        // block cutoff below is from the trace.
        // The game switches it per 128 m terrain block, on when the block's nearest point is within one
        // block of the eye (KF trace: the 6 nearest blocks, no fade). The editor's free camera made that a
        // hard band that jumps with the view (user review A1), so by default it fades per pixel with the
        // distance to the eye instead; tsize.z 2 = "Distant terrain detail: fast" keeps the game's block switch.
        float2 b0 = floor(i.wp.xz / 128.0) * 128.0;
        float mfade = tsize.z > 1.5 ? (length(clamp(eye.xz, b0, b0 + 128.0) - eye.xz) < 128.0 ? 1 : 0)
                                    : 1 - smoothstep(128.0, 320.0, length(i.wp - eye.xyz));
        if (tsize.z > 0.5 && mfade > 0) {
            float3 m0 = mic0.Sample(wrap, i.uv * drep.z).rgb;
            float3 m1 = mic1.Sample(wrap, i.uv * drep.w).rgb;
            // Every level runs the same program (UnderCover s19 trace line 5920, Ill Wind g1 draw 2358132 and the New River City trace
            // draw 3186393, which has no light map: T2 0.033 / 0.051 / 0.080 / 0.094 at w 7.9 / 9.3 / 12.2 / 13.9 m = the fit below; the desk's
            // static decode of 0x4A3D90): src = lerp(0.5 x (M0 + M1), 0.5, T2), T2 = the per-vertex fade (view depth w),
            // sqrt(T2) ~ 0.0497 + 0.0189 w (UC fit, also matches Ill Wind: 0.036 at 7.9 m), so it is neutral grey by ~50 m.
            float vd = mul(float4(i.wp, 1), viewproj).w;  // the view depth w (SV_Position.w is not reliably 1/w here: the fade never ran, micro darkened every near block)
            float t2 = saturate(0.0497 + 0.0189 * vd); t2 *= t2;
            micro = lerp(0.5 * (m0 + m1), 0.5, t2);
            nearblk = mfade;
        }
    }
    float4 lm = lightmap.Sample(samp, i.uv);
    // Game lighting with no lightmap (New River City): plain vertex sun + ambient, as in the game.
    float sl = saturate(dot(normalize(i.n), light.xyz));
    float nl = amb.w > 0.5 ? sl : 0.45 + 0.55 * sl;
    // Game lighting with a lightmap (R2-5b): ambient x baked light (lm.r = A8L8 luminance) +
    // sun x N.L x baked sun shadow (lm.a); amb / sun are already x2.
    // Game combiner (KF stageprobe4 decode, audit #3): out = 2 x L x 4 x C0.a x T0 x detail, L = V1.rgb x V1.a x T3.a + T3.rgb, with V1 = the FILE sun
    // colour (x1: KF and UC both match) and T3.rgb = file ambient x lightmap. The albedo here already carries 4 x DetailBlend and amb / sun are x2,
    // so the sun term needs no extra factor. UC / IYF still read dark: the shadow mask lm.a is lower than the game's T3.a there (open).
    float sunk = 1.0;
    float3 L = amb.w > 0.5 && flags.z > 0.5 ? amb.rgb * lm.r + sunk * sun.rgb * sl * lm.a
                                            : env_light(flags.z > 0.5 ? lm.r * (0.6 + 0.4 * lm.a) * 1.3 : nl);
    float3 col = c * (L + scene_pool(i.wp));
    if (eye.w > 9.5 && eye.w < 24.5) {  // MA2SE_WATER_DEBUG=10..20 (19 lm.r, 20 lm.a, 21 lit col before micro, 22 after micro, 23 SV_Position.w probe): terrain stage dump (10 pre-light c, 11 detail mix, 12 colormap, 13 L/2, 14 micro0, 15 micro1, 16 micro
        int k = (int)(eye.w - 10 + 0.5);
        float3 mixd = ground_mix(i.uv, base.a);
        float3 m0d = mic0.Sample(wrap, i.uv * drep.z).rgb, m1d = mic1.Sample(wrap, i.uv * drep.w).rgb;
        return float4(k == 0 ? saturate(c) : k == 1 ? mixd : k == 2 ? base.rgb : k == 3 ? 0.5 * L : k == 4 ? m0d : k == 5 ? m1d : k == 6 ? micro : k == 7 ? lerp(det0.Sample(wrap, i.uv * drep.x).rgb, det1.Sample(wrap, i.uv * drep.y).rgb, base.a) : k == 8 ? det0.Sample(wrap, i.uv * drep.x).rgb : k == 9 ? det1.Sample(wrap, i.uv * drep.y).rgb : k == 10 ? lm.rrr : k == 11 ? lm.aaa : k == 12 ? saturate(col) : k == 13 ? saturate(nearblk > 0 ? lerp(col, saturate(2 * micro * col), nearblk) : col) : float3(saturate(i.pos.w * 50), saturate(1 / max(i.pos.w, 1e-4) / 200), 0), 1);
    }
    // Near micro-detail blocks (KF trace, 3 passes): the per-pixel light x V1 (colour map) replaces
    // the detail combine, then the micro pass multiplies dst x src x 2. No TerrainDetail there.
    if (nearblk > 0) {
        // UnderCover trace (line 831 + 1193): the lit program already holds the detail combine and the
        // micro pass is dst x src x 2, so the ground-map levels keep the combine.
        // New River City trace (no light map): pass 1 = the detail combine, pass 2 = dst x 2 x M (ratio 1.46 / 1.40 / 1.30 = 2 x M0 at the pixel).
        float3 mc = saturate(2 * micro * col);
        col = lerp(col, mc, nearblk);
    }  // L is x2, so 2 x micro x light x V1 = micro x L x V1
    // The grid sits under the fog, so far lines fade like the ground and the fog distance reads true (BB2A-5).
    if (flags.x > 0.5) {
        float2 g = abs(frac(i.wp.xz / 64.0 + 0.5) - 0.5) * 64.0;
        float gl = min(g.x, g.y);
        float fw = max(fwidth(i.wp.x), fwidth(i.wp.z)) * 1.2;
        col = lerp(col, float3(1, 1, 1), 0.22 * (1 - saturate(gl / fw)));
    }
    col = env_fog(col, i.wp);
    if (flags.y > 0.5) { float4 ov = overlay.Sample(samp, i.uv); col = lerp(col, ov.rgb, ov.a); }
    if (brush.w != 0) {
        // brush.w < 0: a square brush of half-size -brush.w (Shape tools).
        float2 q = abs(i.wp.xz - brush.xz);
        float R = abs(brush.w), d = brush.w < 0 ? max(q.x, q.y) : length(i.wp.xz - brush.xz);
        float r = abs(d - R);
        float fw = fwidth(d) * 1.5 + 0.01;
        // Paired shaping tools: cyan builds up / smooths, orange digs down / roughens (the reverse key flips it).
        float3 rc = ginfo.y > 1.5 ? float3(1, 0.5, 0.15) : ginfo.y > 0.5 ? float3(0.3, 0.85, 1) : float3(1, 0.85, 0.2);
        col = lerp(col, rc, saturate(1 - r / fw));
        if (d < R) col = lerp(col, rc, 0.08);
        // Shaping tools: height contours inside the brush (fresh-eyes 3 #6: a raised bump read as a faint patch on
        // evenly lit ground); every 5th line brighter.
        if (brush.y > 0 && d < R) {
            float h = i.wp.y / brush.y, ch = abs(frac(h + 0.5) - 0.5) / max(fwidth(h), 1e-4);
            float major = abs(frac(h / 5 + 0.5) - 0.5) * 5 < 0.5 ? 0.55 : 0.32;
            // fwidth(h) ~ 0 on flat ground: no line there (flat ground on a whole contour would light the whole disc).
            col = lerp(col, float3(1, 0.92, 0.55), major * saturate(1 - ch) * saturate((R - d) / (0.15 * R)) * saturate(fwidth(h) * 100));
        }
    }
    return float4(col, 1);
}

struct BVIn { float3 p : POSITION; float3 n : NORMAL; float4 r0 : INST0; float4 r1 : INST1; float4 r2 : INST2; float4 col : COLOR0; };
struct BVOut { float4 pos : SV_Position; float3 n : NORMAL; float4 col : COLOR0; };
BVOut bvs(BVIn i) {
    float3 w = float3(dot(i.r0.xyz, i.p) + i.r0.w, dot(i.r1.xyz, i.p) + i.r1.w, dot(i.r2.xyz, i.p) + i.r2.w);
    float3 n = float3(dot(i.r0.xyz, i.n), dot(i.r1.xyz, i.n), dot(i.r2.xyz, i.n));
    BVOut o; o.pos = mul(float4(w, 1), viewproj); o.n = n; o.col = i.col; return o;
}
float4 bps(BVOut i) : SV_Target {
    float l = 0.5 + 0.5 * saturate(dot(normalize(i.n), light.xyz));
    return float4(i.col.rgb * l, i.col.a);
}

cbuffer MeshCB : register(b1) {
    float4 matcolor;
    float4 mflags;   // x alpha mode (0 opaque, 1 cutout, 2 blend, 3 add, 4 sub), y unlit, z textured
    float4 mcol2;    // water: shallow colour; models: multi glow G colour, w has multi
    float4 mcol3;    // models: multi glow B colour, w reflectivity (material_env)
    float4 mdet;     // models: x detail mode (1 opaque, 2 alpha overlay, 3 additive glow card), y COMPLEX tint
                     // mode, z normal map, w untextured additive glow
    float4 mspec;    // models: specular_mask colour, w on
    float4 mext;     // models: x detail mix (blend_level), y COMPLEX + detail has the multi mask, z env cube bound,
                     // w detail_blend (0 modulate2x, 1 alphablend, 2 additive2x)
    float4 mrecv;    // models: x ShadowReceiverType 1 (takes the ground light map), yzw material_color_shift colour
    float4 mskya;    // sky: rgb SkyFogColor, w on
    float4 mskyb;    // sky: x SkyFogTop, y 1 / (Top - Bottom), z player y - camera y x w, w 10000 / (shrink x scale.y)
    float4 mbloom;   // models: bloom mask weights (colorshift0_bloom, colorshift1_bloom, specular_bloom), w kind 1 color_shift, 2 COMPLEX, 3 specular_mask
    float4 mshade;   // models: x 1 = material shading "translucent" (trees, bushes: the game's V0 = Kd x (ambient + sun), N.L = 1 whatever the normal)
};
float3 detail_mix(float3 b, float4 d, float k) {
    float dk = mext.w - 4 * floor(mext.w / 4);
    if (dk > 1.5) return saturate(lerp(b, b + 2 * d.rgb - 1, k));
    if (dk > 0.5) return lerp(b, lerp(b, d.rgb, d.a), k);
    return lerp(b, 2 * b * d.rgb, k);
}
Texture2D diffuse : register(t3);
Texture2D multi : register(t8);
TextureCube envcube : register(t13);
Texture2D detailtex : register(t14);
Texture2D normalmap : register(t15);
struct MVIn { float3 p : POSITION; float3 n : NORMAL; float2 uv : TEXCOORD0; float2 uv1 : TEXCOORD1; float4 vc : COLOR1; float4 r0 : INST0; float4 r1 : INST1; float4 r2 : INST2; float4 col : COLOR0; };
struct MVOut { float4 pos : SV_Position; float3 n : NORMAL; float2 uv : TEXCOORD0; float4 col : COLOR0; float3 wp : TEXCOORD1; float2 uv1 : TEXCOORD2; float4 vc : COLOR1; };
MVOut mvs(MVIn i) {
    float3 w = float3(dot(i.r0.xyz, i.p) + i.r0.w, dot(i.r1.xyz, i.p) + i.r1.w, dot(i.r2.xyz, i.p) + i.r2.w);
    float3 n = float3(dot(i.r0.xyz, i.n), dot(i.r1.xyz, i.n), dot(i.r2.xyz, i.n));
    MVOut o; o.pos = mul(float4(w, 1), viewproj); o.n = n; o.uv = i.uv; o.col = i.col; o.wp = w; o.uv1 = i.uv1; o.vc = i.vc; return o;
}
float4 mps_body(MVOut i, inout float bloom) {
    // Untextured additive glow (Mission_13b turret blink lights, R2-12 #2): 2 x V0, blend
    // SRC_ALPHA / ONE, fog toward black. V0 = material colour x vertex colour (white when absent).
    if (mdet.w > 0.5) {
        float4 g = float4(2 * i.vc.rgb * matcolor.rgb * i.col.rgb, i.vc.a * matcolor.a * i.col.a);
        if (fogc.w > 0.5) g.rgb *= 1 - fog_amt(i.wp);
        return g;
    }
    // Shading normal: the normal_map slot in tangent space (R2-12 #1, the game's DOT_PRODUCT pass
    // N.L = dot(2 x normalmap - 1, light)), with the tangent frame from screen derivatives.
    float3 sn = normalize(i.n);
    float3 dp1 = ddx(i.wp), dp2 = ddy(i.wp);
    float2 du1 = ddx(i.uv), du2 = ddy(i.uv);
    if (mdet.z > 0.5) {
        float3 nm = normalmap.Sample(wrap, i.uv).rgb * 2 - 1;
        float3 p2 = cross(dp2, sn), p1 = cross(sn, dp1);
        float3 T = p2 * du1.x + p1 * du2.x, B = p2 * du1.y + p1 * du2.y;
        float im = rsqrt(max(max(dot(T, T), dot(B, B)), 1e-20));
        float3 pn = nm.x * T * im + nm.y * B * im + nm.z * sn;
        if (dot(pn, pn) > 1e-8) sn = normalize(pn);
    }
    float4 t = mflags.z > 0.5 ? diffuse.Sample(wrap, i.uv) : float4(1, 1, 1, 1);
    bool upm = mflags.x < 0.5 && mflags.y < 0.5 && mflags.z > 0.5 && mcol2.w < 0.5 && sn.y > 0.7 && amb.w > 0.5 && flags.z > 0.5;
    if (mdet.x > 0.5) {
        float4 d = detailtex.Sample(wrap, i.uv1);
        // IYF floor (iyfg trace, draw 14226): albedo = T0 x T1 at x1, the 0.498 constants cancel the x2 stages.
        // material_detail 'modulate' is the fixed-function chain 4 x L x V0 x base x detail (sub_000705F0; M15 arena wall trace draw 600675: out = 2 x V0 x T0 x T1 at level 0.5, no lerp),
        // so at level 0.5 every face takes base x detail at x1, not only the floors (IYF); other levels keep the fitted lerp (M13b 0.8, M05 0.44 traces).
        if (mdet.x < 1.5) t.rgb = ((upm || abs(mext.x - 0.5) < 0.03) && mext.w - 4 * floor(mext.w / 4) < 0.5) ? t.rgb * d.rgb : detail_mix(t.rgb, d, mext.x);
        else if (mdet.x < 2.5) { t.rgb = lerp(t.rgb, d.rgb, mext.x); t.a *= d.a; }
        else if (mdet.x < 3.5) t.rgb = lerp(t.rgb, d.rgb, 0.835);  // additive glow cards (R2-11)
        else {
            // COMPLEX + detail (KillingField trace line 10124): base' = lerp(cube, base, base.a), then the
            // detail mixed in where the multi mask's alpha is low.
            float3 v = normalize(eye.xyz - i.wp);
            if (mext.z > 0.5) t.rgb = lerp(envcube.Sample(wrap, reflect(-v, sn)).rgb, t.rgb, t.a);
            float ma = mext.y > 0.5 ? multi.Sample(wrap, i.uv).a : 0;
            t.rgb = detail_mix(t.rgb, d, (1 - ma) * mext.x);
        }
    }
    if (mext.w > 3.5 && mdet.x < 0.5) {
        float3 v = normalize(eye.xyz - i.wp);
        t.rgb = lerp(envcube.Sample(wrap, reflect(-v, sn)).rgb, t.rgb, t.a);
    }
    if (mflags.x > 0.5 && mflags.x < 1.5) clip(t.a - (mrecv.x > 1.5 ? 1.0 / 255 : 0.5));  // sky: alpha test > 0 + blend (WC trace)
    float nl = mshade.x > 0.5 ? 1.0 : saturate(dot(sn, light.xyz));  // M03 trace: conifer V0 / Kd (146) and bush V0 / Kd (54,62,55) both = ambient + sun = (0.451,0.416,0.475)
    float3 l = mflags.y > 0.5 ? float3(1, 1, 1) : amb.w > 0.5 ? amb.rgb + sun.rgb * nl : 0.5 + 0.6 * nl;
    // Opaque plain parts of any facing (pads, pavements, walls; R2-13 #1, decoded on 8 levels, pass A per near static model): the vertex light is zero in the game, light = the ground pre-pass
    // (N.L x G.a x sun + G.rgb), so use the terrain light map (shadow mask + baked ambient) at the model's XZ.
    // Also alpha-tested / blended parts (not additive), and upward-facing COMPLEX parts (roads, pavements: R2-2/R2-13 pass A+B);
    // COMPLEX spires, crystals and 'Mechs have no pre-pass (KF 10124, M13b 3442) and keep the vertex light.
    bool gpre = mflags.x < 2.5 && mflags.y < 0.5 && mflags.z > 0.5 && mflags.w < 0.5 && mrecv.x > 0.5 && mshade.x < 0.5 && amb.w > 0.5 && flags.z > 0.5;
    if (gpre) {
        float4 gm = lightmap.SampleLevel(samp, map_uv(i.wp), 0);
        // The baked mask is the terrain's own shadow: a surface clearly above the terrain (the IYF arena floor sits on a
        // plate over a pit whose mask alpha is 0.05) is not shadowed by it. IYF trace: light = (0.24,0.22,0.29) x2 = amb + sun x N.L unshadowed.
        float sh = gm.a;
        l = (amb.rgb * gm.r + sun.rgb * nl * sh) * i.vc.rgb;  // V1 = the vertex colour (baked shading)
    }
    float4 c = t * matcolor;
    float3 tc = c.rgb * i.col.rgb;
    if (eye.w > 19.5) {  // MA2SE_WATER_DEBUG=19 texture*matcolor*vertex col, 20 light l/2, 21 vertex col, 22 matcolor
        int k = (int)(eye.w - 20 + 0.5);
        return float4(k == 0 ? saturate(tc) : k == 1 ? 0.5 * l : k == 2 ? i.col.rgb : k == 3 ? lightmap.SampleLevel(samp, map_uv(i.wp), 0).raaa : k == 5 ? multi.Sample(wrap, i.uv).rgb : float3(sn.y, nl, 0), 1);  // --stage 24 = the COMPLEX multi mask
    }
    // COMPLEX tint mode: colorshift1 tints the light where mask.b is set (team / paint colour).
    if (mcol2.w > 0.5 && mdet.y > 0.5) l *= lerp(float3(1, 1, 1), mshade.y >= 0 ? mshade.yzw : mcol3.rgb, multi.Sample(wrap, i.uv).b);
    c.rgb *= l * i.col.rgb;
    if (flags.w > 0.5 && mflags.y < 0.5 && mflags.w < 0.5) {
        // Upward-facing surfaces (roads, pavements, often on plates above the terrain) take the ground
        // light map like the game's roads; walls and units take model lighting.
        // Translucent vegetation takes the point lights with N.L = 1 like its sun, inside V0 (M13a brush trace: V0 = Kd x (A + S + lamp colour x att),
        // the radius-60 lamps adding up to 1.07 x their colour), so x2 like amb / sun.
        c.rgb += tc * (mshade.x > 0.5 ? 2 * point_lights(i.wp, sn, true) : sn.y > 0.7 ? scene_pool(i.wp) : point_lights(i.wp, sn));
    }
    // COMPLEX glow sits inside the combiner's x2: out = 2 x (light x lit + mask.g x C0 + mask.b x C1) (R2-13 #2).
    if (mcol2.w > 0.5) { float4 mk = multi.Sample(wrap, i.uv); c.rgb += 2 * (mk.g * mcol2.rgb + (mdet.y > 0.5 ? 0 : mk.b) * mcol3.rgb); if (mbloom.w > 1.5 && mbloom.w < 2.5) bloom = mbloom.x * mk.g + (mdet.y > 0.5 ? 0 : mbloom.y * mk.b); }
    // material_color_shift (sky domes, desk: ill-wind sky trace draw 2358146): out = 2 x (V0 x T + C0 x V0.a x T.a), V0 = 0.5,
    // so the base alpha weights colorshift0_color on top of the texture.
    c.rgb += mrecv.yzw * t.a;
    if (mbloom.w > 0.5 && mbloom.w < 1.5) bloom = 0.5 * mbloom.x * t.a;  // color_shift: framebuffer alpha = C0.a x T0.a x V0.a, V0.a = 0.5 (M02 bloom2 trace: s0 T0.a = T0.a x V0.a, s1 R0.a = C0.a x T0.a; roof strips 63/255)
    if (mspec.w > 0.5) {
        // Added after the x2, not doubled; COMPLEX masks it with multi.r (R2-15 #1), specular_mask with base.a.
        float3 v = normalize(eye.xyz - i.wp);
        float sm = mspec.w > 2.5 ? detailtex.Sample(wrap, i.uv).r : mspec.w > 1.5 ? multi.Sample(wrap, i.uv).r : t.a;
        float ca = envcube.Sample(wrap, reflect(-v, sn)).a;
        // COMPLEX road slabs (M16 trace, road pass 8233787 s6): spec = multi.r x shin_strength x the ground pre-pass framebuffer alpha (0x2C = 0.173 at the traced pixel), not the cube alpha.
        if (gpre && mspec.w > 1.5 && mspec.w < 2.5) ca = 0.099 + 0.138 * sqrt(saturate(dot(v, sn)));  // g4 FB dump, M16: A = 0.125 / 0.137 / 0.145 / 0.153 / 0.173 at rows 260 / 280 / 300 / 330 / 400 (sin of the view angle 0.08 .. 0.28)
        if (mspec.w > 3.5) {  // M12 crystal (program 08C21 s6/s7): mask = detail.a, added term = cube.rgb x spec
            float3 cc = envcube.Sample(wrap, reflect(-v, sn)).rgb;
            c.rgb += detailtex.Sample(wrap, i.uv).a * cc * mspec.rgb * mbloom.z;  // x specular_bloom = the .149 constant alpha of s6
            bloom += 0.5 * mbloom.z * detailtex.Sample(wrap, i.uv).a;
        } else {
        c.rgb += sm * ca * mspec.rgb;
        bloom += 0.5 * mbloom.z * sm * ca;  // specular_bloom x mask x cube.a (KF / Ill Wind traces) x V0.a = 0.5 (M02 left wall trace: alpha = T0.a x V0.a x C0.a, 12/255 for C0.a 0.098)
        }
    }
    if (mcol3.w > 5) {
        // Opaque env material (envmaptype modulated: building windows, doors). Mission_02 trace (Office16 windows, 3-stage prog 00021):
        // out = 2 x V0 x lerp(T0, T1, C0.a) with V0 = material x (ambient + sun N.L); C0.a = 0x4C (0.298) for reflectivity 0.9 and 0x7F
        // (0.498) for 0.5 (Office20). The map k(reflectivity) below fits those two points; other values are unverified. The old
        // "base x light + reflectivity x cube" added an unlit cube on top, lifting the window frames to grey and the panes 1.7x.
        float3 v = normalize(eye.xyz - i.wp);
        float refl = mcol3.w - 10.0;
        if (refl > 0.75) {
            // High-reflectivity panes (Office16 Window_039, refl 0.9): the 2-stage program of the M02 trace (draw 8292304, left pane, C0.a 0xCA):
            // out = lerp(2 x V0 x T0, cube, T0.a x C0.a), the cube is NOT lit (game 121,133,135 pre-bloom vs 46,43,49 with the lit 3-stage path).
            c.rgb = lerp(l * i.col.rgb * t.rgb, envcube.Sample(wrap, reflect(-v, sn)).rgb, t.a * min(refl, 0.79));
        } else {
        float k = min(0.5, 3.0 * (1.0 - refl));
        c.rgb = l * i.col.rgb * lerp(t.rgb, envcube.Sample(wrap, reflect(-v, sn)).rgb, k);
        }
    } else if (mcol3.w > 0) {
        // Env glass (R2-11): 2 x light x base whatever the diffuse, plus the reflection.
        float3 v = normalize(eye.xyz - i.wp);
        c.rgb = t.rgb * l * i.col.rgb + mcol3.w * envcube.Sample(wrap, reflect(-v, sn)).rgb;
    } else if (mcol3.w < 0) {
        // envmaptype "masked" (Hesperus oily water, Ill Wind trace draws 2358164/5): rgb = lerp(2 x V0 x base, cube, reflectivity),
        // alpha = opacity x base.a over the water (SRC_ALPHA blend); V0 = 0.5 x light (material_only: x1).
        float3 v = normalize(eye.xyz - i.wp);
        c.rgb = lerp(t.rgb * matcolor.rgb * l * i.col.rgb, envcube.Sample(wrap, reflect(-v, sn)).rgb, -mcol3.w);
        c.a *= i.vc.a;  // V0.a = opacity x vertex alpha (Ill Wind g1 draw 2358165: 0.127 / 0.138 per vertex vs 0.15 on oily1)
    }
    if (mflags.w < 0.5) c.rgb = env_fog(c.rgb, i.wp);  // mflags.w 1 = the sky (never fogged by distance)
    else if (mskya.w > 0.5) {
        // Sky-dome height fog (Ill Wind g3 trace: factor 1 - q, q = 4.47 - 10 x model_y = (Top - h) / (Top - Bottom) with
        // h = 10000 x model_y - 2970 = 10000 x (model_y + offset.y / scale.y)). Fully fog colour below Bottom, clear above Top.
        float h = i.wp.y * mskyb.w + mskyb.z;
        float q = (mskyb.x - h) * mskyb.y;
        // Additive layers (sun, moon, clouds: BD / Ill Wind traces, fog coefficients 2 -0.5, colour 0) fog toward black at half the slope:
        // keep = 2 - 0.5 x (1 + q) where the opaque dome has keep = 2 - (1 + q).
        if (mskya.w > 1.5) c.rgb *= saturate(1.5 - 0.5 * q);
        else c.rgb = lerp(c.rgb, mskya.rgb, saturate(q));
    }
    // COMPLEX: base alpha is the env mask; a translucent one blends by its opacity alone (R2-10).
    if (mcol2.w > 0.5) c.a = matcolor.a;
    c.a *= i.col.a;
    if (mflags.x < 1.5 && !(mrecv.x > 1.5 && mflags.x > 0.5)) c.a = 1;  // sky cutout layers keep their texture alpha
    return c;
}
struct MPSOut { float4 c : SV_Target0; float4 m : SV_Target1; };
MPSOut mps(MVOut i) {
    float b = 0;
    MPSOut o; o.c = mps_body(i, b);
    // Blended / additive draws never write framebuffer alpha in the game (M02 traces: colour mask 00010101); only the sky glow sprite does.
    if (mflags.x > 1.5 && mflags.w < 0.5) b = 0;
    o.m = float4(saturate(b), 0, 0, o.c.a); return o;
}

struct LVIn { float3 p : POSITION; float4 c : COLOR0; };
struct LVOut { float4 pos : SV_Position; float4 c : COLOR0; };
LVOut lvs(LVIn i) { LVOut o; o.pos = mul(float4(i.p, 1), viewproj); o.c = i.c; return o; }
float4 lps(LVOut i) : SV_Target { return i.c; }
// Ground outside the map (a flat plane under the lowest terrain point). Overview cameras: dark and unfogged, so past
// the map edge reads as "nothing here" (fogged, it was the same flat light grey as the sky dome's lower half it hides:
// M05's short fog is measured from the orbit target, so everything past the east edge is full fog). Game-like close
// views (fog from the eye, fogh.w set): fogged, so a horizon past the edge stays the fog colour as in the game.
struct OVOut { float4 pos : SV_Position; float4 c : COLOR0; float3 wp : TEXCOORD1; };
OVOut ovs(LVIn i) { OVOut o; o.pos = mul(float4(i.p, 1), viewproj); o.c = i.c; o.wp = i.p; return o; }
float4 ops(OVOut i) : SV_Target { return float4(fogh.w < -1e8 ? i.c.rgb : env_fog(i.c.rgb, i.wp), 1); }

// Particle sprites (View > Effects): camera-facing quads, texture x vertex colour, unlit (the game's
// material_only), fogged toward the fog colour (alpha blend) or toward black (additive, mflags-free: sfog.x).
cbuffer SpriteCB : register(b3) { float4 sprm; };  // x 1 = additive (fog toward black)
struct SVIn { float3 p : POSITION; float2 uv : TEXCOORD0; float4 c : COLOR0; };
struct SVOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; float4 c : COLOR0; float3 wp : TEXCOORD1; };
SVOut svs(SVIn i) { SVOut o; o.pos = mul(float4(i.p, 1), viewproj); o.uv = i.uv; o.c = i.c; o.wp = i.p; return o; }
float4 sps(SVOut i) : SV_Target {
    float4 t = diffuse.Sample(samp, i.uv);
    float4 c = t * i.c;
    if (fogc.w > 0.5) {
        float f = fog_amt(i.wp);
        c.rgb = sprm.x > 0.5 ? c.rgb * (1 - f) : lerp(c.rgb, fogc.rgb, f);
    }
    return c;
}

// Water (material_water), after the game's 5-pass effect (frame trace, spec R2-3):
//   N = wave volume (normal map, uv = world xz x 0.03, w = time x 24 fps / 32 slices)
//   R = sky cube (reflect(-V, N)), F = lerp(fresnel_max, fresnel_min, N.V)
//   a = water thickness (depth to the terrain / 4 m), tint = lerp(shallow, deep, a)
//   out = lerp(scene behind x tint, 2 x R x light, F x sat(4a)), then fog.
// Dual-source blend: target 0 = the reflection part, target 1 = the factor on the scene behind.
// With refraction (mcol3.x, trace 6781) the scene behind comes from a copy of the frame sampled at
// uv + N.xz (less toward the shore), and target 1 is 0.
// matcolor = deep colour, mcol2 = shallow colour; mflags = fresnel min, max, has wave, has cube;
// vertex colour alpha 0 = selected.
Texture3D wave3 : register(t9);
Texture2D scenecopy : register(t16);
TextureCube refl : register(t10);
// Water reflection cube: the game's faces read vertically mirrored against D3D (the M05 trace sample at
// V0 (0.963,0.311,0.504) is (130,130,143); the standard lookup gives (140,138,133), v flipped (128,126,141)).
float3 wflip(float3 d) { float3 a = abs(d); return (a.y > a.x && a.y > a.z) ? float3(d.x, d.y, -d.z) : float3(d.x, -d.y, d.z); }
struct WVOut { float4 pos : SV_Position; float3 wp : TEXCOORD1; float4 c : COLOR0; };
struct WOut { float4 c0 : SV_Target0; float4 c1 : SV_Target1; };
WVOut wvs(LVIn i) { WVOut o; o.pos = mul(float4(i.p, 1), viewproj); o.wp = i.p; o.c = i.c; return o; }
WOut wps(WVOut i) {
    float t = dpar.w;
    float3 n = float3(0, 1, 0);
    if (mflags.z > 0.5) {
        float3 s = wave3.Sample(wrap, float3(i.wp.xz * 0.03, frac(t * 0.75))).rgb * 2 - 1;
        // The raw expanded texel goes straight into the reflection, as in the game (DOT_RFLCT_SPEC):
        // no normalising toward up. tsize.w picks which channel is up (developer flag --wave-up).
        n = tsize.w < 0.5 ? float3(s.x, s.z, s.y) : tsize.w < 1.5 ? float3(s.x, s.y, s.z) : float3(s.y, s.x, s.z);
    }
    if (eye.w > 1.5) {  // MA2SE_WATER_DEBUG=1 cube rgb, 2 cube alpha at V0 (0.963,0.311,0.504): compare with the game (130,130,143) / E9
        int k = (int)(eye.w - 2 + 0.5);
        float3 dirs[6] = {float3(1, 0, 0), float3(-1, 0, 0), float3(0, 1, 0), float3(0, -1, 0), float3(0, 0, 1), float3(0, 0, -1)};
        float3 dd = k >= 2 ? dirs[min(k - 2, 5)] : float3(0.963, 0.311, 0.504);
        if (k >= 8) {  // 8 actual reflect colour, 9 w, 10 lit, 11 N.V
            float3 vv = normalize(eye.xyz - i.wp);
            float nvv = saturate(dot(n, vv));
            float ff = lerp(mflags.y, mflags.x, nvv);
            float aa = saturate((i.wp.y - heights.SampleLevel(samp, map_uv(i.wp), 0).r) / 4.0);
            float4 g2 = flags.z > 0.5 ? lightmap.SampleLevel(samp, map_uv(i.wp), 0) : float4(1, 0, 0, 1);
            float3 o3 = k == 8 ? refl.Sample(wrap, wflip(reflect(-vv, n))).rgb : k == 9 ? float3(ff * saturate(4 * aa), ff, aa) : k == 10 ? 0.5 * (amb.rgb * g2.r + 0.5 * sun.rgb * saturate(dot(n, light.xyz)) * g2.a) : k == 13 ? float3(0.5 * amb.g * g2.r, 0.25 * sun.g * saturate(dot(n, light.xyz)) * g2.a, g2.a) : k == 12 ? float3(saturate((length(i.wp.xz - fogr.zw) - fogr.x) * fogr.y), fogc.w, saturate(fogr.y * 200)) : float3(nvv, mflags.x, mflags.y);
            WOut d2; d2.c0 = float4(o3, 1); d2.c1 = float4(0, 0, 0, 0); return d2;
        }
        float4 cs = refl.SampleLevel(wrap, wflip(normalize(dd)), 0);
        WOut d; d.c0 = float4(k == 1 ? cs.aaa : cs.rgb, 1); d.c1 = float4(0, 0, 0, 0); return d;
    }
    float3 v = normalize(eye.xyz - i.wp);
    float nv = saturate(dot(n, v));  // R1.a = N.V of the raw expanded normal (desk: water-reflection-fresnel-dst-alpha)
    float f = lerp(mflags.y, mflags.x, nv);
    float3 nr = n / max(length(n), 1e-4);  // DOT_RFLCT_SPEC reflects with E x N / N.N: the texel's length does not tilt the reflection
    float3 r = mflags.w > 0.5 ? refl.Sample(wrap, wflip(reflect(-v, nr))).rgb : matcolor.rgb * 1.3;  // cube: direction only
    // Thickness = the depth-buffer difference along the view ray (the game's t1 LUT), so the vertical depth
    // is divided by the view elevation: a grazing view through 0.6 m of water is several metres of path.
    float a = saturate((i.wp.y - heights.SampleLevel(samp, map_uv(i.wp), 0).r) / (24.0 * max(v.y, 0.15)));
    float3 tint = lerp(mcol2.rgb, matcolor.rgb, a);
    float nl = saturate(dot(normalize(n), light.xyz));
    // The game's light here is V0 x G.a + G.rgb (day light <= 1, R2-14 #6): amb / sun are x2, so halve them.
    // M05 trace: light = V0 x G.a + G.rgb with G the ground map (baked ambient x lightmap + sun shadow), about 0.3
    // in the swamp, not the plain ambient + sun.
    float3 lit = 0.5 + 0.5 * nl;
    if (amb.w > 0.5) {
        float4 gm = flags.z > 0.5 ? lightmap.SampleLevel(samp, map_uv(i.wp), 0) : float4(1, 0, 0, 1);
        lit = flags.z > 0.5 ? 0.5 * (amb.rgb * gm.r + 0.5 * sun.rgb * nl * gm.a) : 0.5 * (amb.rgb + sun.rgb * nl);
    }
    lit += scene_pool(i.wp);
    float w = f * saturate(4 * a);
    float3 c0 = 2 * r * lit * w;  // the composite's out x2 (trace pass 6819 stage 3)
    float3 c1 = tint * (1 - w);
    if (mcol3.x > 0.5) {
        float2 sz;
        scenecopy.GetDimensions(sz.x, sz.y);
        float2 suv = i.pos.xy / sz + n.xz * 0.025 * saturate(4 * a);
        c0 += scenecopy.SampleLevel(samp, saturate(suv), 0).rgb * c1;
        c1 = float3(0, 0, 0);
    }
    if (fogc.w > 0.5) {
        float fg = fog_amt(i.wp);
        c0 = lerp(c0, fogc.rgb, fg);
        c1 *= 1 - fg;
    }
    if (i.c.a < 0.5) { c0 = lerp(c0, float3(1, 0.75, 0.25), 0.4); c1 *= 0.6; }
    WOut o; o.c0 = float4(c0, 1); o.c1 = float4(c1, 0); return o;
}
)";

// The palette's water picture: the water pixel shader above on a fixed patch (no scene behind it, so the
// water is as deep as it gets), seen from 45 degrees. cbuffer: deep, shallow, (fresnel min, max, has wave, has cube).
static const char* kWaterPreview = R"(
cbuffer PB : register(b0) { float4 pdeep; float4 pshallow; float4 pfl; };
Texture3D pwave : register(t0);
TextureCube prefl : register(t1);
SamplerState pwrap : register(s0);
struct PVOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
PVOut pvs(uint id : SV_VertexID) {
    float2 p = float2((id << 1) & 2, id & 2);
    PVOut o;
    o.pos = float4(p * float2(2, -2) + float2(-1, 1), 0, 1);
    o.uv = p;
    return o;
}
float3 pflip(float3 d) { float3 a = abs(d); return (a.y > a.x && a.y > a.z) ? float3(d.x, d.y, -d.z) : float3(d.x, -d.y, d.z); }
float4 pps(PVOut i) : SV_Target {
    float2 xz = (i.uv - 0.5) * 48;
    float3 wp = float3(xz.x, 0, xz.y);
    float3 n = float3(0, 1, 0);
    if (pfl.z > 0.5) {
        float3 s = pwave.Sample(pwrap, float3(xz * 0.03, 0.25)).rgb * 2 - 1;
        n = float3(s.x, s.z, s.y);
    }
    float3 v = normalize(float3(0, 40, -40) - wp);
    float nv = saturate(dot(n, v));
    float f = lerp(pfl.y, pfl.x, nv);
    float3 nr = n / max(length(n), 1e-4);
    float3 r = pfl.w > 0.5 ? prefl.Sample(pwrap, pflip(reflect(-v, nr))).rgb : pdeep.rgb * 1.3;
    float lit = 0.5 + 0.5 * saturate(dot(normalize(n), normalize(float3(0.4, 0.8, -0.3))));
    // Half the sky: the tile must show the water's own colour (a full mirror term turned bright-sky waters near white).
    float3 c = lerp(pdeep.rgb * (0.8 + 0.4 * lit), r * lit, f * 0.5);
    return float4(saturate(c), 1);
}
)";

static bool compile(const char* entry, const char* target, ID3DBlob** out, std::string* err) {
    ID3DBlob* errs = nullptr;
    HRESULT hr = D3DCompile(kShader, strlen(kShader), "editor.hlsl", nullptr, nullptr, entry, target,
                            D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, out, &errs);
    if (FAILED(hr)) {
        if (err) *err = errs ? std::string((const char*)errs->GetBufferPointer(), errs->GetBufferSize()) : "shader compile failed";
        release(errs);
        return false;
    }
    release(errs);
    return true;
}

// Bloom post-process (game trace, Bone Dragon frame: backbuffer -> 320x240 with rgb x alpha (blend SRC_ALPHA/ZERO), box 4x4 -> 160x120, four
// blur passes alternating (0,+-1.5) / (+-1.5,0) with tap weights 0.247 / 0.498 / 0.498 / 0.247 (constants 0x3F / 0x7F), max-merged back into the
// 320x240 target, then added to the frame (blend CONSTANT_COLOR both ways, taken as ONE / ONE)).
static const char* kBloom = R"(
cbuffer BloomCB : register(b3) { float4 rect; float4 texel; float4 dir; };
Texture2D src0 : register(t0);
Texture2D src1 : register(t1);
SamplerState lin : register(s0);
struct BO { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
BO bvs(uint id : SV_VertexID) {
    float2 p = float2((id << 1) & 2, id & 2);
    BO o; o.pos = float4(p * 2 - 1, 0, 1); o.uv = float2(p.x, 1 - p.y); return o;
}
float4 bps_down(BO i) : SV_Target {
    float2 uv = rect.xy + i.uv * rect.zw;
    float3 c = 0; float m = 0;
    [unroll] for (int k = 0; k < 4; k++) {
        float2 o = float2((k & 1) ? 1 : -1, (k & 2) ? 1 : -1) * texel.xy;
        c += src0.SampleLevel(lin, uv + o, 0).rgb;
        m += src1.SampleLevel(lin, uv + o, 0).r;
    }
    return float4(c * 0.25 * (m * 0.25), 1);
}
float4 bps_box(BO i) : SV_Target {
    float3 c = 0;
    [unroll] for (int k = 0; k < 4; k++) c += src0.SampleLevel(lin, i.uv + float2((k & 1) ? 1 : -1, (k & 2) ? 1 : -1) * texel.xy, 0).rgb;
    return float4(c * 0.25, 1);
}
float4 bps_blur(BO i) : SV_Target {
    float2 d = dir.xy * texel.xy;
    float3 c = 0.247 * (src0.SampleLevel(lin, i.uv - 1.5 * d, 0).rgb + src0.SampleLevel(lin, i.uv + 1.5 * d, 0).rgb)
             + 0.498 * (src0.SampleLevel(lin, i.uv - 0.5 * d, 0).rgb + src0.SampleLevel(lin, i.uv + 0.5 * d, 0).rgb);
    return float4(c, 1);
}
float4 bps_copy(BO i) : SV_Target { return float4(src0.SampleLevel(lin, i.uv, 0).rgb, 1); }
)";
static bool compile_bloom(const char* entry, const char* target, ID3DBlob** out) {
    ID3DBlob* errs = nullptr;
    HRESULT hr = D3DCompile(kBloom, strlen(kBloom), "bloom.hlsl", nullptr, nullptr, entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, out, &errs);
    release(errs);
    return SUCCEEDED(hr);
}

// Model vertex: position, normal, uv0, uv1, RGBA8 vertex colour (model_cache.cpp load()).
static const UINT kModelStride = 44;
static const UINT kMeshCB = 192;  // MeshCB: 12 float4

struct CB {
    XMFLOAT4X4 viewproj;
    XMFLOAT4 light, brush, flags, tsize, drep, dpar, eye, amb, sun, fogc, fogr, fogh;
    XMFLOAT4 grep0, grep1, ginfo;
};

bool Renderer::init(ID3D11Device* dev, ID3D11DeviceContext* ctx, std::string* err) {
    dev_ = dev;
    ctx_ = ctx;
    ID3DBlob *vs = nullptr, *ps = nullptr;
    // Terrain
    if (!compile("tvs", "vs_4_0", &vs, err) || !compile("tps", "ps_4_0", &ps, err)) return false;
    dev->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &terrain_vs_);
    dev->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &terrain_ps_);
    D3D11_INPUT_ELEMENT_DESC til[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    dev->CreateInputLayout(til, 3, vs->GetBufferPointer(), vs->GetBufferSize(), &terrain_il_);
    release(vs); release(ps);
    // Boxes
    if (!compile("bvs", "vs_4_0", &vs, err) || !compile("bps", "ps_4_0", &ps, err)) return false;
    dev->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &box_vs_);
    dev->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &box_ps_);
    D3D11_INPUT_ELEMENT_DESC bil[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"INST", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 0, D3D11_INPUT_PER_INSTANCE_DATA, 1},
        {"INST", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 16, D3D11_INPUT_PER_INSTANCE_DATA, 1},
        {"INST", 2, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 32, D3D11_INPUT_PER_INSTANCE_DATA, 1},
        {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 48, D3D11_INPUT_PER_INSTANCE_DATA, 1},
    };
    dev->CreateInputLayout(bil, 6, vs->GetBufferPointer(), vs->GetBufferSize(), &box_il_);
    release(vs); release(ps);
    // Models
    if (!compile("mvs", "vs_4_0", &vs, err) || !compile("mps", "ps_4_0", &ps, err)) return false;
    dev->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &model_vs_);
    dev->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &model_ps_);
    D3D11_INPUT_ELEMENT_DESC mil[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 1, DXGI_FORMAT_R32G32_FLOAT, 0, 32, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR", 1, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 40, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"INST", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 0, D3D11_INPUT_PER_INSTANCE_DATA, 1},
        {"INST", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 16, D3D11_INPUT_PER_INSTANCE_DATA, 1},
        {"INST", 2, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 32, D3D11_INPUT_PER_INSTANCE_DATA, 1},
        {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 48, D3D11_INPUT_PER_INSTANCE_DATA, 1},
    };
    dev->CreateInputLayout(mil, 9, vs->GetBufferPointer(), vs->GetBufferSize(), &model_il_);
    release(vs); release(ps);
    // Lines
    if (!compile("lvs", "vs_4_0", &vs, err) || !compile("lps", "ps_4_0", &ps, err)) return false;
    dev->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &line_vs_);
    dev->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &line_ps_);
    D3D11_INPUT_ELEMENT_DESC lil[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    dev->CreateInputLayout(lil, 2, vs->GetBufferPointer(), vs->GetBufferSize(), &line_il_);
    release(vs); release(ps);
    if (!compile("ovs", "vs_4_0", &vs, err) || !compile("ops", "ps_4_0", &ps, err)) return false;
    dev->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &outside_vs_);
    dev->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &outside_ps_);
    release(vs); release(ps);
    if (!compile("svs", "vs_4_0", &vs, err) || !compile("sps", "ps_4_0", &ps, err)) return false;
    dev->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &sprite_vs_);
    dev->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &sprite_ps_);
    D3D11_INPUT_ELEMENT_DESC sil[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 20, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    dev->CreateInputLayout(sil, 3, vs->GetBufferPointer(), vs->GetBufferSize(), &sprite_il_);
    release(vs); release(ps);
    if (!compile("wvs", "vs_4_0", &vs, err) || !compile("wps", "ps_4_0", &ps, err)) return false;
    dev->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &water_vs_);
    dev->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &water_ps_);
    release(vs); release(ps);
    {  // Water palette picture (a failure only loses the picture).
        ID3DBlob *pv = nullptr, *pp = nullptr, *errs = nullptr;
        if (SUCCEEDED(D3DCompile(kWaterPreview, strlen(kWaterPreview), "waterpreview.hlsl", nullptr, nullptr, "pvs", "vs_4_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &pv, &errs)) &&
            SUCCEEDED(D3DCompile(kWaterPreview, strlen(kWaterPreview), "waterpreview.hlsl", nullptr, nullptr, "pps", "ps_4_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &pp, &errs))) {
            dev->CreateVertexShader(pv->GetBufferPointer(), pv->GetBufferSize(), nullptr, &wprev_vs_);
            dev->CreatePixelShader(pp->GetBufferPointer(), pp->GetBufferSize(), nullptr, &wprev_ps_);
            D3D11_BUFFER_DESC cb{};
            cb.ByteWidth = 48;
            cb.Usage = D3D11_USAGE_DEFAULT;
            cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            dev->CreateBuffer(&cb, nullptr, &wprev_cb_);
        }
        release(pv); release(pp); release(errs);
    }

    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = sizeof(CB);
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    dev->CreateBuffer(&bd, nullptr, &cb_);
    bd.ByteWidth = kMeshCB;
    dev->CreateBuffer(&bd, nullptr, &mesh_cb_);
    bd.ByteWidth = 16;
    dev->CreateBuffer(&bd, nullptr, &sprite_cb_);
    bd.ByteWidth = 16 + kModelLights * 16 * 4;
    dev->CreateBuffer(&bd, nullptr, &lights_cb_);

    // Unit cube with its base at y = 0.
    struct V { XMFLOAT3 p, n; };
    std::vector<V> cv;
    std::vector<uint16_t> ci;
    const float faces[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    for (auto& f : faces) {
        XMFLOAT3 n{f[0], f[1], f[2]};
        XMFLOAT3 u = std::fabs(n.y) > 0.5f ? XMFLOAT3{1, 0, 0} : XMFLOAT3{0, 1, 0};
        XMVECTOR N = XMLoadFloat3(&n), U = XMLoadFloat3(&u), W = XMVector3Cross(N, U);
        XMFLOAT3 w;
        XMStoreFloat3(&w, W);
        uint16_t base = uint16_t(cv.size());
        for (int k = 0; k < 4; k++) {
            float a = (k == 0 || k == 3) ? -0.5f : 0.5f, b = (k < 2) ? -0.5f : 0.5f;
            XMFLOAT3 p{n.x * 0.5f + u.x * a + w.x * b, n.y * 0.5f + u.y * a + w.y * b + 0.5f, n.z * 0.5f + u.z * a + w.z * b};
            cv.push_back({p, n});
        }
        uint16_t idx[6] = {0, 1, 2, 0, 2, 3};
        // Wind clockwise as seen from outside (D3D front faces).
        XMVECTOR e1 = XMVectorSubtract(XMLoadFloat3(&cv[base + 1].p), XMLoadFloat3(&cv[base].p));
        XMVECTOR e2 = XMVectorSubtract(XMLoadFloat3(&cv[base + 2].p), XMLoadFloat3(&cv[base].p));
        float s = XMVectorGetX(XMVector3Dot(XMVector3Cross(e1, e2), N));
        if (s > 0) { std::swap(idx[1], idx[2]); std::swap(idx[4], idx[5]); }
        for (uint16_t i : idx) ci.push_back(uint16_t(base + i));
    }
    D3D11_BUFFER_DESC vbd{};
    vbd.ByteWidth = UINT(cv.size() * sizeof(V));
    vbd.Usage = D3D11_USAGE_IMMUTABLE;
    vbd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA sd{cv.data(), 0, 0};
    dev->CreateBuffer(&vbd, &sd, &cube_vb_);
    vbd.ByteWidth = UINT(ci.size() * 2);
    vbd.BindFlags = D3D11_BIND_INDEX_BUFFER;
    sd.pSysMem = ci.data();
    dev->CreateBuffer(&vbd, &sd, &cube_ib_);

    D3D11_SAMPLER_DESC smp{};
    smp.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    smp.AddressU = smp.AddressV = smp.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    smp.MaxLOD = D3D11_FLOAT32_MAX;
    dev->CreateSamplerState(&smp, &sampler_);
    smp.Filter = D3D11_FILTER_ANISOTROPIC;
    smp.MaxAnisotropy = 4;
    smp.AddressU = smp.AddressV = smp.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    dev->CreateSamplerState(&smp, &wrap_sampler_);
    // Mirror addressing per material tiling (U_MIRROR / V_MIRROR): mirror U, mirror V, both.
    for (int k = 1; k <= 3; k++) {
        smp.AddressU = k & 1 ? D3D11_TEXTURE_ADDRESS_MIRROR : D3D11_TEXTURE_ADDRESS_WRAP;
        smp.AddressV = k & 2 ? D3D11_TEXTURE_ADDRESS_MIRROR : D3D11_TEXTURE_ADDRESS_WRAP;
        dev->CreateSamplerState(&smp, &mirror_samplers_[k - 1]);
    }
    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_BACK;
    rd.DepthClipEnable = TRUE;
    dev->CreateRasterizerState(&rd, &rs_solid_);
    rd.CullMode = D3D11_CULL_NONE;
    dev->CreateRasterizerState(&rd, &rs_nocull_);
    D3D11_DEPTH_STENCIL_DESC dsd{};
    dsd.DepthEnable = TRUE;
    dsd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    dsd.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
    dev->CreateDepthStencilState(&dsd, &ds_on_);
    dsd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    dev->CreateDepthStencilState(&dsd, &ds_nowrite_);
    dsd.DepthEnable = FALSE;
    dev->CreateDepthStencilState(&dsd, &ds_off_);
    D3D11_BLEND_DESC bld{};
    bld.RenderTarget[0].BlendEnable = TRUE;
    bld.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    bld.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    bld.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bld.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    bld.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    bld.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bld.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    dev->CreateBlendState(&bld, &bs_alpha_);
    bld.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;
    dev->CreateBlendState(&bld, &bs_add_);
    bld.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
    bld.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;
    bld.RenderTarget[0].BlendOp = D3D11_BLEND_OP_REV_SUBTRACT;
    dev->CreateBlendState(&bld, &bs_sub_);
    bld.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
    bld.RenderTarget[0].DestBlend = D3D11_BLEND_SRC1_COLOR;
    bld.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bld.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO;
    bld.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
    dev->CreateBlendState(&bld, &bs_water_);
    bld.RenderTarget[0].SrcBlend = D3D11_BLEND_DEST_COLOR;  // dst x src x 2
    bld.RenderTarget[0].DestBlend = D3D11_BLEND_SRC_COLOR;
    bld.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO;
    bld.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
    dev->CreateBlendState(&bld, &bs_mod2x_);
    {  // Bloom (a failure only loses the glow)
        ID3DBlob *bv = nullptr, *bp = nullptr;
        bool ok = compile_bloom("bvs", "vs_4_0", &bv) && SUCCEEDED(dev->CreateVertexShader(bv->GetBufferPointer(), bv->GetBufferSize(), nullptr, &bloom_vs_));
        auto mk = [&](const char* e, ID3D11PixelShader** o) {
            ok = ok && compile_bloom(e, "ps_4_0", &bp) && SUCCEEDED(dev->CreatePixelShader(bp->GetBufferPointer(), bp->GetBufferSize(), nullptr, o));
            release(bp);
        };
        mk("bps_down", &bloom_ps_down_); mk("bps_box", &bloom_ps_box_); mk("bps_blur", &bloom_ps_blur_); mk("bps_copy", &bloom_ps_copy_);
        release(bv);
        if (!ok) params.bloom = false;
        D3D11_BUFFER_DESC cbd{};
        cbd.ByteWidth = 48; cbd.Usage = D3D11_USAGE_DYNAMIC; cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER; cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        dev->CreateBuffer(&cbd, nullptr, &bloom_cb_);
        D3D11_BLEND_DESC bb{};
        bb.RenderTarget[0].BlendEnable = TRUE;
        bb.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE; bb.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;
        bb.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE; bb.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
        bb.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN | D3D11_COLOR_WRITE_ENABLE_BLUE;
        bb.RenderTarget[0].BlendOp = bb.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_MAX;
        dev->CreateBlendState(&bb, &bs_bmax_);
        bb.RenderTarget[0].BlendOp = bb.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        dev->CreateBlendState(&bb, &bs_badd_);
        D3D11_SAMPLER_DESC sd{};
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR; sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP; sd.MaxLOD = D3D11_FLOAT32_MAX;
        dev->CreateSamplerState(&sd, &bloom_samp_);
    }
    // Placeholder textures.
    ma2::Image white;
    white.w = white.h = 1;
    white.px = {0xFF808080u};
    set_colormap(white);
    white.px = {0xFFFFFFFFu};
    set_lightmap(white);
    clear_overlay();
    return true;
}

// Particle sprites: camera-facing quads, alpha ones back to front, then additive, then subtractive.
void Renderer::draw_sprites(const Camera& cam) {
    struct SV { XMFLOAT3 p; XMFLOAT2 uv; uint32_t c; };
    const XMFLOAT4X4& v = cam.view;
    XMFLOAT3 right{v._11, v._21, v._31}, up{v._12, v._22, v._32}, fwd{v._13, v._23, v._33};
    std::vector<size_t> order;
    order.reserve(sprites.size());
    for (size_t i = 0; i < sprites.size(); i++) if (sprites[i].tex) order.push_back(i);
    auto dist2 = [&](const Sprite& s) {
        float dx = s.p.x - cam.eye.x, dy = s.p.y - cam.eye.y, dz = s.p.z - cam.eye.z;
        return dx * dx + dy * dy + dz * dz;
    };
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        const Sprite &sa = sprites[a], &sb = sprites[b];
        if (sa.blend != sb.blend) return sa.blend < sb.blend;
        if (sa.blend == 0) return dist2(sa) > dist2(sb);
        return sa.tex < sb.tex;
    });
    std::vector<SV> verts;
    verts.reserve(order.size() * 6);
    struct Run { UINT first, count; ID3D11ShaderResourceView* tex; int blend; };
    std::vector<Run> runs;
    auto pack = [](const float c[4]) {
        auto u = [](float x) { return uint32_t(std::clamp(x, 0.f, 1.f) * 255.f + 0.5f); };
        return u(c[0]) | u(c[1]) << 8 | u(c[2]) << 16 | u(c[3]) << 24;
    };
    for (size_t idx : order) {
        const Sprite& s = sprites[idx];
        XMFLOAT3 r, u;
        if (s.orient) {
            float vl = std::sqrt(s.vel.x * s.vel.x + s.vel.y * s.vel.y + s.vel.z * s.vel.z);
            if (vl > 1e-3f) {
                XMFLOAT3 d{s.vel.x / vl, s.vel.y / vl, s.vel.z / vl};
                XMVECTOR cr = XMVector3Cross(XMLoadFloat3(&d), XMLoadFloat3(&fwd));
                XMFLOAT3 c3;
                XMStoreFloat3(&c3, XMVector3Normalize(cr));
                u = d;
                r = c3;
            } else { r = right; u = up; }
        } else {
            float ca = std::cos(s.angle), sa = std::sin(s.angle);
            r = {right.x * ca + up.x * sa, right.y * ca + up.y * sa, right.z * ca + up.z * sa};
            u = {-right.x * sa + up.x * ca, -right.y * sa + up.y * ca, -right.z * sa + up.z * ca};
        }
        float hw = s.w * 0.5f, hh = s.h * 0.5f;
        XMFLOAT3 c{s.p.x - s.pivot_x * s.w * r.x - s.pivot_y * s.h * u.x, s.p.y - s.pivot_x * s.w * r.y - s.pivot_y * s.h * u.y,
                   s.p.z - s.pivot_x * s.w * r.z - s.pivot_y * s.h * u.z};
        auto P = [&](float a, float b) { return XMFLOAT3{c.x + r.x * a + u.x * b, c.y + r.y * a + u.y * b, c.z + r.z * a + u.z * b}; };
        uint32_t col = pack(s.rgba);
        SV q[4] = {{P(-hw, hh), {0, 0}, col}, {P(hw, hh), {1, 0}, col}, {P(hw, -hh), {1, 1}, col}, {P(-hw, -hh), {0, 1}, col}};
        if (runs.empty() || runs.back().tex != s.tex || runs.back().blend != s.blend) runs.push_back({UINT(verts.size()), 0, s.tex, s.blend});
        for (int k : {0, 1, 2, 0, 2, 3}) verts.push_back(q[k]);
        runs.back().count += 6;
    }
    if (verts.empty()) return;
    if (verts.size() > sprite_cap_) {
        release(sprite_vb_);
        sprite_cap_ = UINT(verts.size() + 4096);
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = sprite_cap_ * sizeof(SV);
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(dev_->CreateBuffer(&bd, nullptr, &sprite_vb_))) { sprite_vb_ = nullptr; sprite_cap_ = 0; return; }
    }
    D3D11_MAPPED_SUBRESOURCE m;
    if (!sprite_vb_ || FAILED(ctx_->Map(sprite_vb_, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return;
    std::memcpy(m.pData, verts.data(), verts.size() * sizeof(SV));
    ctx_->Unmap(sprite_vb_, 0);
    UINT stride = sizeof(SV), off = 0;
    ctx_->IASetInputLayout(sprite_il_);
    ctx_->IASetVertexBuffers(0, 1, &sprite_vb_, &stride, &off);
    ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx_->VSSetShader(sprite_vs_, nullptr, 0);
    ctx_->PSSetShader(sprite_ps_, nullptr, 0);
    ctx_->PSSetConstantBuffers(3, 1, &sprite_cb_);
    ctx_->PSSetSamplers(0, 1, &sampler_);
    ctx_->RSSetState(rs_nocull_);
    ctx_->OMSetDepthStencilState(ds_nowrite_, 0);
    int cur_blend = -1;
    for (const Run& run : runs) {
        if (run.blend != cur_blend) {
            cur_blend = run.blend;
            float f[4] = {cur_blend == 1 ? 1.f : 0.f, 0, 0, 0};
            ctx_->UpdateSubresource(sprite_cb_, 0, nullptr, f, 0, 0);
            ctx_->OMSetBlendState(cur_blend == 1 ? bs_add_ : cur_blend == 2 ? bs_sub_ : bs_alpha_, nullptr, 0xFFFFFFFF);
        }
        ctx_->PSSetShaderResources(3, 1, &run.tex);
        ctx_->Draw(run.count, run.first);
    }
    ID3D11ShaderResourceView* none = nullptr;
    ctx_->PSSetShaderResources(3, 1, &none);
    ctx_->OMSetBlendState(bs_alpha_, nullptr, 0xFFFFFFFF);
}

// Stage-dump selector for the shaders (see the MA2SE_WATER_DEBUG comments above): env var at start, or the --stage flag at run time.
int g_bloom_dbg = 0;
float g_sky_fog_shift = 0;  // developer (--sky-fog-shift): metres added to the sky-fog height h
float g_sky_heading_add = 0;  // developer (--sky-heading-add): radians added to the sky dome's heading
int g_sky_fog_force = -1;   // developer (--sky-fog 0/1): force the sky fog off / on
float g_dbg_w = [] { const char* e = getenv("MA2SE_WATER_DEBUG"); return e ? 1.f + float(atoi(e)) : 1.f; }();

void Renderer::shutdown() {
    release(sprite_vs_); release(sprite_ps_); release(sprite_il_); release(sprite_vb_); release(sprite_cb_); sprite_cap_ = 0;
    release(terrain_vs_); release(box_vs_); release(line_vs_); release(outside_vs_);
    release(terrain_ps_); release(box_ps_); release(line_ps_); release(outside_ps_); release(outside_vb_);
    release(terrain_il_); release(box_il_); release(line_il_);
    release(cb_); release(terrain_vb_); release(terrain_ib_);
    release(cube_vb_); release(cube_ib_); release(inst_vb_); release(line_vb_);
    release(colormap_srv_); release(lightmap_srv_); release(overlay_srv_); release(minimap_srv_);
    release(gw_srv_[0]); release(gw_srv_[1]); release(gl_srv_); release(gw_tex_[0]); release(gw_tex_[1]); release(gl_tex_);
    release(colormap_); release(lightmap_); release(overlay_); release(minimap_);
    release(wprev_vs_); release(wprev_ps_); release(wprev_cb_);
    for (auto& [k, w] : water_pics_) { release(w.srv); release(w.tex); }
    water_pics_.clear(); water_pending_.clear();
    for (auto& [k, w] : sky_pics_) { release(w.srv); release(w.tex); }
    sky_pics_.clear(); sky_pending_.clear();
    release(sampler_); release(rs_solid_); release(rs_nocull_);
    release(ds_on_); release(ds_nowrite_); release(ds_off_); release(bs_alpha_);
    release(model_vs_); release(model_ps_); release(model_il_); release(mesh_cb_); release(model_inst_vb_); release(sky_inst_vb_);
    release(wrap_sampler_); for (auto& t : tex_samplers_) release(t.second); tex_samplers_.clear(); release(water_vs_); release(water_ps_); release(bs_add_); release(bs_sub_);
    free_rt(mask_); free_rt(bl0_); free_rt(bl1_); free_rt(bl2_);
    release(bloom_vs_); release(bloom_ps_down_); release(bloom_ps_box_); release(bloom_ps_blur_); release(bloom_ps_copy_);
    release(bloom_cb_); release(bs_bmax_); release(bs_badd_); release(bloom_samp_);
    release(bs_water_); release(bs_mod2x_); release(lights_cb_); release(lightpool_srv_); release(lightpool_); lights_hash_ = 0; for (auto*& s : mirror_samplers_) release(s); release(height_srv_); release(height_); release(scene_copy_srv_); release(scene_copy_);
    end_frame();
    inst_cap_ = line_cap_ = model_inst_cap_ = 0;
    tw_ = th_ = 0;
    tverts_.clear();
}

void Renderer::draw_sky(const Camera& cam) {
    const GpuModel& g = *sky.model;
    if (!sky_inst_vb_) {
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = sizeof(BoxInstance);
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(dev_->CreateBuffer(&bd, nullptr, &sky_inst_vb_))) return;
    }
    // Centred on the camera like a sky box, with the world's scale and offset; shrunk when the
    // dome would reach past the far plane (it is drawn without depth, so only its shape matters).
    float r = 0;
    for (int k : {0, 2}) r = std::max({r, std::fabs(g.bmin[k]), std::fabs(g.bmax[k])});
    // The game scales the dome uniformly (K = 10000 for every sky; UC sky-fog trace: the matrices are K x the model's node
    // worlds, V = P + offset x K / scale.y), so SkyTerrainSkyScale.y alone sizes it; x / z are unused.
    float sx = sky.scale.y, sy = sky.scale.y, sz = sky.scale.y;
    float reach = r * std::max(sx, sz), f = reach > 12000.f ? 12000.f / reach : 1.f;
    sx *= f; sy *= f; sz *= f;
    float ch = std::cos(sky.heading + g_sky_heading_add), sh = std::sin(sky.heading + g_sky_heading_add);
    BoxInstance bi;
    bi.r0 = {ch * sx, 0, sh * sz, cam.eye.x + sky.offset.x * f};
    bi.r1 = {0, sy, 0, cam.eye.y + sky.offset.y * f};
    bi.r2 = {-sh * sx, 0, ch * sz, cam.eye.z + sky.offset.z * f};
    bi.color = {1, 1, 1, 1};
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ctx_->Map(sky_inst_vb_, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return;
    std::memcpy(m.pData, &bi, sizeof bi);
    ctx_->Unmap(sky_inst_vb_, 0);
    ctx_->IASetInputLayout(model_il_);
    ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx_->VSSetShader(model_vs_, nullptr, 0);
    ctx_->PSSetShader(model_ps_, nullptr, 0);
    ctx_->PSSetConstantBuffers(1, 1, &mesh_cb_);
    ctx_->OMSetDepthStencilState(ds_off_, 0);
    ctx_->RSSetState(rs_nocull_);
    // Opaque / cutout, then the additive layers (stars, moons, glows), then the alpha layers (clouds, mountains): the game's
    // mountains stay black against the sky (Mission_02 / 03 pairs); with the additive layers last they were lifted brown.
    for (int pass = 0; pass < 3; pass++)
        for (const GpuMesh& mesh : g.meshes) {
            bool tr = mesh.alpha >= 2;
            if ((mesh.alpha < 2 ? 0 : mesh.alpha >= 3 ? 1 : 2) != pass) continue;
            if (sky.hide_additive && mesh.alpha == 3) continue;  // WC Env_Sky_Stormy: the game never draws the additive duplicate domes
            if (SUCCEEDED(ctx_->Map(mesh_cb_, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
                float* c = static_cast<float*>(m.pData);
                std::memset(c, 0, kMeshCB);
                c[0] = mesh.skycol[0]; c[1] = mesh.skycol[1]; c[2] = mesh.skycol[2]; c[3] = mesh.ramp.on ? mesh.ramp.at(params.time) : mesh.color[3];
                c[4] = float(mesh.alpha); c[5] = 1.f; c[6] = mesh.srv ? 1.f : 0.f; c[7] = 1;
                c[28] = 2.f;  // sky marker (cutout layers blend by texture alpha)
                c[29] = mesh.cshift[0]; c[30] = mesh.cshift[1]; c[31] = mesh.cshift[2];
                if ((g_sky_fog_force < 0 ? sky.fog_on : g_sky_fog_force != 0) && sky.fog_top > sky.fog_bottom) {
                    c[32] = sky.fog_color.x; c[33] = sky.fog_color.y; c[34] = sky.fog_color.z; c[35] = mesh.alpha == 3 ? 2.f : 1.f;
                    c[36] = sky.fog_top; c[37] = 1.f / (sky.fog_top - sky.fog_bottom); c[39] = 10000.f / std::max(sy, 1e-3f);
                    // UC sky-fog trace (per-vertex fog dumped from the game): q = (Top - y) / (Top - Bottom) exactly (resid 0), y the dome
                    // vertex's ABSOLUTE world height = P.y + 10000 x (model_y + offset.y / scale.y), P = the player (about 3 m above the ground).
                    float ref_y = std::min(cam.eye.y, height_at(cam.eye.x, cam.eye.z) + 3.f);
                    c[38] = ref_y - cam.eye.y * c[39] + g_sky_fog_shift;
                }
                ctx_->Unmap(mesh_cb_, 0);
            }
            ctx_->OMSetBlendState(!tr ? (mesh.alpha == 1 ? bs_alpha_ : nullptr) : mesh.alpha == 3 ? bs_add_ : mesh.alpha == 4 ? bs_sub_ : bs_alpha_, nullptr, 0xFFFFFFFF);
            ID3D11Buffer* vbs[2] = {mesh.vb, sky_inst_vb_};
            UINT strides[2] = {kModelStride, sizeof(BoxInstance)}, offs[2] = {0, 0};
            ctx_->IASetVertexBuffers(0, 2, vbs, strides, offs);
            ctx_->IASetIndexBuffer(mesh.ib, DXGI_FORMAT_R32_UINT, 0);
            ID3D11ShaderResourceView* srv = mesh.srv;
            ctx_->PSSetShaderResources(3, 1, &srv);
            ID3D11SamplerState* smp = tex_sampler(mesh);
            ctx_->PSSetSamplers(1, 1, &smp);
            ctx_->DrawIndexedInstanced(mesh.count, 1, 0, 0, 0);
        }
    ctx_->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
    ctx_->OMSetDepthStencilState(ds_on_, 0);
    ctx_->RSSetState(rs_solid_);
}

// The sampler a mesh's material asks for: <basetexture> filter (point / linear), mipmapping and mipmap_bias, plus its
// mirror addressing, trilinear without anisotropy as the game samples (M03 sky trace: Sky_034_Stars bias 0.5 ->
// TEXFILTER 02062080; Sky_042_Clouds_Citynight mipmapping false + FILTER_POINT -> 01012000).
ID3D11SamplerState* Renderer::tex_sampler(const GpuMesh& m) {
    int bias = int(std::lround(m.tex_bias * 16.f));
    int key = (m.tex_point ? 1 : 0) | (m.tex_nomip ? 2 : 0) | (m.address & 3) << 2 | (bias + 4096) << 4;
    auto it = tex_samplers_.find(key);
    if (it != tex_samplers_.end()) return it->second;
    D3D11_SAMPLER_DESC sd{};
    sd.Filter = m.tex_point ? D3D11_FILTER_MIN_MAG_MIP_POINT : D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = m.address & 1 ? D3D11_TEXTURE_ADDRESS_MIRROR : D3D11_TEXTURE_ADDRESS_WRAP;
    sd.AddressV = m.address & 2 ? D3D11_TEXTURE_ADDRESS_MIRROR : D3D11_TEXTURE_ADDRESS_WRAP;
    sd.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    sd.MipLODBias = m.tex_bias;
    sd.MaxLOD = m.tex_nomip ? 0.f : D3D11_FLOAT32_MAX;
    ID3D11SamplerState* smp = nullptr;
    dev_->CreateSamplerState(&sd, &smp);
    return tex_samplers_[key] = smp;
}

// Copies the bound render target (the frame so far) into scene_copy_ for the water's refraction.
bool Renderer::copy_scene() {
    ID3D11RenderTargetView* rtv = nullptr;
    ctx_->OMGetRenderTargets(1, &rtv, nullptr);
    if (!rtv) return false;
    ID3D11Resource* res = nullptr;
    rtv->GetResource(&res);
    rtv->Release();
    ID3D11Texture2D* rt = nullptr;
    if (!res || FAILED(res->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&rt))) { release(res); return false; }
    res->Release();
    D3D11_TEXTURE2D_DESC d;
    rt->GetDesc(&d);
    D3D11_TEXTURE2D_DESC have{};
    if (scene_copy_) scene_copy_->GetDesc(&have);
    if (!scene_copy_ || have.Width != d.Width || have.Height != d.Height || have.Format != d.Format) {
        retire(scene_copy_srv_);
        retire(scene_copy_);
        scene_copy_srv_ = nullptr;
        scene_copy_ = nullptr;
        D3D11_TEXTURE2D_DESC c = d;
        c.MipLevels = 1; c.ArraySize = 1; c.SampleDesc = {1, 0};
        c.Usage = D3D11_USAGE_DEFAULT; c.BindFlags = D3D11_BIND_SHADER_RESOURCE; c.CPUAccessFlags = 0; c.MiscFlags = 0;
        if (FAILED(dev_->CreateTexture2D(&c, nullptr, &scene_copy_)) || FAILED(dev_->CreateShaderResourceView(scene_copy_, nullptr, &scene_copy_srv_))) {
            release(scene_copy_); release(scene_copy_srv_); rt->Release();
            return false;
        }
    }
    if (d.SampleDesc.Count > 1) ctx_->ResolveSubresource(scene_copy_, 0, rt, 0, d.Format);
    else ctx_->CopyResource(scene_copy_, rt);
    rt->Release();
    return true;
}

void Renderer::draw_models(int pass) {
    bool transparent = pass != 0;
    if (models.empty() || !model_inst_vb_) return;
    ctx_->IASetInputLayout(model_il_);
    ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx_->VSSetShader(model_vs_, nullptr, 0);
    ctx_->PSSetShader(model_ps_, nullptr, 0);
    ctx_->PSSetConstantBuffers(1, 1, &mesh_cb_);
    ctx_->PSSetSamplers(1, 1, &wrap_sampler_);
    ctx_->PSSetShaderResources(13, 1, &env_cube);
    ctx_->PSSetShaderResources(1, 1, &lightmap_srv_);
    // Bloom mask: the model pass writes a second target (R8) beside the frame; decals and everything else leave it alone.
    ID3D11RenderTargetView* rtv0 = nullptr;
    ID3D11DepthStencilView* dsv0 = nullptr;
    bool mrt = false, have_mask = params.bloom && mask_.rtv;
    if (have_mask) ctx_->OMGetRenderTargets(1, &rtv0, &dsv0);
    have_mask = have_mask && rtv0;
    for (size_t b = 0; b < models.size(); b++) {
        const ModelBatch& mb = models[b];
        if (!mb.model || mb.inst.empty()) continue;
        for (const GpuMesh& g : mb.model->meshes) {
            int mp = g.decal ? 2 : g.alpha >= 2 ? 1 : 0;
            if (mp != pass || (g.lamp_part && !params.night_time) || (g.night_light && !params.night_time)) continue;
            if (have_mask) {
                bool want = !g.decal;
                if (want != mrt) {
                    ID3D11RenderTargetView* two[2] = {rtv0, mask_.rtv};
                    ctx_->OMSetRenderTargets(want ? 2 : 1, two, dsv0);
                    mrt = want;
                }
                if (want && g.bloom_kind) bloom_drawn_ = true;
            }
            D3D11_MAPPED_SUBRESOURCE m;
            if (SUCCEEDED(ctx_->Map(mesh_cb_, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
                float* f = static_cast<float*>(m.pData);
                f[0] = g.color[0]; f[1] = g.color[1]; f[2] = g.color[2]; f[3] = g.color[3];
                f[4] = float(g.alpha); f[5] = g.unlit || g.decal ? 1.f : 0.f; f[6] = g.srv ? 1.f : 0.f; f[7] = g.decal ? 1.f : 0.f;
                for (int k = 0; k < 3; k++) { f[8 + k] = g.glow[0][k]; f[12 + k] = g.glow[1][k]; }
                f[11] = g.multi ? 1.f : 0.f; f[15] = g.reflectivity;
                f[16] = float(g.detail ? g.detail_mode : 0); f[17] = g.tint1 ? 1.f : 0.f;
                f[18] = g.normal ? 1.f : 0.f; f[19] = g.vcol_glow ? 1.f : 0.f;
                f[24] = g.detail_level; f[25] = g.multi ? 1.f : 0.f; f[26] = g.cube || env_cube ? 1.f : 0.f; f[27] = float(g.detail_kind + (g.complex_env ? 4 : 0));
                f[20] = g.spec[0]; f[21] = g.spec[1]; f[22] = g.spec[2]; f[28] = mb.receiver ? 1.f : 0.f;
                f[29] = g.cshift[0]; f[30] = g.cshift[1]; f[31] = g.cshift[2];
                f[23] = (g.cube || env_cube) && g.spec[0] + g.spec[1] + g.spec[2] > 0 ? (g.spec_crystal ? 4.f : g.spec_detail ? 3.f : g.spec_multi ? 2.f : 1.f) : 0.f;
                f[40] = g.bloom[0]; f[41] = g.bloom[1]; f[42] = g.bloom[2]; f[43] = float(g.bloom_kind);
                f[44] = g.translucent ? 1.f : 0.f;
                if (mb.tint) { f[45] = float((mb.tint >> 16) & 255) / 255.f; f[46] = float((mb.tint >> 8) & 255) / 255.f; f[47] = float(mb.tint & 255) / 255.f; }
                else f[45] = f[46] = f[47] = -1.f;  // mshade.yzw: entity tint colour, negative = none
                ctx_->Unmap(mesh_cb_, 0);
            }
            ctx_->PSSetSamplers(1, 1, g.address ? &mirror_samplers_[g.address - 1] : &wrap_sampler_);
            if (transparent) {
                ctx_->OMSetBlendState(g.decal ? bs_mod2x_ : g.alpha == 3 ? bs_add_ : g.alpha == 4 ? bs_sub_ : bs_alpha_, nullptr, 0xFFFFFFFF);
                ctx_->OMSetDepthStencilState(ds_nowrite_, 0);
            }
            ctx_->RSSetState(g.two_sided || transparent ? rs_nocull_ : rs_solid_);
            ID3D11Buffer* vbs[2] = {g.vb, model_inst_vb_};
            UINT strides[2] = {kModelStride, sizeof(BoxInstance)}, offs[2] = {0, 0};
            ctx_->IASetVertexBuffers(0, 2, vbs, strides, offs);
            ctx_->IASetIndexBuffer(g.ib, DXGI_FORMAT_R32_UINT, 0);
            ID3D11ShaderResourceView* srv = g.srv;
            ctx_->PSSetShaderResources(3, 1, &srv);
            ID3D11ShaderResourceView* mk = g.multi;
            ctx_->PSSetShaderResources(8, 1, &mk);
            ID3D11ShaderResourceView* dt = g.detail;
            ctx_->PSSetShaderResources(14, 1, &dt);
            ID3D11ShaderResourceView* nm = g.normal;
            ctx_->PSSetShaderResources(15, 1, &nm);
            ID3D11ShaderResourceView* cb = g.cube ? g.cube : env_cube;
            ctx_->PSSetShaderResources(13, 1, &cb);
            ctx_->DrawIndexedInstanced(g.count, UINT(mb.inst.size()), 0, 0, model_inst_start_[b]);
        }
    }
    if (mrt) ctx_->OMSetRenderTargets(1, &rtv0, dsv0);
    release(rtv0); release(dsv0);
    ctx_->RSSetState(rs_solid_);
    ctx_->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
    ctx_->OMSetDepthStencilState(ds_on_, 0);
    ctx_->PSSetSamplers(1, 1, &wrap_sampler_);
}

void Renderer::free_rt(Rt& r) {
    release(r.srv); release(r.rtv); release(r.tex);
    r.w = r.h = 0;
}

bool Renderer::make_rt(Rt& r, UINT w, UINT h, DXGI_FORMAT f) {
    if (r.tex && r.w == w && r.h == h) return true;
    free_rt(r);
    D3D11_TEXTURE2D_DESC d{};
    d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = 1; d.Format = f; d.SampleDesc = {1, 0};
    d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(dev_->CreateTexture2D(&d, nullptr, &r.tex)) || FAILED(dev_->CreateRenderTargetView(r.tex, nullptr, &r.rtv)) ||
        FAILED(dev_->CreateShaderResourceView(r.tex, nullptr, &r.srv))) { free_rt(r); return false; }
    r.w = w; r.h = h;
    return true;
}

bool Renderer::prepare_mask() {
    bloom_drawn_ = false;
    if (!params.bloom || !bloom_vs_ || !bloom_cb_) return false;
    ID3D11RenderTargetView* rtv = nullptr;
    ctx_->OMGetRenderTargets(1, &rtv, nullptr);
    if (!rtv) return false;
    ID3D11Resource* res = nullptr;
    rtv->GetResource(&res);
    release(rtv);
    ID3D11Texture2D* t = nullptr;
    D3D11_TEXTURE2D_DESC d{};
    bool ok = res && SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&t));
    release(res);
    if (!ok) return false;
    t->GetDesc(&d);
    release(t);
    if (d.SampleDesc.Count > 1 || !make_rt(mask_, d.Width, d.Height, DXGI_FORMAT_R8_UNORM)) return false;
    const float zero[4] = {0, 0, 0, 0};
    ctx_->ClearRenderTargetView(mask_.rtv, zero);
    return true;
}

// The post chain (see kBloom): frame x mask -> 1/2 -> 1/4 -> 4 blur passes -> max-merge into the 1/2 target -> added to the frame.
void Renderer::run_bloom(D3D11_VIEWPORT vp) {
    if (!params.bloom || !bloom_drawn_ || !mask_.srv || vp.Width < 8 || vp.Height < 8) return;
    ID3D11RenderTargetView* rtv = nullptr;
    ID3D11DepthStencilView* dsv = nullptr;
    ctx_->OMGetRenderTargets(1, &rtv, &dsv);
    if (!rtv || !copy_scene()) { release(rtv); release(dsv); return; }
    UINT bw = std::max(1u, UINT(vp.Width) / 2), bh = std::max(1u, UINT(vp.Height) / 2), qw = std::max(1u, bw / 2), qh = std::max(1u, bh / 2);
    if (!make_rt(bl0_, bw, bh, DXGI_FORMAT_R8G8B8A8_UNORM) || !make_rt(bl1_, qw, qh, DXGI_FORMAT_R8G8B8A8_UNORM) || !make_rt(bl2_, qw, qh, DXGI_FORMAT_R8G8B8A8_UNORM)) {
        release(rtv); release(dsv); return;
    }
    D3D11_TEXTURE2D_DESC sd{};
    scene_copy_->GetDesc(&sd);
    auto cbset = [&](float rx, float ry, float rw, float rh, float tx, float ty, float dx, float dy) {
        D3D11_MAPPED_SUBRESOURCE m;
        if (SUCCEEDED(ctx_->Map(bloom_cb_, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
            float* f = static_cast<float*>(m.pData);
            f[0] = rx; f[1] = ry; f[2] = rw; f[3] = rh; f[4] = tx; f[5] = ty; f[6] = f[7] = 0; f[8] = dx; f[9] = dy; f[10] = f[11] = 0;
            ctx_->Unmap(bloom_cb_, 0);
        }
    };
    auto pass = [&](Rt* dst, ID3D11PixelShader* ps, ID3D11ShaderResourceView* a, ID3D11ShaderResourceView* b, ID3D11BlendState* bs) {
        ID3D11ShaderResourceView* none[2] = {};
        ctx_->PSSetShaderResources(0, 2, none);
        ctx_->OMSetRenderTargets(1, &dst->rtv, nullptr);
        D3D11_VIEWPORT v{0, 0, float(dst->w), float(dst->h), 0, 1};
        ctx_->RSSetViewports(1, &v);
        ctx_->OMSetBlendState(bs, nullptr, 0xFFFFFFFF);
        ctx_->PSSetShader(ps, nullptr, 0);
        ID3D11ShaderResourceView* in[2] = {a, b};
        ctx_->PSSetShaderResources(0, 2, in);
        ctx_->Draw(3, 0);
    };
    ctx_->IASetInputLayout(nullptr);
    ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx_->VSSetShader(bloom_vs_, nullptr, 0);
    ctx_->PSSetConstantBuffers(3, 1, &bloom_cb_);
    ctx_->PSSetSamplers(0, 1, &bloom_samp_);
    ctx_->RSSetState(rs_nocull_);
    ctx_->OMSetDepthStencilState(ds_off_, 0);
    cbset(vp.TopLeftX / sd.Width, vp.TopLeftY / sd.Height, vp.Width / sd.Width, vp.Height / sd.Height, 1.f / sd.Width, 1.f / sd.Height, 0, 0);
    pass(&bl0_, bloom_ps_down_, scene_copy_srv_, mask_.srv, nullptr);
    cbset(0, 0, 1, 1, 1.f / bw, 1.f / bh, 0, 0);
    pass(&bl1_, bloom_ps_box_, bl0_.srv, nullptr, nullptr);
    cbset(0, 0, 1, 1, 1.f / qw, 1.f / qh, 0, 1);
    pass(&bl2_, bloom_ps_blur_, bl1_.srv, nullptr, nullptr);
    cbset(0, 0, 1, 1, 1.f / qw, 1.f / qh, 1, 0);
    pass(&bl1_, bloom_ps_blur_, bl2_.srv, nullptr, nullptr);
    cbset(0, 0, 1, 1, 1.f / qw, 1.f / qh, 0, 1);
    pass(&bl2_, bloom_ps_blur_, bl1_.srv, nullptr, nullptr);
    cbset(0, 0, 1, 1, 1.f / qw, 1.f / qh, 1, 0);
    pass(&bl1_, bloom_ps_blur_, bl2_.srv, nullptr, nullptr);
    pass(&bl0_, bloom_ps_copy_, bl1_.srv, nullptr, bs_bmax_);
    // Added to the frame.
    ID3D11ShaderResourceView* none[2] = {};
    ctx_->PSSetShaderResources(0, 2, none);
    ctx_->OMSetRenderTargets(1, &rtv, dsv);
    ctx_->RSSetViewports(1, &vp);
    ctx_->OMSetBlendState(bs_badd_, nullptr, 0xFFFFFFFF);
    ctx_->PSSetShader(bloom_ps_copy_, nullptr, 0);
    ctx_->PSSetShaderResources(0, 1, &bl0_.srv);
    if (g_bloom_dbg) {  // developer: --bloom 2 shows the bloom alone on black, --bloom 3 the mask (red) on black
        const float black[4] = {0, 0, 0, 1};
        ctx_->ClearRenderTargetView(rtv, black);
        if (g_bloom_dbg > 1) {
            ctx_->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF); ctx_->PSSetShaderResources(0, 1, &mask_.srv);
            D3D11_VIEWPORT full{0, 0, float(sd.Width), float(sd.Height), 0, 1};
            ctx_->RSSetViewports(1, &full);
        }
    }
    ctx_->Draw(3, 0);
    ctx_->PSSetShaderResources(0, 2, none);
    ctx_->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
    ctx_->OMSetDepthStencilState(ds_on_, 0);
    ctx_->RSSetState(rs_solid_);
    ctx_->PSSetSamplers(0, 1, &sampler_);
    ctx_->PSSetConstantBuffers(0, 1, &cb_);
    release(rtv); release(dsv);
}

// Textures replaced during a frame may still be referenced by that frame's ImGui draw list (the
// minimap and colour-map previews are drawn before strokes finish), so they are released only
// after the frame is presented.
void Renderer::retire(IUnknown* p) {
    if (p) retired_.push_back(p);
}

ID3D11ShaderResourceView* Renderer::water_preview(const WaterLook& w) {
    auto it = water_pics_.find(w.key);
    if (it != water_pics_.end()) return it->second.srv;
    if (!wprev_ps_ || !w.wave && !w.refl) return nullptr;
    for (auto& p : water_pending_) if (p.key == w.key) return nullptr;
    water_pending_.push_back(w);
    return nullptr;
}

// Draws the queued water pictures (128 x 128) before the scene; the frame's render target and viewport are put back.
void Renderer::build_water_previews() {
    if (water_pending_.empty()) return;
    std::vector<WaterLook> todo;
    todo.swap(water_pending_);
    ID3D11RenderTargetView* old_rtv = nullptr;
    ID3D11DepthStencilView* old_dsv = nullptr;
    ctx_->OMGetRenderTargets(1, &old_rtv, &old_dsv);
    D3D11_VIEWPORT old_vp;
    UINT nvp = 1;
    ctx_->RSGetViewports(&nvp, &old_vp);
    for (const WaterLook& w : todo) {
        WaterPic pic;
        D3D11_TEXTURE2D_DESC td{};
        td.Width = td.Height = 128;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        ID3D11RenderTargetView* rtv = nullptr;
        if (SUCCEEDED(dev_->CreateTexture2D(&td, nullptr, &pic.tex)) && SUCCEEDED(dev_->CreateShaderResourceView(pic.tex, nullptr, &pic.srv)) &&
            SUCCEEDED(dev_->CreateRenderTargetView(pic.tex, nullptr, &rtv))) {
            auto c4 = [](uint32_t c, float* f) { f[0] = float(c & 255) / 255; f[1] = float((c >> 8) & 255) / 255; f[2] = float((c >> 16) & 255) / 255; f[3] = 1; };
            float cb[12];
            c4(w.deep, cb);
            c4(w.shallow, cb + 4);
            cb[8] = w.fresnel_min; cb[9] = w.fresnel_max; cb[10] = w.wave ? 1.f : 0.f; cb[11] = w.refl ? 1.f : 0.f;
            ctx_->UpdateSubresource(wprev_cb_, 0, nullptr, cb, 0, 0);
            ctx_->OMSetRenderTargets(1, &rtv, nullptr);
            D3D11_VIEWPORT vp{0, 0, 128, 128, 0, 1};
            ctx_->RSSetViewports(1, &vp);
            ctx_->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
            ctx_->OMSetDepthStencilState(ds_off_, 0);
            ctx_->RSSetState(rs_nocull_);
            ctx_->IASetInputLayout(nullptr);
            ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            ctx_->VSSetShader(wprev_vs_, nullptr, 0);
            ctx_->PSSetShader(wprev_ps_, nullptr, 0);
            ctx_->PSSetConstantBuffers(0, 1, &wprev_cb_);
            ctx_->PSSetSamplers(0, 1, &wrap_sampler_);
            ID3D11ShaderResourceView* srvs[2] = {w.wave, w.refl};
            ctx_->PSSetShaderResources(0, 2, srvs);
            ctx_->Draw(3, 0);
            ID3D11ShaderResourceView* none[2] = {};
            ctx_->PSSetShaderResources(0, 2, none);
            rtv->Release();
        } else {
            release(pic.srv);
            release(pic.tex);
        }
        water_pics_[w.key] = pic;
    }
    ctx_->PSSetConstantBuffers(0, 1, &cb_);
    ctx_->OMSetRenderTargets(1, &old_rtv, old_dsv);
    ctx_->RSSetViewports(1, &old_vp);
    release(old_rtv);
    release(old_dsv);
}

ID3D11ShaderResourceView* Renderer::sky_preview(const SkyLook& k) {
    auto it = sky_pics_.find(k.key);
    if (it != sky_pics_.end()) return it->second.srv;
    if (!k.model || k.model->state != GpuModel::Ready || k.model->meshes.empty()) return nullptr;
    for (auto& p : sky_pending_) if (p.key == k.key) return nullptr;
    sky_pending_.push_back(k);
    return nullptr;
}

// Draws the queued sky pictures before the scene with draw_sky and a camera looking north, 17 degrees up;
// the frame's render target, viewport and sky are put back (render() then rewrites the constant buffer).
void Renderer::build_sky_previews() {
    if (sky_pending_.empty()) return;
    std::vector<SkyLook> todo;
    todo.swap(sky_pending_);
    ID3D11RenderTargetView* old_rtv = nullptr;
    ID3D11DepthStencilView* old_dsv = nullptr;
    ctx_->OMGetRenderTargets(1, &old_rtv, &old_dsv);
    D3D11_VIEWPORT old_vp;
    UINT nvp = 1;
    ctx_->RSGetViewports(&nvp, &old_vp);
    const Sky saved = sky;
    const UINT W = 128, H = 64;
    for (const SkyLook& k : todo) {
        WaterPic pic;
        D3D11_TEXTURE2D_DESC td{};
        td.Width = W;
        td.Height = H;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        ID3D11RenderTargetView* rtv = nullptr;
        if (k.model->state == GpuModel::Ready && SUCCEEDED(dev_->CreateTexture2D(&td, nullptr, &pic.tex)) &&
            SUCCEEDED(dev_->CreateShaderResourceView(pic.tex, nullptr, &pic.srv)) && SUCCEEDED(dev_->CreateRenderTargetView(pic.tex, nullptr, &rtv))) {
            Camera c;
            c.tx = c.tz = 0;
            c.ty = 10;
            c.yaw = 0;
            c.pitch = -0.3f;  // the eye below the target: looking up
            c.dist = 10;
            c.fov = 1.1f;
            c.update(float(W) / float(H));
            D3D11_MAPPED_SUBRESOURCE m;
            if (SUCCEEDED(ctx_->Map(cb_, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
                CB* cb = static_cast<CB*>(m.pData);
                std::memset(cb, 0, sizeof(CB));
                cb->viewproj = c.viewproj;
                cb->eye = {c.eye.x, c.eye.y, c.eye.z, 0};
                cb->dpar = {0, 0, 0, params.time};
                ctx_->Unmap(cb_, 0);
            }
            ctx_->VSSetConstantBuffers(0, 1, &cb_);
            ctx_->PSSetConstantBuffers(0, 1, &cb_);
            const float black[4] = {0, 0, 0, 1};
            ctx_->ClearRenderTargetView(rtv, black);
            ctx_->OMSetRenderTargets(1, &rtv, nullptr);
            D3D11_VIEWPORT vp{0, 0, float(W), float(H), 0, 1};
            ctx_->RSSetViewports(1, &vp);
            ctx_->PSSetSamplers(0, 1, &sampler_);
            sky = {};
            sky.model = k.model;
            sky.scale = k.scale;
            sky.offset = k.offset;
            sky.heading = k.heading;
            draw_sky(c);
            rtv->Release();
        } else {
            release(pic.srv);
            release(pic.tex);
        }
        sky_pics_[k.key] = pic;
    }
    sky = saved;
    ctx_->OMSetRenderTargets(1, &old_rtv, old_dsv);
    ctx_->RSSetViewports(1, &old_vp);
    release(old_rtv);
    release(old_dsv);
}

void Renderer::end_frame() {
    for (IUnknown* p : retired_) p->Release();
    retired_.clear();
}

bool Renderer::make_texture(const ma2::Image& img, ID3D11Texture2D** tex, ID3D11ShaderResourceView** srv) {
    // Same size as the texture already there (a brush stroke repaints the colour map every frame): update it in place instead
    // of creating a new full-mip texture each time. Draws already queued keep the order the immediate context gives them.
    if (*tex && *srv && img.w > 0 && img.h > 0 && img.px.size() >= size_t(img.w) * size_t(img.h)) {
        D3D11_TEXTURE2D_DESC old{};
        (*tex)->GetDesc(&old);
        if (old.Width == UINT(img.w) && old.Height == UINT(img.h) && (old.MiscFlags & D3D11_RESOURCE_MISC_GENERATE_MIPS)) {
            ctx_->UpdateSubresource(*tex, 0, nullptr, img.px.data(), UINT(img.w * 4), 0);
            ctx_->GenerateMips(*srv);
            return true;
        }
    }
    retire(*srv);
    retire(*tex);
    *srv = nullptr;
    *tex = nullptr;
    if (img.w <= 0 || img.h <= 0 || img.px.size() < size_t(img.w) * size_t(img.h)) return false;
    // Full mip chain for clean minification.
    int levels = 1;
    for (int s = std::max(img.w, img.h); s > 1; s >>= 1) levels++;
    D3D11_TEXTURE2D_DESC td{};
    td.Width = UINT(img.w);
    td.Height = UINT(img.h);
    td.MipLevels = UINT(levels);
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    td.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
    if (FAILED(dev_->CreateTexture2D(&td, nullptr, tex)) || !*tex) return false;
    ctx_->UpdateSubresource(*tex, 0, nullptr, img.px.data(), UINT(img.w * 4), 0);
    if (FAILED(dev_->CreateShaderResourceView(*tex, nullptr, srv)) || !*srv) return false;
    ctx_->GenerateMips(*srv);
    return true;
}

void Renderer::set_colormap(const ma2::Image& img) { make_texture(img, &colormap_, &colormap_srv_); }
void Renderer::set_lightmap(const ma2::Image& img) { make_texture(img, &lightmap_, &lightmap_srv_); }
void Renderer::set_overlay(const ma2::Image& img) { make_texture(img, &overlay_, &overlay_srv_); }

void Renderer::set_ground_weights(const ma2::GroundLayers& g) {
    if (!g.weights || g.w <= 0 || g.h <= 0) return;
    ma2::Image plane[2];
    for (int p = 0; p < 2; p++) {
        plane[p].w = g.w; plane[p].h = g.h;
        plane[p].px.resize(size_t(g.w) * g.h);
        const uint8_t* d = g.weights->data() + p * 4;
        for (size_t i = 0; i < plane[p].px.size(); i++) std::memcpy(&plane[p].px[i], d + i * 8, 4);
        make_texture(plane[p], &gw_tex_[p], &gw_srv_[p]);
    }
}

void Renderer::set_ground_layers(const ma2::GroundLayers* g) {
    retire(gl_srv_); retire(gl_tex_); gl_srv_ = nullptr; gl_tex_ = nullptr;
    for (int p = 0; p < 2; p++) { retire(gw_srv_[p]); retire(gw_tex_[p]); gw_srv_[p] = nullptr; gw_tex_[p] = nullptr; }
    ground_count_ = 0;
    std::fill(std::begin(ground_rep_), std::end(ground_rep_), 0.f);
    if (!g || !g->enabled || g->count() < 2 || !g->weights) return;
    set_ground_weights(*g);
    for (int k = 0; k < g->count() && k < 8; k++) ground_rep_[k] = g->layers[size_t(k)].repeats;
    int extra = g->count() - 2, S = ma2::kGroundTextureSize;
    if (extra > 0) {
        int levels = 1;
        for (int s = S; s > 1; s >>= 1) levels++;
        D3D11_TEXTURE2D_DESC td{};
        td.Width = td.Height = UINT(S);
        td.MipLevels = UINT(levels);
        td.ArraySize = UINT(extra);
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        td.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
        if (FAILED(dev_->CreateTexture2D(&td, nullptr, &gl_tex_)) || !gl_tex_) return;
        for (int k = 0; k < extra; k++) {
            ma2::Image im = g->layers[size_t(2 + k)].image;
            if (im.w != S || im.h != S) im = ma2::resample_square(im, S);
            ctx_->UpdateSubresource(gl_tex_, D3D11CalcSubresource(0, UINT(k), UINT(levels)), nullptr, im.px.data(), UINT(S * 4), 0);
        }
        D3D11_SHADER_RESOURCE_VIEW_DESC vd{};
        vd.Format = td.Format;
        vd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
        vd.Texture2DArray.MipLevels = UINT(levels);
        vd.Texture2DArray.ArraySize = UINT(extra);
        if (FAILED(dev_->CreateShaderResourceView(gl_tex_, &vd, &gl_srv_)) || !gl_srv_) return;
        ctx_->GenerateMips(gl_srv_);
    }
    ground_count_ = g->count();
}
void Renderer::set_minimap(const ma2::Image& img) { make_texture(img, &minimap_, &minimap_srv_); }
void Renderer::clear_overlay() {
    ma2::Image none;
    none.w = none.h = 1;
    none.px = {0};
    set_overlay(none);
}

void Renderer::build_vertex(const ma2::Terrain& t, int gx, int gz, TerrainVertex& v) {
    // Drawn at the saved 8-bit height steps (what the game draws), so a sculpted edge or the ground
    // around water looks the same here as in the game.
    auto H = [&](int x, int z) { return t.stored(t.at(x, z)); };
    float x = float(gx) * t.scale, z = float(gz) * t.scale;
    v.p = {x, H(gx, gz), z};
    float hx = H(gx + 1, gz) - H(gx - 1, gz), hz = H(gx, gz + 1) - H(gx, gz - 1);
    XMVECTOR n = XMVector3Normalize(XMVectorSet(-hx, 2 * t.scale, -hz, 0));
    XMStoreFloat3(&v.n, n);
    v.uv = {float(gx) / float(t.w - 1), 1.f - float(gz) / float(t.h - 1)};
}

void Renderer::set_terrain(const ma2::Terrain& t) {
    release(terrain_vb_);
    release(terrain_ib_);
    tw_ = t.w;
    th_ = t.h;
    if (t.w < 2 || t.h < 2) return;
    tverts_.resize(size_t(t.w) * t.h);
    for (int gz = 0; gz < t.h; gz++)
        for (int gx = 0; gx < t.w; gx++) build_vertex(t, gx, gz, tverts_[size_t(gz) * t.w + gx]);
    outside_y_ = 1e9f;
    for (const auto& v : tverts_) outside_y_ = std::min(outside_y_, v.p.y - 2.f);
    std::vector<uint32_t> idx;
    idx.reserve(size_t(t.w - 1) * (t.h - 1) * 6);
    for (int gz = 0; gz + 1 < t.h; gz++)
        for (int gx = 0; gx + 1 < t.w; gx++) {
            uint32_t a = uint32_t(gz * t.w + gx), b = a + 1, c = a + uint32_t(t.w), d = c + 1;
            // Clockwise seen from above (+y) in a left-handed system. Each cell is split along (gx,gz)-(gx+1,gz+1), the game's
            // diagonal (its strips run along the stored rows, z flipped): with the other diagonal the edge where the ground meets
            // flat slabs (Mission_02 grass vs the Bb_34x34 pavement tiles) turns into 8 m stair-steps the game does not show.
            idx.insert(idx.end(), {a, c, d, a, d, b});
        }
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = UINT(tverts_.size() * sizeof(TerrainVertex));
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA sd{tverts_.data(), 0, 0};
    dev_->CreateBuffer(&bd, &sd, &terrain_vb_);
    bd.ByteWidth = UINT(idx.size() * 4);
    bd.BindFlags = D3D11_BIND_INDEX_BUFFER;
    sd.pSysMem = idx.data();
    dev_->CreateBuffer(&bd, &sd, &terrain_ib_);
    terrain_indices_ = UINT(idx.size());
    upload_heights();
    params.terrain_size_x = t.size_x();
    params.terrain_size_z = t.size_z();
}

void Renderer::update_terrain_rect(const ma2::Terrain& t, int x0, int z0, int x1, int z1) {
    if (t.w != tw_ || t.h != th_ || !terrain_vb_) { set_terrain(t); return; }
    x0 = std::max(0, x0 - 1); z0 = std::max(0, z0 - 1);
    x1 = std::min(t.w - 1, x1 + 1); z1 = std::min(t.h - 1, z1 + 1);
    for (int gz = z0; gz <= z1; gz++)
        for (int gx = x0; gx <= x1; gx++) {
            build_vertex(t, gx, gz, tverts_[size_t(gz) * t.w + gx]);
            outside_y_ = std::min(outside_y_, tverts_[size_t(gz) * t.w + gx].p.y - 2.f);  // dug lower: the outside ground follows (never rises until a reload)
        }
    // Upload whole rows (simple and fast enough for 257 x 257).
    D3D11_BOX box{UINT(z0 * t.w) * sizeof(TerrainVertex), 0, 0, UINT((z1 + 1) * t.w) * sizeof(TerrainVertex), 1, 1};
    if (z1 < z0 || x1 < x0) return;
    ctx_->UpdateSubresource(terrain_vb_, 0, &box, &tverts_[size_t(z0) * t.w], 0, 0);
    upload_heights();
}

float Renderer::height_at(float x, float z) const {
    if (tw_ < 2 || th_ < 2 || tverts_.size() < size_t(tw_) * th_) return 0;
    float sx = params.terrain_size_x / float(tw_ - 1), sz = params.terrain_size_z / float(th_ - 1);
    int gx = std::clamp(int(x / sx + 0.5f), 0, tw_ - 1), gz = std::clamp(int(z / sz + 0.5f), 0, th_ - 1);
    return tverts_[size_t(gz) * tw_ + gx].p.y;
}

// The ground light map for Scene lights, after the game's per-frame 512x512 map (frame trace,
// spec R2-4): one additive pool per light, radius lerped from min_radius to max_radius by the
// light's height above the ground (min_height..max_height, extrapolated), colour = the <lightmap>
// min/max_color (C0.rgb; MAPointLight: LightColor x 0.5) x C0.a 1/3, stored x2 (the game's modulate). The falloff is the game's 64x64 RadialFalloffLookup, (1 - r^2)^2. Rebuilt only when the lights change.
void Renderer::build_light_map() {
    uint64_t h = 1469598103934665603ull;
    auto mix = [&](float v) { uint32_t u; std::memcpy(&u, &v, 4); h = (h ^ u) * 1099511628211ull; };
    for (const auto& l : lights) {
        mix(l.pos.x); mix(l.pos.y); mix(l.pos.z); mix(l.max_r);
        mix(l.pool_min.x + l.pool_min.y * 3 + l.pool_min.z * 7); mix(l.pool_max.x + l.pool_max.y * 3 + l.pool_max.z * 7);
    }
    mix(float(lights.size())); mix(params.terrain_size_x); mix(float(tw_));
    if (lightpool_srv_ && h == lights_hash_) return;
    lights_hash_ = h;
    const int N = 1024;
    std::vector<XMFLOAT3> acc(size_t(N) * N, XMFLOAT3{0, 0, 0});
    float mx = params.terrain_size_x / N, mz = params.terrain_size_z / N;  // metres per texel
    for (const auto& l : lights) {
        if (l.max_r <= 0) continue;
        float above = l.pos.y - height_at(l.pos.x, l.pos.z);
        // Not clamped: lamps above max_height get bigger pools (Env_Highway_Light: ~26 m in the trace).
        float t = l.max_h > l.min_h ? std::max(0.f, (above - l.min_h) / (l.max_h - l.min_h)) : 0.f;
        float r = l.min_r + (l.max_r - l.min_r) * t;
        if (r <= 0) continue;
        float tc = std::min(t, 1.f);  // colour lerp min -> max by the same height fraction (inferred; every retail lamp has min = max)
        XMFLOAT3 pc{l.pool_min.x + (l.pool_max.x - l.pool_min.x) * tc, l.pool_min.y + (l.pool_max.y - l.pool_min.y) * tc,
                    l.pool_min.z + (l.pool_max.z - l.pool_min.z) * tc};
        float k = 1.f / 3.f * 2.f;
        int x0 = std::max(0, int((l.pos.x - r) / mx)), x1 = std::min(N - 1, int((l.pos.x + r) / mx));
        int z0 = std::max(0, int((l.pos.z - r) / mz)), z1 = std::min(N - 1, int((l.pos.z + r) / mz));
        for (int zi = z0; zi <= z1; zi++)
            for (int xi = x0; xi <= x1; xi++) {
                float dx = (xi + 0.5f) * mx - l.pos.x, dz = (zi + 0.5f) * mz - l.pos.z;
                float u = std::sqrt(dx * dx + dz * dz) / r;
                if (u >= 1) continue;
                float f = (1 - u * u) * (1 - u * u);  // RadialFalloffLookup: 255 x (1 - r^2)^2 (research radial-falloff-lut)
                XMFLOAT3& a = acc[size_t(N - 1 - zi) * N + xi];  // row 0 = the far z edge, like the colour map
                a.x += pc.x * k * f; a.y += pc.y * k * f; a.z += pc.z * k * f;
            }
    }
    ma2::Image img;
    img.w = img.h = N;
    img.px.resize(size_t(N) * N);
    for (size_t i = 0; i < acc.size(); i++) {
        auto c8 = [](float v) { return uint32_t(std::clamp(v, 0.f, 1.f) * 255.f + 0.5f); };
        img.px[i] = 0xFF000000u | c8(acc[i].z) << 16 | c8(acc[i].y) << 8 | c8(acc[i].x);
    }
    make_texture(img, &lightpool_, &lightpool_srv_);
}

// Heights in texture rows top-down like the colour map (row 0 = the far z edge).
void Renderer::upload_heights() {
    if (tw_ < 2 || th_ < 2 || tverts_.size() < size_t(tw_) * th_) return;
    std::vector<float> h(size_t(tw_) * th_);
    for (int gz = 0; gz < th_; gz++)
        for (int gx = 0; gx < tw_; gx++) h[size_t(th_ - 1 - gz) * tw_ + gx] = tverts_[size_t(gz) * tw_ + gx].p.y;
    D3D11_TEXTURE2D_DESC td{};
    if (height_) height_->GetDesc(&td);
    if (!height_ || td.Width != UINT(tw_) || td.Height != UINT(th_)) {
        release(height_srv_);
        release(height_);
        td = {};
        td.Width = UINT(tw_); td.Height = UINT(th_);
        td.MipLevels = td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R32_FLOAT;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(dev_->CreateTexture2D(&td, nullptr, &height_))) return;
        dev_->CreateShaderResourceView(height_, nullptr, &height_srv_);
    }
    ctx_->UpdateSubresource(height_, 0, nullptr, h.data(), UINT(tw_ * 4), 0);
}

void Renderer::render(const Camera& cam, D3D11_VIEWPORT vp) {
    build_water_previews();
    build_sky_previews();
    ctx_->RSSetViewports(1, &vp);
    prepare_mask();
    D3D11_MAPPED_SUBRESOURCE m;
    if (params.scene_lights) {
        build_light_map();
        // Model lighting: the kModelLights lights nearest the view's target (16 left lamp-lit vegetation
        // a few hundred metres off unlit; the game picks up to MaxAreaLightsPerObject per object, rule unknown).
        std::vector<const SceneLight*> close;
        for (const auto& l : lights) if (l.outer > 0) close.push_back(&l);
        auto d2 = [&](const SceneLight* l) { float dx = l->pos.x - cam.tx, dz = l->pos.z - cam.tz; return dx * dx + dz * dz; };
        if (close.size() > kModelLights) {
            std::partial_sort(close.begin(), close.begin() + kModelLights, close.end(), [&](auto a, auto b) { return d2(a) < d2(b); });
            close.resize(kModelLights);
        }
        if (lights_cb_ && SUCCEEDED(ctx_->Map(lights_cb_, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
            float* f = static_cast<float*>(m.pData);
            std::memset(f, 0, 16 + kModelLights * 16 * 4);
            f[0] = float(close.size());
            for (size_t k = 0; k < close.size(); k++) {
                const SceneLight& l = *close[k];
                float* p = f + 4 + k * 4;
                p[0] = l.pos.x; p[1] = l.pos.y; p[2] = l.pos.z; p[3] = l.outer;
                p += 4 * kModelLights; p[0] = l.color.x; p[1] = l.color.y; p[2] = l.color.z; p[3] = l.inner;
                p += 4 * kModelLights; p[0] = l.dir.x; p[1] = l.dir.y; p[2] = l.dir.z; p[3] = l.spot ? l.cos_out : -2.f;
                p += 4 * kModelLights; p[0] = l.cos_in;
            }
            ctx_->Unmap(lights_cb_, 0);
        }
    }
    if (SUCCEEDED(ctx_->Map(cb_, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        CB* cb = static_cast<CB*>(m.pData);
        cb->viewproj = cam.viewproj;
        // The map's sun even when it is below the horizon (New River City: -11 degrees, so flat
        // ground gets no sun, as in the game); the editor light only when there is no direction.
        bool sun = params.sun_dir.x * params.sun_dir.x + params.sun_dir.y * params.sun_dir.y + params.sun_dir.z * params.sun_dir.z > 1e-6f;
        XMVECTOR l = params.env_light && sun
                         ? XMVector3Normalize(XMVectorSet(-params.sun_dir.x, -params.sun_dir.y, -params.sun_dir.z, 0))
                         : XMVector3Normalize(XMVectorSet(0.5f, 0.8f, 0.3f, 0));
        XMStoreFloat4(&cb->light, l);
        cb->brush = {params.brush.x, params.brush_contour, params.brush.z, params.brush_radius};  // the shader reads xz and w; y = contour spacing
        cb->flags = {params.grid ? 1.f : 0.f, params.overlay ? 1.f : 0.f, params.lighting ? 1.f : 0.f,
                     params.scene_lights && lightpool_srv_ ? 1.f : 0.f};
        cb->tsize = {params.terrain_size_x, params.terrain_size_z, detail.tex[2] && detail.tex[3] ? (params.fast_terrain_detail ? 2.f : 1.f) : 0.f, float(params.wave_up)};
        bool det = params.detail && detail.tex[0] && detail.tex[1];
        cb->drep = {detail.rep[0], detail.rep[1], detail.rep[2], detail.rep[3]};
        cb->grep0 = {ground_rep_[0], ground_rep_[1], ground_rep_[2], ground_rep_[3]};
        cb->grep1 = {ground_rep_[4], ground_rep_[5], ground_rep_[6], ground_rep_[7]};
        cb->ginfo = {float(ground_count_), params.brush_tint, 0, 0};
        cb->dpar = {det ? 1.f : 0.f, detail.blend, detail.modulate ? 1.f : 0.f, params.time};
        cb->eye = {cam.eye.x, cam.eye.y, cam.eye.z, g_dbg_w};
        cb->amb = {params.ambient.x * 2, params.ambient.y * 2, params.ambient.z * 2, params.env_light ? 1.f : 0.f};
        cb->sun = {params.sun.x * 2, params.sun.y * 2, params.sun.z * 2, 0};
        bool fog = params.fog && params.fog_end > params.fog_start;
        cb->fogc = {params.fog_color.x, params.fog_color.y, params.fog_color.z, fog ? 1.f : 0.f};
        // Fog distance from the orbit target (where a player would stand), not the eye (audit UX note).
        // Review BB2A-12: a Large (2 x 2 km) map's whole-map view washed out everything past the level's FogEnd from the
        // centre (a pale sheet with a green blob). Pulling back past 300 m pushes the fog out until, from 1500 m, the whole
        // map sits inside FogStart; closer views keep the level's own fog.
        const float half_diag = 0.5f * std::sqrt(params.terrain_size_x * params.terrain_size_x + params.terrain_size_z * params.terrain_size_z);
        const float fog_push = std::clamp((cam.dist - 300.f) / 1200.f, 0.f, 1.f) * std::max(0.f, half_diag - params.fog_start);
        cb->fogr = {params.fog_start + fog_push, fog ? 1.f / (params.fog_end - params.fog_start) : 0.f, cam.dist < 100.f ? cam.eye.x : cam.tx, cam.dist < 100.f ? cam.eye.z : cam.tz};  // game-like close views: fog from the eye (the game's eye-depth fog); overview cameras keep the orbit target
        cb->fogh = {params.fog_top, params.fog_bottom, params.fog_height && params.fog_top > params.fog_bottom ? 1.f : 0.f, cam.dist < 100.f ? cam.eye.y : -1e9f};
        ctx_->Unmap(cb_, 0);
    }
    ctx_->VSSetConstantBuffers(0, 1, &cb_);
    ctx_->PSSetConstantBuffers(0, 1, &cb_);
    ctx_->PSSetConstantBuffers(2, 1, &lights_cb_);
    ID3D11ShaderResourceView* maps[2] = {height_srv_, lightpool_srv_};
    ctx_->PSSetShaderResources(11, 2, maps);
    ctx_->PSSetSamplers(0, 1, &sampler_);
    ctx_->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
    ctx_->OMSetDepthStencilState(ds_on_, 0);
    ctx_->RSSetState(rs_solid_);
    if (sky.model && sky.model->state == GpuModel::Ready && !sky.model->meshes.empty()) draw_sky(cam);
    // Terrain
    if (terrain_vb_) {
        UINT stride = sizeof(TerrainVertex), off = 0;
        ctx_->IASetInputLayout(terrain_il_);
        ctx_->IASetVertexBuffers(0, 1, &terrain_vb_, &stride, &off);
        ctx_->IASetIndexBuffer(terrain_ib_, DXGI_FORMAT_R32_UINT, 0);
        ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx_->VSSetShader(terrain_vs_, nullptr, 0);
        ctx_->PSSetShader(terrain_ps_, nullptr, 0);
        ID3D11ShaderResourceView* srv[3] = {colormap_srv_, lightmap_srv_, overlay_srv_};
        ctx_->PSSetShaderResources(0, 3, srv);
        ctx_->PSSetShaderResources(4, 4, detail.tex);
        ID3D11ShaderResourceView* ground[3] = {gw_srv_[0], gw_srv_[1], gl_srv_};
        ctx_->PSSetShaderResources(13, 3, ground);
        ctx_->PSSetSamplers(1, 1, &wrap_sampler_);
        ctx_->RSSetState(rs_nocull_);
        ctx_->DrawIndexed(terrain_indices_, 0, 0);
        // Outside the map: dark ground under the lowest point, 12 km each way (bugbash #17: a camera looking past the M05 east
        // edge saw the sky dome's lower half as a flat light-grey wall; the game's camera never looks there).
        if (!outside_vb_) {
            D3D11_BUFFER_DESC bd{};
            bd.ByteWidth = 6 * sizeof(LineVertex);
            bd.Usage = D3D11_USAGE_DYNAMIC;
            bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
            bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            dev_->CreateBuffer(&bd, nullptr, &outside_vb_);
        }
        if (outside_vb_ && outside_vs_ && SUCCEEDED(ctx_->Map(outside_vb_, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
            float cx = params.terrain_size_x * 0.5f, cz = params.terrain_size_z * 0.5f, r = 12000.f;
            const uint32_t c = 0xFF161A18u;  // ABGR, a dark earth tone
            LineVertex a{{cx - r, outside_y_, cz - r}, c}, b{{cx + r, outside_y_, cz - r}, c}, d{{cx + r, outside_y_, cz + r}, c}, e{{cx - r, outside_y_, cz + r}, c};
            LineVertex q[6] = {a, b, d, a, d, e};
            std::memcpy(m.pData, q, sizeof q);
            ctx_->Unmap(outside_vb_, 0);
            UINT ostride = sizeof(LineVertex), ooff = 0;
            ctx_->IASetInputLayout(line_il_);
            ctx_->IASetVertexBuffers(0, 1, &outside_vb_, &ostride, &ooff);
            ctx_->VSSetShader(outside_vs_, nullptr, 0);
            ctx_->PSSetShader(outside_ps_, nullptr, 0);
            ctx_->Draw(6, 0);
        }
        ctx_->RSSetState(rs_solid_);
    }
    // Boxes
    if (!boxes.empty()) {
        if (boxes.size() > inst_cap_) {
            release(inst_vb_);
            inst_cap_ = UINT(boxes.size() + 1024);
            D3D11_BUFFER_DESC bd{};
            bd.ByteWidth = inst_cap_ * sizeof(BoxInstance);
            bd.Usage = D3D11_USAGE_DYNAMIC;
            bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
            bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            if (FAILED(dev_->CreateBuffer(&bd, nullptr, &inst_vb_))) { inst_vb_ = nullptr; inst_cap_ = 0; }
        }
        if (inst_vb_ && SUCCEEDED(ctx_->Map(inst_vb_, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
            std::memcpy(m.pData, boxes.data(), boxes.size() * sizeof(BoxInstance));
            ctx_->Unmap(inst_vb_, 0);
        }
        ID3D11Buffer* vbs[2] = {cube_vb_, inst_vb_};
        UINT strides[2] = {24, sizeof(BoxInstance)}, offs[2] = {0, 0};
        if (inst_vb_) {
            ctx_->IASetInputLayout(box_il_);
            ctx_->IASetVertexBuffers(0, 2, vbs, strides, offs);
            ctx_->IASetIndexBuffer(cube_ib_, DXGI_FORMAT_R16_UINT, 0);
            ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            ctx_->VSSetShader(box_vs_, nullptr, 0);
            ctx_->PSSetShader(box_ps_, nullptr, 0);
            ctx_->RSSetState(rs_nocull_);
            ctx_->DrawIndexedInstanced(36, UINT(boxes.size()), 0, 0, 0);
            ctx_->RSSetState(rs_solid_);
        }
    }
    // Models: one instance buffer for every batch.
    {
        size_t total = 0;
        model_inst_start_.assign(models.size(), 0);
        for (size_t b = 0; b < models.size(); b++) { model_inst_start_[b] = UINT(total); total += models[b].inst.size(); }
        if (total) {
            if (total > model_inst_cap_) {
                release(model_inst_vb_);
                model_inst_cap_ = UINT(total + 2048);
                D3D11_BUFFER_DESC bd{};
                bd.ByteWidth = model_inst_cap_ * sizeof(BoxInstance);
                bd.Usage = D3D11_USAGE_DYNAMIC;
                bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
                bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
                if (FAILED(dev_->CreateBuffer(&bd, nullptr, &model_inst_vb_))) { model_inst_vb_ = nullptr; model_inst_cap_ = 0; }
            }
            if (model_inst_vb_ && SUCCEEDED(ctx_->Map(model_inst_vb_, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
                auto* dst = static_cast<BoxInstance*>(m.pData);
                for (auto& mb : models) { std::memcpy(dst, mb.inst.data(), mb.inst.size() * sizeof(BoxInstance)); dst += mb.inst.size(); }
                ctx_->Unmap(model_inst_vb_, 0);
            }
            draw_models(0);
            draw_models(2);
        }
    }
    // Translucent quads (water) and lines share the line shader.
    auto draw_list = [&](const std::vector<LineVertex>& v, D3D11_PRIMITIVE_TOPOLOGY topo, bool draw = true) {
        if (v.empty()) return;
        if (v.size() > line_cap_) {
            release(line_vb_);
            line_cap_ = UINT(v.size() + 4096);
            D3D11_BUFFER_DESC bd{};
            bd.ByteWidth = line_cap_ * sizeof(LineVertex);
            bd.Usage = D3D11_USAGE_DYNAMIC;
            bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
            bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            if (FAILED(dev_->CreateBuffer(&bd, nullptr, &line_vb_))) { line_vb_ = nullptr; line_cap_ = 0; return; }
        }
        if (!line_vb_) return;
        if (SUCCEEDED(ctx_->Map(line_vb_, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
            std::memcpy(m.pData, v.data(), v.size() * sizeof(LineVertex));
            ctx_->Unmap(line_vb_, 0);
        }
        UINT stride = sizeof(LineVertex), off = 0;
        ctx_->IASetInputLayout(line_il_);
        ctx_->IASetVertexBuffers(0, 1, &line_vb_, &stride, &off);
        ctx_->IASetPrimitiveTopology(topo);
        ctx_->VSSetShader(line_vs_, nullptr, 0);
        ctx_->PSSetShader(line_ps_, nullptr, 0);
        if (draw) ctx_->Draw(UINT(v.size()), 0);
    };
    ctx_->OMSetBlendState(bs_alpha_, nullptr, 0xFFFFFFFF);
    ctx_->OMSetDepthStencilState(ds_nowrite_, 0);
    ctx_->RSSetState(rs_nocull_);
    // Water planes, one draw each (a handful per map).
    bool refract = !quads.empty() && copy_scene();
    for (auto& q : quads) {
        uint32_t vc = q.selected ? 0x00FFFFFFu : 0xFFFFFFFFu;
        float qc = std::cos(q.heading), qs = std::sin(q.heading);
        auto P = [&](float lx, float lz) { return LineVertex{{q.c.x + qc * lx + qs * lz, q.c.y, q.c.z - qs * lx + qc * lz}, vc}; };
        std::vector<LineVertex> tris;
        if (q.oval) {  // a fan; the game's water model is a triangle list of rings, same outline
            const int n = 48;
            for (int k = 0; k < n; k++) {
                float t0 = 6.2831853f * float(k) / n, t1 = 6.2831853f * float(k + 1) / n;
                tris.push_back(P(0, 0));
                tris.push_back(P(std::cos(t1) * q.hx, std::sin(t1) * q.hz));
                tris.push_back(P(std::cos(t0) * q.hx, std::sin(t0) * q.hz));
            }
        } else {
            LineVertex a = P(-q.hx, -q.hz), b = P(q.hx, -q.hz), c = P(q.hx, q.hz), d = P(-q.hx, q.hz);
            tris = {a, b, c, a, c, d};
        }
        if (SUCCEEDED(ctx_->Map(mesh_cb_, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
            float* f = static_cast<float*>(m.pData);
            f[0] = float(q.color & 255) / 255; f[1] = float((q.color >> 8) & 255) / 255; f[2] = float((q.color >> 16) & 255) / 255;
            f[3] = 1;
            f[4] = q.fresnel_min; f[5] = q.fresnel_max; f[6] = q.wave ? 1.f : 0.f; f[7] = q.refl ? 1.f : 0.f;
            f[8] = float(q.shallow & 255) / 255; f[9] = float((q.shallow >> 8) & 255) / 255; f[10] = float((q.shallow >> 16) & 255) / 255; f[11] = 1;
            f[12] = refract ? 1.f : 0.f; f[13] = f[14] = f[15] = 0;
            ctx_->Unmap(mesh_cb_, 0);
        }
        draw_list(tris, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST, false);  // upload with the line layout only
        ctx_->VSSetShader(water_vs_, nullptr, 0);
        ctx_->PSSetShader(water_ps_, nullptr, 0);
        ctx_->PSSetConstantBuffers(1, 1, &mesh_cb_);
        ctx_->PSSetSamplers(1, 1, &wrap_sampler_);
        ID3D11ShaderResourceView* wsrv[3] = {q.wave, q.refl, height_srv_};
        ctx_->PSSetShaderResources(9, 3, wsrv);
        ctx_->PSSetShaderResources(1, 1, &lightmap_srv_);
        ctx_->PSSetShaderResources(16, 1, &scene_copy_srv_);
        ctx_->OMSetBlendState(bs_water_, nullptr, 0xFFFFFFFF);
        ctx_->Draw(UINT(tris.size()), 0);
        ctx_->OMSetBlendState(bs_alpha_, nullptr, 0xFFFFFFFF);
    }
    if (refract) { ID3D11ShaderResourceView* none = nullptr; ctx_->PSSetShaderResources(16, 1, &none); }
    draw_models(1);
    // Effect flame cards: about 8 m wide, a bright orange base fading to nothing 4 m up, additive.
    if (!flames.empty()) {
        std::vector<LineVertex> fv;
        const uint32_t base = 0xFF1E8CFFu, mid = 0xFF0048B0u, top = 0xFF000000u;  // ABGR
        for (const auto& p : flames) {
            float dx = cam.eye.x - p.x, dz = cam.eye.z - p.z, len = std::sqrt(dx * dx + dz * dz);
            if (len < 1e-3f) continue;
            float rx = -dz / len, rz = dx / len;
            auto V = [&](float s, float h, uint32_t c) { return LineVertex{{p.x + rx * s, p.y + h, p.z + rz * s}, c}; };
            LineVertex q[8] = {V(-4.f, 0, base), V(4.f, 0, base), V(3.5f, 1.8f, mid), V(-3.5f, 1.8f, mid),
                               V(-3.5f, 1.8f, mid), V(3.5f, 1.8f, mid), V(1.5f, 4.f, top), V(-1.5f, 4.f, top)};
            for (int k = 0; k < 8; k += 4) fv.insert(fv.end(), {q[k], q[k + 1], q[k + 2], q[k], q[k + 2], q[k + 3]});
        }
        ctx_->OMSetBlendState(bs_add_, nullptr, 0xFFFFFFFF);
        ctx_->OMSetDepthStencilState(ds_nowrite_, 0);
        ctx_->RSSetState(rs_nocull_);
        draw_list(fv, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    }
    if (!sprites.empty()) draw_sprites(cam);
    run_bloom(vp);
    ctx_->OMSetBlendState(bs_alpha_, nullptr, 0xFFFFFFFF);
    ctx_->RSSetState(rs_nocull_);
    ctx_->OMSetDepthStencilState(ds_off_, 0);
    draw_list(lines, D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
    ctx_->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
    ctx_->OMSetDepthStencilState(ds_on_, 0);
    ctx_->RSSetState(rs_solid_);
    ID3D11ShaderResourceView* none[15] = {};
    ctx_->PSSetShaderResources(0, 15, none);
}

}  // namespace ed
