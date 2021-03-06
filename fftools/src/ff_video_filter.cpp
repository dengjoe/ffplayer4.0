
#include "stdafx.h"

#define ENABLE_SDL 1
#define ENABLE_YUVFILE 1


#include "libavcodec/avcodec.h"
#include "libavformat/avformat.h"
#include "libavfilter/avfiltergraph.h"
#include "libavfilter/avcodec.h"
#include "libavfilter/buffersink.h"
#include "libavfilter/buffersrc.h"
#include "libavutil/avutil.h"
#include "libswscale/swscale.h"
	//SDL
#include "sdl/SDL.h"
#include "sdl/SDL_thread.h"


const char *filter_descr = "movie=my_logo.png[wm];[in][wm]overlay=5:5[out]";

static AVFormatContext *pFormatCtx;
static AVCodecContext *pCodecCtx;
AVFilterContext *buffersink_ctx;
AVFilterContext *buffersrc_ctx;
AVFilterGraph *filter_graph;
static int video_stream_index = -1;
static int64_t last_pts = AV_NOPTS_VALUE;



static int open_input_file(const char *filename)
{
    int ret;
    AVCodec *dec;

    if ((ret = avformat_open_input(&pFormatCtx, filename, NULL, NULL)) < 0) {
        av_log(NULL, AV_LOG_ERROR, "Cannot open input file\n");
        return ret;
    }

    if ((ret = avformat_find_stream_info(pFormatCtx, NULL)) < 0) {
        av_log(NULL, AV_LOG_ERROR, "Cannot find stream information\n");
        return ret;
    }

    /* select the video stream */
    ret = av_find_best_stream(pFormatCtx, AVMEDIA_TYPE_VIDEO, -1, -1, &dec, 0);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "Cannot find a video stream in the input file\n");
        return ret;
    }
    video_stream_index = ret;
    pCodecCtx = pFormatCtx->streams[video_stream_index]->codec;

    /* init the video decoder */
    if ((ret = avcodec_open2(pCodecCtx, dec, NULL)) < 0) {
        av_log(NULL, AV_LOG_ERROR, "Cannot open video decoder\n");
        return ret;
    }

    return 0;
}

static int init_filters(const char *filters_descr)
{
    char args[512];
    int ret;
    AVFilter *buffersrc  = avfilter_get_by_name("buffer");
    AVFilter *buffersink = avfilter_get_by_name("ffbuffersink");
	
    AVFilterInOut *outputs = avfilter_inout_alloc();
    AVFilterInOut *inputs  = avfilter_inout_alloc();
	
    enum PixelFormat pix_fmts[] = { PIX_FMT_GRAY8, PIX_FMT_NONE };
    AVBufferSinkParams *buffersink_params;

    filter_graph = avfilter_graph_alloc();

    /* buffer video source: the decoded frames from the decoder will be inserted here. */
    _snprintf(args, sizeof(args),
            "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d",
            pCodecCtx->width, pCodecCtx->height, pCodecCtx->pix_fmt,
            pCodecCtx->time_base.num, pCodecCtx->time_base.den,
            pCodecCtx->sample_aspect_ratio.num, pCodecCtx->sample_aspect_ratio.den);

    ret = avfilter_graph_create_filter(&buffersrc_ctx, buffersrc, "in",
                                       args, NULL, filter_graph);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "Cannot create buffer source\n");
        return ret;
    }

    /* buffer video sink: to terminate the filter chain. */
    buffersink_params = av_buffersink_params_alloc();
    buffersink_params->pixel_fmts = pix_fmts;
    ret = avfilter_graph_create_filter(&buffersink_ctx, buffersink, "out",
                                       NULL, buffersink_params, filter_graph);
    av_free(buffersink_params);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "Cannot create buffer sink\n");
        return ret;
    }

    /* Endpoints for the filter graph. */
    outputs->name       = av_strdup("in");
    outputs->filter_ctx = buffersrc_ctx;
    outputs->pad_idx    = 0;
    outputs->next       = NULL;

    inputs->name       = av_strdup("out");
    inputs->filter_ctx = buffersink_ctx;
    inputs->pad_idx    = 0;
    inputs->next       = NULL;

    if ((ret = avfilter_graph_parse(filter_graph, filters_descr,
                                    &inputs, &outputs, NULL)) < 0)
        return ret;

    if ((ret = avfilter_graph_config(filter_graph, NULL)) < 0)
        return ret;
    return 0;
}


