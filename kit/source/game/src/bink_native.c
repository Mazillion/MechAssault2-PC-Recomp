#include "bink_native.h"
#include "platform/pc_frontend.h"
#include "kernel/kernel.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
/* Log privacy (kernel_thunks.c): a path for a log line, with the player's profile folders written as
 * %LOCALAPPDATA% / %APPDATA% / %USERPROFILE% (players post logs publicly). */
const char *xbox_log_path(const char *path, char *out, size_t size);
const char *xbox_log_pathw(const wchar_t *path, char *out, size_t size);
void xbox_log_redact(char *text, size_t size);
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/mem.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
#define NATIVE_MOVIES 8
extern ptrdiff_t g_xbox_mem_offset;
#define U32(a) (*(uint32_t *)(g_xbox_mem_offset+(uint32_t)(a)))
#define PTR(a) ((uint8_t *)(g_xbox_mem_offset+(uint32_t)(a)))
typedef struct {
    uint32_t movie,width,height,total,last,flags;
    uint64_t used;
    int fullscreen;
    AVCodecContext *codec;
    AVFrame *frame;
    AVPacket *packet;
    struct SwsContext *converter;
    AVFormatContext *enhanced_input;
    AVCodecContext *enhanced_codec;
    AVFrame *enhanced_frame;
    AVPacket *enhanced_packet;
    int enhanced_stream,enhanced_attempted,enhanced_eof;
    uint32_t enhanced_last;
    char enhanced_path[1024];
} NativeMovie;
typedef struct { uint32_t movie; uint64_t used; char path[1024]; } MovieSource;
static NativeMovie movies[NATIVE_MOVIES];
static MovieSource sources[NATIVE_MOVIES];
static uint64_t serial;
/* Accept only the producer's exact final-file mapping. No rate/language
 * substitution, directory scanning, or .part.mkv selection is permitted. */
