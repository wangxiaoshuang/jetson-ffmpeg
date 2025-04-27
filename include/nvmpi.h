#ifndef __NVMPI_H__
#define __NVMPI_H__
#include <stdlib.h>
#include <stdbool.h>
#include <memory.h>

//Maximum size of the encoded buffers on the capture plane in bytes 
#define NVMPI_ENC_CHUNK_SIZE 2*1024*1024

typedef struct nvmpictx nvmpictx;

typedef enum {
	NV_PIX_NV12,
	NV_PIX_YUV420
} nvPixFormat;

typedef enum {
	NV_VIDEO_CodingUnused,
	NV_VIDEO_CodingH264,             /**< H.264 */
	NV_VIDEO_CodingMPEG4,              /**< MPEG-4 */
	NV_VIDEO_CodingMPEG2,              /**< MPEG-2 */
	NV_VIDEO_CodingVP8,                /**< VP8 */
	NV_VIDEO_CodingVP9,                /**< VP9 */
	NV_VIDEO_CodingHEVC,               /**< H.265/HEVC */
} nvCodingType;

typedef struct _NVSIZE{
	unsigned int width;
	unsigned int height;
}nvSize;

typedef struct _NVDECPARAM{
	int frame_pool_size;
	nvCodingType codingType;
	nvPixFormat pixFormat;
	nvSize resized;
} nvDecParam;

typedef struct _NVPACKET{
	unsigned long flags;
	unsigned long payload_size;
	unsigned char *payload;
	unsigned long  pts;
	//NVMPI_pkt pointer. used by encoder
	void* privData;
} nvPacket;

typedef struct _NVFRAME{
	unsigned long flags;
	unsigned long payload_size[3];
	unsigned char *payload[3];
	unsigned int linesize[3];
	nvPixFormat type;
	unsigned int width;
	unsigned int height;
	time_t timestamp;
} nvFrame;

#ifdef __cplusplus
extern "C" {
#endif

	std::shared_ptr<nvmpictx> nvmpi_create_decoder(const nvDecParam& param);

	int nvmpi_decoder_put_packet(const std::shared_ptr<nvmpictx> &ctx, const nvPacket& packet);

	std::shared_ptr<nvFrame> nvmpi_decoder_get_frame(const std::shared_ptr<nvmpictx> &ctx, bool wait);

	int nvmpi_decoder_close(const std::shared_ptr<nvmpictx> &ctx);

#ifdef __cplusplus
}
#endif

#endif /*__NVMPI_H__*/
