#include <stdio.h>
#include <stdarg.h>
#include <stdbool.h>

#include <SDL.h>
#include <SDL_ttf.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
#include <libavdevice/avdevice.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>

#include "rqueue.h"

#define QUEUE_BUFF_LEN (8)
#define FILTER_DESCR "drawtext=fontfile=/usr/share/fonts/truetype/freefont/FreeSerif.ttf:fontsize=60:text='alpha':x=w-text_w-20:y=20"

typedef struct videoFrame {
    AVFrame *frame;
    bool isRendered;
} videoFrame;


typedef struct playerState {
    SDL_Window *pWindow;
    SDL_Renderer *pRender;
    SDL_Texture *pTexture;
    TTF_Font *pFont;

    AVFormatContext *pFormatCtx;
    AVCodecContext *pCodecCtx;
    struct SwsContext *pImgConvertCtx;
    
    AVFilterGraph *pfilterGraph;
    AVFilterContext *pBuffersinkCtx;
    AVFilterContext *pBuffersrcCtx;

    SDL_Thread *readThread;
    rqueue_t *pQueue;

    int videoIndex;
    int windowWidth;
    int windowHeight;
    char *pFileName;
    bool isQuit;

    bool isPause;
    bool isNext;
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

    // double frame = (pFormatCtx->duration / (double)AV_TIME_BASE) * av_q2d(pFormatCtx->streams[ps->videoIndex]->avg_frame_rate);
    // printf("rate num:%d den:%d duration:%ld base:%d frame:%1.0f\n", pFormatCtx->streams[ps->videoIndex]->avg_frame_rate.num,
    //     pFormatCtx->streams[ps->videoIndex]->avg_frame_rate.den, pFormatCtx->duration, AV_TIME_BASE, frame);

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

    if (TTF_Init() < 0) {
        printf("Couldn't initialize TTF: %s\n",SDL_GetError());
        return __LINE__;
    }

    ps->pFont = TTF_OpenFont("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", 24);
    if (ps->pFont == NULL) {
        printf("Couldn't load pt font from %s\n", SDL_GetError());
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

int show_frame_count(playerState *ps) {
    
    char str[12] = "";
    sprintf(str, "%d", ps->renderFrameCount+1);
    SDL_Color white = { 0xFF, 0xFF, 0xFF, 0 };
    
    SDL_Surface *text = TTF_RenderUTF8_Blended(ps->pFont, str, white);
    if(text == NULL){
        printf("TTF_RenderUTF8_Blended error\n");
        return __LINE__;
    }

    SDL_Rect rect;
    rect.x = 20;
    rect.y = 20;
    rect.w = text->w;
    rect.h = text->h;
    SDL_Texture* texture = SDL_CreateTextureFromSurface(ps->pRender, text);
    if(texture == NULL){
        printf("SDL_CreateTextureFromSurface error\n");
        return __LINE__;
    }

    SDL_RenderCopy(ps->pRender, texture, NULL, &rect); // 将帧数纹理复制到渲染器
    SDL_DestroyTexture(texture);
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
    SDL_RenderCopy(ps->pRender, ps->pTexture, NULL, &rect); // 将视频纹理复制到渲染器
    show_frame_count(ps); // 显示帧数
    SDL_RenderPresent(ps->pRender); // 渲染画面

    if (ps->isPause == false) {
        ps->renderFrameCount++;
        vFrame->isRendered = true;
        av_frame_unref(frame);
    }

    av_freep(&pixels[0]);
    return 0;
}

int init_filters(playerState *ps, const char *filters_descr)
{
    char args[512];
    int ret = 0;
    const AVFilter *buffersrc  = avfilter_get_by_name("buffer");
    const AVFilter *buffersink = avfilter_get_by_name("buffersink");
    AVFilterInOut *outputs = avfilter_inout_alloc();
    AVFilterInOut *inputs  = avfilter_inout_alloc();
    AVRational time_base = ps->pFormatCtx->streams[ps->videoIndex]->time_base;
    enum AVPixelFormat pix_fmts[] = { AV_PIX_FMT_YUV420P, AV_PIX_FMT_NONE };

    ps->pfilterGraph = avfilter_graph_alloc();
    if (!outputs || !inputs || !ps->pfilterGraph) {
        ret = AVERROR(ENOMEM);
        goto end;
    }

    /* buffer video source: the decoded frames from the decoder will be inserted here. */
    snprintf(args, sizeof(args),
            "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d",
            ps->pCodecCtx->width, ps->pCodecCtx->height, ps->pCodecCtx->pix_fmt,
            time_base.num, time_base.den,
            ps->pCodecCtx->sample_aspect_ratio.num, ps->pCodecCtx->sample_aspect_ratio.den);

    ret = avfilter_graph_create_filter(&ps->pBuffersrcCtx, buffersrc, "in",
                                       args, NULL, ps->pfilterGraph);
    if (ret < 0) {
        printf("Cannot create buffer source\n");
        goto end;
    }

    /* buffer video sink: to terminate the filter chain. */
    ret = avfilter_graph_create_filter(&ps->pBuffersinkCtx, buffersink, "out",
                                       NULL, NULL, ps->pfilterGraph);
    if (ret < 0) {
        printf("Cannot create buffer sink\n");
        goto end;
    }

    ret = av_opt_set_int_list(ps->pBuffersinkCtx, "pix_fmts", pix_fmts,
                              AV_PIX_FMT_NONE, AV_OPT_SEARCH_CHILDREN);
    if (ret < 0) {
        printf("Cannot set output pixel format\n");
        goto end;
    }

    /*
     * Set the endpoints for the filter graph. The ps->pfilterGraph will
     * be linked to the graph described by filters_descr.
     */

    /*
     * The buffer source output must be connected to the input pad of
     * the first filter described by filters_descr; since the first
     * filter input label is not specified, it is set to "in" by
     * default.
     */
    outputs->name       = av_strdup("in");
    outputs->filter_ctx = ps->pBuffersrcCtx;
    outputs->pad_idx    = 0;
    outputs->next       = NULL;

    /*
     * The buffer sink input must be connected to the output pad of
     * the last filter described by filters_descr; since the last
     * filter output label is not specified, it is set to "out" by
     * default.
     */
    inputs->name       = av_strdup("out");
    inputs->filter_ctx = ps->pBuffersinkCtx;
    inputs->pad_idx    = 0;
    inputs->next       = NULL;

    if ((ret = avfilter_graph_parse_ptr(ps->pfilterGraph, filters_descr,
                                    &inputs, &outputs, NULL)) < 0)
        goto end;

    if ((ret = avfilter_graph_config(ps->pfilterGraph, NULL)) < 0)
        goto end;

end:
    avfilter_inout_free(&inputs);
    avfilter_inout_free(&outputs);

    return ret;
}

// 从文件中读取视频流,解码后发送到队列中
int do_read_video(void *arg) {

    playerState *ps = arg;
    AVPacket *packet = av_packet_alloc();
    AVFrame *frame = av_frame_alloc(); 
    AVFrame *filt_frame = av_frame_alloc();
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

            // 再把帧放到 filter 中，添加文字水印
            frame->pts = frame->best_effort_timestamp;
            if (av_buffersrc_add_frame_flags(ps->pBuffersrcCtx, frame, AV_BUFFERSRC_FLAG_KEEP_REF) < 0) {
                printf("Error while feeding the filtergraph\n");
            }

            while (1) {
                // 从 filter 中读取帧数据
                int ret = av_buffersink_get_frame(ps->pBuffersinkCtx, filt_frame);
                if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
                    break;
                if (ret < 0) {
                    printf("av_buffersink_get_frame error:%d\n", ret);
                    break;
                }
                
                // 复制帧数据        
                av_frame_move_ref(ps->videoBuff[queueIndex].frame, filt_frame);
                ps->videoBuff[queueIndex].isRendered = false;
                av_frame_unref(filt_frame);

                // 写入环形队列
                if (rqueue_write(ps->pQueue, &(ps->videoBuff[queueIndex])) != 0) {
                    printf("rqueue_write error %d:%d\n", frameCount, queueIndex);
                    return __LINE__;
                }
            }


            frameCount++;
            queueIndex++;
            if (queueIndex == QUEUE_BUFF_LEN)
                queueIndex = 0;

            av_frame_unref(frame);
        }

        if (readEnd) {
            break;
        }
    }

    av_packet_free(&packet);
    av_frame_free(&frame);
    av_frame_free(&filt_frame);
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

    avfilter_graph_free(&ps->pfilterGraph);

    SDL_DestroyRenderer(ps->pRender);
    SDL_DestroyWindow(ps->pWindow);

    rqueue_destroy(ps->pQueue);
    SDL_Quit();
    exit(0);
}

