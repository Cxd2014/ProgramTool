INCLUDE_PATH += -I/usr/local/include/SDL2/

LIBRARY_PATH = /usr/local/lib/libavdevice.a \
			/usr/local/lib/libavfilter.a \
			/usr/local/lib/libavformat.a \
			/usr/local/lib/libavcodec.a \
			/usr/local/lib/libavutil.a \
			/usr/local/lib/libswresample.a \
			/usr/local/lib/libswscale.a \
			/usr/local/lib/libSDL2.a \

LIB = -lfontconfig -lfreetype -lexpat -lfribidi -lswresample -lm -lz 

.PHONY: clean 

clean:
	rm -rf videoplayer

videoplayer: rqueue.c videoplayer.c
	gcc $? -o videoplayer $(INCLUDE_PATH) $(LIBRARY_PATH) $(LIB)

build: clean videoplayer

.DEFAULT_GOAL := build
