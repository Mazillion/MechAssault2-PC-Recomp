/* Live DDS replacements for the original immutable texture uploads.
 * DirectXTex supplies the DDS parser/resource loader; jsmn reads the pack index. */
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "nv2a_texture_pack.h"
#include "../platform/pc_cpu_topo.h"   /* T2-0: thread roles */
#include "../platform/pc_frontend.h"
#include <windows.h>
#include <bcrypt.h>
#include <wrl/client.h>
#include "vendor/directxtex/DDSTextureLoader11.h"
#define JSMN_STATIC
#define JSMN_STRICT
#include "vendor/jsmn/jsmn.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
using Microsoft::WRL::ComPtr;
namespace fs=std::filesystem;
namespace {
constexpr uint64_t cache_limit=512ull*1024*1024;
constexpr size_t file_limit=256u*1024*1024;
/* Only actual miss/switch work owns a timer. RAII also records an attempted
 * read/create/digest when the existing fallback exits early or throws. The
 * shared counter API is thread-local; compiler workers never touch it. */
struct CpuPhase {
    Nv2aCpuPhase phase;
    double start;
    uint64_t bytes=0;
    explicit CpuPhase(Nv2aCpuPhase p):phase(p),start(nv2a_gpu_cpu_begin()){}
    ~CpuPhase(){nv2a_gpu_cpu_end(phase,start,bytes);}
};
struct Hash {
    BCRYPT_HASH_HANDLE handle=nullptr;
    static BCRYPT_ALG_HANDLE algorithm() {
        static BCRYPT_ALG_HANDLE value=[](){BCRYPT_ALG_HANDLE h=nullptr;
            return BCryptOpenAlgorithmProvider(&h,BCRYPT_SHA256_ALGORITHM,nullptr,0)>=0?h:nullptr;}();
        return value;
    }
    Hash(){if(algorithm())BCryptCreateHash(algorithm(),&handle,nullptr,0,nullptr,0,0);}
    ~Hash(){if(handle)BCryptDestroyHash(handle);}
    bool add(const void *p,size_t n){return handle && n<=ULONG_MAX && BCryptHashData(handle,(PUCHAR)p,(ULONG)n,0)>=0;}
    bool finish(uint8_t *p){return handle && BCryptFinishHash(handle,p,32,0)>=0;}
};
std::string hex(const uint8_t *data) {
    static const char digits[]="0123456789abcdef";std::string result(64,'0');
    for(int i=0;i<32;i++){result[i*2]=digits[data[i]>>4];result[i*2+1]=digits[data[i]&15];}return result;
}
bool digest_text(const std::string &s) {
    return s.size()==64 && std::all_of(s.begin(),s.end(),[](char c){return (c>='0'&&c<='9')||(c>='a'&&c<='f');});
}
std::vector<uint8_t> read_file(const fs::path &path,size_t limit) {
    std::ifstream f(path,std::ios::binary|std::ios::ate);
    if(!f)throw std::runtime_error("file missing");auto size=f.tellg();
    if(size<0 || (uint64_t)size>limit)throw std::runtime_error("file size invalid");
    std::vector<uint8_t> bytes((size_t)size);f.seekg(0);
    if(!f.read((char *)bytes.data(),size))throw std::runtime_error("file read failed");return bytes;
}
struct Json {
    std::string text;std::vector<jsmntok_t> tokens;std::vector<int> after;
    explicit Json(const fs::path &path) {
        auto data=read_file(path,32u*1024*1024);text.assign(data.begin(),data.end());
        jsmn_parser parser;jsmn_init(&parser);
        int count=jsmn_parse(&parser,text.data(),text.size(),nullptr,0);
        if(count<=0 || count>2000000)throw std::runtime_error("invalid pack JSON");
        tokens.resize(count);jsmn_init(&parser);
        if(jsmn_parse(&parser,text.data(),text.size(),tokens.data(),count)!=count || tokens[0].type!=JSMN_OBJECT)
            throw std::runtime_error("invalid pack JSON");
        after.resize(count);
        for(int i=count-1;i>=0;i--){int j=i+1;while(j<count && tokens[j].start<tokens[i].end)j=after[j];after[i]=j;}
        if(after[0]!=count)throw std::runtime_error("trailing pack JSON");
    }
    std::string value(int i) const {return i>=0?text.substr(tokens[i].start,tokens[i].end-tokens[i].start):std::string();}
    int member(int object,const char *key) const {
        if(object<0 || tokens[object].type!=JSMN_OBJECT)return -1;
        for(int i=object+1;i<after[object];){int v=i+1;if(value(i)==key)return v;i=after[v];}return -1;
    }
    std::string string(int object,const char *key) const {
        int i=member(object,key);return i>=0&&tokens[i].type==JSMN_STRING?value(i):std::string();
    }
    uint32_t number(int object,const char *key,uint32_t fallback=0) const {
        int i=member(object,key);if(i<0)return fallback;
        std::string s=value(i);if(s.empty() || s.size()>10 || !std::all_of(s.begin(),s.end(),[](char c){return c>='0'&&c<='9';}))return fallback;
        auto n=std::stoull(s);return n<=UINT32_MAX?(uint32_t)n:fallback;
    }
};
struct PendingRead;
struct Resource {
    std::string file,sha,name,kind;
    uint32_t width=0,height=0,depth=0,frames=0,mips=0,scale=0;
    uint64_t used=0,bytes=0;
    int status=0;
    std::shared_ptr<PendingRead> pending;   /* async read in flight (see read_worker) */
    ComPtr<ID3D11Resource> gpu;
    std::unordered_map<unsigned,ComPtr<ID3D11ShaderResourceView>> views;
};
struct Entry {size_t resource;uint32_t slice;};
struct Pack {
    bool attempted=false;fs::path folder;
    std::vector<Resource> resources;std::vector<Entry> entries;
    std::unordered_map<std::string,uint32_t> matches;
    std::unordered_set<uint64_t> dims;   /* T2-3 B6.7: (w,h,volume,cube) of every resource */
};
uint64_t dims_key(uint32_t w,uint32_t h,bool volume,bool cube){return (uint64_t)w|(uint64_t)h<<16|(uint64_t)volume<<32|(uint64_t)cube<<33;}
Pack packs[2];Pack *active=nullptr;
int quality=-1;uint64_t generation=0,serial=0;
Nv2aTexturePackStats stats={};
bool tracing(){static bool on=getenv("RECOMP_TEXTURE_PACK_TRACE")!=nullptr;return on;}
void load_index(Pack &p,int scale) {
    p.attempted=true;
    const char *root=getenv("RECOMP_TEXTURE_PACK_DIR");
    p.folder=fs::u8path(root&&*root?root:"game_files/texture_packs")/(std::to_string(scale)+"x");
    try {
        Json j(p.folder/"index.json");
        if(j.number(0,"schema")!=1 || j.number(0,"scale")!=(unsigned)scale)throw std::runtime_error("unsupported pack schema/scale");
        int resources=j.member(0,"resources");if(resources<0 || j.tokens[resources].type!=JSMN_OBJECT)throw std::runtime_error("missing resources");
        for(int i=resources+1;i<j.after[resources];){
            int object=i+1;i=j.after[object];Resource r;
            r.file=j.string(object,"dds_file");r.sha=j.string(object,"dds_sha256");r.name=j.string(object,"name");r.kind=j.string(object,"kind");
            r.width=j.number(object,"original_width");r.height=j.number(object,"original_height");r.depth=j.number(object,"original_depth",1);
            r.frames=j.number(object,"frames",1);r.mips=j.number(object,"mip_levels");r.scale=scale;
            if(!r.width || !r.height || !r.depth || !r.frames || !r.mips || r.width>4096 || r.height>4096 || r.depth>2048 || r.frames>2048 || r.mips>15 ||
               r.file.size()!=77 || r.file.compare(0,9,"textures/") || r.file.compare(73,4,".dds") || !digest_text(r.file.substr(9,64)) || !digest_text(r.sha) ||
               j.number(object,"width")!=r.width*scale || j.number(object,"height")!=r.height*scale || j.number(object,"depth",1)!=r.depth)continue;
            if(r.kind!="2d"&&r.kind!="array"&&r.kind!="cube"&&r.kind!="volume")continue;
            if((r.kind=="cube"&&(r.frames!=6||r.depth!=1)) || (r.kind=="volume"&&r.frames!=1))continue;
            p.dims.insert(dims_key(r.width,r.height,r.kind=="volume",r.kind=="cube"));
            size_t id=p.resources.size();p.resources.push_back(std::move(r));
            auto add=[&](const std::string &key,unsigned slice){if(!digest_text(key))return;
                uint32_t e=(uint32_t)p.entries.size();if(p.matches.emplace(key,e).second)p.entries.push_back({id,slice});};
            if(p.resources.back().kind=="array") {
                int frames=j.member(object,"original_frame_match_keys");
                if(frames>=0 && j.tokens[frames].type==JSMN_ARRAY && j.tokens[frames].size==(int)p.resources.back().frames) {
                    unsigned slice=0;for(int f=frames+1;f<j.after[frames];f=j.after[f])add(j.value(f),slice++);
                }
            } else add(j.string(object,"original_rgba_match_key"),0);
        }
        fprintf(stderr,"[TEXTURE-PACK] %dx index: %zu resources, %zu pixel matches\n",scale,p.resources.size(),p.entries.size());
    } catch(const std::exception &e) {p.resources.clear();p.entries.clear();p.matches.clear();p.dims.clear();
        fprintf(stderr,"[TEXTURE-PACK] %dx unavailable: %s; using originals\n",scale,e.what());}
}
void evict(Resource &r) {
    if(r.gpu)nv2a_gpu_cpu_count(NV2A_CPU_PACK_EVICT,1);
    stats.cache_bytes-=r.bytes;r.views.clear();r.gpu.Reset();r.bytes=0;r.status=0;
    r.pending.reset();   /* a read in flight finishes into nothing */
}
uint32_t word(const std::vector<uint8_t> &data,size_t offset){uint32_t result=0;if(offset+4<=data.size())memcpy(&result,data.data()+offset,4);return result;}
void set_word(std::vector<uint8_t> &data,size_t offset,uint32_t value){memcpy(data.data()+offset,&value,4);}
/* BGRA8 is not a D3D11 Texture3D format. Change only the in-memory channel
 * order, including every mip/slice; the user's DDS stays byte-for-byte intact. */
void volume_rgba(std::vector<uint8_t> &data) {
    if(data.size()<128 || word(data,0)!=0x20534444 || word(data,4)!=124 || word(data,76)!=32)return;
    size_t offset=128;
    if(word(data,84)==0x30315844) {
        if(data.size()<148 || word(data,132)!=4 || word(data,128)!=87)return;
        offset=148;set_word(data,128,28);
    } else {
        if(!(word(data,112)&0x200000) || word(data,88)!=32 || word(data,92)!=0xff0000 || word(data,96)!=0xff00 || word(data,100)!=0xff)return;
        set_word(data,92,0xff);set_word(data,100,0xff0000);
    }
    for(size_t i=offset;i+3<data.size();i+=4)std::swap(data[i],data[i+2]);
}
/* File read, checksum and block check. No D3D and no shared state, so it is also run off the
 * render thread (read_worker). */
std::vector<uint8_t> read_verified(const fs::path &path,const std::string &sha,const std::string &kind) {
    std::vector<uint8_t> bytes=read_file(path,file_limit);
    Hash h;uint8_t digest[32];
    if(!h.add(bytes.data(),bytes.size()) || !h.finish(digest) || hex(digest)!=sha)throw std::runtime_error("DDS checksum mismatch");
    if(bytes.size()>=148 && word(bytes,84)==0x30315844 && (word(bytes,128)==98 || word(bytes,128)==99))
        for(size_t block=148;block<bytes.size();block+=16)if(!bytes[block])throw std::runtime_error("invalid/unwritten BC7 block");
    if(kind=="volume")volume_rgba(bytes);
    return bytes;
}
bool create_gpu(ID3D11Device *device,Resource &r,std::vector<uint8_t> &bytes) {
    try {
        ComPtr<ID3D11Resource> resource;
        uint64_t needed=bytes.size();
        /* T2-3 B6.3 (R11 E3): make room before the create, so peak VRAM is the budget. */
        while(stats.cache_bytes+needed>cache_limit) {
            Resource *oldest=nullptr;
            /* Keep this draw's up-to-four views alive until D3D binds them.
             * The budget can temporarily include those current resources. */
            for(auto &v:active->resources)if(v.gpu && v.used!=serial && (!oldest||v.used<oldest->used))oldest=&v;
            if(!oldest)break;evict(*oldest);
        }
        {
        CpuPhase create(NV2A_CPU_PACK_RESOURCE);create.bytes=bytes.size();
        HRESULT hr=DirectX::CreateDDSTextureFromMemoryEx(device,bytes.data(),bytes.size(),0,D3D11_USAGE_IMMUTABLE,
            D3D11_BIND_SHADER_RESOURCE,0,0,DirectX::DDS_LOADER_IGNORE_SRGB,&resource,nullptr);
        if(FAILED(hr))throw std::runtime_error("DDS GPU load failed");
        D3D11_RESOURCE_DIMENSION kind;resource->GetType(&kind);
        if(r.kind=="volume") {
            if(kind!=D3D11_RESOURCE_DIMENSION_TEXTURE3D)throw std::runtime_error("DDS volume type mismatch");
            D3D11_TEXTURE3D_DESC d;ComPtr<ID3D11Texture3D> t;resource.As(&t);t->GetDesc(&d);
            if(d.Width!=r.width*r.scale || d.Height!=r.height*r.scale || d.Depth!=r.depth || d.MipLevels!=r.mips)throw std::runtime_error("DDS volume dimensions mismatch");
        } else {
            if(kind!=D3D11_RESOURCE_DIMENSION_TEXTURE2D)throw std::runtime_error("DDS 2D type mismatch");
            D3D11_TEXTURE2D_DESC d;ComPtr<ID3D11Texture2D> t;resource.As(&t);t->GetDesc(&d);
            if(d.Width!=r.width*r.scale || d.Height!=r.height*r.scale || d.ArraySize!=r.frames || d.MipLevels!=r.mips)throw std::runtime_error("DDS 2D dimensions mismatch");
        }
        }
        r.gpu=std::move(resource);r.bytes=needed;stats.cache_bytes+=needed;r.status=1;stats.loads++;
        if(tracing() || stats.loads<=8)fprintf(stderr,"[TEXTURE-PACK] loaded %dx %ux%ux%u %s: %s\n",r.scale,r.width*r.scale,r.height*r.scale,r.depth,r.kind.c_str(),r.name.c_str());
        return true;
    } catch(const std::exception &e) {r.status=-1;stats.failures++;if(tracing()||stats.failures<=16)fprintf(stderr,"[TEXTURE-PACK] original fallback: %s (%s)\n",r.name.c_str(),e.what());return false;}
}
bool load_gpu(ID3D11Device *device,Resource &r) {
    std::vector<uint8_t> bytes;
    try {
        CpuPhase read(NV2A_CPU_PACK_READ);
        bytes=read_verified(active->folder/fs::u8path(r.file),r.sha,r.kind);read.bytes=bytes.size();
    } catch(const std::exception &e) {r.status=-1;stats.failures++;if(tracing()||stats.failures<=16)fprintf(stderr,"[TEXTURE-PACK] original fallback: %s (%s)\n",r.name.c_str(),e.what());return false;}
    return create_gpu(device,r,bytes);
}

/* Replacement DDS files are read off the render thread (2026-10-15). Reading and checksumming
 * one took 6-26 ms on an i7-1260P laptop, on the render thread, so the first sight of new
 * effects (an explosion's sprites) froze the frame. Now the first lookup queues the read and
 * the original texture is drawn until it is done -- a few frames -- then the replacement is
 * created (about 0.25 ms) and used from then on. RECOMP_TEXTURE_PACK_ASYNC=0 reads inline. */
struct PendingRead {
    fs::path path;std::string sha,kind;
    std::vector<uint8_t> bytes;std::string error;
    std::atomic<int> state{0};          /* 0 queued, 1 read and verified, -1 failed */
};
struct ReadQueue {
    std::mutex lock;std::condition_variable wake;
    std::deque<std::shared_ptr<PendingRead>> queue;
    bool started=false;
};
ReadQueue &reads=*new ReadQueue;   /* never destroyed: the detached worker may still wait on it at exit */
std::atomic<int> level_start{0};   /* 1 while a level start waits for its replacements (nv2a_texture_pack_hold) */
void read_worker() {
    pc_thread_role_enter(PC_ROLE_PACK);
    /* Low CPU, I/O and memory priority; the original draws until it is done.
     * At a level start the frame waits for these reads (nv2a_texture_pack_hold), so they run at
     * normal priority then. */
    bool background=SetThreadPriority(GetCurrentThread(),THREAD_MODE_BACKGROUND_BEGIN)!=0;
    for(;;) {
        std::shared_ptr<PendingRead> job;
        {
            std::unique_lock<std::mutex> hold(reads.lock);
            reads.wake.wait(hold,[]{return !reads.queue.empty();});
            job=std::move(reads.queue.front());reads.queue.pop_front();
        }
        if(job.use_count()==1)continue;     /* evicted before its turn */
        bool urgent=level_start.load(std::memory_order_relaxed)!=0;
        if(urgent && background){if(SetThreadPriority(GetCurrentThread(),THREAD_MODE_BACKGROUND_END))background=false;}
        else if(!urgent && !background){if(SetThreadPriority(GetCurrentThread(),THREAD_MODE_BACKGROUND_BEGIN))background=true;}
        try {job->bytes=read_verified(job->path,job->sha,job->kind);job->state.store(1,std::memory_order_release);}
        catch(const std::exception &e) {job->error=e.what();job->state.store(-1,std::memory_order_release);}
    }
}
bool async_reads() {
    static int on=-1;
    if(on<0){const char *e=getenv("RECOMP_TEXTURE_PACK_ASYNC");on=!(e && *e=='0');}
    return on!=0;
}
void queue_read(Resource &r) {
    auto job=std::make_shared<PendingRead>();
    job->path=active->folder/fs::u8path(r.file);job->sha=r.sha;job->kind=r.kind;
    {   /* T2-3 B6.2 (R11 E2): a failed thread start must not leave r.pending set forever */
        std::lock_guard<std::mutex> hold(reads.lock);
        if(!reads.started){std::thread(read_worker).detach();reads.started=true;}
        reads.queue.push_back(job);
    }
    reads.wake.notify_one();
    r.pending=std::move(job);
}
}
extern "C" int nv2a_texture_decode_row_canonical(const uint8_t *memory,const Nv2aGpuTexture *texture,
                                                 uint32_t face_offset,uint32_t y,uint32_t z,uint32_t *argb);
