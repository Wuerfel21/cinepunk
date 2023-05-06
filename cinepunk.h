
#ifndef CINEPUNK_H
#define CINEPUNK_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

enum CPColorType {
    CP_RGB24,
    CP_GRAY,
    //CP_RAWYUV,
    CP_YUVBLOCK
};


// The layout of this struct is optimized for SIMD, do not touch.
typedef struct {
    uint16_t weight;
    uint8_t u,v; // With +128 bias
    uint8_t ytl,ytr,ybl,ybr;
} CPYuvBlock;

struct CPEncoderState;
struct CPDecoderState;

#define CP_ENCFLAG_RGB2YUV_FAST (1U<<0) // Use fast RGB->YUV conversion instead of high-quality
#define CP_ENCFLAG_NO_THREADS   (1U<<1) // Don't use threads for speedup

#define CP_DECDEBUG_CRYPTOMATTE (1U<<0)

// maximum size of an encoded frame packet
#define CP_BUFFER_SIZE(width,height,strips) (10+((width)*(height))/4+((width)*(height)+63)/64+(strips)*(12+4*3+1536*2))

// From rgbyuv.cpp
extern void CP_yuv2rgb(uint8_t* dst, const CPYuvBlock *src, unsigned blockWidth, unsigned blockHeight);
extern void CP_rgb2yuv_fast(CPYuvBlock *dst, const uint8_t* src, unsigned blockWidth, unsigned blockHeight);
extern void CP_rgb2yuv_hq(CPYuvBlock *dst, const uint8_t* src, unsigned blockWidth, unsigned blockHeight);
extern void CP_yuv2gray(uint8_t* dst, const CPYuvBlock *src, unsigned blockWidth, unsigned blockHeight);
extern void CP_gray2yuv(CPYuvBlock *dst, const uint8_t* src, unsigned blockWidth, unsigned blockHeight);
extern void CP_yuv_downscale_fast(CPYuvBlock *dst, const CPYuvBlock* src, unsigned blockWidth, unsigned blockHeight,unsigned extra_stride);

// From encoder.cpp
extern CPEncoderState *CP_create_encoder(unsigned frame_width, unsigned frame_height, unsigned max_strips);
extern void CP_destroy_encoder(CPEncoderState *enc);
extern size_t CP_get_buffer_size(CPEncoderState *enc);
extern void CP_set_encflags(CPEncoderState *enc,uint32_t flags);
extern void CP_clear_encflags(CPEncoderState *enc,uint32_t flags);
extern void CP_set_quality(CPEncoderState *enc,uint32_t factor);
extern bool CP_push_frame(CPEncoderState *enc,CPColorType ctype,const void *data);
extern size_t CP_pull_frame(CPEncoderState *enc,uint8_t *buffer);

// From decoder.cpp
extern bool CP_peek_dimensions(uint8_t *data, size_t data_size, unsigned *widthOut, unsigned *heightOut, size_t *sizeOut);
extern CPDecoderState *CP_create_decoder(unsigned frame_width, unsigned frame_height);
extern void CP_destroy_decoder(CPDecoderState *dec);
extern void CP_set_decoder_debug(CPDecoderState *dec,uint32_t flags);
extern void CP_decode_frame(CPDecoderState *dec,const uint8_t *data,size_t data_size,CPColorType ctype,void *frameOut);


#ifdef __cplusplus
}
#endif
#endif