// SDLPlayer.cpp : 此文件包含 "main" 函数。程序执行将在此处开始并结束。
//
#include "pch.h"
#include <iostream>

extern "C" {
#include "libavcodec/avcodec.h"
#include "libavformat/avformat.h"
#include "libavcodec/avcodec.h"
#include "libswscale/swscale.h"
#include "libavutil/imgutils.h"
#include <libswresample/swresample.h>
#include "SDL.h"
}

#define SFM_REFRESH_EVENT  (SDL_USEREVENT + 1)

#define SFM_BREAK_EVENT  (SDL_USEREVENT + 2)


#define MAX_AUDIO_FRAME_SIZE 192000 

int thread_exit = 0;
int thread_pause = 0;

int sfp_refresh_thread(void *opaque) {
	thread_exit = 0;
	thread_pause = 0;

	while (!thread_exit) {
		if (!thread_pause) {
			SDL_Event event;
			event.type = SFM_REFRESH_EVENT;
			SDL_PushEvent(&event);
		}
		SDL_Delay(40);
	}
	thread_exit = 0;
	thread_pause = 0;
	//Break
	SDL_Event event;
	event.type = SFM_BREAK_EVENT;
	SDL_PushEvent(&event);

	return 0;
}


static  Uint8  *audio_chunk;
static  Uint32  audio_len;
static  Uint8  *audio_pos;


void  fill_audio(void *udata, Uint8 *stream, int len) {
	//SDL 2.0
	SDL_memset(stream, 0, len);
	if (audio_len == 0)
		return;

	len = (len > audio_len ? audio_len : len);	/*  Mix  as  much  data  as  possible  */

	SDL_MixAudio(stream, audio_pos, len, SDL_MIX_MAXVOLUME);
	audio_pos += len;
	audio_len -= len;
}


