/* Shared NV2A texel decoding for uploads and software rasterization. */
#include "nv2a_texture.h"
#include "../d3d/d3d8_swizzle.h"

uint32_t nv2a_texture_texel_bytes(uint32_t format)
{
    switch(format) {
    case 0:case 1:case 0xB:case 0x13:case 0x19:case 0x1B:case 0x1F:return 1;
    case 2:case 3:case 4:case 5:case 0xA:case 0x10:case 0x11:case 0x15:case 0x16:case 0x17:
    case 0x18:case 0x1A:case 0x1C:case 0x1D:case 0x20:case 0x24:case 0x25:
    case 0x27:case 0x28:case 0x29:return 2;
    default:return 4;
    }
}

static uint32_t expand(uint32_t v, uint32_t bits,int canonical)
{
    return d3d8_expand_channel_matching(v,bits,canonical);
}

/* The linear format that decodes the same texels as a swizzled one.
 *
 * Swizzling changes where a texel lives, not what it says: A8R8G8B8 (0x06) and
 * LIN_A8R8G8B8 (0x12) are the same four bytes in the same order. So the whole
 * difference is the address calculation, and one of those lets every format
 * below serve both. Pairs read off the table in d3d8_xbox.h rather than
 * recalled -- the comment above this one is about getting exactly that wrong. */
static uint32_t linear_twin(uint32_t fmt)
{
    switch (fmt) {
    case 0x00: return 0x13;                /* L8        -> LIN_L8        */
    case 0x01: return 0x1B;                /* AY8       -> LIN_AY8       */
    case 0x02: return 0x10;                /* A1R5G5B5  -> LIN_A1R5G5B5  */
    case 0x03: return 0x1C;                /* X1R5G5B5  -> LIN_X1R5G5B5  */
    case 0x04: return 0x1D;                /* A4R4G4B4  -> LIN_A4R4G4B4  */
    case 0x05: return 0x11;                /* R5G6B5    -> LIN_R5G6B5    */
    case 0x06: return 0x12;                /* A8R8G8B8  -> LIN_A8R8G8B8  */
    case 0x07: return 0x1E;                /* X8R8G8B8  -> LIN_X8R8G8B8  */
    case 0x19: return 0x1F;                /* A8        -> LIN_A8        */
    case 0x1A: return 0x20;                /* A8Y8      -> LIN_A8Y8      */
    case 0x3A: return 0x3F;
    case 0x3B: return 0x40;
    case 0x3C: return 0x41;
    case 0x28: return 0x17;                /* G8B8      -> LIN_G8B8      */
    case 0x29: return 0x16;                /* R8B8      -> LIN_R8B8      */
    default:   return fmt;                 /* already linear, or unhandled */
    }
}

size_t nv2a_texture_dxt_block_offset(const Nv2aGpuTexture *texture,
                                    uint32_t x,uint32_t y,uint32_t z)
{
    size_t columns=(texture->width+3u)/4u,rows=(texture->height+3u)/4u;
    size_t tile=(y/4u)*columns+x/4u,index;
    if(texture->dimensionality==3) {
        uint32_t depth=texture->depth?texture->depth:1u,first=z&~3u;
        uint32_t slices=depth-first;
        if(slices>4u)slices=4u;
        index=(size_t)first*columns*rows+tile*slices+(z-first);
    } else index=(size_t)z*columns*rows+tile;
    return index*d3d8_format_dxt_block_bytes(texture->color);
}

static inline int decode_format(const uint8_t *mem,const Nv2aGpuTexture *texture,const uint8_t *p,
                                uint32_t fmt,uint32_t u,uint32_t *argb,int canonical);
static int decode_texture(const uint8_t *mem,const Nv2aGpuTexture *texture,
                        uint32_t face_offset,uint32_t u,uint32_t v,uint32_t z,uint32_t *argb,int canonical)
{
    if(!texture->valid || u>=texture->width || v>=texture->height || z>=(texture->depth?texture->depth:1))return 0;
    uint32_t fmt=texture->color,base=texture->offset+face_offset;
    const uint8_t *p;
    uint32_t block=d3d8_format_dxt_block_bytes(fmt);
    if(block) {
        p=mem+base+nv2a_texture_dxt_block_offset(texture,u,v,z);
        return d3d8_dxt_decode_texel_matching(p,fmt,u&3u,v&3u,4,argb,canonical);
    }
    if(d3d8_format_is_swizzled(fmt)) {
        fmt=linear_twin(fmt);
        p=mem+base;
        u=texture->dimensionality==3?xbox_swizzle_offset_3d(u,v,z,texture->width,texture->height,texture->depth):
                                swizzle_offset(u,v,texture->width,texture->height);
    } else p=mem+base+((size_t)z*texture->height+v)*texture->pitch;
    return decode_format(mem,texture,p,fmt,u,argb,canonical);
}

