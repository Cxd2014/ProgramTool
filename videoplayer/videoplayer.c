#include <stdio.h>
#include <stdarg.h>
#include <stdbool.h>

#include <SDL.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
#include <libavdevice/avdevice.h>
#include <libavfilter/avfilter.h>

#include "rqueue.h"

#define QUEUE_BUFF_LEN (8)

typedef struct videoFrame {
    AVFrame *frame;
    bool isRendered;
} videoFrame;


typedef struct playerState {
    SDL_Window *pWindow;
    SDL_Renderer *pRender;
    SDL_Texture *pTexture;

    AVFormatContext *pFormatCtx;
    AVCodecContext *pCodecCtx;
    struct SwsContext *pImgConvertCtx;

    SDL_Thread *readThread;
    rqueue_t *pQueue;

    int videoIndex;
    int windowWidth;
    int windowHeight;
    char *pFileName;
    bool isQuit;

    bool isPause;
    videoFrame *pPauseFrame;

    videoFrame videoBuff[QUEUE_BUFF_LEN];

    int renderFrameCount;

} playerState;


int init_ffmpeg(playerState *ps) {
    
    printf("init ffmpeg...\n");

    //初始化FormatContext
    ps->pFormatCtx = avformat_alloc_context();
    if (!ps->pFormatCtx) {
        printf("alloc format context error!\n");
        return __LINE__;
    }
    AVFormatContext *pFormatCtx = ps->pFormatCtx;

    // 打开视频文件
    int err = avformat_open_input(&pFormatCtx, ps->pFileName, NULL, NULL);
    if (err < 0) {
        printf("avformat_open_input error!\n");
        return __LINE__;
    }

    //读取媒体文件信息
    err = avformat_find_stream_info(pFormatCtx, NULL);
    if (err != 0) {
        printf("[error] find stream error!");
        return __LINE__;
    }

    // 输出视频文件信息
    av_dump_format(pFormatCtx, 0, ps->pFileName, 0);
    
    //初始化pCodecCtx
    ps->pCodecCtx = avcodec_alloc_context3(NULL);
    if (!ps->pCodecCtx) {
        printf("avcodec_alloc_context3 error!");
        return __LINE__;
    }
    AVCodecContext *pCodecCtx = ps->pCodecCtx;

    //寻找到视频流的下标
    ps->videoIndex = av_find_best_stream(pFormatCtx, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);

    //将视频流的的编解码信息拷贝到pCodecCtx中
    err = avcodec_parameters_to_context(pCodecCtx, pFormatCtx->streams[ps->videoIndex]->codecpar);
    if (err != 0) {
        printf("avcodec_parameters_to_context error!");
        return __LINE__;
    }

    //查找解码器
    const AVCodec *pCodec = avcodec_find_decoder(pCodecCtx->codec_id);
    if (!pCodec) {
        printf("avcodec_find_decoder error!");
        return __LINE__;
    }

    //打开解码器
    err = avcodec_open2(pCodecCtx, pCodec, NULL);
    if (err != 0) {
        printf("avcodec_open2 error!");
        return __LINE__;
    }

    return 0;
}


int init_sdl(playerState *ps) {
    printf("init sdl...\n");

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) < 0) {
        printf("could not initialize sdl2: %s\n", SDL_GetError());
        return __LINE__;
    }

    // 创建窗口
    ps->pWindow = SDL_CreateWindow(
            "video player",
            SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
            ps->windowWidth, ps->windowHeight,
            SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
    if (ps->pWindow == NULL) {
        printf("could not create window: %s\n", SDL_GetError());
        return __LINE__;
    }

    // 创建渲染器
    ps->pRender = SDL_CreateRenderer(ps->pWindow, -1, 0);
    if (ps->pRender == NULL) {
        printf("SDL_CreateRenderer error: %s\n", SDL_GetError());
        return __LINE__;
    }

    return 0;
}

