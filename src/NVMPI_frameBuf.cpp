#include "NVMPI_frameBuf.hpp"
#include <iostream>


NVMPI_frameBuf::NVMPI_frameBuf()
{
}

NVMPI_frameBuf::~NVMPI_frameBuf()
{
	destroy();
}

bool NVMPI_frameBuf::alloc(NvBufferCreateParams& input_params)
{
	int ret = 0;
	int dma_fd = -1;
	ret = NvBufSurf::NvAllocate(&input_params, 1, &dma_fd);
	if(ret<0)
	{
		std::cerr << "Failed to allocate buffer" << std::endl;
		return false;
	}
	
	mDMAfd = std::shared_ptr<int>(new int(dma_fd), [](int* fd) { 
		NvBufSurf::NvDestroy(*fd); 
		delete fd;
	});

	NvBufSurface* nvbuf_surf = NULL;
	ret = NvBufSurfaceFromFd(dma_fd, (void**)&nvbuf_surf);
	if(ret<0)
	{
		std::cerr << "Failed to get surface for buffer" << std::endl;
		return false;
	}
		
	if (nvbuf_surf->surfaceList[0].mappedAddr.eglImage == NULL)
	{
		ret = NvBufSurfaceMapEglImage(nvbuf_surf, 0);
		if (ret < 0)
		{
			std::cerr << "Unable to map EGL Image" << std::endl;
			return false;
		}
	}

	auto eglImage = nvbuf_surf->surfaceList[0].mappedAddr.eglImage;
	if (eglImage == NULL)
	{
		std::cerr << "Error while mapping dmabuf fd (" << dma_fd << ") to EGLImage" << std::endl;
		return false;
	}

	cudaFree(0);
	CUgraphicsResource *pResource = new CUgraphicsResource;
	auto status = cuGraphicsEGLRegisterImage(pResource, eglImage, CU_GRAPHICS_MAP_RESOURCE_FLAGS_NONE);
	if (status != CUDA_SUCCESS)
	{
		delete pResource;
		fprintf(stderr, "cuGraphicsEGLRegisterImage failed: %d, cuda process stop\n", status);
		return false;
	}
	mCUResource = std::shared_ptr<CUgraphicsResource>(pResource, [nvbuf_surf](CUgraphicsResource* res) {
		NvBufSurfaceUnMapEglImage(nvbuf_surf, 0);
		cuGraphicsUnregisterResource(*res);
		delete res;
	});
	
	CUeglFrame eglFrame;
	status = cuGraphicsResourceGetMappedEglFrame(&eglFrame, *pResource, 0, 0);
	if (status != CUDA_SUCCESS)
	{
		fprintf(stderr, "cuGraphicsSubResourceGetMappedArray failed:%d\n", status);
		return false;
	}
	mDstDMASurface = nvbuf_surf;
	mCUFrame = std::make_shared<CUeglFrame>(eglFrame);
	return true;
}

bool NVMPI_frameBuf::destroy()
{
	mCUFrame = nullptr;
	mCUResource = nullptr;
	mDMAfd = nullptr;
	return true;
}
