#include "nvmpi.h"
#include "NvVideoDecoder.h"
#include "nvUtils2NvBuf.h"
#include "NVMPI_bufPool.hpp"
#include "NVMPI_frameBuf.hpp"
#include "NvJpegEncoder.h"
#include <vector>
#include <iostream>
#include <thread>
#include <unistd.h>
#include <queue>
#include <mutex>
#include <condition_variable>


#define CHUNK_SIZE 4000000
#define MAX_BUFFERS 32

#define TEST_ERROR(condition, message, errorCode)    \
	if (condition)                               \
{                                                    \
	std::cerr<< message << std::endl;			     \
}

using namespace std;

struct nvmpictx
{
	NvVideoDecoder *dec{nullptr};
	bool eos{false};
	int index{0};
	unsigned int coded_width{0};
	unsigned int coded_height{0};
	unsigned int output_width{0};
	unsigned int output_height{0};
	nvSize resized{0, 0};
	
#if 0	
	std::vector<std::shared_ptr<int> > dmaBufferFileDescriptors;
	
	std::vector<std::shared_ptr<CUgraphicsResource> > cuResources;

	std::vector<CUeglFrame> eglFrames;
#endif

	NvBufferRect src_rect, dest_rect;
	
	nvPixFormat out_pixfmt;
	unsigned int decoder_pixfmt{0};
	std::thread dec_capture_loop;
	
	int frame_pool_size{12};
	std::shared_ptr<NVMPI_bufPool<NVMPI_frameBuf*> > framePool;
	
	//output frame size params
	unsigned int num_planes;
	unsigned int frame_linesize[MAX_NUM_PLANES];
	unsigned int frame_height[MAX_NUM_PLANES];
	unsigned int frame_linedatasize[MAX_NUM_PLANES]; //usable data size for 1 line
	
	//empty frame queue and free buffers memory
	void deinitFramePool();
	//alloc frame buffers based on frame_size data in nvmpictx
	void initFramePool();
	
	//get dst_dma buffer params and set corresponding frame size and linesize in nvmpictx
	void updateFrameSizeParams();
	void updateBufferTransformParams();
	
	void initDecoderCapturePlane(v4l2_format &format);
	/* deinitPlane unmaps the buffers and calls REQBUFS with count 0 */
	void deinitDecoderCapturePlane();
};

class NVFrame : public nvFrame
{
public:
	NVFrame(const std::shared_ptr<NVMPI_bufPool<NVMPI_frameBuf*> > framePool, NVMPI_frameBuf* frameBuffer)
		: mFramePool(framePool)
		, mFrameBuf(frameBuffer)
	{
	}

	virtual ~NVFrame()
	{
		mFramePool->qEmptyBuf(mFrameBuf);
	}
	
	virtual std::vector<unsigned char> toJpeg(int quality) override
	{
		int dmafd = *mFrameBuf->mDMAfd;
		if (dmafd == -1)
			return {};

		if (mJpegEncoder == nullptr)
			mJpegEncoder = std::shared_ptr<NvJPEGEncoder>(NvJPEGEncoder::createJPEGEncoder("jpenenc"));
		
		if (mJpegEncoder == nullptr)
		{
			std::cerr << "NvJPEGEncoder::createJPEGEncoder() fail!" << std::endl;
			return {};
		}

		size_t bufSize = 1024 * 1024;
		std::vector<unsigned char> buf;
		buf.resize(bufSize);
		unsigned char* data = buf.data();
		int ret = mJpegEncoder->encodeFromFd(dmafd, JCS_YCbCr, &data, bufSize, quality);
		if (ret < 0)
		{
			std::cerr << "Error encoding JPEG" << std::endl;
			return {};
		}
		buf.resize(bufSize);
		return buf;
	}

	NVMPI_frameBuf* mFrameBuf{nullptr};
	std::shared_ptr<NVMPI_bufPool<NVMPI_frameBuf*> > mFramePool;

	std::shared_ptr<NvJPEGEncoder> mJpegEncoder;
};