static int enhanced_name(const char *name,int quality,char *out,size_t capacity)
{
    char normalized[256];size_t n=strlen(name);
    if(n>=sizeof(normalized))return 0;
    for(size_t i=0;i<=n;i++)normalized[i]=name[i]=='/'?'\\':name[i];
    const char *p=normalized;
    if(!_strnicmp(p,"D:\\",3))p+=3;
    else if(!_strnicmp(p,"\\Device\\CdRom0\\",15))p+=15;
    else if(*p=='\\')p++;
    if(_strnicmp(p,"Movie\\",6))return 0;
    p+=6;
    if((strncmp(p,"50\\",3)&&strncmp(p,"60\\",3)))return 0;
    const char *clip=p+3;int startup=!_stricmp(clip,"MAIntro.bik");
    int accepted=startup||!_stricmp(clip,"Attract.bik");
    static const char *languages[]={"English","French","German","Italian","Japanese","Spanish"};
    for(unsigned i=0;i<sizeof(languages)/sizeof(languages[0]);i++) {
        size_t length=strlen(languages[i]);
        if(!_strnicmp(clip,languages[i],length)&&clip[length]=='\\') {
            const char *leaf=clip+length+1;
            accepted=!_stricmp(leaf,"SPIntro.bik")||!_stricmp(leaf,"ConquestIntro.bik");break;
        }
    }
    if(!accepted||(!startup&&!quality))return 0;
    int length=snprintf(out,capacity,"D:\\Movie\\Enhanced\\2x\\%s",p);
    if(length<4||(size_t)length>=capacity)return 0;
    memcpy(out+length-4,".mkv",5);return 1;
}
static void enhanced_close(NativeMovie *m)
{
    av_packet_free(&m->enhanced_packet);av_frame_free(&m->enhanced_frame);
    avcodec_free_context(&m->enhanced_codec);avformat_close_input(&m->enhanced_input);
    m->enhanced_last=0;m->enhanced_eof=0;
}
static void release(NativeMovie *m)
{
    if(m->fullscreen)pc_display_movie(-1);
    enhanced_close(m);
    sws_freeContext(m->converter);av_frame_free(&m->frame);
    av_packet_free(&m->packet);avcodec_free_context(&m->codec);memset(m,0,sizeof(*m));
}
static int enhanced_handle_path(uint32_t token,int quality,char *out,size_t capacity)
{
#ifdef _WIN32
    WCHAR opened[1024],logical[1024],root[1024];
    if(!xbox_bridge_file_path(token,opened,1024)) {
        fprintf(stderr,"[FMV] source handle=%08X path query failed; original video\n",token);return 0;
    }
    {char shown[2048];fprintf(stderr,"[FMV] source handle=%08X path=%s\n",token,xbox_log_pathw(opened,shown,sizeof shown));}
    if(!xbox_translate_path("D:\\Movie\\",logical,1024))return 0;
    HANDLE directory=CreateFileW(logical,FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
        NULL,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS,NULL);
    if(directory==INVALID_HANDLE_VALUE)return 0;
    DWORD length=GetFinalPathNameByHandleW(directory,root,1024,FILE_NAME_NORMALIZED|VOLUME_NAME_DOS);
    CloseHandle(directory);
    if(!length||length>=1024)return 0;
    while(length&&root[length-1]==L'\\')root[--length]=0;
    if(_wcsnicmp(opened,root,length)||opened[length]!=L'\\') {
        fprintf(stderr,"[FMV] opened file is outside configured Movie directory; original video\n");return 0;
    }
    /* A canonical root comparison keeps junctions/Unicode portable and avoids
     * confusing a cache/archive handle with a loose original movie. */
    char relative[256],guest[272],candidate[288];
    if(!WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,opened+length+1,-1,relative,sizeof(relative),NULL,NULL))return 0;
    if(snprintf(guest,sizeof(guest),"Movie\\%s",relative)<0)return 0;
    if(!enhanced_name(guest,quality,candidate,sizeof(candidate))) {
        if(getenv("RECOMP_FMV_TRACE"))fprintf(stderr,"[FMV] original selection or unmatched movie: %s\n",guest);
        return 0;
    }
    /* Preserve the exact canonical Movie directory; candidate's D:\Movie\
     * prefix is a validated logical name, not a second root lookup. */
    WCHAR tail[288],replacement[1400];
    if(!MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,candidate+9,-1,tail,288))return 0;
    if(swprintf_s(replacement,1400,L"%s\\%s",root,tail)<0)return 0;
    return WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,replacement,-1,out,(int)capacity,NULL,NULL)!=0;
#else
    (void)token;(void)quality;(void)out;(void)capacity;return 0;