extern "C" ptrdiff_t xbox_GetMemoryOffset(void);
/* rows: decode a whole row at once (nv2a_texture_decode_row_canonical) where it can, else texel
 * by texel through `decode`. RECOMP_PACK_VERIFY=1 fingerprints every texture both ways and logs
 * any difference ([TEXTURE-PACK] verify); the per-texel digest is the one used then. */
static int pack_fingerprint(const Nv2aGpuTexture *t,Nv2aGpuTexel decode,uint32_t stride,uint8_t digest[32],bool rows,uint64_t *bytes) {
    uint32_t depth=t->dimensionality==3?t->depth:1,frames=t->cube?6:1;
    uint32_t header[]={1,t->width,t->height,depth,frames};Hash h;
    if(!h.add(header,sizeof(header)))return 0;std::vector<uint8_t> row((size_t)t->width*4);
    std::vector<uint32_t> argb(t->width);
    const uint8_t *memory=(const uint8_t *)xbox_GetMemoryOffset();
    for(uint32_t f=0;f<frames;f++)for(uint32_t z=0;z<depth;z++)for(uint32_t y=0;y<t->height;y++) {
        int whole=rows?nv2a_texture_decode_row_canonical(memory,t,f*stride,y,z,argb.data()):-1;
        if(whole==0)return 0;
        if(whole<0)for(uint32_t x=0;x<t->width;x++)
            if(!decode(t,f*stride,x,y,z,&argb[x])){*bytes+=(uint64_t)x*4;return 0;}
        for(uint32_t x=0;x<t->width;x++){uint32_t c=argb[x];
            row[x*4]=(uint8_t)(c>>16);row[x*4+1]=(uint8_t)(c>>8);row[x*4+2]=(uint8_t)c;row[x*4+3]=(uint8_t)(c>>24);
        }
        *bytes+=row.size();
        if(!h.add(row.data(),row.size()))return 0;
    }
    return h.finish(digest);
}
extern "C" int nv2a_texture_pack_fingerprint(const Nv2aGpuTexture *t,Nv2aGpuTexel decode,uint32_t stride,uint8_t digest[32]) {
    if(!t || !decode || !t->width || !t->height || t->width>4096 || t->height>4096 || t->color==0x24 || t->color==0x25)return 0;
    uint32_t depth=t->dimensionality==3?t->depth:1,frames=t->cube?6:1;
    if(!depth || depth>2048 || (uint64_t)t->width*t->height*depth*frames>64u*1024*1024)return 0;
    CpuPhase fingerprint(NV2A_CPU_PACK_DIGEST);
    static int verify=-1;
    if(verify<0){const char *e=getenv("RECOMP_PACK_VERIFY");verify=e && *e=='1';}
    try {
        if(!verify)return pack_fingerprint(t,decode,stride,digest,true,&fingerprint.bytes);
        uint8_t fast[32];uint64_t unused=0;
        int a=pack_fingerprint(t,decode,stride,digest,false,&fingerprint.bytes),b=pack_fingerprint(t,decode,stride,fast,true,&unused);
        static unsigned checked,mismatched;checked++;
        if(a!=b || (a && memcmp(fast,digest,32))) {
            if(mismatched++<20)fprintf(stderr,"[TEXTURE-PACK] verify MISMATCH format=%02X %ux%ux%u cube=%u per-texel=%d rows=%d\n",
                                       t->color,t->width,t->height,depth,(unsigned)t->cube,a,b);
        }
        if((checked&255)==0)fprintf(stderr,"[TEXTURE-PACK] verify: %u textures fingerprinted both ways, %u differ\n",checked,mismatched);
        return a;
    } catch(...) {return 0;}
}
static unsigned creates;   /* T2-3 B6.5 (R11 E5): replacement creates this frame, at most 8 */
static unsigned create_limit=8;   /* 64 while a level start holds the frame (nothing is shown then) */
static unsigned frame_waiting;    /* lookups this frame that drew the original because the replacement was not ready */
/* T2-3b test hook: RECOMP_PACK_TQ_SCHEDULE="60:2,120:0,160:1" switches texture quality at those
 * seconds after the first frame, as the settings menu would (a TQ-switch run without UI input). */
