
extern "C" {

#include <libavutil/imgutils.h>
#include <libavutil/samplefmt.h>
#include <libavutil/timestamp.h>
#include <libavformat/avformat.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xos.h>
#include <vdpau/vdpau_x11.h>
}
#include <stdio.h>
#include <assert.h>
#include <stdarg.h>

#include "VDPAUDecoder.h"

#define ASSERT(exp, msg) assert((exp) && (msg))
#define LOG(...) do { printf(__VA_ARGS__); printf("\n"); } while(0)

static AVCodec* pCodec = NULL;
static AVCodecContext* pCodec_ctx = NULL;
static AVFrame* pFrame = NULL;
static AVPacket avpkt;
static AVCodecParserContext* pParser_Ctx = NULL;
static VDPAUDecoder* pVdpDec = NULL;
static enum AVCodecID codecID = AV_CODEC_ID_H264;
static int isCodecInited = 0;
FILE* fpOutput = NULL;

/*--------------------------X-variables---------------------------*/
Display                          *dis;
int                              screen;
Window                           win;
GC                               gc;

/*-------------------------VDPAU-variables-----------------------*/
static VdpOutputSurface                 output_surface;
static VdpVideoSurface                  video_surface;
static VdpPresentationQueueStatus       status;
static VdpProcamp                       procamp;
VdpVideoMixer                           video_mixer;
static uint32_t                         vid_width, vid_height;
static VdpChromaType                    vdp_chroma_type;
static VdpDevice                        vdp_device;
static VdpYCbCrFormat                   vdp_pixel_format;
static int                              colorspace;
static VdpPresentationQueueTarget       vdp_target;
static VdpPresentationQueue             vdp_queue;


void init_x(){

    /*
     vid_width = 1280;
     vid_height = 720;
     vdp_device = getVDPAUDevice();
     XSetWindowAttributes attributes;

     attributes.background_pixel = XWhitePixel(dis, 0);  
     win = XCreateWindow(dis, XRootWindow(dis, 0), 
         0, 0, 1280, 720, 0, DefaultDepth(dis, 0), 
         InputOutput, DefaultVisual(dis, 0), 
         CWBackPixel,&attributes); 

     XSelectInput(dis, win, ExposureMask | KeyPressMask ); 

     gc = XCreateGC(dis, win, 0, NULL); 
          XSynchronize(dis, 1);
         XMapWindow(dis, win); 

    */ 
    vid_width = 1280;
    vid_height = 720;
    vdp_device = getVDPAUDevice();
    unsigned long black, white;
    black = BlackPixel(dis, screen);
    white = WhitePixel(dis, screen);
    win = XCreateSimpleWindow(dis, DefaultRootWindow(dis), 0, 0, 1280, 720, 5, black, white);
    XSetStandardProperties(dis, win, "VDPAU Player", "VDPAU", None, NULL, 0, NULL);
    XSelectInput(dis, win, ExposureMask|ButtonPressMask|KeyPressMask);
    gc=XCreateGC(dis, win, 0,NULL);
    XSynchronize(dis, 1);
    XSetBackground(dis,gc,white);
    XSetBackground(dis,gc,black);
    XClearWindow(dis, win);
}

void close_x() {
	XFreeGC(dis, gc);
    XCloseDisplay(dis);
	XDestroyWindow(dis,win);
	exit(0);
}

int init_vdpau_queue()
{
    VdpStatus vdp_st;

    vdp_st = vdp_presentation_queue_target_create_x11(vdp_device, win,
                                                      &vdp_target);
    if(vdp_st != VDP_STATUS_OK) {
        printf("vdp_presentation_queue_target_crate_x11 fail, vdp_st: %d\n", vdp_st);
        return -1;
    }

    XMapRaised(dis, win);

    vdp_st = vdp_presentation_queue_create(vdp_device, vdp_target,
                                           &vdp_queue);
    if(vdp_st != VDP_STATUS_OK) {
        printf("vdp_presentation_queue_crate fail, vdp_st: %d\n", vdp_st);
        return -1;
    }

    return 0;
}

int create_output_surface(){
 
    VdpStatus vdp_st;

    vdp_st = vdp_output_surface_create(vdp_device, VDP_RGBA_FORMAT_B8G8R8A8,
                                           1280, 720,
                                           &output_surface);

    if (vdp_st != VDP_STATUS_OK) {
        printf("vdp_output_surface_create fail, vdp_st: %d", vdp_st);
        return -1;
    }

    return 0;
}

