#include <libavutil/imgutils.h>
#include <libavutil/samplefmt.h>
#include <libavutil/timestamp.h>
#include <libavformat/avformat.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xos.h>
#include <vdpau/vdpau_x11.h>
#include <vdpau/vdpau.h>
#include <libavcodec/vdpau.h>
#include <signal.h>
#include <libavfilter/avfiltergraph.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/avutil.h>
#include <pthread.h>
#include <stdlib.h>

//是否使用多线程绘制鼠标
#define MULTI_THREAD 1
//是否垂直翻转视频图像
#define VFLIP 0

//yuv 输出文件
FILE* fpOutput = NULL;
//鼠标位图文件
FILE* fpBitmap = NULL;

//使用命令行参数控制是否翻转，需要VFLIP 宏打开
int vflip = 0;
pthread_mutex_t queue_lock = PTHREAD_MUTEX_INITIALIZER;

VdpGetProcAddress* vdp_get_proc_address;
VdpVideoSurfaceGetParameters* vdp_video_surface_get_parameters;
VdpVideoSurfaceGetBitsYCbCr* vdp_video_surface_get_bits_y_cb_cr;
VdpVideoSurfaceCreate* vdp_video_surface_create;
VdpVideoSurfaceDestroy* vdp_video_surface_destroy;
VdpVideoSurfacePutBitsYCbCr* vdp_video_surface_put_bits_y_cb_cr;

VdpDeviceDestroy* vdp_device_destroy;
VdpDecoderCreate* vdp_decoder_create;
VdpDecoderDestroy* vdp_decoder_destroy;
VdpDecoderRender* vdp_decoder_render;

VdpOutputSurfaceCreate* vdp_output_surface_create;
VdpOutputSurfaceDestroy* vdp_output_surface_destroy;
VdpOutputSurfaceGetBitsNative* vdp_output_surface_get_bits_native;
VdpOutputSurfacePutBitsNative* vdp_output_surface_put_bits_native;

VdpOutputSurfaceGetParameters* vdp_output_surface_get_parameters;
VdpOutputSurfaceRenderOutputSurface* vdp_output_surface_render_output_surface;
VdpOutputSurfaceRenderBitmapSurface* vdp_output_surface_render_bitmap_sruface;

VdpBitmapSurfaceCreate* vdp_bitmap_surface_create;
VdpBitmapSurfaceDestroy* vdp_bitmap_surface_destroy;
VdpBitmapSurfaceQueryCapabilities* vdp_bitmap_surface_query_capabilities;
VdpBitmapSurfacePutBitsNative* vdp_bitmap_surface_put_bits_native;

VdpVideoMixerCreate* vdp_video_mixer_create;
VdpVideoMixerDestroy* vdp_video_mixer_destroy;
VdpVideoMixerRender* vdp_video_mixer_render;
VdpVideoMixerSetAttributeValues* vdp_video_mixer_set_attribute_values;
VdpVideoMixerQueryFeatureSupport* vdp_video_mixer_query_feature_support;

VdpPresentationQueueCreate* vdp_presentation_queue_create;
VdpPresentationQueueDestroy* vdp_presentation_queue_destroy;
VdpPresentationQueueGetTime* vdp_presentation_queue_get_time;
VdpPresentationQueueTargetCreateX11* vdp_presentation_queue_target_create_x11;
VdpPresentationQueueQuerySurfaceStatus* vdp_presentation_queue_query_surface_status;
VdpPresentationQueueDisplay* vdp_presentation_queue_display;
VdpPresentationQueueBlockUntilSurfaceIdle*
        vdp_presentation_queue_block_until_surface_idle;
VdpPresentationQueueTargetDestroy* vdp_presentation_queue_target_destroy;

//video_surface 缓冲区最大数
#define MAX_VIDEO_SURFACES 20

typedef struct vdpau_render_state vdpau_render_state;
typedef enum AVPixelFormat        AVPixelFormat;

typedef struct _video_surface_entry{
    VdpVideoSurface               surface;
    int                           in_use;
    int                           is_alloc;
}video_surface_entry;

struct _vdpau_decoder_context{
    
/*---------------------X-variable-----------------------------*/
    Display*                            dis;
    int                                 screen;
    Window                              win;
    GC                                  gc;

/*---------------------VDPAU-variable-------------------------*/
    video_surface_entry                 surface_entrys[MAX_VIDEO_SURFACES];
    VdpOutputSurface                    output_surface[2];
    int                                 cur_index;
    VdpOutputSurface                    tmp_output_surface;
    VdpOutputSurface                    dest_surface[2];
    VdpOutputSurface                    black_pixel;
    VdpVideoSurface                     video_surface;
    VdpBitmapSurface                    bitmap_surface;
    uint8_t*                            bitmap_buffer;
    int                                 bmp_width;
    int                                 bmp_height;
    int                                 x_position;
    int                                 y_position;
    volatile int                        last_x_pos;
    volatile int                        last_y_pos;
    VdpVideoSurface                     tmp_video_surface;
    VdpVideoMixer                       video_mixer;
    int                                 vid_width, vid_height;
    VdpDevice                           vdp_device;
    VdpPresentationQueueTarget          vdp_target;
    VdpPresentationQueue                vdp_queue;
    VdpDecoder                          vdp_decoder;
    vdpau_render_state*                 pRender_state;

/*----------------------FFMPEG-variable-----------------------*/
    AVCodec*                            pCodec_vdp;
    AVCodecContext*                     pCodec_ctx;
    AVFrame*                            pFrame;
    AVPacket                            avpkt;

/*----------------------LIBAVFILTER-variable-----------------*/
    AVFrame*                            frame_in;
    AVFrame*                            frame_out;
    unsigned char*                      frame_buffer_in;
    unsigned char*                      frame_buffer_out;
    AVFilterContext*                    buffersink_ctx;
    AVFilterContext*                    buffersrc_ctx;
    AVFilterGraph*                      filter_graph;
    char*                               filter_descr;
    uint8_t*                            data1;
    uint8_t*                            data2;
    uint8_t*                            data3;
    uint8_t*                            rdata1;
    uint8_t*                            rdata2;
    uint8_t*                            rdata3;
    
};

typedef struct _vdpau_decoder_context vdpau_decoder_context;

static vdpau_decoder_context vdpDecRdCtx;


void vdp_safe_get_proc_address(VdpFuncId functionId, void** functionPointer)
{
    VdpStatus status;
    status = vdp_get_proc_address(vdpDecRdCtx.vdp_device, functionId, functionPointer);
    if(status != VDP_STATUS_OK){
        return;
    }
}