/* One texel of a linear (or de-swizzled) layout: p is the row or image base, u the texel index. */
static inline int decode_format(const uint8_t *mem,const Nv2aGpuTexture *texture,const uint8_t *p,
                                uint32_t fmt,uint32_t u,uint32_t *argb,int canonical)
{
    switch (fmt) {

    /* 32-bit, alpha-red-green-blue in the dword. */
    case 0x12:                                      /* LIN_A8R8G8B8 */
        *argb = ((const uint32_t *)p)[u];
        return 1;
    case 0x1E:                                      /* LIN_X8R8G8B8 */
        *argb = ((const uint32_t *)p)[u] | 0xFF000000u;
        return 1;

    /* 8-bit palette index, swizzled. Burnout 3 draws its logo and frontend
     * header art this way; without a case every such quad came out as the
     * unsupported-format fill, a white box where the logo should be. */
    case 0x0B:                                      /* SZ_I8_A8R8G8B8 */
        if (!texture->palette)
            return 0;
        *argb = ((const uint32_t *)(mem + texture->palette))[p[u]];
        return 1;

    /* 32-bit, other channel orders. The name gives the byte order from the
     * top of the dword down, so each is a permutation of the same four. */
    case 0x3F: {                                    /* LIN_A8B8G8R8 */
        uint32_t t = ((const uint32_t *)p)[u];
        *argb = (t & 0xFF00FF00u) | ((t & 0xFF) << 16) | ((t >> 16) & 0xFF);
        return 1;
    }
    case 0x40: {                                    /* LIN_B8G8R8A8 */
        uint32_t t = ((const uint32_t *)p)[u];
        *argb = ((t & 0xFFu) << 24)                 /* A, from the bottom */
              | (((t >>  8) & 0xFFu) << 16)         /* R */
              | (((t >> 16) & 0xFFu) <<  8)         /* G */
              |  ((t >> 24) & 0xFFu);               /* B, from the top */
        return 1;
    }
    case 0x41: {                                    /* LIN_R8G8B8A8 */
        uint32_t t = ((const uint32_t *)p)[u];
        *argb = ((t & 0xFFu) << 24) | (t >> 8);
        return 1;
    }

    /* 16-bit. */
    case 0x17:                                      /* LIN_G8B8 */
        *argb = 0xFF000000u | ((const uint16_t *)p)[u];
        return 1;
    case 0x16: {                                    /* LIN_R8B8 */
        uint32_t t = ((const uint16_t *)p)[u];
        *argb = 0xFF000000u | ((t & 0xFF00u) << 8) | (t & 0xFFu);
        return 1;
    }
    case 0x10: {                                    /* LIN_A1R5G5B5 */
        uint32_t t = ((const uint16_t *)p)[u];
        *argb = ((t & 0x8000u) ? 0xFF000000u : 0u)
              | (expand((t >> 10) & 0x1F, 5,canonical) << 16)
              | (expand((t >>  5) & 0x1F, 5,canonical) <<  8)
              |  expand( t        & 0x1F, 5,canonical);
        return 1;
    }
    case 0x1C: {                                    /* LIN_X1R5G5B5 */
        uint32_t t = ((const uint16_t *)p)[u];
        *argb = 0xFF000000u
              | (expand((t >> 10) & 0x1F, 5,canonical) << 16)
              | (expand((t >>  5) & 0x1F, 5,canonical) <<  8)
              |  expand( t        & 0x1F, 5,canonical);
        return 1;
    }
    case 0x11: {                                    /* LIN_R5G6B5 */
        uint32_t t = ((const uint16_t *)p)[u];
        *argb = 0xFF000000u
              | (expand((t >> 11) & 0x1F, 5,canonical) << 16)
              | (expand((t >>  5) & 0x3F, 6,canonical) <<  8)
              |  expand( t        & 0x1F, 5,canonical);
        return 1;
    }
    case 0x1D: {                                    /* LIN_A4R4G4B4 */
        uint32_t t = ((const uint16_t *)p)[u];
        *argb = (expand((t >> 12) & 0x0F, 4,canonical) << 24)
              | (expand((t >>  8) & 0x0F, 4,canonical) << 16)
              | (expand((t >>  4) & 0x0F, 4,canonical) <<  8)
              |  expand( t        & 0x0F, 4,canonical);
        return 1;
    }

    case 0x20: {                                    /* LIN_A8Y8 */
        uint32_t value = ((const uint16_t *)p)[u], y = value & 255u;
        *argb = ((value >> 8) << 24) | (y << 16) | (y << 8) | y;
        return 1;
    }
    case 0x1B: {                                    /* LIN_AY8: same byte is Y and A */
        uint32_t y = p[u];
        *argb = y * 0x01010101u;
        return 1;
    }

    /* 8-bit. */
    case 0x13: {                                    /* LIN_L8 */
        uint32_t t = p[u];
        *argb = 0xFF000000u | (t << 16) | (t << 8) | t;
        return 1;
    }
    case 0x1F:                                      /* LIN_A8 */
        *argb = ((uint32_t)p[u] << 24) | 0x00FFFFFFu;
        return 1;

    /* 4:2:2 packed YUV, two texels per four bytes.
     *
     * This is how a title hands over a decoded video frame, and without it
     * the frame falls through to `default` -- which returns 0, so the caller
     * paints the quad's vertex colour and the movie is a flat rectangle.
     *
     * The chroma pair is shared between an even texel and the one after it,
     * so the group is found by masking the bottom bit of the index. BT.601,
     * the same coefficients the D3D8 upload path converts with, so the two
     * paths agree rather than each having its own idea of the colour. */
    case 0x24:                                      /* LC_CR8YB8CB8YA8, YUY2 */
    case 0x25: {                                    /* LC_YB8CR8YA8CB8, UYVY */
        uint32_t yoff = (fmt == 0x24) ? 0u : 1u;
        const uint8_t *g = p + (size_t)(u & ~1u) * 2;
        int c  = (int)g[(u & 1u) ? 2 + yoff : yoff] - 16;
        int cu = (int)g[1 - yoff] - 128;
        int cv = (int)g[3 - yoff] - 128;
        int r = (298 * c + 409 * cv + 128) >> 8;
        int gg = (298 * c - 100 * cu - 208 * cv + 128) >> 8;
        int b = (298 * c + 516 * cu + 128) >> 8;
        if (r < 0) r = 0;
        if (r > 255) r = 255;
        if (gg < 0) gg = 0;
        if (gg > 255) gg = 255;
        if (b < 0) b = 0;
        if (b > 255) b = 255;
        *argb = 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)gg << 8)
              | (uint32_t)b;
        return 1;
    }

    default:
        return 0;
    }
}
int nv2a_texture_decode(const uint8_t *mem,const Nv2aGpuTexture *t,uint32_t face,uint32_t x,uint32_t y,uint32_t z,uint32_t *argb)
{
    return decode_texture(mem,t,face,x,y,z,argb,0);
}
int nv2a_texture_decode_canonical(const uint8_t *mem,const Nv2aGpuTexture *t,uint32_t face,uint32_t x,uint32_t y,uint32_t z,uint32_t *argb)
{
    return decode_texture(mem,t,face,x,y,z,argb,1);
}

