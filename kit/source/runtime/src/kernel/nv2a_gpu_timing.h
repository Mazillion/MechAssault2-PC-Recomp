/* Opt-in, one-frame GPU timestamps. No queries exist in normal gameplay. */
#define GPU_TIMING_DRAWS 4096
typedef struct {
    ID3D11Query *before,*after;
    uintptr_t shader;
    uint32_t surface,width,height,indices,stage_program,stages;
    uint32_t depth_test,depth_mask,depth_func,z_clamp,alpha_test,blend_src,blend_dst;
    float clip_min,clip_max;
    Nv2aGpuTexture textures[4];
} GpuTimedDraw;
static struct {
    const char *flag,*csv;
    int configured,active,finished;
    unsigned count;
    ID3D11Query *disjoint;
    GpuTimedDraw *draws;
} gpu_timing;

static HRESULT gpu_timing_result(ID3D11DeviceContext *context,ID3D11Query *query,
                                void *data,UINT bytes,ULONGLONG deadline)
{
    HRESULT hr;
    while((hr=ID3D11DeviceContext_GetData(context,(ID3D11Asynchronous *)query,data,bytes,
                                        D3D11_ASYNC_GETDATA_DONOTFLUSH))==S_FALSE) {
        if(GetTickCount64()>=deadline)return E_PENDING;
        Sleep(1);
    }
    return hr;
}

static void gpu_timing_release(void)
{
    if(gpu_timing.draws) {
        for(unsigned i=0;i<GPU_TIMING_DRAWS;i++) {
            RELEASE(gpu_timing.draws[i].before);RELEASE(gpu_timing.draws[i].after);
        }
        free(gpu_timing.draws);gpu_timing.draws=NULL;
    }
    RELEASE(gpu_timing.disjoint);gpu_timing.active=0;gpu_timing.finished=1;
}

static void gpu_timing_present(ID3D11Device *device,ID3D11DeviceContext *context)
{
    if(!gpu_timing.configured) {
        gpu_timing.flag=getenv("RECOMP_GPU_TIMING_FLAG");
        gpu_timing.csv=getenv("RECOMP_GPU_TIMING_CSV");
        gpu_timing.configured=1;
    }
    if(!gpu_timing.flag || !gpu_timing.csv || gpu_timing.finished)return;
    if(gpu_timing.active) {
        ID3D11DeviceContext_End(context,(ID3D11Asynchronous *)gpu_timing.disjoint);
        ID3D11DeviceContext_Flush(context);
        ULONGLONG deadline=GetTickCount64()+5000;
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT clock={0};
        HRESULT hr=gpu_timing_result(context,gpu_timing.disjoint,&clock,sizeof(clock),deadline);
        FILE *file=SUCCEEDED(hr) && !clock.Disjoint && clock.Frequency ? fopen(gpu_timing.csv,"w") : NULL;
        if(file) {
            fprintf(file,"draw,gpu_ms,shader,surface,width,height,indices,stage_program,stages,depth_test,depth_mask,depth_func,z_clamp,clip_min,clip_max,alpha_test,blend_src,blend_dst,tex0,tex1,tex2,tex3\n");
            for(unsigned i=0;i<gpu_timing.count;i++) {
                GpuTimedDraw *d=&gpu_timing.draws[i];UINT64 before=0,after=0;
                if(FAILED(gpu_timing_result(context,d->before,&before,sizeof(before),deadline)) ||
                   FAILED(gpu_timing_result(context,d->after,&after,sizeof(after),deadline))) {hr=E_PENDING;break;}
                fprintf(file,"%u,%.9g,%llX,%08X,%u,%u,%u,%05X,%u,%u,%u,%X,%u,%.9g,%.9g,%u,%X,%X,%08X,%08X,%08X,%08X\n",
                    i,(double)(after-before)*1000/clock.Frequency,(unsigned long long)d->shader,
                    d->surface,d->width,d->height,d->indices,d->stage_program,d->stages,
                    d->depth_test,d->depth_mask,d->depth_func,d->z_clamp,d->clip_min,d->clip_max,
                    d->alpha_test,d->blend_src,d->blend_dst,d->textures[0].valid?d->textures[0].offset:0,
                    d->textures[1].valid?d->textures[1].offset:0,d->textures[2].valid?d->textures[2].offset:0,
                    d->textures[3].valid?d->textures[3].offset:0);
            }
            fclose(file);
        }
        fprintf(stderr,"[GPU-TIMING] finished draws=%u capacity=%u status=%08X disjoint=%u frequency=%llu csv=%s\n",
            gpu_timing.count,GPU_TIMING_DRAWS,(unsigned)hr,(unsigned)clock.Disjoint,
            (unsigned long long)clock.Frequency,gpu_timing.csv);
        gpu_timing_release();return;
    }
    if(GetFileAttributesA(gpu_timing.flag)==INVALID_FILE_ATTRIBUTES)return;
    gpu_timing.draws=calloc(GPU_TIMING_DRAWS,sizeof(*gpu_timing.draws));
    if(!gpu_timing.draws){gpu_timing_release();return;}
    D3D11_QUERY_DESC desc={D3D11_QUERY_TIMESTAMP_DISJOINT,0};
    HRESULT hr=ID3D11Device_CreateQuery(device,&desc,&gpu_timing.disjoint);
    desc.Query=D3D11_QUERY_TIMESTAMP;
    for(unsigned i=0;i<GPU_TIMING_DRAWS && SUCCEEDED(hr);i++) {
        hr=ID3D11Device_CreateQuery(device,&desc,&gpu_timing.draws[i].before);
        if(SUCCEEDED(hr))hr=ID3D11Device_CreateQuery(device,&desc,&gpu_timing.draws[i].after);
    }
    if(FAILED(hr)) {
        fprintf(stderr,"[GPU-TIMING] query creation failed %08X\n",(unsigned)hr);
        gpu_timing_release();return;
    }
    ID3D11DeviceContext_Begin(context,(ID3D11Asynchronous *)gpu_timing.disjoint);
    gpu_timing.active=1;
}

static GpuTimedDraw *gpu_timing_begin_draw(ID3D11DeviceContext *context,const Nv2aGpuState *g,
                                          uint32_t width,uint32_t height,uint32_t indices,
                                          ID3D11PixelShader *shader)
{
    if(!gpu_timing.active || gpu_timing.count>=GPU_TIMING_DRAWS)return NULL;
    GpuTimedDraw *d=&gpu_timing.draws[gpu_timing.count++];
    d->shader=(uintptr_t)shader;d->surface=g->color_va;d->width=width;d->height=height;
    d->indices=indices;d->stage_program=g->rc.stage_program;d->stages=g->rc.control&255;
    d->depth_test=g->depth_test;d->depth_mask=g->depth_mask;d->depth_func=g->depth_func;
    d->z_clamp=g->z_clamp;d->clip_min=g->clip_min;d->clip_max=g->clip_max;
    d->alpha_test=g->alpha_test;d->blend_src=g->blend_enable?g->blend_src:0;
    d->blend_dst=g->blend_enable?g->blend_dst:0;memcpy(d->textures,g->textures,sizeof(d->textures));
    ID3D11DeviceContext_End(context,(ID3D11Asynchronous *)d->before);return d;
}