//过去vdpau 相关设备
VdpDevice vdp_get_device()
{
    static int bInitFailed = 0;
    
    if(vdpDecRdCtx.vdp_device){
        return vdpDecRdCtx.vdp_device;
    }

    if(bInitFailed){
        return 0;
    }

    vdpDecRdCtx.dis = XOpenDisplay(0);
    if(!vdpDecRdCtx.dis){
        return 0;
    }
    vdpDecRdCtx.screen = DefaultScreen(vdpDecRdCtx.dis);

    VdpStatus status;
    status = vdp_device_create_x11(vdpDecRdCtx.dis, vdpDecRdCtx.screen, &vdpDecRdCtx.vdp_device, &vdp_get_proc_address);
    if(status != VDP_STATUS_OK){
        bInitFailed = 1;
        return 0;
    }

    vdp_safe_get_proc_address(VDP_FUNC_ID_DEVICE_DESTROY, (void**)&vdp_device_destroy);
    vdp_safe_get_proc_address(VDP_FUNC_ID_OUTPUT_SURFACE_CREATE,
            (void**)&vdp_output_surface_create);
    vdp_safe_get_proc_address(VDP_FUNC_ID_OUTPUT_SURFACE_DESTROY,
            (void**)&vdp_output_surface_destroy);
    vdp_safe_get_proc_address(VDP_FUNC_ID_OUTPUT_SURFACE_GET_BITS_NATIVE,
            (void**)&vdp_output_surface_get_bits_native);
    vdp_safe_get_proc_address(VDP_FUNC_ID_OUTPUT_SURFACE_RENDER_OUTPUT_SURFACE,
            (void**)&vdp_output_surface_render_output_surface);
    vdp_safe_get_proc_address(VDP_FUNC_ID_VIDEO_SURFACE_CREATE, 
            (void**)&vdp_video_surface_create);
    vdp_safe_get_proc_address(VDP_FUNC_ID_VIDEO_SURFACE_DESTROY,
            (void**)&vdp_video_surface_destroy);
    vdp_safe_get_proc_address(VDP_FUNC_ID_DECODER_CREATE, (void**)&vdp_decoder_create);
    vdp_safe_get_proc_address(VDP_FUNC_ID_DECODER_DESTROY, (void**)&vdp_decoder_destroy);
    vdp_safe_get_proc_address(VDP_FUNC_ID_DECODER_RENDER, (void**)&vdp_decoder_render);
    vdp_safe_get_proc_address(VDP_FUNC_ID_VIDEO_SURFACE_GET_BITS_Y_CB_CR,
            (void**)&vdp_video_surface_get_bits_y_cb_cr);
    vdp_safe_get_proc_address(VDP_FUNC_ID_VIDEO_MIXER_CREATE,
            (void**)&vdp_video_mixer_create);
    vdp_safe_get_proc_address(VDP_FUNC_ID_VIDEO_MIXER_DESTROY,
            (void**)&vdp_video_mixer_destroy);
    vdp_safe_get_proc_address(VDP_FUNC_ID_VIDEO_MIXER_RENDER,
            (void**)&vdp_video_mixer_render);
    vdp_safe_get_proc_address(VDP_FUNC_ID_PRESENTATION_QUEUE_CREATE, 
            (void**)&vdp_presentation_queue_create);
    vdp_safe_get_proc_address(VDP_FUNC_ID_PRESENTATION_QUEUE_DESTROY, 
            (void**)&vdp_presentation_queue_destroy);
    vdp_safe_get_proc_address(VDP_FUNC_ID_PRESENTATION_QUEUE_TARGET_CREATE_X11,
            (void**)&vdp_presentation_queue_target_create_x11);
    vdp_safe_get_proc_address(VDP_FUNC_ID_PRESENTATION_QUEUE_QUERY_SURFACE_STATUS, 
            (void**)&vdp_presentation_queue_query_surface_status);
    vdp_safe_get_proc_address(VDP_FUNC_ID_PRESENTATION_QUEUE_DISPLAY,
            (void**)&vdp_presentation_queue_display);
    vdp_safe_get_proc_address(VDP_FUNC_ID_PRESENTATION_QUEUE_GET_TIME,
            (void**)&vdp_presentation_queue_get_time);
    vdp_safe_get_proc_address(VDP_FUNC_ID_PRESENTATION_QUEUE_BLOCK_UNTIL_SURFACE_IDLE, 
            (void**)&vdp_presentation_queue_block_until_surface_idle);
    vdp_safe_get_proc_address(VDP_FUNC_ID_VIDEO_SURFACE_GET_PARAMETERS,
            (void**)&vdp_video_surface_get_parameters);
    vdp_safe_get_proc_address(VDP_FUNC_ID_OUTPUT_SURFACE_GET_PARAMETERS,
            (void**)&vdp_output_surface_get_parameters);
    vdp_safe_get_proc_address(VDP_FUNC_ID_VIDEO_MIXER_SET_ATTRIBUTE_VALUES, 
            (void**)&vdp_video_mixer_set_attribute_values);
    vdp_safe_get_proc_address(VDP_FUNC_ID_BITMAP_SURFACE_CREATE, 
            (void**)&vdp_bitmap_surface_create);
    vdp_safe_get_proc_address(VDP_FUNC_ID_BITMAP_SURFACE_DESTROY,
            (void**)&vdp_bitmap_surface_destroy);
    vdp_safe_get_proc_address(VDP_FUNC_ID_BITMAP_SURFACE_QUERY_CAPABILITIES,
            (void**)&vdp_bitmap_surface_query_capabilities);
    vdp_safe_get_proc_address(VDP_FUNC_ID_BITMAP_SURFACE_PUT_BITS_NATIVE,
            (void**)&vdp_bitmap_surface_put_bits_native);
    vdp_safe_get_proc_address(VDP_FUNC_ID_OUTPUT_SURFACE_RENDER_BITMAP_SURFACE,
            (void**)&vdp_output_surface_render_bitmap_sruface);
    vdp_safe_get_proc_address(VDP_FUNC_ID_PRESENTATION_QUEUE_TARGET_DESTROY, 
            (void**)&vdp_presentation_queue_target_destroy);
    vdp_safe_get_proc_address(VDP_FUNC_ID_OUTPUT_SURFACE_PUT_BITS_NATIVE, 
            (void**)&vdp_output_surface_put_bits_native);
    vdp_safe_get_proc_address(VDP_FUNC_ID_VIDEO_MIXER_QUERY_FEATURE_SUPPORT,
            (void**)&vdp_video_mixer_query_feature_support);
    vdp_safe_get_proc_address(VDP_FUNC_ID_VIDEO_SURFACE_PUT_BITS_Y_CB_CR,
            (void**)&vdp_video_surface_put_bits_y_cb_cr);
    
    return vdpDecRdCtx.vdp_device;
}