NvBufferColorFormat getNvColorFormatFromV4l2Format(v4l2_format &format)
{
	NvBufferColorFormat ret_cf = NvBufferColorFormat_NV12; 
	switch (format.fmt.pix_mp.colorspace)
	{
		case V4L2_COLORSPACE_SMPTE170M:
			if (format.fmt.pix_mp.quantization == V4L2_QUANTIZATION_DEFAULT)
			{
				// "Decoder colorspace ITU-R BT.601 with standard range luma (16-235)"
				ret_cf = NvBufferColorFormat_NV12;
			}
			else
			{
				//"Decoder colorspace ITU-R BT.601 with extended range luma (0-255)";
				ret_cf = NvBufferColorFormat_NV12_ER;
			}
			break;
		case V4L2_COLORSPACE_REC709:
			if (format.fmt.pix_mp.quantization == V4L2_QUANTIZATION_DEFAULT)
			{
				//"Decoder colorspace ITU-R BT.709 with standard range luma (16-235)";
				ret_cf = NvBufferColorFormat_NV12_709;
			}
			else
			{
				//"Decoder colorspace ITU-R BT.709 with extended range luma (0-255)";
				ret_cf = NvBufferColorFormat_NV12_709_ER;
			}
			break;
		case V4L2_COLORSPACE_BT2020:
			{
				//"Decoder colorspace ITU-R BT.2020";
				ret_cf = NvBufferColorFormat_NV12_2020;
			}
			break;
		default:
			if (format.fmt.pix_mp.quantization == V4L2_QUANTIZATION_DEFAULT)
			{
				//"Decoder colorspace ITU-R BT.601 with standard range luma (16-235)";
				ret_cf = NvBufferColorFormat_NV12;
			}
			else
			{
				//"Decoder colorspace ITU-R BT.601 with extended range luma (0-255)";
				ret_cf = NvBufferColorFormat_NV12_ER;
			}
			break;
	}
	return ret_cf;
}

void nvmpictx::initDecoderCapturePlane(v4l2_format &format)
{
	int ret=0;
	int32_t minimumDecoderCaptureBuffers;
	
	ret=dec->setCapturePlaneFormat(format.fmt.pix_mp.pixelformat,format.fmt.pix_mp.width,format.fmt.pix_mp.height);
	TEST_ERROR(ret < 0, "Error in setting decoder capture plane format", ret);

	dec->getMinimumCapturePlaneBuffers(minimumDecoderCaptureBuffers);
	TEST_ERROR(ret < 0, "Error while getting value of minimum capture plane buffers",ret);

    /* Request buffers on decoder capture plane. Refer ioctl VIDIOC_REQBUFS */
	ret = dec->capture_plane.setupPlane(V4L2_MEMORY_MMAP, minimumDecoderCaptureBuffers, false, false);
	TEST_ERROR(ret < 0, "Error in decoder capture plane streamon", ret);
	
    /* Decoder capture plane STREAMON. Refer ioctl VIDIOC_STREAMON */
	dec->capture_plane.setStreamStatus(true);
	TEST_ERROR(ret < 0, "Error in decoder capture plane streamon", ret);

	/* Enqueue all the empty decoder capture plane buffers. */
	for (uint32_t i = 0; i < dec->capture_plane.getNumBuffers(); i++)
	{
		struct v4l2_buffer v4l2_buf;
		struct v4l2_plane planes[MAX_PLANES];

		memset(&v4l2_buf, 0, sizeof(v4l2_buf));
		memset(planes, 0, sizeof(planes));

		v4l2_buf.index = i;
		v4l2_buf.m.planes = planes;
		ret = dec->capture_plane.qBuffer(v4l2_buf, NULL);
		TEST_ERROR(ret < 0, "Error Qing buffer at output plane", ret);
	}
}

void nvmpictx::deinitDecoderCapturePlane()
{
	int ret = 0;
	dec->capture_plane.setStreamStatus(false);
	dec->capture_plane.deinitPlane();
}

void nvmpictx::updateFrameSizeParams()
{
	//it's safe when called from respondToResolutionEvent() after initFramePool()
	NVMPI_frameBuf* fb = framePool->peekEmptyBuf();
	const NvBufSurfaceParams &dst_dma_surface_params = fb->mDstDMASurface->surfaceList[0];
	const NvBufSurfacePlaneParams &parm = dst_dma_surface_params.planeParams;

	num_planes = parm.num_planes;
	for(unsigned int i = 0; i < num_planes; i++)
	{
		frame_linesize[i] = parm.pitch[i];
		frame_height[i] = parm.height[i];
		frame_linedatasize[i] = parm.width[i] * parm.bytesPerPix[i]; //valid only for nvutils
	}
	
}