void free_pause_frame(playerState *ps) {
    if (ps->pPauseFrame != NULL) {
        ps->renderFrameCount++;
        ps->pPauseFrame->isRendered = true;
        av_frame_unref(ps->pPauseFrame->frame);
        ps->pPauseFrame = NULL;
    }
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
                        ps->isNext = false;
                        printf("space key kickdown isPause:%d\n", ps->isPause);
                        break;
                    case SDLK_RIGHT:
                        if (ps->isPause) {
                            ps->isNext = true; // 暂停状态下才能逐帧播放
                            printf("right key kickdown isNext:%d\n", ps->isNext);
                        }
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

                if (ps->isNext == true) {
                    free_pause_frame(ps);
                    ps->isNext = false;
                }

                if (ps->pPauseFrame == NULL) {
                    ps->pPauseFrame = rqueue_read(ps->pQueue);
                }

                render_video_frame(ps, ps->pPauseFrame);
                SDL_Delay(50);
            } else {
                free_pause_frame(ps);
                render_video_frame(ps, rqueue_read(ps->pQueue));
            }
        } else {
            free_pause_frame(ps);
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
    ps->pFont = NULL;

    ps->pfilterGraph = NULL;
    ps->pBuffersinkCtx = NULL;
    ps->pBuffersrcCtx = NULL;

    ps->readThread = NULL;

    ps->videoIndex = 0;
    ps->windowWidth = 1080; // 默认窗口大小
    ps->windowHeight = 720;

    ps->pFileName = NULL;
    ps->isQuit = false;
    ps->isPause = false;
    ps->isNext = false;
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

// av_seek_frame 
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

    if (init_filters(&ps, FILTER_DESCR) != 0) {
        printf("init_filters error\n");
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