//X window 初始化
int vdp_init_x(int x, int y)
{
     vdpDecRdCtx.vid_width = x;
     vdpDecRdCtx.vid_height = y;
     vdpDecRdCtx.vdp_device = vdp_get_device();
     XSetWindowAttributes attributes;

     attributes.background_pixel = XWhitePixel(vdpDecRdCtx.dis, 0);
     //attributes.override_redirect = True;
     int width  = DisplayWidth(vdpDecRdCtx.dis, vdpDecRdCtx.screen);   /* 获取屏幕宽度 */
     int height = DisplayHeight(vdpDecRdCtx.dis, vdpDecRdCtx.screen); /* 获取屏幕高度 */
     vdpDecRdCtx.win = XCreateWindow(vdpDecRdCtx.dis, XRootWindow(vdpDecRdCtx.dis, 0), 
         0, 0, vdpDecRdCtx.vid_width, vdpDecRdCtx.vid_height, 0, DefaultDepth(vdpDecRdCtx.dis, 0), 
         InputOutput, DefaultVisual(vdpDecRdCtx.dis, 0), 
         CWBackPixel,&attributes); 

     XSelectInput(vdpDecRdCtx.dis, vdpDecRdCtx.win, PointerMotionMask |ExposureMask | KeyPressMask ); 

     vdpDecRdCtx.gc = XCreateGC(vdpDecRdCtx.dis, vdpDecRdCtx.win, 0, NULL); 
     XSynchronize(vdpDecRdCtx.dis, 1);
     XMapWindow(vdpDecRdCtx.dis, vdpDecRdCtx.win); 
     
    /*
    * anthoer way to create window
    vdpDecRdCtx.vid_width = x;
    vdpDecRdCtx.vid_height = y;

    vdp_get_device();

    unsigned long black, white;
    black = BlackPixel(vdpDecRdCtx.dis, vdpDecRdCtx.screen);
    white = WhitePixel(vdpDecRdCtx.dis, vdpDecRdCtx.screen);
    vdpDecRdCtx.win = XCreateSimpleWindow(vdpDecRdCtx.dis, DefaultRootWindow(vdpDecRdCtx.dis), 0, 0, 
                                          vdpDecRdCtx.vid_width, vdpDecRdCtx.vid_height, 5, black, white);
    //XSetStandardProperties(vdpDecRdCtx.dis, vdpDecRdCtx.win, "VDPAU Player", "VDPAU", NULL, NULL, 0, NULL);
    XSelectInput(vdpDecRdCtx.dis, vdpDecRdCtx.win, ExposureMask|ButtonPressMask|KeyPressMask);
    vdpDecRdCtx.gc=XCreateGC(vdpDecRdCtx.dis, vdpDecRdCtx.win, 0,NULL);
    XSynchronize(vdpDecRdCtx.dis, 1);
    XSetBackground(vdpDecRdCtx.dis, vdpDecRdCtx.gc,white);
    XSetBackground(vdpDecRdCtx.dis, vdpDecRdCtx.gc,black);
    XClearWindow(vdpDecRdCtx.dis, vdpDecRdCtx.win);
    */
}

//关闭X window 的正确流程, 顺序很重要
int vdp_close_x()
{
    printf("vdp_close_x\n");
    vdp_presentation_queue_target_destroy(vdpDecRdCtx.vdp_target);
    vdp_presentation_queue_destroy(vdpDecRdCtx.vdp_queue);
    vdp_device_destroy(vdpDecRdCtx.vdp_device);
    vdp_video_mixer_destroy(vdpDecRdCtx.video_mixer);
    vdp_video_surface_destroy(vdpDecRdCtx.video_surface);
    for(int i=0; i< MAX_VIDEO_SURFACES; i++){
        if(vdpDecRdCtx.surface_entrys[i].is_alloc)
            vdp_video_surface_destroy(vdpDecRdCtx.surface_entrys[i].surface);
        else
            break;
    }
    XUnmapWindow(vdpDecRdCtx.dis, vdpDecRdCtx.win);
    XDestroyWindow(vdpDecRdCtx.dis, vdpDecRdCtx.win);
    XFreeGC(vdpDecRdCtx.dis, vdpDecRdCtx.gc);
    XCloseDisplay(vdpDecRdCtx.dis);
}

static enum AVPixelFormat get_format_hwdec(struct AVCodecContext *avctx,
                                           const enum AVPixelFormat *pix_fmt)
{
    static int ischange = 0;
    int ret = 0;

    if(!ischange){
        
        ret = av_vdpau_bind_context(vdpDecRdCtx.pCodec_ctx, vdp_get_device(),
                                     vdp_get_proc_address,
                                     AV_HWACCEL_FLAG_IGNORE_LEVEL |
                                     AV_HWACCEL_FLAG_ALLOW_HIGH_DEPTH);
        printf("av_vdpau_bind_context, ret: %d\n", ret);
        ischange++;
    }
    
   return AV_PIX_FMT_VDPAU;
}

int isAVCodecAvailable()
{
#if LIBAVCODEC_VERSION_INT > AV_VERSION_INT(53, 34, 0)
    return vdp_get_device() != 0;
#else
    return 0;
#endif
}

static void call_free(void *opaque, uint8_t *data)
{
    VdpVideoSurface surface = *(VdpVideoSurface*)data;
    AVCodecContext* avctx = opaque;
    printf("call_free, data: %p, opaque: %p\n", data, opaque);
    vdp_video_surface_destroy(surface);
    av_freep(&data);
}

static void surface_entry_release(void* opaque, uint8_t *data)
{
    int* index = (int*)opaque;
    vdpDecRdCtx.surface_entrys[*index].in_use = 0;
    free(index);
}

static int get_video_surface()
{
    VdpStatus status;
    int i;
    
    for(i = 0; i < MAX_VIDEO_SURFACES; i++){
        if(!vdpDecRdCtx.surface_entrys[i].is_alloc)
            break;
        if(!vdpDecRdCtx.surface_entrys[i].in_use){
            vdpDecRdCtx.surface_entrys[i].in_use = 1;
            return i;
        }
    }
    
    if(i == MAX_VIDEO_SURFACES)
        return -1;
    
    status = vdp_video_surface_create(vdp_get_device(), VDP_CHROMA_TYPE_420, vdpDecRdCtx.vid_width, 
                                                vdpDecRdCtx.vid_height, &vdpDecRdCtx.surface_entrys[i].surface);
    if(status != VDP_STATUS_OK){
        printf("vdp video surface create fail, status: %d\n", status);
        return -1;
    }

    vdpDecRdCtx.surface_entrys[i].is_alloc = 1;
    vdpDecRdCtx.surface_entrys[i].in_use = 1;
    
    return i;
}


static int get_buffer2_hwdec(AVCodecContext *avctx, AVFrame *pic, int flags)
{
    
#if 1
    int* index = (int*)malloc(sizeof(int));
    *index = 0;
    *index = get_video_surface();
    if(*index < 0){
        printf("get video surface fail\n");
        return -1;
    }
    pic->buf[0] = av_buffer_create(NULL, 0, surface_entry_release, index, 0);
    pic->data[0] = (void*)"dummy";
    pic->data[3] = (void*)(intptr_t)(vdpDecRdCtx.surface_entrys[*index].surface);
#endif
#if 0    
    int ret = 0;
    VdpVideoSurface *surface;
    surface = av_malloc(sizeof(*surface));
    pic->buf[0] = av_buffer_create((uint8_t*)surface, sizeof(*surface), call_free, avctx, 0/*AV_BUFFER_FLAG_READONLY*/);
    VdpStatus status = vdp_video_surface_create(vdp_get_device(), VDP_CHROMA_TYPE_420, vdpDecRdCtx.vid_width, 
                                                vdpDecRdCtx.vid_height, surface);

    pic->data[0] = (void*)"dummy";
    pic->data[3] = (void*)(intptr_t)(*surface);
#endif
    return 0;
}


