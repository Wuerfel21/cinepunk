#pragma once
#include "../cinepunk.h"

#if (defined(__amd64__) or defined(__i386__)) and not defined(CINEPUNK_NOVECTOR)
#define CINEPUNK_AVX2
#endif

#ifdef CINEPUNK_AVX2
#include <immintrin.h>
#endif


typedef unsigned uint;
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

#define CP_API extern "C"

#include <algorithm>
#include <memory>
#include <numeric>
#include <cstring>
#include <cassert>


struct PacketWriter {
    u8 *begin,*ptr;

    inline void write_u8(u8 x) {
        *ptr++ = x;
    }
    inline void write_u16(u16 x) {
        *ptr++ = x>>8;
        *ptr++ = x&255;
    }
    inline void write_u24(u32 x) {
        *ptr++ = (x>>16)&255;
        *ptr++ = (x>>8)&255;
        *ptr++ = x&255;
    }
    inline void write_u32(u32 x) {
        *ptr++ = x>>24;
        *ptr++ = (x>>16)&255;
        *ptr++ = (x>>8)&255;
        *ptr++ = x&255;
    }
    inline void write_data(uint8_t *data,size_t size) {
        memcpy(ptr,data,size);
        ptr += size;
    }
    inline size_t get_length() {
        return ptr - begin;
    }
    inline void skip(size_t size) {
        ptr += size;
    }

    PacketWriter(u8 *begin) : begin{begin},ptr{begin} {};
};

struct BitstreamWriter {
    PacketWriter &packet;
    std::vector<uint8_t> buffer;
    uint bit_fill = 0;
    u32 bit_buffer = 0;
    inline void write_u8(u8 x) {
        buffer.push_back(x);
    }
    inline void flush() {
        packet.write_u32(bit_buffer<<(32-bit_fill));
        packet.write_data(buffer.data(),buffer.size());
        buffer.clear();
        bit_fill = 0;
        bit_buffer = 0;
    }
    inline void put_bit(bool x) {
        if (bit_fill == 32) flush();
        bit_buffer = (bit_buffer<<1)+(x?1:0);
        bit_fill++;
    }
    BitstreamWriter(PacketWriter &packet) : packet{packet} {};
};

struct PacketReader {
    const u8 *ptr;
    inline u8 read_u8() {
        return *ptr++;
    }
    inline u16 read_u16() {
        u16 x = (*ptr++)<<8;
        x |= *ptr++;
        return x;
    }
    inline u32 read_u24() {
        u32 x = (*ptr++)<<16;
        x |= (*ptr++)<<8;
        x |= *ptr++;
        return x;
    }
    inline u32 read_u32() {
        u32 x = (*ptr++)<<24;
        x |= (*ptr++)<<16;
        x |= (*ptr++)<<8;
        x |= *ptr++;
        return x;
    }
    PacketReader(const u8 *begin) : ptr{begin} {};
};

struct BitstreamReader {
    PacketReader &packet;
    uint bit_fill = 0;
    u32 bit_buffer = 0;
    inline bool read_bit() {
        if (bit_fill == 0) {
            bit_buffer = packet.read_u32();
            bit_fill = 32;
        }
        return (bit_buffer>>(--bit_fill))&1;
    }
    inline u8 read_u8() {
        return packet.read_u8();
    }
    BitstreamReader(PacketReader &packet) : packet{packet} {};
};

struct CPDecoderState {
    const uint frame_mbWidth,frame_mbHeight;
    uint32_t debug_flags = 0;
    std::unique_ptr<CPYuvBlock[]> frame;
    std::vector<std::array<CPYuvBlock,256>> codes_v4;
    std::vector<std::array<CPYuvBlock,256>> codes_v1;

    uint total_macroblocks() {return frame_mbWidth*frame_mbHeight;}
    uint total_blocks() {return total_macroblocks()*4;}
    uint mb_index(uint x,uint y) {return x+y*frame_mbWidth*1;}
    uint blk_index(uint x,uint y) {return x+y*frame_mbWidth*2;}

    void do_decode(const uint8_t *data,size_t data_size);

    CPDecoderState(uint frame_width,uint frame_height);
};

struct CPEncoderState {

    const uint frame_mbWidth,frame_mbHeight,max_strips;
    uint32_t encoder_flags = 0;
    std::unique_ptr<CPYuvBlock[]> cur_frame;
    std::unique_ptr<CPYuvBlock[]> cur_frame_v1;
    std::unique_ptr<CPYuvBlock[]> next_frame;
    std::unique_ptr<u32[]> skip_mb_distortion;
    CPDecoderState decode_state;
    std::vector<std::array<CPYuvBlock,256>> prev_codes_v4;
    std::vector<std::array<CPYuvBlock,256>> prev_codes_v1;
    uint64_t frame_count = 0;
    uint inter_frames_left = 0;
    uint frames_pushed = 0;
    
    uint total_macroblocks() {return frame_mbWidth*frame_mbHeight;}
    uint total_blocks() {return total_macroblocks()*4;}
    uint mb_index(uint x,uint y) {return x+y*frame_mbWidth*1;}
    uint blk_index(uint x,uint y) {return x+y*frame_mbWidth*2;}

    CPEncoderState(uint frame_width,uint frame_height, uint max_strips);

    struct StripEncoding {
        enum MBEncType : u8 {
            MB_UNDECIDED,MB_V1,MB_V4,MB_SKIP
        };
        MBEncType strip_type;
        uint ytop,height;
        uint unused_left,unused_right,unused_top,unused_bottom;
        std::vector<MBEncType> mb_types;
        std::vector<u8> mb_v1; // Best V1 vector per macroblock
        std::vector<u8> blk_v4; // Best V4 vector per *Block*
        std::vector<CPYuvBlock> code_v1,code_v4;

