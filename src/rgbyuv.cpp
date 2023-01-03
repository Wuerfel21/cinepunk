#include "cinepunk_internal.hpp"
#include <array>
#include <cmath>

inline void yuv2rgb_single(uint8_t *dst,int y,int u,int v) {
    dst[0] = clamp_u8(y + v*2    ); // R
    dst[1] = clamp_u8(y - u/2 - v); // G
    dst[2] = clamp_u8(y + u*2    ); // B
}

CP_API void CP_yuv2rgb(uint8_t* dst, const CPYuvBlock *src, unsigned blockWidth, unsigned blockHeight) {
    for (uint row=0;row<blockHeight;row++) {
        for (uint column=0;column<blockWidth;column++) {
            auto *block = src++;
            int u = block->u - 128;
            int v = block->v - 128;
            auto dstrow = dst+(column+row*blockWidth*2)*6;
            yuv2rgb_single(dstrow+0,block->ytl,u,v);
            yuv2rgb_single(dstrow+3,block->ytr,u,v);
            dstrow += blockWidth*2*3;
            yuv2rgb_single(dstrow+0,block->ybl,u,v);
            yuv2rgb_single(dstrow+3,block->ybr,u,v);
        }
    }
}

CP_API void CP_yuv2gray(uint8_t* dst, const CPYuvBlock *src, unsigned blockWidth, unsigned blockHeight) {
    for (uint row=0;row<blockHeight;row++) {
        for (uint column=0;column<blockWidth;column++) {
            auto *block = src++;
            auto dstrow = dst+(column+row*blockWidth*2)*2;
            dstrow[0] = block->ytl;
            dstrow[1] = block->ytr;
            dstrow += blockWidth*2;
            dstrow[0] = block->ybl;
            dstrow[1] = block->ybr;
        }
    }
}

CP_API void CP_gray2yuv(CPYuvBlock *dst, const uint8_t* src, unsigned blockWidth, unsigned blockHeight) {
    for (uint row=0;row<blockHeight;row++) {
        for (uint column=0;column<blockWidth;column++) {
            auto *block = dst++;
            auto srcrow = src+(column+row*blockWidth*2)*2;
            block->u = 128;
            block->v = 128;
            block->ytl = srcrow[0];
            block->ytr = srcrow[1];
            srcrow += blockWidth*2;
            block->ybl = srcrow[0];
            block->ybr = srcrow[1];
        }
    }
}

constexpr int mat_shift = 20;
constexpr int mat_scale = 1<<mat_shift;
constexpr int mat_round = mat_scale>>1;
constexpr std::array<int,9> yuv_matrix = {
    int(+0.2857*mat_scale),int(+0.5714*mat_scale),int(+0.1429*mat_scale), // RGB -> Y
    int(-0.1429*mat_scale),int(-0.2857*mat_scale),int(+0.4286*mat_scale), // RGB -> U
    int(+0.3571*mat_scale),int(-0.2857*mat_scale),int(-0.0714*mat_scale), // RGB -> V
};