/* Texels by..by+n of one DXT block row, exactly as d3d8_dxt_decode_texel_matching computes each
 * (same arithmetic, same canonical expansion; DXT3's explicit alpha keeps the plain expansion),
 * with the endpoint and alpha tables built once instead of once per texel. */
static void dxt_decode_block_row(const uint8_t *b,uint32_t fmt,uint32_t by,uint32_t n,uint32_t *out,int canonical)
{
    uint32_t block_bytes=d3d8_format_dxt_block_bytes(fmt);
    uint32_t alpha[4]={255u,255u,255u,255u},c[4]={0},c0,c1,i,idx;
    if(block_bytes==16) {
        if(fmt==0x0E) {
            for(i=0;i<n;i++) {
                uint32_t texel=by*4u+i,nib=b[texel>>1];
                alpha[i]=d3d8_expand_channel((texel&1u)?(nib>>4):(nib&0x0Fu),4);
            }
        } else {
            uint32_t a0=b[0],a1=b[1],a[8];
            uint64_t bits=0;
            for(i=0;i<6;i++)bits|=(uint64_t)b[2+i]<<(8*i);
            a[0]=a0;a[1]=a1;
            if(a0>a1)
                for(i=1;i<7;i++)a[i+1]=((7-i)*a0+i*a1)/7;
            else {
                for(i=1;i<5;i++)a[i+1]=((5-i)*a0+i*a1)/5;
                a[6]=0;a[7]=255;
            }
            for(i=0;i<n;i++)alpha[i]=a[(uint32_t)((bits>>(3*(by*4u+i)))&7u)];
        }
        b+=8;
    }
    c0=(uint32_t)b[0]|((uint32_t)b[1]<<8);
    c1=(uint32_t)b[2]|((uint32_t)b[3]<<8);
    for(i=0;i<2;i++) {
        uint32_t rgb=i?c1:c0;
        c[i]=(d3d8_expand_channel_matching((rgb>>11)&0x1Fu,5,canonical)<<16)
            |(d3d8_expand_channel_matching((rgb>> 5)&0x3Fu,6,canonical)<< 8)
            | d3d8_expand_channel_matching( rgb     &0x1Fu,5,canonical);
    }
    if(c0>c1 || block_bytes==16) {
        for(i=0;i<3;i++) {
            uint32_t sh=i*8,m0=(c[0]>>sh)&0xFFu,m1=(c[1]>>sh)&0xFFu;
            c[2]=(c[2]&~(0xFFu<<sh))|(((2*m0+m1)/3)<<sh);
            c[3]=(c[3]&~(0xFFu<<sh))|(((m0+2*m1)/3)<<sh);
        }
    } else {
        for(i=0;i<3;i++) {
            uint32_t sh=i*8,m0=(c[0]>>sh)&0xFFu,m1=(c[1]>>sh)&0xFFu;
            c[2]=(c[2]&~(0xFFu<<sh))|(((m0+m1)/2)<<sh);
        }
        c[3]=0;
    }
    uint32_t word=(uint32_t)b[4]|((uint32_t)b[5]<<8)|((uint32_t)b[6]<<16)|((uint32_t)b[7]<<24);
    for(i=0;i<n;i++) {
        uint32_t a=alpha[i];
        idx=(word>>(2*(by*4u+i)))&3u;
        if(block_bytes==8 && c0<=c1 && idx==3)a=0;
        out[i]=(a<<24)|(c[idx]&0x00FFFFFFu);
    }
}