        StripEncoding(uint size) {
            mb_types.resize(size,MB_UNDECIDED);
            mb_v1.resize(size);
            blk_v4.resize(size*4);
            // Codebooks are left empty
        }
    };

    // In encoder.cpp
    void doFrame(PacketWriter &packet);
    StripEncoding tryStrip(uint ytop,uint height,bool keyframe);
    void writeStrip(PacketWriter &packet,StripEncoding &strip);
    void writeCodebook(PacketWriter &packet,std::vector<CPYuvBlock> book,bool isV4);


    // In vq_dummy.cpp
    u64 vq_dummy(std::vector<CPYuvBlock> &codebook,uint target_codebook_size,const CPYuvBlock *data,std::vector<uint> &applicable_indices,std::vector<u8> *closest_out);

    // In vq_elbg.cpp
    u64 vq_elbg(std::vector<CPYuvBlock> &codebook,uint target_codebook_size,const CPYuvBlock *data,std::vector<uint> &applicable_indices,std::vector<u8> *closest_out);

    // In vq_fastpnn.cpp
    u64 vq_fastpnn(std::vector<CPYuvBlock> &codebook,uint target_codebook_size,const CPYuvBlock *data,std::vector<uint> &applicable_indices,std::vector<u8> *closest_out);
};

// In vq_elbg.cpp
extern u64 voronoi_partition(const std::vector<CPYuvBlock> &codebook,const CPYuvBlock *data,std::vector<uint> &applicable_indices,
u64 *code_distortion, std::vector<std::vector<uint>> &partition
);

constexpr u8 CHUNK_FRAME_INTRA = 0x00;
constexpr u8 CHUNK_FRAME_INTER = 0x01;

constexpr u8 CHUNK_STRIP_INTRA = 0x10;
constexpr u8 CHUNK_STRIP_INTER = 0x11;

constexpr u8 CHUNK_V4_COLOR_FULL    = 0x20;
constexpr u8 CHUNK_V4_COLOR_PARTIAL = 0x21;
constexpr u8 CHUNK_V1_COLOR_FULL    = 0x22;
constexpr u8 CHUNK_V1_COLOR_PARTIAL = 0x23;
constexpr u8 CHUNK_V4_MONO_FULL     = 0x24;
constexpr u8 CHUNK_V4_MONO_PARTIAL  = 0x25;
constexpr u8 CHUNK_V1_MONO_FULL     = 0x26;
constexpr u8 CHUNK_V1_MONO_PARTIAL  = 0x27;

constexpr u8 CB_PARTIAL_MASK = 0x01;
constexpr u8 CB_V1_MASK      = 0x02;
constexpr u8 CB_MONO_MASK    = 0x04;

constexpr u8 CHUNK_IMAGE_INTRA = 0x30;
constexpr u8 CHUNK_IMAGE_INTER = 0x31;
constexpr u8 CHUNK_IMAGE_V1    = 0x32;

inline u32 square(int x) {
    return x*x;
}

constexpr uint Y_WEIGHT =   1;
constexpr uint U_WEIGHT =   2;
constexpr uint V_WEIGHT =   2;
constexpr uint TOTAL_WEIGHT = Y_WEIGHT*4 + U_WEIGHT + V_WEIGHT;

inline u32 blockDistortion(CPYuvBlock a,CPYuvBlock b) {
    u32 y_dist = square(a.ytl-b.ytl) + square(a.ytr-b.ytr) + square(a.ybl-b.ybl) + square(a.ybr-b.ybr);
    return y_dist * Y_WEIGHT
         + square(a.u - b.u) * U_WEIGHT
         + square(a.v - b.v) * V_WEIGHT;
}

inline u32 macroblockV1Distortion_sub(CPYuvBlock a, u8 y, u8 u, u8 v) {
    u32 y_dist = square(a.ytl - y) + square(a.ytr - y) + square(a.ybl - y) + square(a.ybr - y);
    return y_dist * Y_WEIGHT
         + square(a.u - u) * U_WEIGHT
         + square(a.v - v) * V_WEIGHT;
}

inline u32 macroblockV1Distortion(CPYuvBlock tl,CPYuvBlock tr,CPYuvBlock bl,CPYuvBlock br,CPYuvBlock v1) {
    return macroblockV1Distortion_sub(tl,v1.ytl,v1.u,v1.v)
         + macroblockV1Distortion_sub(tr,v1.ytr,v1.u,v1.v)
         + macroblockV1Distortion_sub(bl,v1.ybl,v1.u,v1.v)
         + macroblockV1Distortion_sub(br,v1.ybr,v1.u,v1.v);
}

inline u64 block_packed(CPYuvBlock a) {
    union {
        CPYuvBlock blk;
        uint64_t vec;
    } lmao;
    lmao.blk = a;
    return lmao.vec;
}
inline CPYuvBlock block_unpacked(u64 a) {
    union {
        CPYuvBlock blk;
        uint64_t vec;
    } lmao;
    lmao.vec = a;
    return lmao.blk;
}
inline u8 block_byte(CPYuvBlock a, uint n) {
    union {
        CPYuvBlock blk;
        uint8_t bytes[8];
    } lmao;
    lmao.blk = a;
    return lmao.bytes[n];
}

inline constexpr u8 clamp_u8(int x) {
    return std::clamp(x,0,255);
}
inline constexpr u16 clamp_u16(int x) {
    return std::clamp(x,0,0xffff);
}