static int scheduled_quality(int selected) {
    static int parsed=0,count=0,at[8],to[8],step=0,forced=-1;static ULONGLONG start=0;
    if(!parsed) {
        parsed=1;const char *v=getenv("RECOMP_PACK_TQ_SCHEDULE");
        while(v && *v && count<8) {
            char *end=nullptr;long sec=strtol(v,&end,10);if(end==v||*end!=':')break;
            long q=strtol(end+1,&end,10);at[count]=(int)sec;to[count]=(int)std::clamp(q,0L,2L);count++;
            v=*end==','?end+1:end;
        }
        if(count)fprintf(stderr,"[TEXTURE-PACK] TQ schedule: %d steps\n",count);
    }
    if(!count)return selected;
    ULONGLONG now=GetTickCount64();if(!start)start=now;
    while(step<count && now-start>=(ULONGLONG)at[step]*1000ull){forced=to[step];
        fprintf(stderr,"[TEXTURE-PACK] TQ schedule step %d: quality %d at %d s\n",step,forced,at[step]);step++;}
    return forced>=0?forced:selected;
}
extern "C" void nv2a_texture_pack_frame(void) {
    creates=0;
    int selected=scheduled_quality(std::clamp(pc_setting_get(PC_TEXTURE_QUALITY,0),0,2));
    if(selected==quality)return;
    CpuPhase select(NV2A_CPU_PACK_SELECT);
    if(active)for(auto &r:active->resources)evict(r);   /* T2-3 B6.1: reads in flight too */
    if(quality>=0)fprintf(stderr,"[TEXTURE-PACK] previous: hits=%llu misses=%llu loads=%llu failures=%llu\n",
        (unsigned long long)stats.hits,(unsigned long long)stats.misses,(unsigned long long)stats.loads,(unsigned long long)stats.failures);
    quality=selected;++generation;active=selected?&packs[selected-1]:nullptr;stats={};stats.quality=selected;
    if(active&&!active->attempted)load_index(*active,selected==1?2:4);
    if(active)for(auto &r:active->resources)if(r.status<0)r.status=0;
    stats.entries=active?(uint32_t)active->entries.size():0;
    fprintf(stderr,"[TEXTURE-PACK] active=%s matches=%u (frame boundary)\n",selected==1?"2x Enhanced":selected==2?"4x Enhanced":"Original",stats.entries);
}
/* Level start (2026-11-18). With an enhanced pack, a new level's first frames drew the original
 * textures (and, on a first launch, vertex programs through the CPU fallback) for about a second
 * while the replacements were read on the background worker and the shaders compiled: the level
 * appeared washed out and grey, then popped to the enhanced textures. nv2a_texture_pack_level_begin
 * (called when a level's Partition5 cache opens, i.e. during its loading screen) arms a level
 * start. While armed, a frame that drew any replacement or vertex program that was not ready yet
 * is not presented (nv2a_gpu_present keeps the previous frame on screen: the loading screen or
 * its fade), the reads run at normal priority and held frames may create 64 replacements. The
 * level start ends with the first complete frame of the level's scene (100 or more draws), after
 * 6 s of holding (then it shows what it has, as before), or 120 s after arming. A warm launch reads
 * from the OS file cache and the shader cache, so it waits a few frames at most.
 * RECOMP_PACK_LEVEL_HOLD=0 turns it off. */