int init_avctx(AVCodecContext* avctx, enum AVCodecID codec_id)
{
    avctx->codec_type = AVMEDIA_TYPE_VIDEO;
    avctx->codec_id = codec_id;
    avctx->refcounted_frames = 1;
    avctx->thread_count = 1;
    avctx->get_buffer2 = get_buffer2_hwdec;
    avctx->get_format = get_format_hwdec;
    avctx->coded_width = vdpDecRdCtx.vid_width;
    avctx->coded_height = vdpDecRdCtx.vid_height;
    avctx->pix_fmt = AV_PIX_FMT_YUV420P;
    avctx->has_b_frames = 0;
    avctx->width = vdpDecRdCtx.vid_width;
    avctx->height = vdpDecRdCtx.vid_height;
    avctx->chroma_sample_location = 1;
}

int init_ffmpeg_with_vdpau_decoder()
{
    avcodec_register_all();
    vdpDecRdCtx.pCodec_vdp = avcodec_find_decoder_by_name("h264");//"h264" 很关键，名字不能弄错
    if(!vdpDecRdCtx.pCodec_vdp){
        printf("find decoder h264_vdpau fail\n");
        return -1;
    }

    vdpDecRdCtx.pCodec_ctx = avcodec_alloc_context3(vdpDecRdCtx.pCodec_vdp);
    if(!vdpDecRdCtx.pCodec_ctx){
        printf("alloc context3 fail\n");
        return -1;
    }

    if(!isAVCodecAvailable()){
        printf("not available\n");
        return -1;
    }

    init_avctx(vdpDecRdCtx.pCodec_ctx, vdpDecRdCtx.pCodec_vdp->id);
    
    if(avcodec_open2(vdpDecRdCtx.pCodec_ctx, vdpDecRdCtx.pCodec_vdp, NULL) < 0){
        printf("open2 faild\n");
        return -1;
    }

    vdpDecRdCtx.pFrame = av_frame_alloc();
    if(!vdpDecRdCtx.pFrame){
        printf("frame alloc fail\n");
        return -1;
    }
    vdpDecRdCtx.pFrame->width = vdpDecRdCtx.vid_width;
    vdpDecRdCtx.pFrame->height = vdpDecRdCtx.vid_height;
    vdpDecRdCtx.pFrame->format = AV_PIX_FMT_VDPAU;
    
    av_init_packet(&vdpDecRdCtx.avpkt);

    return 0;
    
}

int init_vdpau_queue()
{
    VdpStatus status;

    status = vdp_presentation_queue_target_create_x11(vdpDecRdCtx.vdp_device, vdpDecRdCtx.win,
                                                      &vdpDecRdCtx.vdp_target);
    if(status != VDP_STATUS_OK) {
        return -1;
    }

    //XMapRaised(vdpDecRdCtx.dis, vdpDecRdCtx.win);

    status = vdp_presentation_queue_create(vdpDecRdCtx.vdp_device, vdpDecRdCtx.vdp_target,
                                           &vdpDecRdCtx.vdp_queue);
    if(status != VDP_STATUS_OK) {
        printf("vdp_presentation_queue_crate fail, vdp_st: %d\n", status);
        return -1;
    }

    return 0;
}

int init_vdpau_decoder_and_mixer()
{
    VdpStatus status;

    /*
    VdpDecoderProfile profile = VDP_DECODER_PROFILE_H264_CONSTRAINED_HIGH;
    status = vdp_decoder_create(vdp_get_device(), profile, vdpDecRdCtx.vid_width,vdpDecRdCtx.vid_height, 16,
            &vdpDecRdCtx.vdp_decoder);
    if(status != VDP_STATUS_OK){
        return -1;
    }
    */

    VdpBool ok = 0;
    status = vdp_video_mixer_query_feature_support(vdpDecRdCtx.vdp_device, VDP_VIDEO_MIXER_FEATURE_INVERSE_TELECINE, &ok);
    if(status == VDP_STATUS_OK && ok == VDP_TRUE){
        printf("support inverse telecine\n");
    }
    else{
        printf("not support inverse telecine\n");
    }

    VdpVideoMixerFeature features[] = {
        VDP_VIDEO_MIXER_FEATURE_DEINTERLACE_TEMPORAL,
        VDP_VIDEO_MIXER_FEATURE_DEINTERLACE_TEMPORAL_SPATIAL,
        //VDP_VIDEO_MIXER_FEATURE_NOISE_REDUCTION,
        //VDP_VIDEO_MIXER_FEATURE_INVERSE_TELECINE,
    };
    VdpVideoMixerParameter params[] = { 
        VDP_VIDEO_MIXER_PARAMETER_VIDEO_SURFACE_WIDTH,
        VDP_VIDEO_MIXER_PARAMETER_VIDEO_SURFACE_HEIGHT,
        VDP_VIDEO_MIXER_PARAMETER_CHROMA_TYPE,
        //VDP_VIDEO_MIXER_PARAMETER_LAYERS
    };
    VdpChromaType chroma = VDP_CHROMA_TYPE_420;
    //int  numLayers = 0;
    void const* paramValues [] = { &vdpDecRdCtx.vid_width, &vdpDecRdCtx.vid_height, &chroma/*, &numLayers*/ };

    status = vdp_video_mixer_create(vdp_get_device(), 2, features, 3, params, 
            paramValues, &vdpDecRdCtx.video_mixer);

    if(status != VDP_STATUS_OK){
        return -1;
    }

    
    static const VdpVideoMixerAttribute attributes[] = {VDP_VIDEO_MIXER_ATTRIBUTE_SKIP_CHROMA_DEINTERLACE};
    float noise_level = 1.0;
    float sharpness_level = -1.0;
    uint8_t skip_chroma_de = 0;
    const void *attribute_values[] = {&skip_chroma_de};

    
   // status = vdp_video_mixer_set_attribute_values(vdpDecRdCtx.video_mixer, 1, attributes, attribute_values);
                                                  
    return 0;
}

int init_video_surface()
{
    VdpStatus status;
    status = vdp_output_surface_create(vdpDecRdCtx.vdp_device, VDP_RGBA_FORMAT_B8G8R8A8,
                                               vdpDecRdCtx.vid_width, vdpDecRdCtx.vid_height,
                                               &vdpDecRdCtx.output_surface[0]);
    status = vdp_output_surface_create(vdpDecRdCtx.vdp_device, VDP_RGBA_FORMAT_B8G8R8A8,
                                               vdpDecRdCtx.vid_width, vdpDecRdCtx.vid_height,
                                               &vdpDecRdCtx.output_surface[1]);
    status = vdp_video_surface_create(vdp_get_device(), VDP_CHROMA_TYPE_420, vdpDecRdCtx.vid_width, 
                                                vdpDecRdCtx.vid_height, &vdpDecRdCtx.tmp_video_surface);
    

    for(int i = 0; i< MAX_VIDEO_SURFACES; i++){
        vdpDecRdCtx.surface_entrys[i].in_use = 0;
        vdpDecRdCtx.surface_entrys[i].is_alloc = 0;
    }

    vdpDecRdCtx.data1 = malloc(vdpDecRdCtx.vid_width*vdpDecRdCtx.vid_height);
    vdpDecRdCtx.data2 = malloc(vdpDecRdCtx.vid_width/2*vdpDecRdCtx.vid_height);
    vdpDecRdCtx.data3 = malloc(vdpDecRdCtx.vid_width/2*vdpDecRdCtx.vid_height);
    vdpDecRdCtx.cur_index = 0;
    return 0;
}