int InitDecoder(int codec_id){

    if(isCodecInited != 0){
        printf("isCodecInited is true, init failed");
        return -1;
    }

    avcodec_register_all();

    //VDPAUDecoder* pVdpDecoder = new VDPAUDecoder();

    //pCodec = avcodec_find_decoder(codec_id);
    pCodec = avcodec_find_decoder_by_name("h264_vdpau");
    if(!pCodec){
        printf("avcodec_find_decoder fail\n");
        return -1;
    }

    pCodec_ctx = avcodec_alloc_context3(pCodec);
    if(!pCodec_ctx){
        printf("avcodec_alloc_context3 fail\n");
        return -1;
    }

    pVdpDec = new VDPAUDecoder();
    pCodec_ctx->opaque = pVdpDec;
    pVdpDec->openCodec(pCodec_ctx);
    
    if(avcodec_open2(pCodec_ctx, pCodec, NULL) < 0){
        printf("avcodec_open2 fail\n");
        return -1;
    }

    pParser_Ctx = av_parser_init(codec_id);
    if(!pParser_Ctx){
        printf("av_parser_init fail\n");
        return -1;
    }

    pFrame = av_frame_alloc();
    if(!pFrame){
        printf("av_frame_alloc fail\n");
        return -1;
    }

    av_init_packet(&avpkt);
    isCodecInited = 1;

}

int get_bits() {
    uint32_t **data;
    int i,j;
    const uint32_t a[1] = {vid_width*4};
    VdpStatus vdp_st;
    
    data = (uint32_t * * )calloc(1, sizeof(uint32_t *));
    
    for(i = 0; i < 1; i++)
        data[i] = (uint32_t *)calloc(vid_width*vid_height, sizeof(uint32_t *));
    
    vdp_st = vdp_output_surface_get_bits_native(output_surface,NULL,
                                                (void * const*)data,
                                                 a);
 
    if (vdp_st != VDP_STATUS_OK){
        printf("vdp_ouput_surface_get_bits_native faile, vdp_st: %d\n", vdp_st);
        free(data[0]);
        free(data);
        return -1;
    }
    fwrite(data[0], sizeof(char), vid_width*vid_height*4, fpOutput);
    fflush(fpOutput);
    free(data[0]);
    free(data);

    return 0; 
}


int get_ycbcr_bits()
{
    uint8_t ** data;
    const uint32_t a[3] = {vid_width, vid_width/2, vid_width/2};
    VdpStatus vdp_st;
    data = (uint8_t**) calloc(3, sizeof(uint8_t*));
    data[0] = (uint8_t*)calloc(vid_width*vid_height, sizeof(uint8_t));
    data[1] = (uint8_t*)calloc(vid_width*vid_height/4, sizeof(uint8_t));
    data[2] = (uint8_t*)calloc(vid_width*vid_height/4, sizeof(uint8_t));

    vdp_st = vdp_video_surface_get_bits_y_cb_cr(video_surface, VDP_YCBCR_FORMAT_YV12, (void* const*)data, a);
    if(vdp_st != VDP_STATUS_OK){
        printf("vdp_video_surface_get_bits_ycb_cr fail, vdp_st: %d\n", vdp_st);
        free(data[0]);
        free(data[1]);
        free(data[2]);
        free(data);
        return -1;
    }

    fwrite(data[0],sizeof(uint8_t), vid_height*vid_width, fpOutput);
    fwrite(data[2],sizeof(uint8_t), vid_height*vid_width/4, fpOutput);
    fwrite(data[1],sizeof(uint8_t), vid_height*vid_width/4, fpOutput);

    fflush(fpOutput);
    
    free(data[0]);
    free(data[1]);
    free(data[2]);
    free(data);

    return 0;
}


