#pragma once
#include "nvUtils2NvBuf.h"
#include <memory>
#include <cuda.h>
#include <cuda_runtime.h>
#include <cudaEGL.h>

struct NVMPI_frameBuf
{
	NVMPI_frameBuf();
	virtual ~NVMPI_frameBuf();

	NvBufSurface* mDstDMASurface = NULL;

	std::shared_ptr<int> mDMAfd;
	std::shared_ptr<CUgraphicsResource> mCUResource;
	std::shared_ptr<CUeglFrame> mCUFrame;
	unsigned long long mTimestamp = 0;
	
	//allocate DMA buffer
	bool alloc(NvBufferCreateParams& input_params);
	//destroy DMA buffer
	bool destroy();
};