void nvmpictx::updateBufferTransformParams()
{
#if 0
	src_rect.top = 0;
	src_rect.left = 0;
	src_rect.width = coded_width;
	src_rect.height = coded_height;
	dest_rect.top = 0;
	dest_rect.left = 0;
	dest_rect.width = output_width;
	dest_rect.height = output_height;
	
	memset(&transform_params,0,sizeof(transform_params));
	transform_params.transform_flag = NVBUFFER_TRANSFORM_FILTER;
	transform_params.transform_flip = NvBufferTransform_None;
	transform_params.transform_filter = NvBufferTransform_Filter_Smart;
	//ctx->transform_params.transform_filter = NvBufSurfTransformInter_Nearest;
	transform_params.src_rect = &src_rect;
	transform_params.dst_rect = &dest_rect;
#endif
}

void nvmpictx::deinitFramePool()
{
	//TODO check that all allocated buffers returned to pool before deinit
	NVMPI_frameBuf* fb = NULL;
	
	while((fb = framePool->dqEmptyBuf()))
	{
		fb->destroy();
		delete fb;
	}
	
	while((fb = framePool->dqFilledBuf()))
	{
		fb->destroy();
		delete fb;
	}
}

void nvmpictx::initFramePool()
{
	NvBufferColorFormat cFmt = out_pixfmt == NV_PIX_NV12 ? NvBufferColorFormat_NV12 : NvBufferColorFormat_YUV420;
	
	NvBufferCreateParams input_params;
	memset(&input_params, 0, sizeof(input_params));
	/* Create PitchLinear output buffer for transform. */
	input_params.width = output_width;
	input_params.height = output_height;
	input_params.layout = NvBufferLayout_Pitch;
	input_params.colorFormat = cFmt;
	input_params.memType = NVBUF_MEM_SURFACE_ARRAY;
	input_params.memtag = NvBufSurfaceTag_VIDEO_CONVERT;
	for(int i=0; i < frame_pool_size; i++)
	{
		NVMPI_frameBuf* fb = new NVMPI_frameBuf();
		if(!fb->alloc(input_params))
			break;

		framePool->qEmptyBuf(fb);
	}
}

void respondToResolutionEvent(v4l2_format &format, v4l2_crop &crop, nvmpictx *ctx)
{
	int ret=0;

    /* Get capture plane format from the decoder.
       This may change after resolution change event.
       Refer ioctl VIDIOC_G_FMT */
	ret = ctx->dec->capture_plane.getFormat(format);	
	TEST_ERROR(ret < 0, "Error: Could not get format from decoder capture plane", ret);

    /* Get the display resolution from the decoder.
       Refer ioctl VIDIOC_G_CROP */
	ret = ctx->dec->capture_plane.getCrop(crop);
	TEST_ERROR(ret < 0, "Error: Could not get crop from decoder capture plane", ret);

	ctx->coded_width = crop.c.width;
	ctx->coded_height = crop.c.height;
	ctx->output_width = ctx->resized.width ? ctx->resized.width : crop.c.width;
	ctx->output_height = ctx->resized.height ? ctx->resized.height : crop.c.height;
	
	//init/reinit DecoderCapturePlane
	ctx->deinitDecoderCapturePlane();
	ctx->initDecoderCapturePlane(format);
	
	//alloc frame pool buffers (dst_dma buffers). TODO: check if already allocated and deinit pool first
	ctx->initFramePool();
	//get dst_dma buffer params and set corresponding frame size and linesize in nvmpictx
	ctx->updateFrameSizeParams();
	
	//reset buffer transformation params based on new resolution data
	ctx->updateBufferTransformParams();
}

