//
//  libavg - Media Playback Engine. 
//  Copyright (C) 2003-2014 Ulrich von Zadow
//
//  This library is free software; you can redistribute it and/or
//  modify it under the terms of the GNU Lesser General Public
//  License as published by the Free Software Foundation; either
//  version 2 of the License, or (at your option) any later version.
//
//  This library is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
//  Lesser General Public License for more details.
//
//  You should have received a copy of the GNU Lesser General Public
//  License along with this library; if not, write to the Free Software
//  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
//
//  Current versions can be found at www.libavg.de
//
#include "VDPAUDecoder.h"

#include <iostream>

/*
Notes on adding Render-to-texture support:
- For every Surface, call
    VDPAURegisterSurfaceNV()
    VDPAUSurfaceAccessNV(..., READ) 
- VideoNode: Create OGLSurface and GLTexture objs each frame from texids,
  map VDPAU surfaces while textures exist.
- New OGLSurface pixel format VDPAU_INTERLACED
- Support new OGLSurface pixel format in shader
*/

using namespace std;

extern VdpVideoMixer video_mixer;

VDPAUDecoder::VDPAUDecoder()
    : m_VDPDecoder(VDP_INVALID_HANDLE),
      m_VDPMixer(VDP_INVALID_HANDLE),
      m_PixFmt(PIX_FMT_NONE)
{
	m_Size.x = -1;
	m_Size.y = -1;
}

VDPAUDecoder::~VDPAUDecoder()
{
    if (m_VDPMixer != VDP_INVALID_HANDLE) {
        vdp_video_mixer_destroy(m_VDPMixer);
    }
    if (m_VDPDecoder != VDP_INVALID_HANDLE) {
        vdp_decoder_destroy(m_VDPDecoder);
    }
    for (unsigned i = 0; i < m_RenderStates.size(); i++) {
        vdp_video_surface_destroy(m_RenderStates[i]->surface);
        delete m_RenderStates[i];
    }
}

void VDPAUDecoder::set_size(int x, int y){
	m_Size.x = x;
	m_Size.y = y;
}

VdpVideoMixer VDPAUDecoder::getVDPMixer()
{
    return m_VDPMixer;
}


int VDPAUDecoder::openCodec(AVCodecContext* pContext)
{
    if (!isAvailable()) {
        return -1;
    }

    pContext->get_buffer = VDPAUDecoder::getBuffer;
    pContext->release_buffer = VDPAUDecoder::releaseBuffer;
    pContext->draw_horiz_band = VDPAUDecoder::drawHorizBand;
    pContext->get_format = VDPAUDecoder::getFormat;
    pContext->slice_flags = SLICE_FLAG_CODED_ORDER | SLICE_FLAG_ALLOW_FIELD;
    //m_Size = IntPoint(pContext->width, pContext->height);

    return 0;
}

bool VDPAUDecoder::isAvailable()
{
#if LIBAVCODEC_VERSION_INT > AV_VERSION_INT(53, 34, 0)
    return getVDPAUDevice() != 0;
#else
    return false;
#endif
}

int VDPAUDecoder::getBuffer(AVCodecContext* pContext, AVFrame* pFrame)
{
    VDPAUDecoder* pVDPAUDecoder = (VDPAUDecoder*)pContext->opaque;
    return pVDPAUDecoder->getBufferInternal(pContext, pFrame);
}

// does not release the render structure, that will be unlocked after getting data
void VDPAUDecoder::releaseBuffer(struct AVCodecContext* pContext, AVFrame* pFrame)
{
    pFrame->data[0] = 0;
}


// main rendering routine
void VDPAUDecoder::drawHorizBand(struct AVCodecContext* pContext, const AVFrame* src,
        int offset[4], int y, int type, int height)
{
    VDPAUDecoder* pVDPAUDecoder = (VDPAUDecoder*)pContext->opaque;
    pVDPAUDecoder->render(pContext, src);
}

AVPixelFormat VDPAUDecoder::getFormat(AVCodecContext* pContext, const AVPixelFormat* pFmt)
{
    switch (pContext->codec_id) {
        case AV_CODEC_ID_H264:
            return PIX_FMT_VDPAU_H264;
        case AV_CODEC_ID_MPEG1VIDEO:
            return PIX_FMT_VDPAU_MPEG1;
        case AV_CODEC_ID_MPEG2VIDEO:
            return PIX_FMT_VDPAU_MPEG2;
        case AV_CODEC_ID_WMV3:
            return PIX_FMT_VDPAU_WMV3;
        case AV_CODEC_ID_VC1:
            return PIX_FMT_VDPAU_VC1;
        default:
            return pFmt[0];
    }
}

