#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>

#include <dlfcn.h>
#include <nvmpi.h>
#include "avcodec.h"
#include "decode.h"
#include "internal.h"
#include "libavutil/buffer.h"
#include "libavutil/common.h"
#include "libavutil/frame.h"
#include "libavutil/hwcontext.h"
#include "libavutil/hwcontext_drm.h"
#include "libavutil/imgutils.h"
#include "libavutil/log.h"
#include "libavutil/opt.h"

#if LIBAVCODEC_VERSION_MAJOR >= 60
#include "codec_internal.h"
#endif

#define OPT_frame_pool_size_MIN 1
#define OPT_frame_pool_size_MAX 32
#define OPT_frame_pool_size_DEFAULT 5

void* g_nvmpi_lib = NULL;

struct NVMPIFunctions
{
	nvmpictx* (*nvmpi_create_decoder)(nvDecParam* param);
	int (*nvmpi_decoder_put_packet)(nvmpictx* ctx, nvPacket* packet);
	int (*nvmpi_decoder_get_frame)(nvmpictx* ctx, nvFrame* frame,bool wait);
	int (*nvmpi_decoder_close)(nvmpictx* ctx);
};

typedef struct {
	char eos_reached;
	nvmpictx* ctx;
	AVClass *av_class;
	AVFrame *bufFrame;
	char *resize_expr;
	int frame_pool_size;
	NVMPIFunctions nvmpi;
} nvmpiDecodeContext;

static nvCodingType nvmpi_get_codingtype(AVCodecContext *avctx)
{
	switch (avctx->codec_id) {
		case AV_CODEC_ID_H264:          return NV_VIDEO_CodingH264;
		case AV_CODEC_ID_HEVC:          return NV_VIDEO_CodingHEVC;
		case AV_CODEC_ID_VP8:           return NV_VIDEO_CodingVP8;
		case AV_CODEC_ID_VP9:           return NV_VIDEO_CodingVP9;
		case AV_CODEC_ID_MPEG4:		return NV_VIDEO_CodingMPEG4;
		case AV_CODEC_ID_MPEG2VIDEO:    return NV_VIDEO_CodingMPEG2;
		default:                        return NV_VIDEO_CodingUnused;
	}
};