void dec_capture_loop_fcn(nvmpictx* ctx)
{
	NvVideoDecoder *dec = ctx->dec;
	
	struct v4l2_format v4l2Format;
	struct v4l2_crop v4l2Crop;
	struct v4l2_event v4l2Event;
	int ret;
	NVMPI_frameBuf* fb = NULL;
	//std::thread transformWorkersPool[3];

    /* Need to wait for the first Resolution change event, so that
       the decoder knows the stream resolution and can allocate appropriate
       buffers when we call REQBUFS. */
    do
    {
        /* Refer ioctl VIDIOC_DQEVENT */
        ret = dec->dqEvent(v4l2Event, 100);
        if (ret < 0)
        {
            if (errno == EAGAIN)
            {
                continue;
            }
            else
            {
               ERROR_MSG("Error in dequeueing decoder event");
               ctx->eos=true;
            }
        }
    }
    while ((v4l2Event.type != V4L2_EVENT_RESOLUTION_CHANGE) && !ctx->eos);

    /* Received the resolution change event, now can do respondToResolutionEvent. */
    if (!ctx->eos)
		respondToResolutionEvent(v4l2Format, v4l2Crop, ctx);
	
	while (!(ctx->eos || dec->isInError()))
	{
		NvBuffer *dec_buffer;
		
		// Check for Resolution change again.
		ret = dec->dqEvent(v4l2Event, false);
		if (ret == 0)
		{
			switch (v4l2Event.type)
			{
				case V4L2_EVENT_RESOLUTION_CHANGE:
					respondToResolutionEvent(v4l2Format, v4l2Crop, ctx);
					continue;
			}
		}
		
		/* Decoder capture loop */
		while(!ctx->eos)
		{
			struct v4l2_buffer v4l2_buf;
			struct v4l2_plane planes[MAX_PLANES];
			v4l2_buf.m.planes = planes;
			
			/* Dequeue a filled buffer. */
			if (dec->capture_plane.dqBuffer(v4l2_buf, &dec_buffer, NULL, 0))
			{
				if (errno == EAGAIN)
				{
					if (v4l2_buf.flags & V4L2_BUF_FLAG_LAST)
					{
						ERROR_MSG("Got EoS at capture plane");
						ctx->eos=true;
					}
					usleep(10);
				}
				else
				{
					ERROR_MSG("Error while calling dequeue at capture plane");
					ctx->eos=true;
				}
				break;
			}
			
			//dec_buffer->planes[0].fd = ctx->dmaBufferFileDescriptor[v4l2_buf.index];
			
			fb = ctx->framePool->dqEmptyBuf();

			NvBufSurf::NvCommonTransformParams transform_params;
			transform_params.src_top = 0;
			transform_params.src_left = 0;
			transform_params.src_width = ctx->coded_width;
			transform_params.src_height = ctx->coded_height;
			transform_params.dst_top = 0;
			transform_params.dst_left = 0;
			transform_params.dst_width = ctx->output_width;
			transform_params.dst_height = ctx->output_height;
			transform_params.flag = NVBUFSURF_TRANSFORM_FILTER;
			transform_params.flip = NvBufSurfTransform_None;
			transform_params.filter = NvBufSurfTransformInter_Algo3;

			if(fb)
			{
				ret = NvBufSurf::NvTransform(&transform_params, dec_buffer->planes[0].fd, *fb->mDMAfd);
				
				TEST_ERROR(ret==-1, "Transform failed",ret);
				fb->mTimestamp = (v4l2_buf.timestamp.tv_usec % 1000) + (v4l2_buf.timestamp.tv_sec * 1000);
				fb->mWidth = ctx->output_width;
				fb->mHeight = ctx->output_height;
				fb->mLineSize[0] = ctx->frame_linesize[0];
				fb->mLineSize[1] = ctx->frame_linesize[1];
				fb->mLineSize[2] = ctx->frame_linesize[2];
				ctx->framePool->qFilledBuf(fb);
			}
			else
			{
				//no buffers available in the pool. wait for EOS or for user to read.
				while(!ctx->eos)
				{
					usleep(10);
					fb = ctx->framePool->dqEmptyBuf();
					if(fb)
					{
						ret = NvBufSurf::NvTransform(&transform_params, dec_buffer->planes[0].fd, *fb->mDMAfd);
						TEST_ERROR(ret==-1, "Transform failed",ret);
						fb->mTimestamp = (v4l2_buf.timestamp.tv_usec % 1000) + (v4l2_buf.timestamp.tv_sec * 1000);
						
						ctx->framePool->qFilledBuf(fb);
						break;
					}
				}
			}

			if (dec->capture_plane.qBuffer(v4l2_buf, NULL) < 0)
			{
				ERROR_MSG("Error while queueing buffer at decoder capture plane");
			}
		}
	}
}

