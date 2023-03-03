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
#include <libswresample/swresample.h>
#include <libavutil/time.h>

#include "rqueue.h"

#define QUEUE_BUFF_LEN (16)
#define FILTER_DESCR "drawtext=fontfile=/usr/share/fonts/truetype/freefont/FreeSerif.ttf:fontsize=60:text='alpha':x=w-text_w-20:y=20"


typedef struct AudioParams {
    int freq;
    AVChannelLayout ch_layout;
    enum AVSampleFormat fmt;
    int frame_size;
    int bytes_per_sec;
} AudioParams;

typedef struct playerState {
    SDL_Window *pWindow;
    SDL_Renderer *pRender;
    SDL_Texture *pTexture;
    TTF_Font *pFont;

    AVFormatContext *pFormatCtx;
    AVCodecContext *pVideoCodecCtx;
    AVCodecContext *pAudioCodecCtx;

    struct SwsContext *pImgConvertCtx;
    
    AVFilterGraph *pfilterGraph;
    AVFilterContext *pBuffersinkCtx;
    AVFilterContext *pBuffersrcCtx;

    SDL_Thread *readThread;
    SDL_Thread *videoThread;
    SDL_Thread *audioThread;

    rqueue_t *packetVideoQueue;
    rqueue_t *frameVideoQueue;
    rqueue_t *packetAudioQueue;
    rqueue_t *frameAudioQueue;

    int videoIndex;
    int audioIndex;
    SDL_AudioDeviceID audioDeviceId;
    struct AudioParams audioTarget;
    struct SwrContext *swr_ctx; // 音频滤镜

    int windowWidth;
    int windowHeight;
    char *pFileName;
    bool isQuit;

    bool isPause;
    bool isNext;
    AVFrame *pPauseFrame;

    int renderFrameCount;

} playerState;


static void sdl_audio_callback(void *arg, unsigned char *stream, int len);