CP_API void CP_rgb2yuv_fast(CPYuvBlock *dst, const uint8_t* src, unsigned blockWidth, unsigned blockHeight) {
    // "Fast" RGB2YUV
    for (uint row=0;row<blockHeight;row++) {
        for (uint column=0;column<blockWidth;column++) {
            auto *block = dst++;
            int r = 0,g = 0,b = 0;
            auto srcrow = src+(column+row*blockWidth*2)*6;
            block->ytl = (srcrow[0]*yuv_matrix[0] + srcrow[1]*yuv_matrix[1] + srcrow[2]*yuv_matrix[2] + mat_round) >> mat_shift;
            r         +=  srcrow[0]; g          +=  srcrow[1]; b          +=  srcrow[2];

            block->ytr = (srcrow[3]*yuv_matrix[0] + srcrow[4]*yuv_matrix[1] + srcrow[5]*yuv_matrix[2] + mat_round) >> mat_shift;
            r         +=  srcrow[3]; g          +=  srcrow[4]; b          +=  srcrow[5];
            srcrow += blockWidth*2*3;
            block->ybl = (srcrow[0]*yuv_matrix[0] + srcrow[1]*yuv_matrix[1] + srcrow[2]*yuv_matrix[2] + mat_round) >> mat_shift;
            r         +=  srcrow[0]; g          +=  srcrow[1]; b          +=  srcrow[2];

            block->ybr = (srcrow[3]*yuv_matrix[0] + srcrow[4]*yuv_matrix[1] + srcrow[5]*yuv_matrix[2] + mat_round) >> mat_shift;
            r         +=  srcrow[3]; g          +=  srcrow[4]; b          +=  srcrow[5];

            block->u = clamp_u8(((r*yuv_matrix[3]+g*yuv_matrix[4]+b*yuv_matrix[5] + mat_round*4)>>(mat_shift+2)) + 128);
            block->v = clamp_u8(((r*yuv_matrix[6]+g*yuv_matrix[7]+b*yuv_matrix[8] + mat_round*4)>>(mat_shift+2)) + 128);
        }
    }
}

static constexpr int Y_FIX = 2;
static constexpr int Y_MAX = 255<<Y_FIX;
static constexpr int Y_ROUND = (1<<Y_FIX)>>1;

static inline constexpr int clamp_y(int x) {
    return std::clamp(x,0,Y_MAX);
}

static inline constexpr float srgb2lin(int i) {
    float f = i/float(Y_MAX);
    return (f>0.04045f ? powf((f+0.055f)/1.055f,2.4f) : f/12.92f);
}
static inline constexpr int lin2srgb(float f) {
    return std::clamp((int)roundf((f>0.0031308f ? powf(f,1.0f/2.4f)*1.055f-0.055f : f*12.92f)*Y_MAX),0,Y_MAX);
}

// Gamma-correct luma value
// The visually correct formula really depends on monitor calibration...
static inline float srgb2lingrey(int r, int g, int b) {
    return srgb2lin(r)*0.2126f + srgb2lin(g)*0.7152f + srgb2lin(b)*0.0722f;
    //return srgb2lin(r)*0.299f + srgb2lin(g)*0.587f + srgb2lin(b)*0.144f;
    //return srgb2lin(r)*0.2627f + srgb2lin(g)*0.6780f + srgb2lin(b)*0.0593f;
}

// Gamma-correct average of four pixels
static inline int srgb_avg(int a, int b, int c, int d) {
    return lin2srgb((srgb2lin(a) + srgb2lin(b) + srgb2lin(c) + srgb2lin(d))/4);
}


