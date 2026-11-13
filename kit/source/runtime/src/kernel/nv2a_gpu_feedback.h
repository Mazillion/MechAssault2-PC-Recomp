/* Conservative source bounds for ordinary projective 2D feedback. */
typedef struct {
    float lo[2],hi[2];
    unsigned valid,samples,q_mode;
} GpuFeedbackBounds;

static unsigned sampled_texture_mask(const Nv2aGpuState *g)
{
    if(!g->use_rc)return g->textured?1:0;
    unsigned mask=0;
    for(unsigned i=0;i<4;i++) {
        unsigned mode=(g->rc.stage_program>>(i*5))&31;
        /* These stages consume coordinates or prior-stage values only. */
        if(mode!=0 && mode!=4 && mode!=5 && mode!=10 && mode!=17)mask|=1u<<i;
    }
    return mask;
}

static unsigned feedback_bounds_begin(const Nv2aGpuState *g,GpuFeedbackBounds bounds[4])
{
    memset(bounds,0,sizeof(*bounds)*4);
    static int full_copy=-1;
    if(full_copy<0){const char *v=getenv("RECOMP_FEEDBACK_FULL_COPY");full_copy=v&&atoi(v)!=0;}
    if(full_copy)return 0;
    unsigned mask=sampled_texture_mask(g),result=0;
    for(unsigned i=0;i<4;i++) {
        const Nv2aGpuTexture *t=&g->textures[i];
        unsigned mode=g->use_rc?(g->rc.stage_program>>(i*5))&31:1;
        if((mask&(1u<<i)) && t->valid && t->offset==g->color_va && t->width && t->height &&
           t->dimensionality!=3 && !t->cube && !d3d8_format_is_swizzled(t->color) &&
           !d3d8_format_dxt_block_bytes(t->color) && (mode==1 || mode==2)) {
            bounds[i].valid=1;result|=1u<<i;
        }
    }
    return result;
}

static void feedback_bounds_add(const Nv2aGpuState *g,GpuFeedbackBounds bounds[4],
                                const Nv2aVshOutput *out)
{
    for(unsigned i=0;i<4;i++)if(bounds[i].valid) {
        GpuFeedbackBounds *b=&bounds[i];const float *c=out->tex[i];
        unsigned q_mode=c[3]==0?1:c[3]>=.001f?2:0;
        if(!isfinite(out->pos[3]) || out->pos[3]<=1e-8f || !q_mode ||
           !isfinite(c[0]) || !isfinite(c[1]) || !isfinite(c[3]) ||
           (b->q_mode && b->q_mode!=q_mode)) {b->valid=0;continue;}
        b->q_mode=q_mode;
        /* With positive raster W and one nonzero Q sign, the interpolated
         * projected coordinate stays inside the vertex coordinate hull.
         * All-zero Q follows the shader's explicit division-by-one rule. */
        float uv[2]={c[0]/(c[3]?c[3]:1)/g->textures[i].width,
                     c[1]/(c[3]?c[3]:1)/g->textures[i].height};
        if(!isfinite(uv[0]) || !isfinite(uv[1])){b->valid=0;continue;}
        for(unsigned a=0;a<2;a++) {
            if(!b->samples)b->lo[a]=b->hi[a]=uv[a];
            else {b->lo[a]=fminf(b->lo[a],uv[a]);b->hi[a]=fmaxf(b->hi[a],uv[a]);}
        }
        b->samples++;
    }
}

static int feedback_bounds_box(const GpuFeedbackBounds *b,const Nv2aGpuTexture *t,
                                uint32_t width,uint32_t height,D3D11_BOX *box)
{
    if(!b || !b->valid || !b->samples)return 0;
    const uint32_t sizes[2]={width,height},address[2]={t->addr_u,t->addr_v};
    unsigned lo[2],hi[2];
    for(unsigned a=0;a<2;a++) {
        /* Include filtering/anisotropy and CPU/GPU floating-point differences.
         * Wrap near either edge can fetch the opposite edge, so use the full
         * resource there rather than retaining a stale wrapped texel. */
        float margin=16+fmaxf(fabsf(b->lo[a]),fabsf(b->hi[a]))*sizes[a]*.002f;
        if((address[a]==1 || address[a]==2) &&
           (b->lo[a]*sizes[a]<margin || b->hi[a]*sizes[a]+margin>sizes[a]))return 0;
        float minimum=fmaxf(0,fminf(1,b->lo[a]))*sizes[a]-margin;
        float maximum=fmaxf(0,fminf(1,b->hi[a]))*sizes[a]+margin;
        lo[a]=(unsigned)fmaxf(0,floorf(minimum));
        hi[a]=(unsigned)fminf((float)sizes[a],ceilf(maximum));
        if(lo[a]>=hi[a])return 0;
    }
    *box=(D3D11_BOX){lo[0],lo[1],0,hi[0],hi[1],1};return 1;
}