int main(int argc, char* argv[])
{

	AVFormatContext	*pFormatCtx;
	int				i, videoindex, audioindex;
	AVCodecContext	*videoCodecCtx;
	AVCodecContext  *audioCodecCtx;
	AVCodec			*videoCodec;
	AVCodec			*audioCodec;
	AVFrame	*pFrame, *pFrameYUV;
	unsigned char *out_buffer;
	unsigned char *audio_out_buffer;
	AVPacket *packet;
	int ret, got_picture;

	int64_t in_channel_layout;


	//------------SDL----------------
	int screen_w, screen_h;
	SDL_Window *screen;
	SDL_Renderer* sdlRenderer;
	SDL_Texture* sdlTexture;
	SDL_Rect sdlRect;
	SDL_Thread *video_tid;
	SDL_Event event;

	struct SwsContext *img_convert_ctx;
	struct SwrContext *au_convert_ctx;

	SDL_AudioSpec wanted_spec;
	// 
	//char filepath[] = "pm.mp4";
	//char filepath[] = "http://ivi.bupt.edu.cn/hls/cctv1hd.m3u8";
	char filepath[] = "hhg.mp4";

	// 初始化相关的内容
	av_register_all();
	avformat_network_init();
	pFormatCtx = avformat_alloc_context();

	// 打开视频文件
	if (avformat_open_input(&pFormatCtx, filepath, NULL, NULL) != 0) {
		printf("Couldn't open input stream.\n");
		return -1;
	}

	// 发现流信息，并将流信息的内容给到上下文
	if (avformat_find_stream_info(pFormatCtx, NULL) < 0) {
		printf("Couldn't find stream information.\n");
		return -1;
	}

	// 查找Video的索引值
	videoindex = -1;
	for (i = 0; i < pFormatCtx->nb_streams; i++)
		if (pFormatCtx->streams[i]->codec->codec_type == AVMEDIA_TYPE_VIDEO) {
			videoindex = i;
			break;
		}
	if (videoindex == -1) {
		printf("Didn't find a video stream.\n");
		return -1;
	}


	// Find the first audio stream
	audioindex = -1;
	for (i = 0; i < pFormatCtx->nb_streams; i++)
		if (pFormatCtx->streams[i]->codec->codec_type == AVMEDIA_TYPE_AUDIO) {
			audioindex = i;
			break;
		}

	if (audioindex == -1) {
		printf("Didn't find a audio stream.\n");
		return -1;
	}


	// 获取到这个视频流的Codec上下文
	videoCodecCtx = pFormatCtx->streams[videoindex]->codec;
	audioCodecCtx = pFormatCtx->streams[audioindex]->codec;


	// 通过Codec的上下文获得Codec
	videoCodec = avcodec_find_decoder(videoCodecCtx->codec_id);
	if (videoCodec == NULL) {
		printf("VideoCodec not found.\n");
		return -1;
	}

	audioCodec = avcodec_find_decoder(audioCodecCtx->codec_id);
	if (audioCodec == NULL) {
		printf("AudioCodec not found.\n");
		return -1;
	}

	// 打开这个视频流的Codec
	if (avcodec_open2(videoCodecCtx, videoCodec, NULL) < 0) {
		printf("Could not open videocodec.\n");
		return -1;
	}

	if (avcodec_open2(audioCodecCtx, audioCodec, NULL) < 0) {
		printf("Could not open videocodec.\n");
		return -1;
	}

	pFrame = av_frame_alloc();
	pFrameYUV = av_frame_alloc();

	//Out Audio Param
	uint64_t out_channel_layout = AV_CH_LAYOUT_STEREO;
	//nb_samples: AAC-1024 MP3-1152
	int out_nb_samples = audioCodecCtx->frame_size;
	AVSampleFormat out_sample_fmt = AV_SAMPLE_FMT_S16;
	int out_sample_rate = 44100;
	int out_channels = av_get_channel_layout_nb_channels(out_channel_layout);
	//Out Buffer Size
	int out_buffer_size = av_samples_get_buffer_size(NULL, out_channels, out_nb_samples, out_sample_fmt, 1);

	audio_out_buffer = (uint8_t *)av_malloc(MAX_AUDIO_FRAME_SIZE * 2);
	

	out_buffer = (unsigned char *)av_malloc(av_image_get_buffer_size(AV_PIX_FMT_YUV420P, videoCodecCtx->width, videoCodecCtx->height, 1));

	av_image_fill_arrays(pFrameYUV->data, pFrameYUV->linesize, out_buffer, AV_PIX_FMT_YUV420P, videoCodecCtx->width, videoCodecCtx->height, 1);

	//Output Info-----------------------------
	printf("---------------- File Information ---------------\n");
	av_dump_format(pFormatCtx, 0, filepath, 0);
	printf("-------------------------------------------------\n");

	img_convert_ctx = sws_getContext(videoCodecCtx->width, videoCodecCtx->height, videoCodecCtx->pix_fmt, videoCodecCtx->width, videoCodecCtx->height, AV_PIX_FMT_YUV420P, SWS_BICUBIC, NULL, NULL, NULL);


	if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER)) {
		printf("Could not initialize SDL - %s\n", SDL_GetError());
		return -1;
	}





	//SDL 2.0 Support for multiple windows
	screen_w = videoCodecCtx->width;
	screen_h = videoCodecCtx->height;

	screen = SDL_CreateWindow("Simplest ffmpeg player's Window", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, screen_w, screen_h, SDL_WINDOW_OPENGL);

	if (!screen) {
		printf("SDL: could not create window - exiting:%s\n", SDL_GetError());
		return -1;
	}
	sdlRenderer = SDL_CreateRenderer(screen, -1, 0);
	//IYUV: Y + U + V  (3 planes)
	//YV12: Y + V + U  (3 planes)
	sdlTexture = SDL_CreateTexture(sdlRenderer, SDL_PIXELFORMAT_IYUV, SDL_TEXTUREACCESS_STREAMING, videoCodecCtx->width, videoCodecCtx->height);

	sdlRect.x = 0;
	sdlRect.y = 0;
	sdlRect.w = screen_w;
	sdlRect.h = screen_h;

	packet = (AVPacket *)av_malloc(sizeof(AVPacket));

	video_tid = SDL_CreateThread(sfp_refresh_thread, NULL, NULL);

	//SDL_AudioSpec
	wanted_spec.freq = out_sample_rate;
	wanted_spec.format = AUDIO_S16SYS;
	wanted_spec.channels = out_channels;
	wanted_spec.silence = 0;
	wanted_spec.samples = out_nb_samples;
	wanted_spec.callback = fill_audio;
	wanted_spec.userdata = audioCodecCtx;

	if (SDL_OpenAudio(&wanted_spec, NULL) < 0) {
		printf("can't open audio.\n");
		return -1;
	}

	//FIX:Some Codec's Context Information is missing
	in_channel_layout = av_get_default_channel_layout(audioCodecCtx->channels);

	au_convert_ctx = swr_alloc();
	au_convert_ctx = swr_alloc_set_opts(au_convert_ctx, out_channel_layout, out_sample_fmt, out_sample_rate,
		in_channel_layout, audioCodecCtx->sample_fmt, audioCodecCtx->sample_rate, 0, NULL);
	swr_init(au_convert_ctx);

	//Play
	SDL_PauseAudio(0);
	
	//------------SDL End------------
	//Event Loop

	for (;;) {
		//Wait
		SDL_WaitEvent(&event);
		if (event.type == SFM_REFRESH_EVENT) {
			int isVideo = 1;
			while (1) {
				if (av_read_frame(pFormatCtx, packet) < 0)
					thread_exit = 1;

				if (packet->stream_index == videoindex) {
					isVideo = 1;
					break;
				}

				if (packet->stream_index == audioindex) {
					isVideo = 0;
					break;
				}
			}

			if (isVideo == 1) {
				ret = avcodec_decode_video2(videoCodecCtx, pFrame, &got_picture, packet);
				if (ret < 0) {
					printf("Decode Error.\n");
					return -1;
				}
				if (got_picture) {
					sws_scale(img_convert_ctx, (const unsigned char* const*)pFrame->data, pFrame->linesize, 0, videoCodecCtx->height, pFrameYUV->data, pFrameYUV->linesize);
					//SDL---------------------------
					SDL_UpdateTexture(sdlTexture, NULL, pFrameYUV->data[0], pFrameYUV->linesize[0]);
					SDL_RenderClear(sdlRenderer);
					//SDL_RenderCopy( sdlRenderer, sdlTexture, &sdlRect, &sdlRect );  
					SDL_RenderCopy(sdlRenderer, sdlTexture, NULL, NULL);
					SDL_RenderPresent(sdlRenderer);
					//SDL End-----------------------
				}
				av_free_packet(packet);
			}
			else {
				ret = avcodec_decode_audio4(audioCodecCtx, pFrame, &got_picture, packet);
				if (ret < 0) {
					printf("Error in decoding audio frame.\n");
					return -1;
				}
				if (got_picture > 0) {
					swr_convert(au_convert_ctx, &out_buffer, MAX_AUDIO_FRAME_SIZE, (const uint8_t **)pFrame->data, pFrame->nb_samples);
				}

				while (audio_len > 0)//Wait until finish
					SDL_Delay(1);

				//Set audio buffer (PCM data)
				audio_chunk = (Uint8 *)out_buffer;
				//Audio buffer length
				audio_len = out_buffer_size;
				audio_pos = audio_chunk;
			}

			
		}
		else if (event.type == SDL_KEYDOWN) {
			//Pause
			if (event.key.keysym.sym == SDLK_SPACE)
				thread_pause = !thread_pause;
		}
		else if (event.type == SDL_QUIT) {
			thread_exit = 1;
		}
		else if (event.type == SFM_BREAK_EVENT) {
			break;
		}

	}

	sws_freeContext(img_convert_ctx);

	SDL_Quit();
	//--------------
	av_frame_free(&pFrameYUV);
	av_frame_free(&pFrame);
	avcodec_close(videoCodecCtx);
	avformat_close_input(&pFormatCtx);

	return 0;

}