vdpau_render_state* VDPAUDecoder::getFreeRenderState()
{
    for (unsigned i = 0; i < m_RenderStates.size(); i++) {
        vdpau_render_state* pRenderState = m_RenderStates[i];
        if (!(pRenderState->state & FF_VDPAU_STATE_USED_FOR_REFERENCE)) {
            return m_RenderStates[i];
        }
    }
    
    // No free surfaces available -> create new surface
    vdpau_render_state* pRenderState = new vdpau_render_state;
    m_RenderStates.push_back(pRenderState);
    memset(pRenderState, 0, sizeof(vdpau_render_state));
    pRenderState->surface = VDP_INVALID_HANDLE;
    VdpStatus status = vdp_video_surface_create(getVDPAUDevice(), VDP_CHROMA_TYPE_420,
            m_Size.x, m_Size.y, &pRenderState->surface);
    //AVG_ASSERT(status == VDP_STATUS_OK);
    if(status != VDP_STATUS_OK){
        printf("vdp_video_surface_create fail\n");
        return 0;
    }

    return pRenderState;
}

int VDPAUDecoder::getBufferInternal(AVCodecContext* pContext, AVFrame* pFrame)
{
    vdpau_render_state* pRenderState = getFreeRenderState();
    pFrame->data[0] = (uint8_t*)pRenderState;
    pFrame->type = FF_BUFFER_TYPE_USER;

    //pRenderState->state |= FF_VDPAU_STATE_USED_FOR_REFERENCE;
    pRenderState->state |= FF_VDPAU_STATE_USED_FOR_REFERENCE;
    return 0;
}

void VDPAUDecoder::render(AVCodecContext* pContext, const AVFrame* pFrame)
{
    vdpau_render_state* pRenderState = (vdpau_render_state*)pFrame->data[0];

    if (m_VDPDecoder == VDP_INVALID_HANDLE) {
        setupDecoder(pContext);
    }

    VdpStatus status = vdp_decoder_render(m_VDPDecoder, pRenderState->surface,
            (VdpPictureInfo const*)&(pRenderState->info),
            pRenderState->bitstream_buffers_used, pRenderState->bitstream_buffers);
   //    AVG_ASSERT(status == VDP_STATUS_OK);
   if(status != VDP_STATUS_OK){
        printf("vdp_decoder_render fail, status: %d\n", status);
   }
}

void VDPAUDecoder::setupDecoder(AVCodecContext* pContext)
{
    VdpStatus status;

    // Create new decoder and mixer.
    VdpDecoderProfile profile = 0;
    switch (pContext->pix_fmt) {
        case PIX_FMT_VDPAU_MPEG1:
            profile = VDP_DECODER_PROFILE_MPEG1;
            break;
        case PIX_FMT_VDPAU_MPEG2:
            profile = VDP_DECODER_PROFILE_MPEG2_MAIN;
            break;
        case PIX_FMT_VDPAU_H264:
            profile = VDP_DECODER_PROFILE_H264_HIGH;
            break;
        case PIX_FMT_VDPAU_WMV3:
            profile = VDP_DECODER_PROFILE_VC1_SIMPLE;
            break;
        case PIX_FMT_VDPAU_VC1:
            profile = VDP_DECODER_PROFILE_VC1_SIMPLE;
            break;
        default:
            //AVG_ASSERT(false);
            printf("setDecoder fail, pix_fmt: %d", pContext->pix_fmt);
    }
    status = vdp_decoder_create(getVDPAUDevice(), profile, m_Size.x, m_Size.y, 16,
            &m_VDPDecoder);
//    AVG_ASSERT(status == VDP_STATUS_OK);
    if(status != VDP_STATUS_OK){
        printf("vdp_decoder_create fail, status: %d\n", status);
        return;
    }
    
    m_PixFmt = pContext->pix_fmt;

    VdpVideoMixerFeature features[] = {
        VDP_VIDEO_MIXER_FEATURE_DEINTERLACE_TEMPORAL,
        VDP_VIDEO_MIXER_FEATURE_DEINTERLACE_TEMPORAL_SPATIAL,
    };
    VdpVideoMixerParameter params[] = { 
        VDP_VIDEO_MIXER_PARAMETER_VIDEO_SURFACE_WIDTH,
        VDP_VIDEO_MIXER_PARAMETER_VIDEO_SURFACE_HEIGHT,
        VDP_VIDEO_MIXER_PARAMETER_CHROMA_TYPE,
        VDP_VIDEO_MIXER_PARAMETER_LAYERS
    };
    VdpChromaType chroma = VDP_CHROMA_TYPE_420;
    int  numLayers = 0;
    void const* paramValues [] = { &m_Size.x, &m_Size.y, &chroma, &numLayers };

    status = vdp_video_mixer_create(getVDPAUDevice(), 2, features, 4, params, 
            paramValues, &m_VDPMixer);
    //m_VDPMixer = video_mixer;
//    AVG_ASSERT(status == VDP_STATUS_OK);
    if(status != VDP_STATUS_OK){
        printf("vdp_video_mixer_create fail, status: %d\n", status);
    }
}