CP_API void CP_rgb2yuv_hq(CPYuvBlock *dst, const uint8_t* src, unsigned blockWidth, unsigned blockHeight) {
    // "High Quality" RGB2YUV with luma correction
    for (uint row=0;row<blockHeight;row++) {
        for (uint column=0;column<blockWidth;column++) {
            auto *block = dst++;
            int r[4],g[4],b[4]; 
            int rdiff,gdiff,bdiff;
            int avg_y;
            int y[4];
            float w_target[4]; // computed real luminance value of pixels

            auto srcrow = src+(column+row*blockWidth*2)*6;
            r[0] = srcrow[0]<<Y_FIX;g[0] = srcrow[1]<<Y_FIX;b[0] = srcrow[2]<<Y_FIX;
            r[1] = srcrow[3]<<Y_FIX;g[1] = srcrow[4]<<Y_FIX;b[1] = srcrow[5]<<Y_FIX;
            srcrow += blockWidth*2*3;
            r[2] = srcrow[0]<<Y_FIX;g[2] = srcrow[1]<<Y_FIX;b[2] = srcrow[2]<<Y_FIX;
            r[3] = srcrow[3]<<Y_FIX;g[3] = srcrow[4]<<Y_FIX;b[3] = srcrow[5]<<Y_FIX;


            rdiff = srgb_avg(r[0],r[1],r[2],r[3]);
            gdiff = srgb_avg(g[0],g[1],g[2],g[3]);
            bdiff = srgb_avg(b[0],b[1],b[2],b[3]);
            avg_y = (rdiff*yuv_matrix[0]+gdiff*yuv_matrix[1]+bdiff*yuv_matrix[2])/mat_scale;
            rdiff -= avg_y;
            gdiff -= avg_y;
            bdiff -= avg_y;

            for (uint i=0;i<4;i++) {
                // Find Y values that result in same real luminance as original pixel
                w_target[i] = srgb2lingrey(r[i],g[i],b[i]);
                y[i] = (r[i]*yuv_matrix[0]+g[i]*yuv_matrix[1]+b[i]*yuv_matrix[2]+mat_round)>>mat_shift;
                float cur_luma = srgb2lingrey(clamp_y(y[i]+rdiff),clamp_y(y[i]+gdiff),clamp_y(y[i]+bdiff));
                y[i] = lin2srgb(srgb2lin(y[i])+w_target[i]-cur_luma);
            }

            block->ytl = clamp_u8((y[0]+Y_ROUND)>>Y_FIX);
            block->ytr = clamp_u8((y[1]+Y_ROUND)>>Y_FIX);
            block->ybl = clamp_u8((y[2]+Y_ROUND)>>Y_FIX);
            block->ybr = clamp_u8((y[3]+Y_ROUND)>>Y_FIX);
            block->u   = clamp_u8(((rdiff*yuv_matrix[3]+gdiff*yuv_matrix[4]+bdiff*yuv_matrix[5]+(mat_round<<Y_FIX))>>(mat_shift+Y_FIX)) + 128);
            block->v   = clamp_u8(((rdiff*yuv_matrix[6]+gdiff*yuv_matrix[7]+bdiff*yuv_matrix[8]+(mat_round<<Y_FIX))>>(mat_shift+Y_FIX)) + 128);
        }
    }
}

CP_API void CP_yuv_downscale_fast(CPYuvBlock *dst, const CPYuvBlock* src, unsigned blockWidth, unsigned blockHeight, unsigned extra_stride) {
    // Width/height relate to the destination
    // TODO: write HQ version

    for (uint row=0;row<blockHeight;row++) {
        for (uint column=0;column<blockWidth;column++) {
            uint u = 0, v = 0, weight = 0;
            auto srcblk = src+(column*2+row*(blockWidth*4+extra_stride));
            auto tl = *srcblk;
            u += srcblk->u;
            v += srcblk->v;
            weight += srcblk->weight;
            dst->ytl = (srcblk->ytl + srcblk->ytr + srcblk->ybl + srcblk->ybr + 2)>>2;
            srcblk++;
            auto tr = *srcblk;
            u += srcblk->u;
            v += srcblk->v;
            weight += srcblk->weight;
            dst->ytr = (srcblk->ytl + srcblk->ytr + srcblk->ybl + srcblk->ybr + 2)>>2;
            srcblk += blockWidth*2 - 1;
            auto bl = *srcblk;
            u += srcblk->u;
            v += srcblk->v;
            weight += srcblk->weight;
            dst->ybl = (srcblk->ytl + srcblk->ytr + srcblk->ybl + srcblk->ybr + 2)>>2;
            srcblk++;
            auto br = *srcblk;
            u += srcblk->u;
            v += srcblk->v;
            weight += srcblk->weight;
            dst->ybr = (srcblk->ytl + srcblk->ytr + srcblk->ybl + srcblk->ybr + 2)>>2;

            dst->u = (u+2)>>2;
            dst->v = (v+2)>>2;

            // weighting hack: de-weight poor-performing blocks
            // TODO: tune magic number
            auto mbdist = macroblockV1Distortion(tl,tr,bl,br,*dst);
            if (mbdist < 48*TOTAL_WEIGHT) weight*=2;
            if (mbdist < 6*TOTAL_WEIGHT) weight*=2;
            dst->weight = clamp_u8(weight);
            dst++;
        }
    }

}