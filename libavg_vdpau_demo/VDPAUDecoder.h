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
#ifndef _VDPAUDecoder_H_
#define _VDPAUDecoder_H_

extern "C"{

#include <vdpau/vdpau.h>
#include <libavcodec/vdpau.h>
}
#include "VDPAUHelper.h"
#include <vector>

typedef struct _IntPoint{
    int x;
    int y;
}IntPoint;

class VDPAUDecoder
{
public:
    VDPAUDecoder();
    ~VDPAUDecoder();
    int openCodec(AVCodecContext* pCodec);

    static bool isAvailable();
    void set_size(int x, int y);
    VdpVideoMixer getVDPMixer();

private:
    // Callbacks
    static int getBuffer(AVCodecContext* pContext, AVFrame* pFrame);
    static void releaseBuffer(struct AVCodecContext* pContext, AVFrame* pFrame);
    static void drawHorizBand(AVCodecContext* pContext, const AVFrame* pFrame, 
            int offset[4], int y, int type, int height);
    static AVPixelFormat getFormat(AVCodecContext* pContext, const AVPixelFormat* pFmt);

    vdpau_render_state* getFreeRenderState();
    int getBufferInternal(AVCodecContext* pContext, AVFrame* pFrame);
    void render(AVCodecContext* pContext, const AVFrame* pFrame);
    void setupDecoder(AVCodecContext* pContext);

    VdpDecoder m_VDPDecoder;
public:
    VdpVideoMixer m_VDPMixer;
    AVPixelFormat  m_PixFmt;
    IntPoint m_Size;
    std::vector<vdpau_render_state*> m_RenderStates;

};

#endif

