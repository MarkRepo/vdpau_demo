CC=g++ -D__STDC_CONSTANT_MACROS
CFLAGS=-lX11 -lvdpau -lavcodec -lavutil

vdpau_decoder: vdpau_decoder.cpp VDPAUDecoder.cpp VDPAUHelper.cpp 
	$(CC) -o vdpau_decoder  vdpau_decoder.cpp VDPAUDecoder.cpp VDPAUHelper.cpp $(CFLAGS)