int init_bitmap_surface()
{
    VdpStatus status;
    
    fseek(fpBitmap, 0, SEEK_END);
    size_t bmp_size = ftell(fpBitmap);
    rewind(fpBitmap);
    vdpDecRdCtx.bitmap_buffer = (uint8_t*)malloc(bmp_size);
    if(!vdpDecRdCtx.bitmap_buffer){
        printf("bitmap_buffer malloc fail\n");
        return -1;
    }
    
    fread(vdpDecRdCtx.bitmap_buffer, sizeof(uint8_t), bmp_size, fpBitmap);
    
    status = vdp_bitmap_surface_create(vdpDecRdCtx.vdp_device, VDP_RGBA_FORMAT_B8G8R8A8,
                                       vdpDecRdCtx.bmp_width, vdpDecRdCtx.bmp_height,
                                       VDP_TRUE, &vdpDecRdCtx.bitmap_surface);
    if(status != VDP_STATUS_OK){
        printf("vdp_bitmap_surface_create fail, status: %d\n", status);
        return -1;
    }

    uint8_t* data[1] = {vdpDecRdCtx.bitmap_buffer};
    uint32_t stride = vdpDecRdCtx.bmp_width*4;
    status = vdp_bitmap_surface_put_bits_native(vdpDecRdCtx.bitmap_surface, (void* const*)data, &stride, NULL);
    if(status != VDP_STATUS_OK){
        printf("dp_bitmap_surface_put_bits_native faild, status: %d \n", status);
        return -1;
    }

    return 0;
}

//从vdpVideoSurface获取yuv12数据
int get_ycbcr_bits()
{
    uint8_t ** data;
    const uint32_t a[3] = {vdpDecRdCtx.vid_width,  vdpDecRdCtx.vid_width/2, vdpDecRdCtx.vid_width/2};
    VdpStatus vdp_st;
    data = (uint8_t**) calloc(3, sizeof(uint8_t*));
    data[0] = (uint8_t*)calloc(vdpDecRdCtx.vid_width*vdpDecRdCtx.vid_height, sizeof(uint8_t));
    data[1] = (uint8_t*)calloc(vdpDecRdCtx.vid_width*vdpDecRdCtx.vid_height/4, sizeof(uint8_t));
    data[2] = (uint8_t*)calloc(vdpDecRdCtx.vid_width*vdpDecRdCtx.vid_height/4, sizeof(uint8_t));

    vdp_st = vdp_video_surface_get_bits_y_cb_cr(vdpDecRdCtx.video_surface, VDP_YCBCR_FORMAT_YV12, (void* const*)data, a);
    if(vdp_st != VDP_STATUS_OK){
        printf("vdp_video_surface_get_bits_ycb_cr fail, vdp_st: %d\n", vdp_st);
        free(data[0]);
        free(data[1]);
        free(data[2]);
        free(data);
        return -1;
    }

    fwrite(data[0],sizeof(uint8_t), vdpDecRdCtx.vid_height*vdpDecRdCtx.vid_width, fpOutput);
    fwrite(data[2],sizeof(uint8_t), vdpDecRdCtx.vid_height*vdpDecRdCtx.vid_width/4, fpOutput);
    fwrite(data[1],sizeof(uint8_t), vdpDecRdCtx.vid_height*vdpDecRdCtx.vid_width/4, fpOutput);

    fflush(fpOutput);
    
    free(data[0]);
    free(data[1]);
    free(data[2]);
    free(data);

    return 0;
}

//在使用ffmpeg的libavfilter垂直翻转视频帧的情况下，从vdpVideoSurface中获取数据到AVFrame(frame_in)
int get_ycbcr_bits2()
{
    static int is_get_bits = 0;
    uint8_t * data[3] = {vdpDecRdCtx.frame_in->data[0], vdpDecRdCtx.frame_in->data[1], vdpDecRdCtx.frame_in->data[2]};
    const uint32_t a[3] = {vdpDecRdCtx.vid_width,  vdpDecRdCtx.vid_width/2, vdpDecRdCtx.vid_width/2};
    VdpStatus vdp_st;
        
    vdp_st = vdp_video_surface_get_bits_y_cb_cr(vdpDecRdCtx.video_surface, VDP_YCBCR_FORMAT_YV12, (void* const*)data, a);
    if(vdp_st != VDP_STATUS_OK){
        printf("vdp_video_surface_get_bits_ycb_cr fail, vdp_st: %d\n", vdp_st);
        return -1;
    }

    return 0;
}

//在使用ffmpeg的libavfilter垂直翻转视频帧的情况下，将AVFrame(frame_out)中的数据写入vdpVideoSurface，
//注意，libavfilter处理后的frame_out中的数据是按行倒着放的，并且data指针指向最后一行的开头位置，linesize为负值。
//所以为了使其符合vdpVideoSurface的存储方式，这里还是需要逐行拷贝。
int put_ycbcr_bits2()
{
    static int frame_cnt = 0;
    if(frame_cnt == 0){
        printf("put_ycbcr_bits, width:%d, height:%d\n", vdpDecRdCtx.frame_out->width, vdpDecRdCtx.frame_out->height);
    }
    frame_cnt++;
#if 1
    if(vdpDecRdCtx.frame_out->format==AV_PIX_FMT_YUV420P){
		for(int i=0;i<vdpDecRdCtx.frame_out->height;i++){
			//fwrite(vdpDecRdCtx.frame_out->data[0]+vdpDecRdCtx.frame_out->linesize[0]*i,1,vdpDecRdCtx.frame_out->width,fpOutput);
			memcpy(vdpDecRdCtx.data1+vdpDecRdCtx.vid_width*i, vdpDecRdCtx.frame_out->data[0]+vdpDecRdCtx.frame_out->linesize[0]*i, vdpDecRdCtx.frame_out->width);
		}
		for(int i=0;i<vdpDecRdCtx.frame_out->height/2;i++){
			//fwrite(vdpDecRdCtx.frame_out->data[1]+vdpDecRdCtx.frame_out->linesize[1]*i,1,vdpDecRdCtx.frame_out->width/2,fpOutput);
		    memcpy(vdpDecRdCtx.data2+vdpDecRdCtx.vid_width/2*i, vdpDecRdCtx.frame_out->data[1]+vdpDecRdCtx.frame_out->linesize[1]*i, vdpDecRdCtx.frame_out->width/2);
		}
		for(int i=0;i<vdpDecRdCtx.frame_out->height/2;i++){
			//fwrite(vdpDecRdCtx.frame_out->data[2]+vdpDecRdCtx.frame_out->linesize[2]*i,1,vdpDecRdCtx.frame_out->width/2,fpOutput);
			memcpy(vdpDecRdCtx.data3+vdpDecRdCtx.vid_width/2*i, vdpDecRdCtx.frame_out->data[2]+vdpDecRdCtx.frame_out->linesize[2]*i, vdpDecRdCtx.frame_out->width/2);			
		}
    }
		//printf("Process %d frame!\n", frame_cnt);
#endif		

    uint8_t *data[3] = {vdpDecRdCtx.data1, vdpDecRdCtx.data2, vdpDecRdCtx.data3};
    const uint32_t a[3] = {vdpDecRdCtx.vid_width, vdpDecRdCtx.vid_width/2, vdpDecRdCtx.vid_width/2};
    VdpStatus vdp_st;
#if 1
    vdp_st = vdp_video_surface_put_bits_y_cb_cr(vdpDecRdCtx.tmp_video_surface, VDP_YCBCR_FORMAT_YV12, (void const*const*)data, a);
 
    if (vdp_st != VDP_STATUS_OK) {
        printf("vdp_video_surface_put_bits_y_cb_cr fail, vdp_st : %d\n", vdp_st);
        return -1;
    }
#endif
    return 0;
}

