// NV2A register combiner evaluation on the host GPU. Constants retain the
// guest's ICW/OCW encoding; inputs and outputs follow nv2a_combiner.c.
cbuffer Guest : register(b0) {
    float4 viewport;                  // width, height, depth range, use RC
    float4 fogColor;
    uint4 runtimeControl;             // combiner control, stage program, clip, alpha test
    uint4 runtimeFinalWords;          // final0, final1, alpha func, legacy texture mode
    float4 finalC0, finalC1, misc;     // alpha ref in misc.x
    uint4 runtimePrograms[8];         // color ICW, alpha ICW, color OCW, alpha OCW
    float4 factor0[8], factor1[8];
    float4 texInfo[4];                // coordinate scale xy, valid, 0=2D/1=cube/2=volume
    uint4 runtimeTextureControl;      // dot mappings and prior-stage selectors
    float4 bumpMatrix[4], bumpLuminance[4], eyeVector;
};
#ifndef NV2A_SPECIALIZED
#define control runtimeControl
#define finalWords runtimeFinalWords
#define programs runtimePrograms
#define textureControl runtimeTextureControl
#define NV2A_USE_RC viewport.w
#define NV2A_COMBINER_LOOP [loop]
#else
#define NV2A_COMBINER_LOOP [unroll]
#endif
Texture2DArray tex0 : register(t0);
Texture2DArray tex1 : register(t1);
Texture2DArray tex2 : register(t2);
Texture2DArray tex3 : register(t3);
Texture3D volume0 : register(t4);
Texture3D volume1 : register(t5);
Texture3D volume2 : register(t6);
Texture3D volume3 : register(t7);
SamplerState smp0 : register(s0);
SamplerState smp1 : register(s1);
SamplerState smp2 : register(s2);
SamplerState smp3 : register(s3);

// Vertex pulling preserves the original indexed buffers. Packed Xbox formats
// are decoded once per GPU vertex invocation, without expanding mesh vertices
// to sixteen float4 attributes on the CPU.
cbuffer VertexFetch : register(b2) {
    uint4 vertexStreams[16]; // byte offset, stride, type, component count
    float4 vertexDefaults[16];
};
ByteAddressBuffer vertexData0 : register(t0);
ByteAddressBuffer vertexData1 : register(t1);
ByteAddressBuffer vertexData2 : register(t2);
ByteAddressBuffer vertexData3 : register(t3);
ByteAddressBuffer vertexData4 : register(t4);
ByteAddressBuffer vertexData5 : register(t5);
ByteAddressBuffer vertexData6 : register(t6);
ByteAddressBuffer vertexData7 : register(t7);
ByteAddressBuffer vertexData8 : register(t8);
ByteAddressBuffer vertexData9 : register(t9);
ByteAddressBuffer vertexData10 : register(t10);
ByteAddressBuffer vertexData11 : register(t11);
ByteAddressBuffer vertexData12 : register(t12);
ByteAddressBuffer vertexData13 : register(t13);
ByteAddressBuffer vertexData14 : register(t14);
ByteAddressBuffer vertexData15 : register(t15);
uint vertex_word(uint stream,uint address) {
    if(stream==0)return vertexData0.Load(address);
    if(stream==1)return vertexData1.Load(address);
    if(stream==2)return vertexData2.Load(address);
    if(stream==3)return vertexData3.Load(address);
    if(stream==4)return vertexData4.Load(address);
    if(stream==5)return vertexData5.Load(address);
    if(stream==6)return vertexData6.Load(address);
    if(stream==7)return vertexData7.Load(address);
    if(stream==8)return vertexData8.Load(address);
    if(stream==9)return vertexData9.Load(address);
    if(stream==10)return vertexData10.Load(address);
    if(stream==11)return vertexData11.Load(address);
    if(stream==12)return vertexData12.Load(address);
    if(stream==13)return vertexData13.Load(address);
    if(stream==14)return vertexData14.Load(address);
    return vertexData15.Load(address);
}
uint vertex_bytes(uint stream,uint address) {
    uint word=vertex_word(stream,address&~3u);
    uint shift=(address&3u)*8;
    if(shift)word=(word>>shift)|(vertex_word(stream,(address&~3u)+4)<<(32-shift));
    return word;
}
float4 vertex_attribute(uint stream,uint id,uint type,uint size) {
    uint4 info=vertexStreams[stream];
    if(!size)return vertexDefaults[stream];
    uint address=info.x+id*info.y;
    float4 result=float4(0,0,0,1);
    if(type==0) {
        uint c=vertex_bytes(stream,address);
        return float4((c>>16)&255,(c>>8)&255,c&255,c>>24)/255;
    }
    if(type==6) {
        uint c=vertex_bytes(stream,address);
        return float4((int(c<<21)>>21)/1023.0,(int(c<<10)>>21)/1023.0,(int(c)>>22)/511.0,1);
    }
    [unroll] for(uint j=0;j<4;j++)if(j<size) {
        if(type==2)result[j]=asfloat(vertex_bytes(stream,address+j*4));
        else if(type==4)result[j]=(vertex_bytes(stream,address+j)&255)/255.0;
        else {
            int v=int(vertex_bytes(stream,address+j*2)<<16)>>16;
            result[j]=type==1?v/32767.0:v;
        }
    }
    return result;
}