//TODO: accept in nvmpi_create_decoder input stream params (width and height, etc...) from ffmpeg.
nvmpictx* nvmpi_create_decoder(nvDecParam* param)
{
	int ret;
	log_level = LOG_LEVEL_INFO;

	nvmpictx *ctx = new nvmpictx();

	ctx->dec = NvVideoDecoder::createVideoDecoder("dec0");
	TEST_ERROR(!ctx->dec, "Could not create decoder",ret);

	ret=ctx->dec->subscribeEvent(V4L2_EVENT_RESOLUTION_CHANGE, 0, 0);
	TEST_ERROR(ret < 0, "Could not subscribe to V4L2_EVENT_RESOLUTION_CHANGE", ret);
	
	ctx->frame_pool_size = param->frame_pool_size;
	
	switch(param->codingType)
	{
		case NV_VIDEO_CodingH264:
			ctx->decoder_pixfmt=V4L2_PIX_FMT_H264;
			break;
		case NV_VIDEO_CodingHEVC:
			ctx->decoder_pixfmt=V4L2_PIX_FMT_H265;
			break;
		case NV_VIDEO_CodingMPEG4:
			ctx->decoder_pixfmt=V4L2_PIX_FMT_MPEG4;
			break;
		case NV_VIDEO_CodingMPEG2:
			ctx->decoder_pixfmt=V4L2_PIX_FMT_MPEG2;
			break;
		case NV_VIDEO_CodingVP8:
			ctx->decoder_pixfmt=V4L2_PIX_FMT_VP8;
			break;
		case NV_VIDEO_CodingVP9:
			ctx->decoder_pixfmt=V4L2_PIX_FMT_VP9;
			break;
		default:
			ctx->decoder_pixfmt=V4L2_PIX_FMT_H264;
			break;
	}

	ret=ctx->dec->setOutputPlaneFormat(ctx->decoder_pixfmt, CHUNK_SIZE);

	TEST_ERROR(ret < 0, "Could not set output plane format", ret);

	ret = ctx->dec->setFrameInputMode(0);
	TEST_ERROR(ret < 0, "Error in decoder setFrameInputMode for NALU", ret);
	
	//TODO: create option to enable max performace mode (?)
	//ret = ctx->dec->setMaxPerfMode(true);
	//TEST_ERROR(ret < 0, "Error while setting decoder to max perf", ret);

	ret = ctx->dec->output_plane.setupPlane(V4L2_MEMORY_USERPTR, 10, false, true);
	TEST_ERROR(ret < 0, "Error while setting up output plane", ret);

	ret = ctx->dec->output_plane.setStreamStatus(true);
	TEST_ERROR(ret < 0, "Error in output plane stream on", ret);

	ctx->out_pixfmt=param->pixFormat;
	ctx->resized = param->resized;
	ctx->framePool = std::make_shared<NVMPI_bufPool<NVMPI_frameBuf*> >();
	ctx->eos=false;
	ctx->index=0;

	ctx->dec_capture_loop = std::thread(dec_capture_loop_fcn, ctx);

	return ctx;
}

int nvmpi_decoder_put_packet(nvmpictx *ctx, nvPacket* packet)
{
	int ret;
	struct v4l2_buffer v4l2_buf;
	struct v4l2_plane planes[MAX_PLANES];
	NvBuffer *nvBuffer;

	memset(&v4l2_buf, 0, sizeof(v4l2_buf));
	memset(planes, 0, sizeof(planes));

	v4l2_buf.m.planes = planes;

	if (ctx->index < (int)ctx->dec->output_plane.getNumBuffers())
	{
		nvBuffer = ctx->dec->output_plane.getNthBuffer(ctx->index);
		v4l2_buf.index = ctx->index;
		ctx->index++;
	}
	else
	{
		ret = ctx->dec->output_plane.dqBuffer(v4l2_buf, &nvBuffer, NULL, -1);
		if (ret < 0)
		{
			std::cerr << "Error DQing buffer at output plane" << std::endl;
			return -1;
		}
	}

	memcpy(nvBuffer->planes[0].data, packet->payload, packet->payload_size);
	nvBuffer->planes[0].bytesused = packet->payload_size;
	v4l2_buf.m.planes[0].bytesused = nvBuffer->planes[0].bytesused;

	v4l2_buf.flags |= V4L2_BUF_FLAG_TIMESTAMP_COPY;
	v4l2_buf.timestamp.tv_sec = packet->pts / 1000;
	v4l2_buf.timestamp.tv_usec = packet->pts % 1000;

	ret = ctx->dec->output_plane.qBuffer(v4l2_buf, NULL);
	if (ret < 0)
	{
		std::cerr << "Error Qing buffer at output plane" << std::endl;
		ctx->index--;
		return -2;
	}

	if (v4l2_buf.m.planes[0].bytesused == 0)
	{
		ctx->eos=true;
		//std::cout << "Input file read complete" << std::endl; //TODO log it
	}

	return 0;
}

