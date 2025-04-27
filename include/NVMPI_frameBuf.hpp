#pragma once
#include "nvUtils2NvBuf.h"
#include <memcory>

struct NVMPI_frameBuf
{
	NVMPI_frameBuf();
	virtual ~NVMPI_frameBuf();

	NvBufSurface* mDstDMASurface = NULL;

	std::std::shared_ptr<int> mDMAfd;
	std::shared_ptr<CUgraphicsResource> mCUResource;
	std::shared_ptr<CUeglFrame> mCUFrame;
	unsigned long long mTimestamp = 0;
	
	//allocate DMA buffer
	bool alloc(NvBufferCreateParams& input_params);
	//destroy DMA buffer
	bool destroy();
};