/* The texture pack fingerprints every new texture: decode it all, then SHA-256. Decoding texel by
 * texel recomputed the swizzle masks and both deposits, or the whole DXT block, for every texel --
 * about 140 ns each on an i7-1260P, so a burst of new textures (an explosion's sprites) cost
 * hundreds of milliseconds of render thread. This decodes a row with the masks found once (the
 * swizzle is deposit(x) | deposit(y), and X steps by the masked increment) and each DXT block row
 * decoded once. Values are those of nv2a_texture_decode_canonical; RECOMP_PACK_VERIFY=1 checks. */
static int decode_row(const uint8_t *mem,const Nv2aGpuTexture *texture,
                      uint32_t face_offset,uint32_t v,uint32_t z,uint32_t *out,int canonical)
{
    if(!texture->valid || v>=texture->height || z>=(texture->depth?texture->depth:1))return 0;
    uint32_t fmt=texture->color,base=texture->offset+face_offset,w=texture->width,u;
    if(d3d8_format_dxt_block_bytes(fmt)) {
        /* The upload path's per-texel DXT decode is the executor's block cache over
         * d3d8_dxt_decode_texel, not this table; leave those (only tiny or volume DXT
         * reach the per-texel upload) on the per-texel path. */
        if(!canonical)return -1;
        for(u=0;u<w;u+=4) {
            const uint8_t *p=mem+base+nv2a_texture_dxt_block_offset(texture,u,v,z);
            dxt_decode_block_row(p,fmt,v&3u,w-u<4u?w-u:4u,out+u,1);
        }
        return 1;
    }
    if(d3d8_format_is_swizzled(fmt)) {
        if(texture->dimensionality==3)return -1;
        uint32_t mask_x,mask_y,ox=0,oy;
        xbox_swizzle_masks(texture->width,texture->height,&mask_x,&mask_y);
        oy=swizzle_deposit(v,mask_y);
        fmt=linear_twin(fmt);
        const uint8_t *p=mem+base;
        for(u=0;u<w;u++) {
            if(!decode_format(mem,texture,p,fmt,ox|oy,out+u,canonical))return 0;
            ox=(ox-mask_x)&mask_x;
        }
        return 1;
    }
    const uint8_t *p=mem+base+((size_t)z*texture->height+v)*texture->pitch;
    for(u=0;u<w;u++)if(!decode_format(mem,texture,p,fmt,u,out+u,canonical))return 0;
    return 1;
}
int nv2a_texture_decode_row_canonical(const uint8_t *mem,const Nv2aGpuTexture *texture,
                                      uint32_t face_offset,uint32_t v,uint32_t z,uint32_t *out)
{
    return decode_row(mem,texture,face_offset,v,z,out,1);
}
/* T2-3b H1: the same row decode with the plain (non-canonical) expansion, for texture uploads. */
int nv2a_texture_decode_row(const uint8_t *mem,const Nv2aGpuTexture *texture,
                            uint32_t face_offset,uint32_t v,uint32_t z,uint32_t *out)
{
    return decode_row(mem,texture,face_offset,v,z,out,0);
}