/** 在窗口中绘制一帧画面 **/
int render_video_frame(playerState *ps, videoFrame *vFrame) {
    if (vFrame == NULL || vFrame->frame == NULL) {
        printf("frame is null error\n");
        return __LINE__;
    }

    AVFrame *frame = vFrame->frame;
    SDL_Rect rect;
    rect.x = 0;
    rect.y = 0;
    rect.w = ps->windowWidth;
    rect.h = ps->windowHeight;

    if (ps->pTexture == NULL) {
        // 窗口大小调整之后需要重新创建纹理
        ps->pTexture = SDL_CreateTexture(ps->pRender, SDL_PIXELFORMAT_IYUV,
                        SDL_TEXTUREACCESS_STREAMING, ps->windowWidth, ps->windowHeight);
        if (ps->pTexture == NULL) {
            printf("SDL_CreateTexture error: %s\n", SDL_GetError());
            return __LINE__;
        }
    }

    if (ps->pImgConvertCtx == NULL) {
        ps->pImgConvertCtx = sws_getCachedContext(ps->pImgConvertCtx,
                frame->width, frame->height, frame->format, ps->windowWidth, ps->windowHeight,
                frame->format, SWS_BICUBIC, NULL, NULL, NULL);
        if (ps->pImgConvertCtx == NULL) {
            printf("sws_getCachedContext error\n");
            return __LINE__;
        }
    }

    // 根据窗口大小,调整视频帧的大小
    // 分配新视频帧的内存空间
    uint8_t *pixels[4] = {NULL};
    int linesize[4];
    int ret = av_image_alloc(pixels, linesize, ps->windowWidth, ps->windowHeight, frame->format, 1);
    if (ret < 0) {
        printf("av_image_alloc error %d %d\n", ret, frame->format);
        return __LINE__;
    }
    
    // 调整帧大小
    sws_scale(ps->pImgConvertCtx, (const uint8_t * const *)frame->data, frame->linesize,
                            0, frame->height, pixels, linesize);

    //上传YUV数据到Texture
    SDL_UpdateYUVTexture(ps->pTexture, &rect,
                         pixels[0], linesize[0],
                         pixels[1], linesize[1],
                         pixels[2], linesize[2]);


    SDL_RenderClear(ps->pRender); // 先清空渲染器
    SDL_RenderCopy(ps->pRender, ps->pTexture, NULL, &rect); // 将纹理数据复制到渲染器
    SDL_RenderPresent(ps->pRender); // 渲染画面

    if (ps->isPause == false) {
        ps->renderFrameCount++;
        vFrame->isRendered = true;
        av_frame_unref(frame);
    }

    av_freep(&pixels[0]);
    return 0;
}

// 从文件中读取视频流,解码后发送到队列中
int do_read_video(void *arg) {

    playerState *ps = arg;
    AVPacket *packet = av_packet_alloc();
    AVFrame *frame = av_frame_alloc(); 
    int frameCount = 0;
    bool readEnd = false;
    int queueIndex = 0;

    while (ps->isQuit == false) {
        // av_read_frame 从文件中读取一帧未解码的数据
        if (av_read_frame(ps->pFormatCtx, packet) >= 0) {
            // 如果是视频数据
            if (packet->stream_index == ps->videoIndex) {
                // 发送给解码器
                if (avcodec_send_packet(ps->pCodecCtx, packet) < 0) {
                    printf("avcodec_send_packet error\n");
                    return __LINE__;
                }
            }
            av_packet_unref(packet);
        } else {
            // 文件读完
            readEnd = true;
        }

        while ((ps->videoBuff[queueIndex].isRendered == false) && (ps->isQuit == false)) {
            // 队列满了,等50ms后再次尝试
            //printf("write queue full sleep 50ms %d:%d\n", frameCount, queueIndex);
            SDL_Delay(50);
        }

        // 从解码器中循环读取帧数据,直到读取失败
        while (avcodec_receive_frame(ps->pCodecCtx, frame) >= 0) {
            // 复制帧数据        
            av_frame_move_ref(ps->videoBuff[queueIndex].frame, frame);
            ps->videoBuff[queueIndex].isRendered = false;

            av_frame_unref(frame);
            // 写入环形队列
            if (rqueue_write(ps->pQueue, &(ps->videoBuff[queueIndex])) != 0) {
                printf("rqueue_write error %d:%d\n", frameCount, queueIndex);
                return __LINE__;
            }

            frameCount++;
            queueIndex++;
            if (queueIndex == QUEUE_BUFF_LEN)
                queueIndex = 0;
        }

        if (readEnd) {
            break;
        }
    }

    av_packet_free(&packet);
    av_frame_free(&frame);
    printf("read file end, write frame count to queue: %d\n", frameCount);
    return 0;
}

int parse_cmdline(playerState *ps, int argc, char *argv[]) {
    if (argc < 2) {
        printf("please input video file!\n");
        return __LINE__;
    }

    ps->pFileName = argv[1];
    printf("video file name:%s\n", ps->pFileName);
    return 0;
}