struct Vertex {
    float4 pos : POSITION;
    float4 d0 : COLOR0;
    float4 d1 : COLOR1;
    float4 fog : FOG;
    float4 t0 : TEXCOORD0;
    float4 t1 : TEXCOORD1;
    float4 t2 : TEXCOORD2;
    float4 t3 : TEXCOORD3;
};
struct Pixel {
    float4 pos : SV_POSITION;
    // NV2A smooth colors and fog use the same perspective interpolation as
    // texture coordinates. Affine values produce seams when terrain LOD
    // changes the mesh's tessellation, even on an unchanged surface.
    float4 d0 : COLOR0;
    float4 d1 : COLOR1;
    float fog : FOG;
    float4 t0 : TEXCOORD0;
    float4 t1 : TEXCOORD1;
    float4 t2 : TEXCOORD2;
    float4 t3 : TEXCOORD3;
};
bool nv_isnan(float c) { return (asuint(c)&0x7FFFFFFFu)>0x7F800000u; }
float4 vertex_color(float4 c) {
    return float4(nv_isnan(c.x)?1:saturate(c.x),nv_isnan(c.y)?1:saturate(c.y),
                  nv_isnan(c.z)?1:saturate(c.z),nv_isnan(c.w)?1:saturate(c.w));
}
Pixel vertex_main(Vertex v) {
    Pixel o;
    // NV2A clamps signed W away from zero/infinity and truncates screen
    // coordinates to its four fractional bits before rasterization.
    float w = clamp(abs(v.pos.w),asfloat(0x1F800000u),asfloat(0x5F800000u));
    if(asuint(v.pos.w)&0x80000000u)w=-w;
    float2 screen=trunc(v.pos.xy*16)/16;
    float2 ndc = screen / viewport.xy * float2(2,-2) + float2(-1,1);
    o.pos = float4(ndc * w, v.pos.z / viewport.z * w, w);
    o.d0 = vertex_color(v.d0); o.d1 = vertex_color(v.d1); o.fog = v.fog.x;
    o.t0 = v.t0; o.t1 = v.t1; o.t2 = v.t2; o.t3 = v.t3;
    return o;
}
float3 cube_coordinate(float3 c) {
    float3 a=abs(c);
    float sc,tc,ma,face;
    if (a.x >= a.y && a.x >= a.z) {
        face=c.x>0?0:1; ma=a.x; sc=c.x>0?-c.z:c.z; tc=-c.y;
    } else if (a.y >= a.z) {
        face=c.y>0?2:3; ma=a.y; sc=c.x; tc=c.y>0?c.z:-c.z;
    } else {
        face=c.z>0?4:5; ma=a.z; sc=c.z>0?c.x:-c.x; tc=-c.y;
    }
    return float3((float2(sc,tc)/max(ma,1e-20)+1)*0.5,face);
}
float2 cube_as_2d(float3 c) {
    float3 a=abs(c);
    if(a.x>=a.y && a.x>=a.z)return float2(c.x>0?-c.z:c.z,c.y)/max(a.x,1e-20);
    if(a.y>=a.z)return float2(c.x,c.y>0?-c.z:c.z)/max(a.y,1e-20);
    return float2(c.z>0?c.x:-c.x,c.y)/max(a.z,1e-20);
}
float4 texture_sample(uint s,float3 c,bool cube) {
    if (!texInfo[s].z) return 1;
    if(texInfo[s].w==2) {
        if(s==0)return volume0.Sample(smp0,c);
        if(s==1)return volume1.Sample(smp1,c);
        if(s==2)return volume2.Sample(smp2,c);
        return volume3.Sample(smp3,c);
    }
    float3 uv;
    if(cube && texInfo[s].w)uv=cube_coordinate(c);
    else uv=float3((cube?cube_as_2d(c):c.xy)*texInfo[s].xy,0);
    if (s==0) return tex0.Sample(smp0,uv);
    if (s==1) return tex1.Sample(smp1,uv);
    if (s==2) return tex2.Sample(smp2,uv);
    return tex3.Sample(smp3,uv);
}
float3 signed_dot_map(float3 v,uint mode) {
    float3 b=v*255;
    if(mode==1)return (b-128)/127;
    if(mode==2)return float3(b.x>=128?(b.x-255.5)/127.5:(b.x+.5)/127.5,
                            b.y>=128?(b.y-255.5)/127.5:(b.y+.5)/127.5,
                            b.z>=128?(b.z-255.5)/127.5:(b.z+.5)/127.5);
    return float3(b.x>=128?(b.x-256)/127:b.x/127,
                  b.y>=128?(b.y-256)/127:b.y/127,
                  b.z>=128?(b.z-256)/127:b.z/127);
}
float3 dot_map(float4 c,uint mode) {
    if(mode==0)return c.rgb;
    if(mode<=3)return signed_dot_map(c.rgb,mode);
    if(mode==4) {
        uint4 b=(uint4)(c*255);
        return float3(((b.a<<8)|b.r)/65535.0,((b.g<<8)|b.b)/65535.0,1);
    }
    // HILO hemisphere formats need a signed texture decode, added separately.
    return c.rgb;
}
uint stage_input(uint s) {
    return s<2?0:min((textureControl.x>>(s==2?16:20))&15,s-1);
}
uint stage_dot_map(uint s) {
    return s ? (textureControl.x>>((s-1)*4))&15 : 0;
}
float4 sample_stage(uint s,float4 coords[4],float4 prior[4],inout float4 dots,uint mode) {
    float4 c=coords[s];
    if (mode == 0) return float4(0,0,0,1);
    // PASS_THROUGH_NV / texcoord converts interpolated coordinates to a
    // clamped RGBA result before any register-combiner arithmetic. Terrain
    // shaders use this for distance fades, with coordinates well above 1.
    if (mode == 4) return saturate(c);
    if (mode == 5) {
        [unroll] for (uint j=0;j<4;j++) {
            bool ge = (control.z >> (s*4+j)) & 1;
            if (ge ? c[j] >= 0 : c[j] < 0) discard;
        }
        return 0;
    }
    if(mode==1 || mode==2)return texture_sample(s,c.xyz/(c.w!=0?c.w:1),false);
    if(mode==3)return texture_sample(s,c.xyz,true);
    float4 previous=prior[stage_input(s)];
    if(mode==6 || mode==7) {
        float2 bump=signed_dot_map(previous.bgg,3).xy;
        float4 m=bumpMatrix[s];
        float2 dsdt=float2(m.x*bump.x+m.z*bump.y,m.y*bump.x+m.w*bump.y);
        float4 color=texture_sample(s,float3(c.xy+dsdt,c.z),false);
        return mode==7?color*(bumpLuminance[s].x*previous.r+bumpLuminance[s].y):color;
    }
    if(mode==15)return texture_sample(s,float3(previous.ar,0),false);
    if(mode==16)return texture_sample(s,float3(previous.gb,0),false);
    if(mode==17 || (mode>=9 && mode<=14) || mode==18) {
        dots[s]=dot(c.xyz,dot_map(previous,stage_dot_map(s)));
        if(mode==17 || mode==10)return 0;
        if(mode==9)return texture_sample(s,float3(dots[max(s,1)-1],dots[s],0),false);
        if(mode==11) {
            float nextDot=dot(coords[3].xyz,dot_map(prior[stage_input(3)],stage_dot_map(3)));
            return texture_sample(s,float3(dots[1],dots[2],nextDot),true);
        }
        float3 n=float3(dots[1],dots[2],dots[3]);
        if(mode==12 || mode==18) {
            float3 e=mode==18?eyeVector.xyz:float3(coords[1].w,coords[2].w,coords[3].w);
            n=2*n*dot(n,e)/max(dot(n,n),1e-20)-e;
        }
        return texture_sample(s,n,mode!=13);
    }
    return 0;
}
float3 input_rgb(float4 R[16], uint b) {
    float3 x=(b&16)?R[b&15].aaa:R[b&15].rgb;
    float3 p=max(x,0);
    switch ((b>>5)&7) {
        case 0:return p; case 1:return 1-saturate(x);
        case 2:return 2*p-1; case 3:return 1-2*p;
        case 4:return p-.5; case 5:return .5-p;
        case 6:return x; default:return -x;
    }
}
float input_alpha(float4 R[16], uint b) {
    float x=(b&16)?R[b&15].a:R[b&15].b;
    float p=max(x,0);
    switch ((b>>5)&7) {
        case 0:return p; case 1:return 1-saturate(x);
        case 2:return 2*p-1; case 3:return 1-2*p;
        case 4:return p-.5; case 5:return .5-p;
        case 6:return x; default:return -x;
    }
}
float3 output_rgb(float3 x,uint m) {
    switch(m) {
        case 8:x-=.5;break; case 16:x*=2;break;
        case 24:x=(x-.5)*2;break; case 32:x*=4;break;case 48:x*=.5;break;
    }
    return clamp(x,-1,1);
}
float output_alpha(float x,uint m) { return output_rgb(x.xxx,m).x; }
bool alpha_pass(float a) {
    float r=misc.x;
    switch(finalWords.z) {
        case 0x200:return false;case 0x201:return a<r;case 0x202:return a==r;
        case 0x203:return a<=r;case 0x204:return a>r;case 0x205:return a!=r;
        case 0x206:return a>=r;default:return true;
    }
}
float4 pixel_color(Pixel p) {
    float4 result;
    if (!NV2A_USE_RC) {
        // Legacy screen-space path samples a texture directly; otherwise color.
        result=finalWords.w ? texture_sample(0,p.t0.xyz/(p.t0.w!=0?p.t0.w:1),false) : p.d0;
        if (finalWords.w == 2) result *= p.d0;
    } else {
        float4 R[16];
        [unroll] for(uint j=0;j<16;j++) R[j]=0;
        R[3]=float4(fogColor.rgb,saturate(p.fog)); R[4]=p.d0;R[5]=p.d1;
        float4 coords[4]={p.t0,p.t1,p.t2,p.t3};
        float4 prior[4];float4 dots=0;
        [unroll] for(uint t=0;t<4;t++)prior[t]=0;
        [unroll] for(uint st=0;st<4;st++) {
            prior[st]=sample_stage(st,coords,prior,dots,(control.y>>(st*5))&31);
            R[8+st]=prior[st];
        }
        R[12].a=(control.y&31)?R[8].a:1;
        uint flags=control.x>>8;
        NV2A_COMBINER_LOOP for(uint s=0;s<min(control.x&255,8);s++) {
            uint icw=programs[s].x,aicw=programs[s].y;
            uint ocw=programs[s].z,aocw=programs[s].w;
            uint fl=ocw>>12,afl=aocw>>12;
            R[1]=factor0[(flags&16)?s:0];R[2]=factor1[(flags&256)?s:0];
            bool mux=(flags&1)?R[12].a>=.5:((int(R[12].a*255)&1)!=0);
            float3 A=input_rgb(R,icw>>24),B=input_rgb(R,icw>>16);
            float3 C=input_rgb(R,icw>>8),D=input_rgb(R,icw);
            float aA=input_alpha(R,aicw>>24),aB=input_alpha(R,aicw>>16);
            float aC=input_alpha(R,aicw>>8),aD=input_alpha(R,aicw);
            float3 ab=(fl&2)?dot(A,B).xxx:A*B;
            float3 cd=(fl&1)?dot(C,D).xxx:C*D;
            float3 ms=(fl&4)?(mux?cd:ab):ab+cd;
            float aab=aA*aB,acd=aC*aD,ams=(afl&4)?(mux?acd:aab):aab+acd;
            ab=output_rgb(ab,fl&56);cd=output_rgb(cd,fl&56);ms=output_rgb(ms,fl&56);
            aab=output_alpha(aab,afl&56);acd=output_alpha(acd,afl&56);ams=output_alpha(ams,afl&56);
            if ((ocw>>4)&15) { R[(ocw>>4)&15].rgb=ab;if(fl&128)R[(ocw>>4)&15].a=ab.b; }
            if (ocw&15) { R[ocw&15].rgb=cd;if(fl&64)R[ocw&15].a=cd.b; }
            if ((ocw>>8)&15) R[(ocw>>8)&15].rgb=ms;
            if ((aocw>>4)&15) R[(aocw>>4)&15].a=aab;
            if (aocw&15) R[aocw&15].a=acd;
            if ((aocw>>8)&15) R[(aocw>>8)&15].a=ams;
            R[0]=0;
        }
        uint f0=finalWords.x,f1=finalWords.y;
        if(f0 || f1) {
            R[1]=finalC0;R[2]=finalC1;
            float3 a=(f1&64)?1-R[5].rgb:R[5].rgb;
            float3 b=(f1&32)?1-R[12].rgb:R[12].rgb;
            R[14]=float4((f1&128)?saturate(a+b):a+b,0);
            R[15]=float4(input_rgb(R,f1>>24)*input_rgb(R,f1>>16),0);
            float3 A=input_rgb(R,f0>>24),B=input_rgb(R,f0>>16);
            float3 C=input_rgb(R,f0>>8),D=input_rgb(R,f0);
            result=float4(D+C*(1-A)+B*A,input_alpha(R,f1>>8));
        } else result=R[12];
    }
    result=saturate(result);
    if(control.w && !alpha_pass(result.a)) discard;
    return result;
}
float4 pixel_main(Pixel p) : SV_TARGET {
    if (misc.w && (p.pos.z < misc.y || p.pos.z > misc.z)) discard;
    return pixel_color(p);
}
struct ClampedPixel {
    float4 color : SV_TARGET;
    float depth : SV_Depth;
};
ClampedPixel pixel_clamped(Pixel p) {
    ClampedPixel result;
    result.color = pixel_color(p);
    // NV2A clamps interpolated depth before comparison. Disabling D3D depth
    // clipping alone still rejects sky geometry beyond the cleared far depth.
    result.depth = clamp(p.pos.z, misc.y, misc.z);
    return result;
}