#if 0
int copyNvBufToFrame(nvmpictx* ctx, NVMPI_frameBuf *nvmpiBuf, nvFrame* frame)
{
	int ret;
	char *dataDst;
	char *dataSrc;
	
	for(unsigned int plane=0; plane<ctx->num_planes; plane++)
	{
#ifdef WITH_NVUTILS
		NvBufSurface *nvbuf_surf = nvmpiBuf->dst_dma_surface;
		ret = NvBufSurfaceMap(nvbuf_surf, 0, plane, NVBUF_MAP_READ_WRITE);
		NvBufSurfaceSyncForCpu (nvbuf_surf, 0, plane);
		dataSrc = (char *)nvbuf_surf->surfaceList[0].mappedAddr.addr[plane];
#else
		int dmabuf_fd = nvmpiBuf->dst_dma_fd;
		void *psrc_data;
		ret = NvBufferMemMap(dmabuf_fd, plane, NvBufferMem_Read_Write, &psrc_data);
		NvBufferMemSyncForCpu(dmabuf_fd, plane, &psrc_data);
		dataSrc = (char *)psrc_data;
#endif
		if(ret != 0)
		{
			printf("NvBufferMap failed \n");
			return ret;
		}
		
		dataDst = (char *)frame->payload[plane];
		unsigned int &dstFrameLineSize = frame->linesize[plane];
		unsigned int &srcFrameLineSize = ctx->frame_linesize[plane];
		unsigned int &copySz = ctx->frame_linedatasize[plane];
		
		for (unsigned int i = 0; i < ctx->frame_height[plane]; i++)
		{
			memcpy(dataDst, dataSrc, copySz);
			dataDst += dstFrameLineSize;
			dataSrc += srcFrameLineSize;
		}
		
#ifdef WITH_NVUTILS
		NvBufSurfaceUnMap(nvbuf_surf, 0, plane);
#else
		NvBufferMemUnMap(dmabuf_fd, plane, &psrc_data);
#endif
	}
    return 0;
}
#endif

nvFrame* nvmpi_decoder_get_frame(nvmpictx *ctx)
{
	int ret;
	NVMPI_frameBuf* fb = ctx->framePool->dqFilledBuf();
	if(!fb)
		return NULL;
	
	auto frame = new NVFrame(ctx->framePool, fb);
	frame->width = fb->mWidth;
	frame->height = fb->mHeight;
	frame->payload[0] = (unsigned char*)fb->mCUFrame->frame.pPitch[0];
	frame->payload[1] = (unsigned char*)fb->mCUFrame->frame.pPitch[1];
	frame->payload[2] = (unsigned char*)fb->mCUFrame->frame.pPitch[2];
	frame->linesize[0] = fb->mLineSize[0];
	frame->linesize[1] = fb->mLineSize[1];
	frame->linesize[2] = fb->mLineSize[2];
	frame->timestamp = fb->mTimestamp;
	frame->type = NV_PIX_NV12;
	
	return frame;
}

int nvmpi_decoder_close(nvmpictx *ctx)
{
	ctx->eos=true;
	ctx->dec->capture_plane.setStreamStatus(false);
	if (ctx->dec_capture_loop.joinable())
		ctx->dec_capture_loop.join();
	
	//deinit DstDmaBuffer and DecoderCapturePlane
	ctx->deinitDecoderCapturePlane();
	//empty frame queue and free buffers
	ctx->deinitFramePool();
	
	delete ctx->dec;
	ctx->dec = nullptr;
	return 0;
}