int init_ffmpeg(playerState *ps) {
    
    printf("init ffmpeg...\n");

    //初始化FormatContext
    ps->pFormatCtx = avformat_alloc_context();
    if (!ps->pFormatCtx) {
        printf("alloc format context error!\n");
        return __LINE__;
    }

    // 打开视频文件
    int err = avformat_open_input(&ps->pFormatCtx, ps->pFileName, NULL, NULL);
    if (err < 0) {
        printf("avformat_open_input error!\n");
        return __LINE__;
    }

    //读取媒体文件信息
    err = avformat_find_stream_info(ps->pFormatCtx, NULL);
    if (err != 0) {
        printf("[error] find stream error!");
        return __LINE__;
    }

    // 输出视频文件信息
    av_dump_format(ps->pFormatCtx, 0, ps->pFileName, 0);
    
    //寻找到视频流的下标
    ps->videoIndex = av_find_best_stream(ps->pFormatCtx, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    ps->audioIndex = av_find_best_stream(ps->pFormatCtx, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);


    //视频初始化pCodecCtx
    ps->pVideoCodecCtx = avcodec_alloc_context3(NULL);
    if (!ps->pVideoCodecCtx) {
        printf("avcodec_alloc_context3 error!");
        return __LINE__;
    }

    //将视频流的的编解码信息拷贝到pCodecCtx中
    err = avcodec_parameters_to_context(ps->pVideoCodecCtx, ps->pFormatCtx->streams[ps->videoIndex]->codecpar);
    if (err != 0) {
        printf("avcodec_parameters_to_context error!");
        return __LINE__;
    }

    //查找解码器
    const AVCodec *pCodec = avcodec_find_decoder(ps->pVideoCodecCtx->codec_id);
    if (!pCodec) {
        printf("avcodec_find_decoder error!");
        return __LINE__;
    }

    //打开解码器
    err = avcodec_open2(ps->pVideoCodecCtx, pCodec, NULL);
    if (err != 0) {
        printf("avcodec_open2 error!");
        return __LINE__;
    }


     // 音频初始化
    ps->pAudioCodecCtx = avcodec_alloc_context3(NULL);
    if (!ps->pAudioCodecCtx) {
        printf("avcodec_alloc_context3 error!");
        return __LINE__;
    }

    err = avcodec_parameters_to_context(ps->pAudioCodecCtx, ps->pFormatCtx->streams[ps->audioIndex]->codecpar);
    if (err != 0) {
        printf("avcodec_parameters_to_context error!");
        return __LINE__;
    }

    //查找解码器
    pCodec = avcodec_find_decoder(ps->pAudioCodecCtx->codec_id);
    if (!pCodec) {
        printf("avcodec_find_decoder error!");
        return __LINE__;
    }

    //打开解码器
    err = avcodec_open2(ps->pAudioCodecCtx, pCodec, NULL);
    if (err != 0) {
        printf("avcodec_open2 error!");
        return __LINE__;
    }

    return 0;
}


int init_sdl(playerState *ps) {
    printf("init sdl...\n");

    if (!SDL_getenv("SDL_AUDIO_ALSA_SET_BUFFER_SIZE"))
        SDL_setenv("SDL_AUDIO_ALSA_SET_BUFFER_SIZE","1", 1);

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER) < 0) {
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

    SDL_AudioSpec wanted_spec, hw_spec;
    wanted_spec.freq = 48000;
    wanted_spec.format = AUDIO_S16LSB;
    wanted_spec.channels = 2;
    wanted_spec.silence = 0;
    wanted_spec.samples = 2048;
    wanted_spec.callback = sdl_audio_callback;
    wanted_spec.userdata = ps;

    if ((ps->audioDeviceId = SDL_OpenAudioDevice(NULL, 0, &wanted_spec, &hw_spec, SDL_AUDIO_ALLOW_ANY_CHANGE)) < 2){
        printf("open audio device failed:%s\n", SDL_GetError());
        return __LINE__;
    }
    
    ps->audioTarget.fmt = AV_SAMPLE_FMT_S16;
    ps->audioTarget.freq = hw_spec.freq;
    av_channel_layout_default(&(ps->audioTarget.ch_layout), hw_spec.channels);

    ps->audioTarget.frame_size = av_samples_get_buffer_size(NULL, ps->audioTarget.ch_layout.nb_channels, 1, ps->audioTarget.fmt, 1);
    ps->audioTarget.bytes_per_sec = av_samples_get_buffer_size(NULL, ps->audioTarget.ch_layout.nb_channels, ps->audioTarget.freq, ps->audioTarget.fmt, 1);

    printf("fmt:%d freq:%d channels:%d frame_size:%d bytes_per_sec:%d audioDeviceId:%u status:%d\n", 
        hw_spec.format, hw_spec.freq, ps->audioTarget.ch_layout.nb_channels,
        ps->audioTarget.frame_size, 
        ps->audioTarget.bytes_per_sec,
        ps->audioDeviceId, SDL_GetAudioDeviceStatus(ps->audioDeviceId));

    // 创建窗口
    ps->pWindow = SDL_CreateWindow(
            "video player",
            0, 0,
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


void get_duration(playerState *ps, char *str , int len) {
    if (ps->pFormatCtx->duration != AV_NOPTS_VALUE && str != NULL && len != 0) {
        int64_t mins, secs, us;
        int64_t duration = ps->pFormatCtx->duration + (ps->pFormatCtx->duration <= INT64_MAX - 5000 ? 5000 : 0);
        secs  = duration / AV_TIME_BASE;
        us    = duration % AV_TIME_BASE;
        mins  = secs / 60;
        secs %= 60;
        snprintf(str, len, "%02"PRId64":%02"PRId64".%02"PRId64"", mins, secs,
                (100 * us) / AV_TIME_BASE);
    }
}

void get_video_current_time(playerState *ps, AVFrame *frame, char *str , int len) {
    float secs = frame->pts * av_q2d(ps->pFormatCtx->streams[ps->videoIndex]->time_base);
    int32_t mins  = secs / 60;
    secs = secs - mins*60;

    snprintf(str, len, "%02d:%02.2f", mins, secs);
}

int show_time(playerState *ps, AVFrame *frame) {
    
    char duration[16] = "";
    get_duration(ps, duration, sizeof(duration));

    char current_time[16] = "";
    get_video_current_time(ps, frame, current_time, sizeof(current_time));

    char str[32] = "";
    snprintf(str, sizeof(str), "%s/%s", current_time, duration);
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

int show_slidbar(playerState *ps, AVFrame *frame) {

    SDL_Rect rect;
    rect.x = 0;
    rect.y = ps->windowHeight-10;
    rect.w = ps->windowWidth;
    rect.h = 10;

    SDL_Color bg_color = {255, 255, 255, 255};
    SDL_SetRenderDrawColor(ps->pRender, bg_color.r, bg_color.g, bg_color.b, bg_color.a);
    SDL_RenderFillRect(ps->pRender, &rect);

    SDL_Color board_color = {0, 0, 0, 255};
    SDL_SetRenderDrawColor(ps->pRender, board_color.r, board_color.g, board_color.b, board_color.a);
    SDL_RenderDrawRect(ps->pRender, &rect);

    rect.w = (frame->pts * av_q2d(ps->pFormatCtx->streams[ps->videoIndex]->time_base)) / (ps->pFormatCtx->duration/AV_TIME_BASE) * (ps->windowWidth);
    SDL_Color button_color = {50, 50, 50, 255};
    SDL_SetRenderDrawColor(ps->pRender, button_color.r, button_color.g, button_color.b, button_color.a);
    SDL_RenderFillRect(ps->pRender, &rect);
    return 0;
}

/** 在窗口中绘制一帧画面 **/
int render_video_frame(playerState *ps, AVFrame *frame) {
    if (frame == NULL) {
        printf("frame is null error\n");
        return __LINE__;
    }

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
    show_time(ps, frame); // 显示播放时间
    show_slidbar(ps, frame); // 展示进度条

    SDL_RenderPresent(ps->pRender); // 渲染画面

    if (ps->isPause == false) {
        ps->renderFrameCount++;
        av_frame_unref(frame);
        av_frame_free(&frame);
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
            ps->pVideoCodecCtx->width, ps->pVideoCodecCtx->height, ps->pVideoCodecCtx->pix_fmt,
            time_base.num, time_base.den,
            ps->pVideoCodecCtx->sample_aspect_ratio.num, ps->pVideoCodecCtx->sample_aspect_ratio.den);

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

int decode_video(void *arg) {
    
    playerState *ps = arg;
    int ret = 0;
    int video_frame_count = 0;
    AVFrame *frame = av_frame_alloc(); 

    while (ps->isQuit == false) {

        AVPacket *packet = rqueue_read(ps->packetVideoQueue);
        if (packet == NULL) {
            //printf("rqueue_read packetVideoQueue error\n");
            SDL_Delay(100);
            continue;
        }

        // 发送给解码器
        ret = avcodec_send_packet(ps->pVideoCodecCtx, packet);
        if (ret < 0) {
            printf("avcodec_send_packet video packet error\n");
        } else {
            av_packet_unref(packet);
            av_packet_free(&packet);
        }

        // 从解码器中循环读取帧数据,直到读取失败
        while (avcodec_receive_frame(ps->pVideoCodecCtx, frame) >= 0) {

            // printf("Frame %c (%d) pts %d dts %d key_frame %d pkt_size %d\n",
            //     av_get_picture_type_char(frame->pict_type),
            //     ps->pVideoCodecCtx->frame_number,
            //     frame->pts,
            //     frame->pkt_dts,
            //     frame->key_frame,
            //     frame->pkt_size);

            // 再把帧放到 filter 中，添加文字水印
            frame->pts = frame->best_effort_timestamp;
            if (av_buffersrc_add_frame_flags(ps->pBuffersrcCtx, frame, AV_BUFFERSRC_FLAG_KEEP_REF) < 0) {
                printf("Error while feeding the filtergraph\n");
            }

            while (1) {
                // 从 filter 中读取帧数据
                AVFrame *filt_frame = av_frame_alloc();
                ret = av_buffersink_get_frame(ps->pBuffersinkCtx, filt_frame);
                if (ret < 0) {
                    av_frame_free(&filt_frame);
                    //printf("av_buffersink_get_frame error:%d\n", ret);
                    break;
                }

                video_frame_count++;
                // 写入环形队列
                do 
                {
                    ret = rqueue_write(ps->frameVideoQueue, filt_frame);
                    if (ret != 0) {
                        SDL_Delay(100);
                        //printf("rqueue_write frameVideoQueue error %d:%d\n", ret, video_frame_count);
                    }
                } while (ret != 0 && ps->isQuit == false);
            }

            av_frame_unref(frame);
        }
    }
    
    av_frame_free(&frame);
    printf("decode_video done %d\n", video_frame_count);
    return 0;
}

static void sdl_audio_callback(void *arg, unsigned char *stream, int len) {
    playerState *ps = arg;
    int data_size = 0;
    int ret = 0;

    SDL_memset(stream, 0, len);
    int64_t audio_callback_time = av_gettime_relative();
    printf("\n sdl_audio_callback %ld\n", audio_callback_time);

    while (len > 0) {
        
        AVFrame *frame = rqueue_read(ps->frameAudioQueue);
        if (frame != NULL) {

            printf("frame fmt:%d freq:%d channels:%d\n", 
                frame->format, frame->sample_rate, frame->ch_layout.nb_channels);
            
            if (ps->swr_ctx == NULL) {
                ps->swr_ctx = swr_alloc();
                ret = swr_alloc_set_opts2(&(ps->swr_ctx), &ps->audioTarget.ch_layout, ps->audioTarget.fmt, ps->audioTarget.freq,
                                                    &frame->ch_layout, frame->format, frame->sample_rate, 0, NULL);
                if (ret != 0 || swr_init(ps->swr_ctx) < 0) {
                    printf("swr_init error len\n");
                    ps->swr_ctx == NULL;
                }
            }

            if (ps->swr_ctx) {

                int out_count = (int64_t)frame->nb_samples * ps->audioTarget.freq / frame->sample_rate + 256;;
                int out_size  = av_samples_get_buffer_size(NULL, ps->audioTarget.ch_layout.nb_channels, out_count, ps->audioTarget.fmt, 0);
                printf("out_size=%d out_count=%d len=%d\n", out_size, out_count, len);
                
                uint8_t *out = av_malloc(out_size);
                const uint8_t **in = (const uint8_t **)frame->extended_data;

                memset(out, 0, out_size);
                data_size = swr_convert(ps->swr_ctx, &out, out_count, in, frame->nb_samples);
                if (data_size < 0) {
                    printf("swr_convert() failed\n");
                }
                printf("out_size=%d out_count=%d len=%d data_size=%d\n", out_size, out_count, len, data_size);

                SDL_MixAudioFormat(stream, out, AUDIO_S16SYS, data_size, SDL_MIX_MAXVOLUME);
                stream += data_size;
                len = len - data_size;

                av_freep(&out);
            }

            av_frame_unref(frame);
            av_frame_free(&frame);
            printf("audio len:%d %d\n", len, data_size);

        } else {
            SDL_Delay(100);
        }
    }
}

int decode_audio(void *arg) {
    
    playerState *ps = arg;
    int ret = 0;
    int audio_frame_count = 0;
    SDL_PauseAudioDevice(ps->audioDeviceId, 0);

    while (ps->isQuit == false) {

        AVPacket *packet = rqueue_read(ps->packetAudioQueue);
        if (packet == NULL) {
            //printf("rqueue_read packetAudioQueue error\n");
            SDL_Delay(100);
            continue;
        }

        // 发送给解码器
        ret = avcodec_send_packet(ps->pAudioCodecCtx, packet);
        if (ret < 0) {
            //printf("avcodec_send_packet audio packet error\n");
        } else {
            av_packet_unref(packet);
            av_packet_free(&packet);
        }

        // 从解码器中循环读取帧数据,直到读取失败
        while (1) {
            AVFrame *frame = av_frame_alloc(); 
            ret = avcodec_receive_frame(ps->pAudioCodecCtx, frame);
            if (ret < 0) {
                    av_frame_free(&frame);
                    //printf("avcodec_receive_frame audio error:%d\n", ret);
                    break;
            }
            audio_frame_count++;

            // 写入环形队列
            do 
            {
                ret = rqueue_write(ps->frameAudioQueue, frame);
                if (ret != 0) {
                    SDL_Delay(100);
                    //printf("rqueue_write frameAudioQueue error %d:%d\n", ret, audio_frame_count);
                }
            } while (ret != 0 && ps->isQuit == false);
        }
    }
    
    printf("decode_audio done %d\n", audio_frame_count);
    return 0;
}


// 从文件中读取视频流,解码后发送到队列中
int do_read_file(void *arg) {

    playerState *ps = arg;
    int ret = 0;
    int video_packet_count = 0;
    int audio_packet_count = 0;

    while (ps->isQuit == false) {

        // av_read_frame 从文件中读取一帧未解码的数据
        AVPacket *packet = av_packet_alloc();
        if (av_read_frame(ps->pFormatCtx, packet) >= 0) {
            // 如果是视频数据
            if (packet->stream_index == ps->videoIndex) {
                video_packet_count++;
                // 写入环形队列
                do 
                {
                    ret = rqueue_write(ps->packetVideoQueue, packet);
                    if (ret != 0) {
                        SDL_Delay(100);
                        //printf("rqueue_write packetVideoQueue error %d:%d\n", ret, video_packet_count);
                    }
                } while (ret != 0 && ps->isQuit == false);
            } else if (packet->stream_index == ps->audioIndex) {
                audio_packet_count++;
                // 写入环形队列
                do 
                {
                    ret = rqueue_write(ps->packetAudioQueue, packet);
                    if (ret != 0) {
                        SDL_Delay(100);
                        //printf("rqueue_write packetAudioQueue error %d:%d\n", ret, video_packet_count);
                    }
                } while (ret != 0 && ps->isQuit == false);
            }
        } else {
            // 文件读完
            av_packet_free(&packet);
            SDL_Delay(100);
        }
    }

    printf("read file done %d %d\n", video_packet_count, audio_packet_count);
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

    ps->isQuit = true;
    if (ps->readThread)
        SDL_WaitThread(ps->readThread, NULL);
    if (ps->videoThread)
        SDL_WaitThread(ps->videoThread, NULL);
    if (ps->audioThread)
        SDL_WaitThread(ps->audioThread, NULL);
    if (ps->pTexture)
        SDL_DestroyTexture(ps->pTexture);
    if (ps->pImgConvertCtx)
        sws_freeContext(ps->pImgConvertCtx);
    if (ps->pRender)
        SDL_DestroyRenderer(ps->pRender);
    if (ps->swr_ctx)
        swr_free(&ps->swr_ctx);

    avfilter_graph_free(&ps->pfilterGraph);

    SDL_CloseAudioDevice(ps->audioDeviceId);
    SDL_DestroyWindow(ps->pWindow);
    SDL_Quit();

    rqueue_destroy(ps->packetVideoQueue);
    rqueue_destroy(ps->frameVideoQueue);
    rqueue_destroy(ps->packetAudioQueue);
    rqueue_destroy(ps->frameAudioQueue);
    printf("render frame count: %d\nbye...\n", ps->renderFrameCount);

    exit(0);
}

void free_pause_frame(playerState *ps) {
    if (ps->pPauseFrame != NULL) {
        av_frame_unref(ps->pPauseFrame);
        av_frame_free(&(ps->pPauseFrame));
        ps->pPauseFrame = NULL;
    }
}

void event_loop(playerState *ps) {
    SDL_Event event;
    while (1)
    {
        SDL_PumpEvents();
        // 查看键盘，鼠标事件
        while (SDL_PeepEvents(&event, 1, SDL_GETEVENT, SDL_FIRSTEVENT, SDL_LASTEVENT) != 0) {
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
            case SDL_MOUSEBUTTONDOWN:
            {
                SDL_Rect rect;
                rect.x = 0;
                rect.y = ps->windowHeight-50;
                rect.w = ps->windowWidth;
                rect.h = 50;

                SDL_Point pot;
                pot.x = event.motion.x;
                pot.y = event.motion.y;
                printf("x:%d y:%d rect x y w h %d:%d:%d:%d\n", pot.x, pot.y, rect.x, rect.y, rect.w, rect.h);

                if (SDL_PointInRect(&pot, &rect) && ps->isPause == false) {

                    AVRational ratio = ps->pFormatCtx->streams[ps->videoIndex]->time_base;
                    int64_t seekPoint = (pot.x * ps->pFormatCtx->duration * ratio.den) / (ps->windowWidth * AV_TIME_BASE * ratio.num);
                    
                    printf("x:%d y:%d seekPoint:%ld\n", pot.x, pot.y, seekPoint);
                    if (av_seek_frame(ps->pFormatCtx, ps->videoIndex, seekPoint, AVSEEK_FLAG_BACKWARD) < 0) {
                        printf("av_seek_frame error\n");
                    }
                }
                break;
            }
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

        AVFrame *render_frame = NULL;
        if (rqueue_isempty(ps->frameVideoQueue)) {
            free_pause_frame(ps);
            SDL_Delay(100);
            //printf("frameVideoQueue is empty!\n");
        } else {
            if (ps->isPause == true) {

                if (ps->isNext == true) {
                    free_pause_frame(ps);
                    ps->isNext = false;
                }

                if (ps->pPauseFrame == NULL) {
                    ps->pPauseFrame = rqueue_read(ps->frameVideoQueue);
                }

                render_frame = ps->pPauseFrame;
            } else {
                free_pause_frame(ps);
                render_frame = rqueue_read(ps->frameVideoQueue);
            }
        }

        if (render_frame != NULL) {
            render_video_frame(ps, render_frame);
            SDL_Delay(10);
        }
    }
}

int init_playerState(playerState *ps) {

    ps->pWindow = NULL;
    ps->pRender = NULL;
    ps->pTexture = NULL;

    ps->pFormatCtx = NULL;
    ps->pVideoCodecCtx = NULL;
    ps->pImgConvertCtx = NULL;
    ps->pFont = NULL;

    ps->pfilterGraph = NULL;
    ps->pBuffersinkCtx = NULL;
    ps->pBuffersrcCtx = NULL;

    ps->readThread = NULL;
    ps->videoThread = NULL;
    ps->audioThread = NULL;

    ps->videoIndex = 0;
    ps->audioIndex = 0;
    ps->swr_ctx == NULL;

    ps->windowWidth = 1080; // 默认窗口大小
    ps->windowHeight = 720;

    ps->pFileName = NULL;
    ps->isQuit = false;
    ps->isPause = false;
    ps->isNext = false;
    ps->pPauseFrame = NULL;

    ps->renderFrameCount = 0;

    ps->packetVideoQueue = rqueue_create(QUEUE_BUFF_LEN, RQUEUE_MODE_BLOCKING);
    if (ps->packetVideoQueue == NULL) {
        printf("rqueue_create error\n");
        return __LINE__;
    }

    ps->frameVideoQueue = rqueue_create(QUEUE_BUFF_LEN, RQUEUE_MODE_BLOCKING);
    if (ps->frameVideoQueue == NULL) {
        printf("rqueue_create error\n");
        return __LINE__;
    }

    ps->packetAudioQueue = rqueue_create(QUEUE_BUFF_LEN, RQUEUE_MODE_BLOCKING);
    if (ps->packetAudioQueue == NULL) {
        printf("rqueue_create error\n");
        return __LINE__;
    }

    ps->frameAudioQueue = rqueue_create(QUEUE_BUFF_LEN, RQUEUE_MODE_BLOCKING);
    if (ps->frameAudioQueue == NULL) {
        printf("rqueue_create error\n");
        return __LINE__;
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

    ps.readThread = SDL_CreateThread(do_read_file, "read_thread", &ps);
    if (!ps.readThread) {
        printf("SDL_CreateThread(): %s\n", SDL_GetError());
        return __LINE__;
    }


    ps.videoThread = SDL_CreateThread(decode_video, "decode_video", &ps);
    if (!ps.videoThread) {
        printf("SDL_CreateThread(): %s\n", SDL_GetError());
        return __LINE__;
    }

    ps.audioThread = SDL_CreateThread(decode_audio, "decode_audio", &ps);
    if (!ps.audioThread) {
        printf("SDL_CreateThread(): %s\n", SDL_GetError());
        return __LINE__;
    }

    printf("playing...\n");
    event_loop(&ps);
    return 0;
}