static int decode_frame(){

    VdpStatus vdp_st;
    int len = 0, i = 0;
    int got_frame = 0;
    static int decoded_cnt = 0;
    int field = VDP_VIDEO_MIXER_PICTURE_STRUCTURE_FRAME;
    
    if(!isCodecInited){
        printf("%s decoder uninited", __func__);
        return -1;
    }

    len = avcodec_decode_video2(pCodec_ctx, pFrame, &got_frame, &avpkt);
    if(len < 0){
        printf("Error while decoding frames\n");
        return len;
    }

    if(got_frame){
        decoded_cnt ++;
        printf("got frame, pix_fmt: %d, decoded_cnt: %d, len: %d, width: %d, height: %d\n", 
                pCodec_ctx->pix_fmt, decoded_cnt, len, pCodec_ctx->width, pCodec_ctx->height);

        vdpau_render_state* pRenderState = (vdpau_render_state*)pFrame->data[0];
        video_surface = pRenderState->surface;
        //video_mixer = pVdpDec->getVDPMixer();


        //if(decoded_cnt <= 100)
        //get_ycbcr_bits();
        //pRenderState->state &= ~FF_VDPAU_STATE_USED_FOR_REFERENCE;
        create_output_surface();
        vdp_st = vdp_video_mixer_render(pVdpDec->m_VDPMixer, VDP_INVALID_HANDLE, 0,
                                        VDP_VIDEO_MIXER_PICTURE_STRUCTURE_TOP_FIELD, 0, (VdpVideoSurface*)VDP_INVALID_HANDLE,
                                        video_surface,
                                        0, (VdpVideoSurface*)VDP_INVALID_HANDLE,
                                        NULL,
                                        output_surface,
                                        NULL, NULL, 0, NULL);
        if(vdp_st != VDP_STATUS_OK){
            printf("vdp_video_mixer_render fail, vpd_st: %d", vdp_st);
            return -1;
        }
        printf("vdp_video_mixer_render success\n");

        /*
        if(decoded_cnt <= 100)
            get_bits();
        */
        
        vdp_st = vdp_presentation_queue_display(vdp_queue,
                                                  output_surface, 
                                                  1280, 720,
                                                  0);
        if(vdp_st != VDP_STATUS_OK){
            printf("vdp_presentation_queue_display fail, vpd_st: %d", vdp_st);
            return -1;
        }
        printf("vdp_presentation_queue_display success\n");
        
        pRenderState->state &= ~FF_VDPAU_STATE_USED_FOR_REFERENCE;
        vdp_output_surface_destroy(output_surface);
        
        switch(pCodec_ctx->pix_fmt){
      
            case AV_PIX_FMT_YUV420P:
            {
                for(i=0; i<pFrame->height; i++) {
                    fwrite(pFrame->data[0]+pFrame->linesize[0]*i, sizeof(char), pFrame->width, fpOutput);
                }
                for(i=0; i<pFrame->height/2; i++) {
                    fwrite(pFrame->data[1]+pFrame->linesize[1]*i, sizeof(char), pFrame->width/2, fpOutput);
                }
                for(i=0; i<pFrame->height/2; i++) {
                    fwrite(pFrame->data[2]+pFrame->linesize[2]*i, sizeof(char), pFrame->width/2, fpOutput);
                }
                fflush(fpOutput);
                
            }
            break;
            
            default:
                break;
            //return -1;
        }
    }
    
    return 0;
}

void UninitDecoder(){

    if(isCodecInited){
       avcodec_close(pCodec_ctx);
       av_free(pCodec_ctx);
       av_frame_free(&pFrame);
       isCodecInited = 0;
    }
}

int main(int argc, char** argv){

    XEvent event;

    ASSERT(argc == 3, "Invalid Parameter");

    FILE* fpInput = fopen(argv[1], "rb");
    fseek(fpInput, 0, SEEK_END);
    size_t sizeInput = ftell(fpInput);
    rewind(fpInput);

    uint8_t *pBuffer = (uint8_t*)malloc(sizeInput);
    ASSERT(pBuffer, "alloc memory for input buffer failed");

    ASSERT( fread(pBuffer, sizeof(char), sizeInput, fpInput) == sizeInput, "read input data failed");

    fpOutput = fopen(argv[2], "wb");
    ASSERT(fpOutput, "open output file failed");

    int sts;
    InitDecoder(codecID);
    init_x();
    /*
    sts = create_output_surface();
    if(sts == -1){
        printf("Error in creating VdpOutputSurface\n");
        return -1;
    }
    */
    sts = init_vdpau_queue();    
    if(sts == -1){
        printf("Error in initializing VdpPresentationQueue\n");
        return -1;
    }
    
    int isPrintwh = 0;

    uint8_t* ptBuff = pBuffer;
    int sizeLeft = sizeInput;
    while(1){

        usleep(30000);
        int len = av_parser_parse2(pParser_Ctx, pCodec_ctx, &avpkt.data, &avpkt.size, ptBuff, sizeLeft, 
            AV_NOPTS_VALUE, AV_NOPTS_VALUE, AV_NOPTS_VALUE);
        if(!isPrintwh){
	    pVdpDec->set_size(1280, 720);
            //printf("video_size : w: %d, h: %d", pCodec_ctx->width, pCodec_ctx->height);
            isPrintwh++;
        }
        ptBuff += len;
        sizeLeft -= len;

        if(avpkt.size == 0)
            break;

        if(decode_frame() < 0){
            //printf("%s decode fail\n", __func__);
            //return -1;
        }
            
    }

    avpkt.data = NULL;
    avpkt.size = 0;
    if(decode_frame() < 0){
        printf("%s decode_frame, last==1, fail\n", __func__);
        //return -1;
    }
/*
    while(1){
        XNextEvent(dis, &event);

        if(event.type == KeyPress)
            close_x();
    }*/
    
    //UninitDecoder();
    close_x();
}