int _tmain(int argc, _TCHAR* argv[])
{
    int ret;
    AVPacket packet;
    AVFrame frame;
    int got_frame;

    if (argc != 2) {
        fprintf(stderr, "Usage: %s file\n", argv[0]);
        return -1;
    }

    avcodec_register_all();
    av_register_all();
    avfilter_register_all();

    if ((ret = open_input_file(argv[1])) < 0)
        goto end;
    if ((ret = init_filters(filter_descr)) < 0)
        goto end;
#if ENABLE_YUVFILE
	FILE *fp_yuv=fopen("test.yuv","wb+");
#endif
#if ENABLE_SDL
	SDL_Surface *screen; 
	SDL_Overlay *bmp; 
	SDL_Rect rect;
	if(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER)) {  
		printf( "Could not initialize SDL - %s\n", SDL_GetError()); 
		return -1;
	} 
	screen = SDL_SetVideoMode(pCodecCtx->width, pCodecCtx->height, 0, 0);
	if(!screen) {  
		printf("SDL: could not set video mode - exiting\n");  
		return -1;
	}
	bmp = SDL_CreateYUVOverlay(pCodecCtx->width, pCodecCtx->height,SDL_YV12_OVERLAY, screen); 
#endif

    /* read all packets */
    while (1) {
        AVFilterBufferRef *picref;
        if ((ret = av_read_frame(pFormatCtx, &packet)) < 0)
            break;

        if (packet.stream_index == video_stream_index) {
            avcodec_get_frame_defaults(&frame);
            got_frame = 0;
            ret = avcodec_decode_video2(pCodecCtx, &frame, &got_frame, &packet);
            if (ret < 0) {
                av_log(NULL, AV_LOG_ERROR, "Error decoding video\n");
                break;
            }

            if (got_frame) {
                frame.pts = av_frame_get_best_effort_timestamp(&frame);
				
                /* push the decoded frame into the filtergraph */
                if (av_buffersrc_add_frame(buffersrc_ctx, &frame) < 0) {
                    av_log(NULL, AV_LOG_ERROR, "Error while feeding the filtergraph\n");
                    break;
                }

                /* pull filtered pictures from the filtergraph */
                while (1) {
                    ret = av_buffersink_get_buffer_ref(buffersink_ctx, &picref, 0);
                    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
                        break;
                    if (ret < 0)
                        goto end;

                    if (picref) {
#if ENABLE_YUVFILE
						int y_size=picref->video->w*picref->video->h;
						fwrite(picref->data[0],1,y_size,fp_yuv);
						fwrite(picref->data[1],1,y_size/4,fp_yuv);
						fwrite(picref->data[2],1,y_size/4,fp_yuv);
#endif
						
#if ENABLE_SDL
						SDL_LockYUVOverlay(bmp);
						bmp->pixels[0]=picref->data[0];
						bmp->pixels[2]=picref->data[1];
						bmp->pixels[1]=picref->data[2];     
						bmp->pitches[0]=picref->linesize[0];
						bmp->pitches[2]=picref->linesize[1];   
						bmp->pitches[1]=picref->linesize[2];
						SDL_UnlockYUVOverlay(bmp); 
						rect.x = 0;    
						rect.y = 0;    
						rect.w = picref->video->w;    
						rect.h = picref->video->h;    
						SDL_DisplayYUVOverlay(bmp, &rect); 
						//Delay 40ms
						SDL_Delay(40);
#endif
                        avfilter_unref_bufferp(&picref);
                    }
                }
            }
        }
        av_free_packet(&packet);
    }
#if ENABLE_YUVFILE
	fclose(fp_yuv);
#endif
end:
    avfilter_graph_free(&filter_graph);
    if (pCodecCtx)
        avcodec_close(pCodecCtx);
    avformat_close_input(&pFormatCtx);

    if (ret < 0 && ret != AVERROR_EOF) {
        char buf[1024];
        av_strerror(ret, buf, sizeof(buf));
        fprintf(stderr, "Error occurred: %s\n", buf);
        return -1;
    }

    return 0;
}