//init libavfilter with vertical flip
static int init_avfilter_vflip()
{
    int ret = 0;
    vdpDecRdCtx.filter_descr = "vflip";
    avfilter_register_all();
    char args[512];
    AVFilter* buffersrc = avfilter_get_by_name("buffer");
    AVFilter* buffersink = avfilter_get_by_name("ffbuffersink");
    AVFilterInOut* outputs = avfilter_inout_alloc();
    AVFilterInOut* inputs = avfilter_inout_alloc();

    enum PixelFormat pix_fmts[] = {AV_PIX_FMT_YUV420P, AV_PIX_FMT_NONE};
    AVBufferSinkParams* buffersink_params;
    vdpDecRdCtx.filter_graph = avfilter_graph_alloc();
    snprintf(args, sizeof(args),
		"video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d",
		vdpDecRdCtx.vid_width,vdpDecRdCtx.vid_height,AV_PIX_FMT_YUV420P,
		1, 40,1,1);

    ret = avfilter_graph_create_filter(&vdpDecRdCtx.buffersrc_ctx, buffersrc, "in",
		args, NULL, vdpDecRdCtx.filter_graph);
	if (ret < 0) {
		printf("Cannot create buffer source\n");
		return ret;
	}
 
	/* buffer video sink: to terminate the filter chain. */
	buffersink_params = av_buffersink_params_alloc();
	buffersink_params->pixel_fmts = pix_fmts;
	ret = avfilter_graph_create_filter(&vdpDecRdCtx.buffersink_ctx, buffersink, "out",
		NULL, buffersink_params, vdpDecRdCtx.filter_graph);
	av_free(buffersink_params);
	if (ret < 0) {
		printf("Cannot create buffer sink\n");
		return ret;
	}
 
	/* Endpoints for the filter graph. */
	outputs->name       = av_strdup("in");
	outputs->filter_ctx = vdpDecRdCtx.buffersrc_ctx;
	outputs->pad_idx    = 0;
	outputs->next       = NULL;
 
	inputs->name       = av_strdup("out");
	inputs->filter_ctx = vdpDecRdCtx.buffersink_ctx;
	inputs->pad_idx    = 0;
	inputs->next       = NULL;
 
	if ((ret = avfilter_graph_parse_ptr(vdpDecRdCtx.filter_graph, vdpDecRdCtx.filter_descr,
		&inputs, &outputs, NULL)) < 0)
		return ret;
 
	if ((ret = avfilter_graph_config(vdpDecRdCtx.filter_graph, NULL)) < 0)
		return ret;
 
	vdpDecRdCtx.frame_in=av_frame_alloc();
	vdpDecRdCtx.frame_buffer_in=(unsigned char *)av_malloc(av_image_get_buffer_size(AV_PIX_FMT_YUV420P, vdpDecRdCtx.vid_width,vdpDecRdCtx.vid_height,1));
	av_image_fill_arrays(vdpDecRdCtx.frame_in->data, vdpDecRdCtx.frame_in->linesize,vdpDecRdCtx.frame_buffer_in,
		AV_PIX_FMT_YUV420P,vdpDecRdCtx.vid_width, vdpDecRdCtx.vid_height,1);
    /*
    printf("frame_buffer_in av_image_fill_arrays, data[0]:%p,data[1]:%p, data[2]:%p, linesize: %d, %d, %d, width:%d, height:%d\n", 
            vdpDecRdCtx.frame_in->data[0], vdpDecRdCtx.frame_in->data[1], vdpDecRdCtx.frame_in->data[2],
            vdpDecRdCtx.frame_in->linesize[0], vdpDecRdCtx.frame_in->linesize[1],vdpDecRdCtx.frame_in->linesize[2],
            vdpDecRdCtx.frame_in->width, vdpDecRdCtx.frame_in->height);*/
 
	vdpDecRdCtx.frame_out=av_frame_alloc();
	vdpDecRdCtx.frame_buffer_out=(unsigned char *)av_malloc(av_image_get_buffer_size(AV_PIX_FMT_YUV420P, vdpDecRdCtx.vid_width,vdpDecRdCtx.vid_height,1));
	av_image_fill_arrays(vdpDecRdCtx.frame_out->data, vdpDecRdCtx.frame_out->linesize,vdpDecRdCtx.frame_buffer_out,
		AV_PIX_FMT_YUV420P,vdpDecRdCtx.vid_width, vdpDecRdCtx.vid_height,1);
    /*
     printf("frame_buffer_out av_image_fill_arrays, data[0]:%p,data[1]:%p, data[2]:%p, linesize: %d, %d, %d, width:%d, height:%d\n", 
        vdpDecRdCtx.frame_out->data[0], vdpDecRdCtx.frame_out->data[1], vdpDecRdCtx.frame_out->data[2],
        vdpDecRdCtx.frame_out->linesize[0], vdpDecRdCtx.frame_out->linesize[1],vdpDecRdCtx.frame_out->linesize[2],
        vdpDecRdCtx.frame_out->width, vdpDecRdCtx.frame_out->height);*/
	vdpDecRdCtx.frame_in->width=vdpDecRdCtx.vid_width;
	vdpDecRdCtx.frame_in->height=vdpDecRdCtx.vid_height;
	vdpDecRdCtx.frame_in->format=AV_PIX_FMT_YUV420P;

    return 0;
    
}