void do_exit(playerState *ps) {

    printf("render frame count: %d\nbye...\n", ps->renderFrameCount);

    ps->isQuit = true;
    if (ps->readThread)
        SDL_WaitThread(ps->readThread, NULL);
    if (ps->pTexture)
        SDL_DestroyTexture(ps->pTexture);
    if (ps->pImgConvertCtx)
        sws_freeContext(ps->pImgConvertCtx);

    SDL_DestroyRenderer(ps->pRender);
    SDL_DestroyWindow(ps->pWindow);

    rqueue_destroy(ps->pQueue);
    SDL_Quit();
    exit(0);
}

void event_loop(playerState *ps) {
    SDL_Event event;
    while (1)
    {
        SDL_PumpEvents();
        // 查看键盘，鼠标事件
        if (SDL_PeepEvents(&event, 1, SDL_GETEVENT, SDL_FIRSTEVENT, SDL_LASTEVENT) != 0) {
            switch (event.type) {
            case SDL_KEYDOWN:
                 switch (event.key.keysym.sym) {
                    case SDLK_SPACE:
                        ps->isPause = !(ps->isPause); // 暂停，开启播放
                        printf("space key kickdown isPause:%d\n", ps->isPause);
                        break;
                 }
                 break;
            case SDL_WINDOWEVENT:
                switch (event.window.event) {
                    case SDL_WINDOWEVENT_SIZE_CHANGED:
                        ps->windowWidth = event.window.data1;
                        ps->windowHeight = event.window.data2;
                        if (ps->pTexture) {
                            SDL_DestroyTexture(ps->pTexture);
                            ps->pTexture = NULL;
                        }

                        if (ps->pImgConvertCtx) {
                            sws_freeContext(ps->pImgConvertCtx);
                            ps->pImgConvertCtx = NULL;
                        }
                        printf("window size changed width:%d height:%d\n",
                                    event.window.data1, event.window.data2);
                }
                break;
            case SDL_QUIT:
                do_exit(ps);
                break;
            }
        }

        if (!rqueue_isempty(ps->pQueue)) {
            if (ps->isPause == true) {

                if (ps->pPauseFrame == NULL) {
                    ps->pPauseFrame = rqueue_read(ps->pQueue);
                }

                render_video_frame(ps, ps->pPauseFrame);
                SDL_Delay(50);
            } else {
                
                if (ps->pPauseFrame != NULL) {
                    ps->renderFrameCount++;
                    ps->pPauseFrame->isRendered = true;
                    av_frame_unref(ps->pPauseFrame->frame);
                    ps->pPauseFrame = NULL;
                }

                render_video_frame(ps, rqueue_read(ps->pQueue));
            }
        } else {
            SDL_Delay(10);
        }
    }
}

int init_playerState(playerState *ps) {

    ps->pWindow = NULL;
    ps->pRender = NULL;
    ps->pTexture = NULL;

    ps->pFormatCtx = NULL;
    ps->pCodecCtx = NULL;
    ps->pImgConvertCtx = NULL;

    ps->readThread = NULL;

    ps->videoIndex = 0;
    ps->windowWidth = 1080; // 默认窗口大小
    ps->windowHeight = 720;

    ps->pFileName = NULL;
    ps->isQuit = false;
    ps->isPause = false;
    ps->pPauseFrame = NULL;

    ps->renderFrameCount = 0;

    ps->pQueue = rqueue_create(QUEUE_BUFF_LEN, RQUEUE_MODE_BLOCKING);
    if (ps->pQueue == NULL) {
        printf("rqueue_create error\n");
        return __LINE__;
    }

    for (int i = 0; i < QUEUE_BUFF_LEN; i++) {
        ps->videoBuff[i].isRendered = true;
        if (!(ps->videoBuff[i].frame = av_frame_alloc())) {
            printf("av_frame_alloc error\n");
            return __LINE__;
        }
    }

    return 0;
}

int main(int argc, char *argv[]) {

    playerState ps;
    if (init_playerState(&ps) != 0) {
        printf("init_playerState error\n");
        return __LINE__;
    }

    if (parse_cmdline(&ps, argc, argv) != 0) {
        printf("parse_cmdline error\n");
        return __LINE__;
    }

    if (init_sdl(&ps) != 0) {
        printf("init_sdl error\n");
        return __LINE__;
    }
   
    if (init_ffmpeg(&ps) != 0) {
        printf("init_ffmpeg error\n");
        return __LINE__;
    }

    ps.readThread = SDL_CreateThread(do_read_video, "read_thread", &ps);
    if (!ps.readThread) {
        printf("SDL_CreateThread(): %s\n", SDL_GetError());
        return __LINE__;
    }

    printf("playing...\n");
    event_loop(&ps);
    return 0;
}