#endif
}
void ma2_bink_release(uint32_t movie)
{
    for(unsigned i=0;i<NATIVE_MOVIES;i++) {
        if(movies[i].movie==movie)release(&movies[i]);
        if(sources[i].movie==movie)memset(&sources[i],0,sizeof(sources[i]));
    }
}
void ma2_bink_opened(uint32_t movie,uint32_t name,uint32_t flags)
{
    ma2_bink_release(movie);
    /* BINKFROMMEMORY supplies compressed bytes instead of a filename.
     * Custom I/O remains eligible only with an exact whitelisted path. */
    if(!movie||(flags&0x04000000u))return;
    char selected[1024];int quality=pc_setting_get(PC_FMV_QUALITY,0);
    if(flags&0x00800000u) {
        if(!enhanced_handle_path(name,quality,selected,sizeof(selected)))return;
    } else {
    if(name<0x10000||name>=0x04000000)return;
    size_t limit=0x04000000u-name;if(limit>255)limit=255;
    const char *text=(const char *)PTR(name);
    if(!memchr(text,0,limit))return;
    char candidate[256];
    if(!enhanced_name(text,quality,candidate,sizeof(candidate)))return;
    xbox_host_char host[1024];
    if(!xbox_translate_path(candidate,host,1024))return;
#ifdef _WIN32
    if(!WideCharToMultiByte(CP_UTF8,0,host,-1,selected,sizeof(selected),NULL,NULL))return;
#else
    if(strlen(host)>=sizeof(selected))return;
    strcpy(selected,host);
#endif
    }
    MovieSource *source=&sources[0];
    for(unsigned i=0;i<NATIVE_MOVIES;i++)if(!sources[i].movie||sources[i].used<source->used)source=&sources[i];
    memset(source,0,sizeof(*source));strcpy(source->path,selected);
    source->movie=movie;source->used=++serial;
}
static int enhanced_open(NativeMovie *m)
{
    const AVInputFormat *format=av_find_input_format("matroska,webm");
    if(!format||avformat_open_input(&m->enhanced_input,m->enhanced_path,format,NULL)<0)return 0;
    if(avformat_find_stream_info(m->enhanced_input,NULL)<0)return 0;
    const AVCodec *decoder=NULL;
    int stream=av_find_best_stream(m->enhanced_input,AVMEDIA_TYPE_VIDEO,-1,-1,&decoder,0);
    if(stream<0||!decoder)return 0;
    AVStream *video=m->enhanced_input->streams[stream];
    AVCodecParameters *parameters=video->codecpar;
    if(parameters->codec_id!=AV_CODEC_ID_H264||parameters->width!=(int)m->width*2||parameters->height!=(int)m->height*2)return 0;
    /* Original BinkOpen copies FrameRate/Div to +0x14/+0x18;
     * +0x10 is LastFrameNum and must not participate in eligibility. */
    uint32_t rate=U32(m->movie+0x14),divisor=U32(m->movie+0x18);
    AVRational fps=av_guess_frame_rate(m->enhanced_input,video,NULL);
    if(!rate||!divisor||!fps.num||!fps.den||(int64_t)fps.num*divisor!=(int64_t)fps.den*rate) {
        fprintf(stderr,"[FMV] frame rate rejected: guest=%u/%u replacement=%d/%d\n",rate,divisor,fps.num,fps.den);return 0;
    }
    if(video->nb_frames&&video->nb_frames!=m->total)return 0;
    m->enhanced_codec=avcodec_alloc_context3(decoder);
    m->enhanced_frame=av_frame_alloc();m->enhanced_packet=av_packet_alloc();
    if(!m->enhanced_codec||!m->enhanced_frame||!m->enhanced_packet)return 0;
    if(avcodec_parameters_to_context(m->enhanced_codec,parameters)<0)return 0;
    m->enhanced_codec->thread_count=1;
    if(avcodec_open2(m->enhanced_codec,decoder,NULL)<0)return 0;
    m->enhanced_stream=stream;return 1;
}
static int enhanced_next(NativeMovie *m)
{
    for(;;) {
        int error=avcodec_receive_frame(m->enhanced_codec,m->enhanced_frame);
        if(error>=0) {
            if(m->enhanced_frame->width!=(int)m->width*2||m->enhanced_frame->height!=(int)m->height*2)return 0;
            ++m->enhanced_last;return 1;
        }
        if(error!=AVERROR(EAGAIN)||m->enhanced_eof)return 0;
        do {
            av_packet_unref(m->enhanced_packet);
            error=av_read_frame(m->enhanced_input,m->enhanced_packet);
        } while(error>=0&&m->enhanced_packet->stream_index!=m->enhanced_stream);
        if(error<0) {
            if(error!=AVERROR_EOF)return 0;
            m->enhanced_eof=1;error=avcodec_send_packet(m->enhanced_codec,NULL);
        } else error=avcodec_send_packet(m->enhanced_codec,m->enhanced_packet);
        if(error<0)return 0;
    }
}
static void enhanced_decode(NativeMovie *m,uint32_t frame)
{
    if(!m->enhanced_attempted) {
        m->enhanced_attempted=1;
        for(unsigned i=0;i<NATIVE_MOVIES;i++)if(sources[i].movie==m->movie)strcpy(m->enhanced_path,sources[i].path);
        if(!*m->enhanced_path)return;
        if(!enhanced_open(m))goto fallback;
        {char shown[2048];fprintf(stderr,"[FMV] enhanced video, original audio/timing: %s\n",xbox_log_path(m->enhanced_path,shown,sizeof shown));}
    }
    if(!m->enhanced_codec)return;
    if(frame<m->enhanced_last) {
        enhanced_close(m);if(!enhanced_open(m))goto fallback;
    }
    while(m->enhanced_last<frame)if(!enhanced_next(m))goto fallback;
    return;
fallback:
    {char shown[2048];fprintf(stderr,"[FMV] original video fallback at frame %u: %s\n",frame,xbox_log_path(m->enhanced_path,shown,sizeof shown));}
    enhanced_close(m);
}
static NativeMovie *get(uint32_t movie)
{
    NativeMovie *m=NULL,*oldest=&movies[0];
    for(unsigned i=0;i<NATIVE_MOVIES;i++) {
        if(movies[i].movie==movie){m=&movies[i];break;}
        if(!movies[i].movie || movies[i].used<oldest->used)oldest=&movies[i];
    }
    uint32_t width=U32(movie),height=U32(movie+4),total=U32(movie+8);
    if(!m)m=oldest;
    if(m->movie && (m->movie!=movie || m->width!=width || m->height!=height || m->total!=total))release(m);
    if(!m->codec) {
        const AVCodec *codec=avcodec_find_decoder(AV_CODEC_ID_BINKVIDEO);
        if(!codec || !width || !height || width>4096 || height>4096)return NULL;
        m->codec=avcodec_alloc_context3(codec);m->frame=av_frame_alloc();m->packet=av_packet_alloc();
        if(!m->codec || !m->frame || !m->packet){release(m);return NULL;}
        m->movie=movie;m->width=width;m->height=height;m->total=total;
        m->flags=U32(movie+0x24);
        m->codec->width=(int)width;m->codec->height=(int)height;
        /* The game's loose and archived movie headers use BIKi. The public
         * BinkType field carries the on-disk alpha/grayscale flags. */
        m->codec->codec_tag=0x694B4942u;m->codec->thread_count=1;
        m->codec->extradata=av_mallocz(4+AV_INPUT_BUFFER_PADDING_SIZE);
        if(!m->codec->extradata){release(m);return NULL;}
        memcpy(m->codec->extradata,&m->flags,4);m->codec->extradata_size=4;
        int error=avcodec_open2(m->codec,codec,NULL);
        if(error<0){fprintf(stderr,"[BINK-NATIVE] codec initialization failed: %d\n",error);release(m);return NULL;}
        fprintf(stderr,"[BINK-NATIVE] movie=%08X size=%ux%u frames=%u type=%08X\n",movie,width,height,total,m->flags);
    }
    m->used=++serial;return m;
}
int ma2_bink_decode(uint32_t movie,uint32_t video)
{
    static int enabled=-1;
    if(enabled<0){const char *s=getenv("RECOMP_BINK_NATIVE");enabled=!s || atoi(s)!=0;}
    if(!enabled)return 0;
    NativeMovie *m=get(movie);if(!m)return 0;
    uint32_t frame=U32(movie+0xC),base=U32(movie+0x104),offsets=U32(movie+0x10C);
    if(!offsets || !frame || frame>m->total)return 0;
    /* +0x278 is playback time, not the compressed packet's size. The retail
     * reader uses these same two index entries to bound each frame's I/O. */
    uint32_t start=U32(offsets+(frame-1)*4)&~1u,end=U32(offsets+frame*4)&~1u;
    if(end<=start)return 0;
    uint32_t length=end-start;
    if(m->last==frame)return 1;
    if(frame==1 && m->last>1){release(m);m=get(movie);if(!m)return 0;}
    if(video<base || video-base>=length || length>16*1024*1024)return 0;
    unsigned bytes=length-(video-base);
    av_packet_unref(m->packet);
    if(av_new_packet(m->packet,(int)bytes)<0)return 0;
    memcpy(m->packet->data,PTR(video),bytes);
    if(U32(offsets+(frame-1)*4)&1)m->packet->flags|=AV_PKT_FLAG_KEY;
    int error=avcodec_send_packet(m->codec,m->packet);
    if(error>=0)error=avcodec_receive_frame(m->codec,m->frame);
    if(error<0){fprintf(stderr,"[BINK-NATIVE] movie=%08X frame=%u decode failed: %d, bytes=%u\n",movie,frame,error,bytes);return 0;}
    m->last=frame;
    /* Keep the original inter-frame decoder current. Any replacement error
     * can use this exact original frame without reopening or losing audio. */
    enhanced_decode(m,frame);
    return 1;
}
int ma2_bink_copy(uint32_t movie,uint32_t destination,int pitch,int height,int x,int y,uint32_t flags)
{
    NativeMovie *m=NULL;
    for(unsigned i=0;i<NATIVE_MOVIES;i++)if(movies[i].movie==movie){m=&movies[i];break;}
    if(!m || !m->last || m->last!=U32(movie+0xC) || !destination || pitch<=0 || x<0 || y<0 || height<(int)m->height+y)return 0;
    if(!m->fullscreen && m->width>=512 && m->height>=360){m->fullscreen=1;pc_display_movie(1);}
    enum AVPixelFormat format;int pixel_bytes;
    switch(flags&15) {
    case 1:format=AV_PIX_FMT_RGB24;pixel_bytes=3;break;
    case 2:format=AV_PIX_FMT_BGR24;pixel_bytes=3;break;
    case 3:case 5:format=AV_PIX_FMT_BGRA;pixel_bytes=4;break;
    case 4:case 6:format=AV_PIX_FMT_RGBA;pixel_bytes=4;break;
    case 8:case 9:case 11:format=AV_PIX_FMT_RGB555LE;pixel_bytes=2;break;
    case 10:format=AV_PIX_FMT_RGB565LE;pixel_bytes=2;break;
    case 13:format=AV_PIX_FMT_YUYV422;pixel_bytes=2;break;
    case 14:format=AV_PIX_FMT_UYVY422;pixel_bytes=2;break;
    default:return 0;
    }
    if(pitch<((int)m->width+x)*pixel_bytes)return 0;
    AVFrame *source=m->enhanced_codec&&m->enhanced_last==m->last?m->enhanced_frame:m->frame;
copy_frame:
    /* The retail destination still has guest resolution. Higher-resolution
     * movie render textures require a separate renderer integration. */
    m->converter=sws_getCachedContext(m->converter,source->width,source->height,(enum AVPixelFormat)source->format,
        m->width,m->height,format,SWS_BILINEAR,NULL,NULL,NULL);
    if(!m->converter)goto copy_failed;
    int range=source->color_range==AVCOL_RANGE_JPEG;
    const int *matrix=sws_getCoefficients(source==m->frame?SWS_CS_ITU601:source->colorspace==AVCOL_SPC_BT709?SWS_CS_ITU709:SWS_CS_ITU601);
    sws_setColorspaceDetails(m->converter,matrix,range,matrix,1,0,1<<16,1<<16);
    uint8_t *out[4]={PTR(destination)+(size_t)y*pitch+x*pixel_bytes,NULL,NULL,NULL};
    int strides[4]={pitch,0,0,0};
    int rows=sws_scale(m->converter,(const uint8_t *const *)source->data,source->linesize,0,source->height,out,strides);
    if(rows!=(int)m->height)goto copy_failed;
    return rows==(int)m->height;
copy_failed:
    if(source!=m->frame){enhanced_close(m);source=m->frame;goto copy_frame;}
    return 0;
}