//decode and post-process one frame
int vdp_decode_frame()
{
    VdpStatus status;
    int got_frame = 0;
    static int decoded_cnt = 0;
    int len = 0;
    len = avcodec_decode_video2(vdpDecRdCtx.pCodec_ctx, vdpDecRdCtx.pFrame, &got_frame, &vdpDecRdCtx.avpkt);
    if(len < 0){
        printf("decode fail, len: %d\n", len);
        return len;
    }

    if(got_frame){
        decoded_cnt++;
        pthread_mutex_lock(&queue_lock);
        vdpDecRdCtx.video_surface = (VdpVideoSurface)vdpDecRdCtx.pFrame->data[3];

        if(vflip != 0){
            get_ycbcr_bits2();

            if(av_buffersrc_add_frame(vdpDecRdCtx.buffersrc_ctx, vdpDecRdCtx.frame_in) < 0){
                printf("av buffersrc add frame failed\n");
                return -1;
            }

            if(av_buffersink_get_frame(vdpDecRdCtx.buffersink_ctx, vdpDecRdCtx.frame_out) < 0){
                printf("av buffersink get frame failed\n");
                return -1;
            }
            put_ycbcr_bits2();
            av_frame_unref(vdpDecRdCtx.frame_out);
        }

        vdpDecRdCtx.tmp_output_surface = vdpDecRdCtx.output_surface[vdpDecRdCtx.cur_index];
        vdpDecRdCtx.cur_index = (vdpDecRdCtx.cur_index+1)%2;
        

         status = vdp_video_mixer_render(vdpDecRdCtx.video_mixer, VDP_INVALID_HANDLE, 0,
                                        VDP_VIDEO_MIXER_PICTURE_STRUCTURE_FRAME, 0, NULL,
                                        //vdpDecRdCtx.video_surface,
                                        (vflip ? vdpDecRdCtx.tmp_video_surface : vdpDecRdCtx.video_surface),
                                        0, NULL,
                                        NULL,
                                        vdpDecRdCtx.output_surface[0],
                                        NULL, NULL, 0, NULL);
         status = vdp_video_mixer_render(vdpDecRdCtx.video_mixer, VDP_INVALID_HANDLE, 0,
                                        VDP_VIDEO_MIXER_PICTURE_STRUCTURE_FRAME, 0, NULL,
                                        //vdpDecRdCtx.video_surface,
                                        (vflip ? vdpDecRdCtx.tmp_video_surface : vdpDecRdCtx.video_surface),
                                        0, NULL,
                                        NULL,
                                        vdpDecRdCtx.output_surface[1],
                                        NULL, NULL, 0, NULL);
         

        int mod = decoded_cnt%20;
        VdpRect output_rect = {vdpDecRdCtx.last_x_pos, vdpDecRdCtx.last_y_pos, vdpDecRdCtx.last_x_pos+vdpDecRdCtx.bmp_width, vdpDecRdCtx.last_y_pos+vdpDecRdCtx.bmp_height};

        VdpColor color = {1,1,1,1}; 
        VdpOutputSurfaceRenderBlendState blend_state = {
            .struct_version = VDP_OUTPUT_SURFACE_RENDER_BLEND_STATE_VERSION,
            .blend_factor_source_color = VDP_OUTPUT_SURFACE_RENDER_BLEND_FACTOR_ONE,
            .blend_factor_source_alpha = VDP_OUTPUT_SURFACE_RENDER_BLEND_FACTOR_ZERO,
            .blend_factor_destination_color = VDP_OUTPUT_SURFACE_RENDER_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
            .blend_factor_destination_alpha = VDP_OUTPUT_SURFACE_RENDER_BLEND_FACTOR_ZERO,
            .blend_equation_color = VDP_OUTPUT_SURFACE_RENDER_BLEND_EQUATION_ADD,
            .blend_equation_alpha = VDP_OUTPUT_SURFACE_RENDER_BLEND_EQUATION_ADD,
        };

        status = vdp_output_surface_render_bitmap_sruface(vdpDecRdCtx.output_surface[0],
                                                 &output_rect,
                                                 vdpDecRdCtx.bitmap_surface,
                                                 NULL,
                                                 //&color,
                                                 NULL,
                                                 &blend_state,
                                                 VDP_OUTPUT_SURFACE_RENDER_ROTATE_0);

        if(status != VDP_STATUS_OK){
            printf("vdp_output_surface_render_bitmap_sruface fail, status: %d\n", status);
            return -1;
        }                                                 

        status = vdp_presentation_queue_display( vdpDecRdCtx.vdp_queue,
                                                  vdpDecRdCtx.output_surface[0], 
                                                  vdpDecRdCtx.vid_width, vdpDecRdCtx.vid_height,
                                                  0);

        pthread_mutex_unlock(&queue_lock);

        if(status != VDP_STATUS_OK){
            printf("vdp display fail, status: %d\n", status);
            return -1;
        }        
    }else{
        printf("gotframe is 0\n");
    }

    return 0;
}


// need setting environment variable VDPAU_OSD to enable OSD :"export VDPAU_OSD = 1";
// draw mouse-pointer
void draw_pointer(int x, int y)
{
    pthread_mutex_lock(&queue_lock);
    if(!vdpDecRdCtx.video_surface){
        pthread_mutex_unlock(&queue_lock);
        return;
    }
    VdpStatus status;
    static int draw_cnt = 0;
    draw_cnt++;
    VdpRect output_rect = {x, y, x+vdpDecRdCtx.bmp_width, y+vdpDecRdCtx.bmp_height};
    //vdpDecRdCtx.tmp_output_surface = ((draw_cnt % 2 == 0)?vdpDecRdCtx.output_surface[0]:vdpDecRdCtx.output_surface[1]);
    vdpDecRdCtx.tmp_output_surface = vdpDecRdCtx.output_surface[vdpDecRdCtx.cur_index];
    vdpDecRdCtx.cur_index = (vdpDecRdCtx.cur_index+1)%2;
    
    status = vdp_video_mixer_render(vdpDecRdCtx.video_mixer, VDP_INVALID_HANDLE, 0,
                                        VDP_VIDEO_MIXER_PICTURE_STRUCTURE_FRAME, 0, NULL,
                                        //vdpDecRdCtx.video_surface,
                                        (vflip ? vdpDecRdCtx.tmp_video_surface : vdpDecRdCtx.video_surface),
                                        0, NULL,
                                        NULL,
                                        vdpDecRdCtx.output_surface[0],
                                        NULL, NULL, 0, NULL);
    VdpColor color = {1,1,1,1}; 
    VdpOutputSurfaceRenderBlendState blend_state = {
        .struct_version = VDP_OUTPUT_SURFACE_RENDER_BLEND_STATE_VERSION,
        .blend_factor_source_color = VDP_OUTPUT_SURFACE_RENDER_BLEND_FACTOR_ONE,
        .blend_factor_source_alpha = VDP_OUTPUT_SURFACE_RENDER_BLEND_FACTOR_ZERO,
        .blend_factor_destination_color = VDP_OUTPUT_SURFACE_RENDER_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        .blend_factor_destination_alpha = VDP_OUTPUT_SURFACE_RENDER_BLEND_FACTOR_ZERO,
        .blend_equation_color = VDP_OUTPUT_SURFACE_RENDER_BLEND_EQUATION_ADD,
        .blend_equation_alpha = VDP_OUTPUT_SURFACE_RENDER_BLEND_EQUATION_ADD,
    };
    
    status = vdp_output_surface_render_bitmap_sruface(vdpDecRdCtx.output_surface[0],
                                             &output_rect,
                                             vdpDecRdCtx.bitmap_surface,
                                             NULL,
                                             //&color,
                                             NULL,
                                             &blend_state,
                                             VDP_OUTPUT_SURFACE_RENDER_ROTATE_0);

    if(status != VDP_STATUS_OK){
        printf("draw pointer vdp_output_surface_render_bitmap_sruface fail, status: %d\n", status);
        return -1;
    }                                                 

    status = vdp_presentation_queue_display( vdpDecRdCtx.vdp_queue,
                                              vdpDecRdCtx.output_surface[0], 
                                              vdpDecRdCtx.vid_width, vdpDecRdCtx.vid_height,
                                              0);
    
    pthread_mutex_unlock(&queue_lock);
    if(status != VDP_STATUS_OK){
        printf("vdp display fail, status: %d\n", status);
        return -1;
    }
}