static std::atomic<int> level_request{0};
extern "C" void nv2a_texture_pack_level_begin(void){level_request.store(1,std::memory_order_release);}
extern "C" int nv2a_texture_pack_hold(unsigned programs_pending) {
    static int enabled=-1;
    if(enabled<0){const char *e=getenv("RECOMP_PACK_LEVEL_HOLD");enabled=!(e && *e=='0');}
    static ULONGLONG armed_at,hold_start;static unsigned held,peak_textures,peak_programs;
    static uint64_t last_serial;uint64_t draws=serial-last_serial;last_serial=serial;
    unsigned waiting=frame_waiting;frame_waiting=0;
    ULONGLONG now=GetTickCount64();
    if(level_request.exchange(0,std::memory_order_acq_rel) && enabled){
        if(!level_start.load())held=peak_textures=peak_programs=0,hold_start=0;
        level_start.store(1);armed_at=now;
    }
    create_limit=8;
    if(!level_start.load())return 0;
    if(waiting || programs_pending) {
        if(!hold_start)hold_start=now;
        held++;peak_textures=std::max(peak_textures,waiting);peak_programs=std::max(peak_programs,programs_pending);
        if(now-hold_start<6000){create_limit=64;return 1;}   /* the next frame is not shown either: create more */
        fprintf(stderr,"[TEXTURE-PACK] level start: still waiting after %llu ms (%u replacements, %u shaders); showing the level\n",
            (unsigned long long)(now-hold_start),waiting,programs_pending);
        level_start.store(0);hold_start=0;return 0;
    }
    if(hold_start) {
        fprintf(stderr,"[TEXTURE-PACK] level start: held %u frames for %llu ms until %u replacements and %u shader draws were ready (%llu draws)\n",
            held,(unsigned long long)(now-hold_start),peak_textures,peak_programs,(unsigned long long)draws);
        hold_start=0;held=peak_textures=peak_programs=0;
    }
    /* The first complete frame of the level's scene (100+ draws; a loading screen has a few)
     * ends the level start: later new textures and shaders appear as before, without a wait. */
    if(draws>=100 || now-armed_at>120000)level_start.store(0);
    return 0;
}
extern "C" void nv2a_texture_pack_device_lost(void) {
    for(auto &pack:packs)for(auto &r:pack.resources)evict(r);
    ++generation;
    stats.cache_bytes=0;
}
extern "C" void nv2a_texture_pack_draw(void){++serial;}
extern "C" ID3D11ShaderResourceView *nv2a_texture_pack_view(ID3D11Device *device,const Nv2aGpuTexture *t,
    Nv2aGpuTexel decode,uint32_t stride,Nv2aTexturePackKey *key,uint32_t *levels,uint32_t *scale) {
    *scale=1;
    try {
    if(!active || active->matches.empty() || pc_display_movie_active())return nullptr;
    if(key->valid==2)return nullptr;
    if(!key->valid && !active->dims.count(dims_key(t->width,t->height,t->dimensionality==3,!!t->cube)))
        return nullptr;   /* T2-3 B6.7: no resource of this shape; :size check below rejects it anyway */
    if(!key->valid) {if(!nv2a_texture_pack_fingerprint(t,decode,stride,key->digest)){key->valid=2;return nullptr;}key->valid=1;key->generation=0;}
    if(key->generation!=generation) {
        auto it=active->matches.find(hex(key->digest));key->entry=it==active->matches.end()?UINT32_MAX:it->second;key->generation=generation;
        if(key->entry==UINT32_MAX)stats.misses++;
    }
    if(key->entry>=active->entries.size())return nullptr;
    const auto &entry=active->entries[key->entry];auto &r=active->resources[entry.resource];
    if(t->width!=r.width || t->height!=r.height || (t->dimensionality==3)!=(r.kind=="volume") || !!t->cube!=(r.kind=="cube"))return nullptr;
    r.used=serial;
    if(r.status<0)return nullptr;
    if(!r.gpu) {
        if(!async_reads()) {if(creates>=create_limit){frame_waiting++;return nullptr;}creates++;if(!load_gpu(device,r))return nullptr;}
        else {
            if(!r.pending){queue_read(r);frame_waiting++;return nullptr;}   /* the original this frame */
            int state=r.pending->state.load(std::memory_order_acquire);
            if(!state){frame_waiting++;return nullptr;}
            if(state>0 && creates>=create_limit){frame_waiting++;return nullptr;}   /* Next frame */
            if(state>0)creates++;
            auto job=std::move(r.pending);
            if(state<0) {
                r.status=-1;stats.failures++;
                if(tracing()||stats.failures<=16)fprintf(stderr,"[TEXTURE-PACK] original fallback: %s (%s)\n",r.name.c_str(),job->error.c_str());
                return nullptr;
            }
            if(!create_gpu(device,r,job->bytes))return nullptr;
        }
    }
    auto &view=r.views[entry.slice];
    if(!view) {
        CpuPhase create(NV2A_CPU_PACK_SRV);
        D3D11_SHADER_RESOURCE_VIEW_DESC d={};
        if(r.kind=="volume") {
            ComPtr<ID3D11Texture3D> texture;r.gpu.As(&texture);D3D11_TEXTURE3D_DESC desc;texture->GetDesc(&desc);d.Format=desc.Format;
            d.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE3D;d.Texture3D.MipLevels=r.mips;
        } else {
            ComPtr<ID3D11Texture2D> texture;r.gpu.As(&texture);D3D11_TEXTURE2D_DESC desc;texture->GetDesc(&desc);d.Format=desc.Format;
            d.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2DARRAY;d.Texture2DArray.MipLevels=r.mips;
            d.Texture2DArray.FirstArraySlice=entry.slice;d.Texture2DArray.ArraySize=r.kind=="cube"?6:1;
        }
        if(FAILED(device->CreateShaderResourceView(r.gpu.Get(),&d,&view))) {r.status=-1;stats.failures++;return nullptr;}
    }
    *levels=r.mips;*scale=r.scale;stats.hits++;return view.Get();
    } catch(...) {
        // Keep optional pack allocation failures inside the C++ boundary.
        key->valid=2;stats.failures++;
        fprintf(stderr,"[TEXTURE-PACK] replacement allocation failed; using original texture\n");
        return nullptr;
    }
}
extern "C" Nv2aTexturePackStats nv2a_texture_pack_stats(void){return stats;}