static int nvmpi_init_decoder(AVCodecContext *avctx)
{
	nvmpiDecodeContext *nvmpi_context = avctx->priv_data;
	g_nvmpi_lib = dlopen("libnvmpi.so", RTLD_NOW);
	if (!g_nvmpi_lib)
	{
		av_log(avctx, AV_LOG_ERROR, "load libnvmpi.so fail!\n");
		return AVERROR_UNKNOWN;
	}
#define LOAD_SYMBOL(sym) \
    nvmpi_context->nvmpi.sym = dlsym(g_nvmpi_lib, ""#sym);\
	if (nvmpi_context->nvmpi.sym == NULL)\
	{\
		av_log(avctx, AV_LOG_ERROR, "dlsym "#sym" fail!\n");\
		return AVERROR_UNKNOWN;\
	}
	
	LOAD_SYMBOL(nvmpi_create_decoder);
	LOAD_SYMBOL(nvmpi_decoder_put_packet);
	LOAD_SYMBOL(nvmpi_decoder_get_frame);
	LOAD_SYMBOL(nvmpi_decoder_close);
	
	nvDecParam param={0};
	
	param.codingType =nvmpi_get_codingtype(avctx);
	if (param.codingType == NV_VIDEO_CodingUnused)
	{
		av_log(avctx, AV_LOG_ERROR, "Unknown codec type (%d).\n", avctx->codec_id);
		return AVERROR_UNKNOWN;
	}
	
	param.frame_pool_size = nvmpi_context->frame_pool_size;
	if(param.frame_pool_size < OPT_frame_pool_size_MIN || param.frame_pool_size > OPT_frame_pool_size_MAX)
	{
		av_log(avctx, AV_LOG_WARNING, "Incorrect frame_pool_size specified: %d. Default (%d) will be used.\n", param.frame_pool_size, OPT_frame_pool_size_DEFAULT);
		param.frame_pool_size = OPT_frame_pool_size_DEFAULT;
	}

	//Workaround for default pix_fmt not being set, so check if it isnt set and set it,
	//or if it is set, but isnt set to something we can work with.
	param.pixFormat = NV_PIX_YUV420;
	if(avctx->pix_fmt == AV_PIX_FMT_NONE)
	{
		 avctx->pix_fmt = AV_PIX_FMT_NV12;
    	 param.pixFormat = NV_PIX_NV12;
	}
	else if(!(avctx->pix_fmt == AV_PIX_FMT_YUV420P || avctx->pix_fmt == AV_PIX_FMT_YUVJ420P || avctx->pix_fmt == AV_PIX_FMT_NV12))
	{
		av_log(avctx, AV_LOG_ERROR, "Invalid Pix_FMT for NVMPI: Only YUV420P YUVJ420P NV12 are supported\n");
		return AVERROR_INVALIDDATA;
	}
	//TODO more pixformats support

    if (nvmpi_context->resize_expr && sscanf(nvmpi_context->resize_expr, "%dx%d",
                                             &param.resized.width, &param.resized.height) != 2)
	{
        av_log(avctx, AV_LOG_ERROR, "Invalid resize expressions\n");
        return AVERROR(EINVAL);
    }
	
	//overwrite avctx w and h if resize option is used
	if(param.resized.width && param.resized.height)
	{
		avctx->width = param.resized.width;
		avctx->height = param.resized.height;
	}

	nvmpi_context->bufFrame = av_frame_alloc();
	nvmpi_context->bufFrame->width = avctx->width;
	nvmpi_context->bufFrame->height = avctx->height;
	if (ff_get_buffer(avctx, nvmpi_context->bufFrame, 0) < 0)
	{
		av_frame_free(&(nvmpi_context->bufFrame));
		nvmpi_context->bufFrame = NULL;
		return AVERROR(ENOMEM);
	}

	nvmpi_context->ctx=nvmpi_context->nvmpi.nvmpi_create_decoder(&param);

	if(!nvmpi_context->ctx)
	{
		av_frame_free(&(nvmpi_context->bufFrame));
		nvmpi_context->bufFrame = NULL;
		av_log(avctx, AV_LOG_ERROR, "Failed to nvmpi_create_decoder (code = %d).\n", AVERROR_EXTERNAL);
		return AVERROR_EXTERNAL;
	}
	
   return 0;
}

static int nvmpi_close(AVCodecContext *avctx)
{
	nvmpiDecodeContext *nvmpi_context = avctx->priv_data;
	if(nvmpi_context->bufFrame)
	{
		av_frame_free(&(nvmpi_context->bufFrame));
		nvmpi_context->bufFrame = NULL;
	}
	return nvmpi_context->nvmpi.nvmpi_decoder_close(nvmpi_context->ctx);
}

#if LIBAVCODEC_VERSION_MAJOR >= 60
static int nvmpi_decode(AVCodecContext *avctx, AVFrame *data, int *got_frame, AVPacket *avpkt)
#else
static int nvmpi_decode(AVCodecContext *avctx, void *data, int *got_frame, AVPacket *avpkt)
#endif
{
	nvmpiDecodeContext *nvmpi_context = avctx->priv_data;
	AVFrame *frame = data;
	AVFrame *bufFrame = nvmpi_context->bufFrame;
	nvFrame _nvframe={0};
	nvPacket packet;
	int res;
	int decode_ret = avpkt->size;

	if(avpkt->size)
	{
		packet.payload_size=avpkt->size;
		packet.payload=avpkt->data;
		packet.pts=avpkt->pts;

		res=nvmpi_context->nvmpi.nvmpi_decoder_put_packet(nvmpi_context->ctx,&packet);
		if(res < 0)
		{
			if(res == -1)
			{
				decode_ret = AVERROR(EAGAIN); //TODO log
			}
			//TODO error handling
		}
	}

	_nvframe.payload[0] = bufFrame->data[0];
	_nvframe.payload[1] = bufFrame->data[1];
	_nvframe.payload[2] = bufFrame->data[2];
	_nvframe.linesize[0] = bufFrame->linesize[0];
	_nvframe.linesize[1] = bufFrame->linesize[1];
	_nvframe.linesize[2] = bufFrame->linesize[2];

	res=nvmpi_context->nvmpi.nvmpi_decoder_get_frame(nvmpi_context->ctx,&_nvframe,avctx->flags & AV_CODEC_FLAG_LOW_DELAY);

	if(res<0)
	{
		return decode_ret;
	}

	bufFrame->format=avctx->pix_fmt;
	bufFrame->pts=_nvframe.timestamp;
	bufFrame->pkt_dts = AV_NOPTS_VALUE;
	av_frame_move_ref(frame, bufFrame);
	
	*got_frame = 1;
	
	bufFrame->width = avctx->width;
	bufFrame->height = avctx->height;
	if (ff_get_buffer(avctx, bufFrame, 0) < 0)
	{
		av_log(avctx, AV_LOG_ERROR, "ff_get_buffer failed\n");
		return AVERROR(ENOMEM);
	}
	
	frame->metadata = bufFrame->metadata;
	bufFrame->metadata = NULL;

	return decode_ret;
}



#define OFFSET(x) offsetof(nvmpiDecodeContext, x)
#define VD AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_DECODING_PARAM
static const AVOption options[] = {
    { "resize",   "Resize (width)x(height)", OFFSET(resize_expr), AV_OPT_TYPE_STRING, { .str = NULL }, 0, 0, VD, "resize" },
    { "frame_pool_size", "Number of frames that could be buffered in the decoder before user must read it with avcodec_receive_frame()", OFFSET(frame_pool_size), AV_OPT_TYPE_INT, {.i64 = OPT_frame_pool_size_DEFAULT }, OPT_frame_pool_size_MIN, OPT_frame_pool_size_MAX, VD, "frame_pool_size" },
    { NULL }
};

#define NVMPI_DEC_CLASS(NAME) \
	static const AVClass nvmpi_##NAME##_dec_class = { \
		.class_name = "nvmpi_" #NAME "_dec", \
		.option     = options, \
		.version    = LIBAVUTIL_VERSION_INT, \
	};

#if LIBAVCODEC_VERSION_MAJOR >= 60
	#define NVMPI_DEC(NAME, ID, BSFS) \
		NVMPI_DEC_CLASS(NAME) \
		FFCodec ff_##NAME##_nvmpi_decoder = { \
			.p.name           = #NAME "_nvmpi", \
			CODEC_LONG_NAME(#NAME " (nvmpi)"), \
			.p.type           = AVMEDIA_TYPE_VIDEO, \
			.p.id             = ID, \
			.priv_data_size = sizeof(nvmpiDecodeContext), \
			.init           = nvmpi_init_decoder, \
			.close          = nvmpi_close, \
			FF_CODEC_DECODE_CB(nvmpi_decode), \
			.p.priv_class     = &nvmpi_##NAME##_dec_class, \
			.p.capabilities   = AV_CODEC_CAP_DELAY | AV_CODEC_CAP_AVOID_PROBING | AV_CODEC_CAP_HARDWARE, \
			.p.pix_fmts	=(const enum AVPixelFormat[]){AV_PIX_FMT_YUV420P,AV_PIX_FMT_NV12,AV_PIX_FMT_NONE},\
			.bsfs           = BSFS, \
			.p.wrapper_name   = "nvmpi", \
		};
#else
	#define NVMPI_DEC(NAME, ID, BSFS) \
		NVMPI_DEC_CLASS(NAME) \
		AVCodec ff_##NAME##_nvmpi_decoder = { \
			.name           = #NAME "_nvmpi", \
			.long_name      = NULL_IF_CONFIG_SMALL(#NAME " (nvmpi)"), \
			.type           = AVMEDIA_TYPE_VIDEO, \
			.id             = ID, \
			.priv_data_size = sizeof(nvmpiDecodeContext), \
			.init           = nvmpi_init_decoder, \
			.close          = nvmpi_close, \
			.decode         = nvmpi_decode, \
			.priv_class     = &nvmpi_##NAME##_dec_class, \
			.capabilities   = AV_CODEC_CAP_DELAY | AV_CODEC_CAP_AVOID_PROBING | AV_CODEC_CAP_HARDWARE, \
			.pix_fmts	=(const enum AVPixelFormat[]){AV_PIX_FMT_YUV420P,AV_PIX_FMT_NV12,AV_PIX_FMT_NONE},\
			.bsfs           = BSFS, \
			.wrapper_name   = "nvmpi", \
		};
#endif


NVMPI_DEC(h264,  AV_CODEC_ID_H264,"h264_mp4toannexb");
NVMPI_DEC(hevc,  AV_CODEC_ID_HEVC,"hevc_mp4toannexb");
NVMPI_DEC(mpeg2, AV_CODEC_ID_MPEG2VIDEO,NULL);
NVMPI_DEC(mpeg4, AV_CODEC_ID_MPEG4,NULL);
NVMPI_DEC(vp9,  AV_CODEC_ID_VP9,NULL);
NVMPI_DEC(vp8, AV_CODEC_ID_VP8,NULL);