void vdp_uninit_decoder()
{
    pthread_mutex_lock(&queue_lock);
    avcodec_close(vdpDecRdCtx.pCodec_ctx);
    av_free(vdpDecRdCtx.pCodec_ctx);
    av_frame_free(&vdpDecRdCtx.pFrame);

    vdp_output_surface_destroy(vdpDecRdCtx.output_surface[0]);
    vdp_output_surface_destroy(vdpDecRdCtx.output_surface[1]);
    for(int i=0; i< MAX_VIDEO_SURFACES; i++){
        if(vdpDecRdCtx.surface_entrys[i].is_alloc){
            vdp_video_surface_destroy(vdpDecRdCtx.surface_entrys[i].surface);
            vdpDecRdCtx.surface_entrys[i].is_alloc = 0;
            vdpDecRdCtx.surface_entrys[i].surface = 0;
        }
    }
    
    vdp_presentation_queue_target_destroy(vdpDecRdCtx.vdp_target);
    vdp_presentation_queue_destroy(vdpDecRdCtx.vdp_queue);
    vdp_video_mixer_destroy(vdpDecRdCtx.video_mixer);
    vdp_device_destroy(vdpDecRdCtx.vdp_device);
    
    //vdp_video_surface_destroy(vdpDecRdCtx.tmp_video_surface);
        //pthread_mutex_destroy(&vdpDecRdCtx.vdp_queue_lock);
    XUnmapWindow(vdpDecRdCtx.dis, vdpDecRdCtx.win);
    //XDestroyWindow(vdpDecRdCtx.dis, vdpDecRdCtx.win);
    //XFreeGC(vdpDecRdCtx.dis, vdpDecRdCtx.gc);
    //XCloseDisplay(vdpDecRdCtx.dis);
    vdpDecRdCtx.video_surface = 0;
    vdpDecRdCtx.vdp_device = 0;
    vdpDecRdCtx.vdp_target = 0;
    vdpDecRdCtx.vdp_queue =0;
    vdpDecRdCtx.video_mixer = 0;
    vdpDecRdCtx.output_surface[0] = 0;
    vdpDecRdCtx.output_surface[1] = 0;
    pthread_mutex_unlock(&queue_lock);
}

volatile int closed = 0;
int mouse_exit = 0;

int int_handler(){
    printf("handle sigint \n");
    closed = 1;
    return 0;
}

int segv_handler(){
    printf("handle segment fault\n");
    closed = 1;
    return 0;
}


void* handle_pointer_motion(void* arg)
{
    XEvent xevent;
    struct timeval t1, t2;
    int cnt = 0;
    gettimeofday(&t1, NULL);
    double elapsetime = 0;
    
    while(!closed){
         
        XNextEvent(vdpDecRdCtx.dis, &xevent);
 
        switch (xevent.type) {
            case MotionNotify:
                cnt++;
                //printf("Mouse move      : [%d, %d]\n", xevent.xmotion.x_root, xevent.xmotion.y_root);
                vdpDecRdCtx.last_x_pos = xevent.xmotion.x_root;
                vdpDecRdCtx.last_y_pos = xevent.xmotion.y_root;
                draw_pointer(xevent.xmotion.x_root, xevent.xmotion.y_root);
                
                if(cnt % 100 == 0){
                    gettimeofday(&t2, NULL);
                    elapsetime = (t2.tv_sec*1000000+t2.tv_usec-t1.tv_sec*1000000-t1.tv_usec);
                    printf("elapsetime: %6.2f ms, cnt: %d\n", elapsetime/1000.0, cnt);
                }
                
                break;
        }
    }
    mouse_exit  = 1;
}

int main(int argc, char** argv){

    //enable vdpau osd
    sigset(SIGINT, int_handler);
    setenv("VDPAU_OSD", "1", 1);
    
    FILE* fpInput = fopen(argv[1], "rb");
    fseek(fpInput, 0, SEEK_END);
    size_t sizeInput = ftell(fpInput);
    rewind(fpInput);
    uint8_t *pBuffer = (uint8_t*)malloc(sizeInput);
    fread(pBuffer, sizeof(char), sizeInput, fpInput);

    fpOutput               = fopen(argv[2], "wb");
    int width              = atoi(argv[3]);
    int height             = atoi(argv[4]);
    vflip                  = atoi(argv[5]);
    fpBitmap               = fopen(argv[6],"rb");
    vdpDecRdCtx.bmp_width  = atoi(argv[7]);
    vdpDecRdCtx.bmp_height = atoi(argv[8]);
        
    vdp_init_x(width, height);
    init_vdpau_decoder_and_mixer();
    init_video_surface();
    init_vdpau_queue();
    init_ffmpeg_with_vdpau_decoder();
    init_bitmap_surface();
    
    if(vflip != 0){
        if(init_avfilter_vflip()<0){
            printf("init avfilter faild\n");
            return -1;
        }
    }

    AVCodecParserContext* pParser_Ctx = av_parser_init(AV_CODEC_ID_H264);
    if(!pParser_Ctx){
        printf("av_parser_init fail\n");
        return -1;
    }

    uint8_t* ptBuff = pBuffer;
    int sizeLeft = sizeInput;
    int total_len = 0;
    int total_frame = 0;       
    pthread_t tid;
    
    int err = pthread_create(&tid, NULL, handle_pointer_motion, NULL);
    if(err != 0){
        printf("pthread_create fail\n");
        return -1;
    }

    while(!closed ){
        total_frame++;
        usleep(30000);
        
        int len = av_parser_parse2(pParser_Ctx, vdpDecRdCtx.pCodec_ctx, &vdpDecRdCtx.avpkt.data, &vdpDecRdCtx.avpkt.size, ptBuff, sizeLeft, 
            AV_NOPTS_VALUE, AV_NOPTS_VALUE, AV_NOPTS_VALUE);
        ptBuff += len;
        sizeLeft -= len;
        total_len += len;
        if(sizeLeft <=0 || vdpDecRdCtx.avpkt.size == 0)
            break;

        vdp_decode_frame();       
    }
    printf("totalframe:%d\n",total_frame);
    closed = 1;
    vdpDecRdCtx.avpkt.data = NULL;
    vdpDecRdCtx.avpkt.size = 0;
    vdp_decode_frame();
    av_frame_free(&vdpDecRdCtx.frame_in);
	av_frame_free(&vdpDecRdCtx.frame_out);
    avfilter_graph_free(&vdpDecRdCtx.filter_graph);
    
    while(!mouse_exit){
        sleep(1);
        printf("wait pointer thread finish\n");
    }
    vdp_close_x();
}


