#include "cinepunk_internal.hpp"
#include <cstdio>

CP_API bool CP_peek_dimensions(uint8_t *data, size_t data_size, unsigned *widthOut, unsigned *heightOut, size_t *sizeOut) {
    if (data_size < 8) return false;
    PacketReader packet(data);
    u8 frame_type = packet.read_u8();
    if (frame_type != CHUNK_FRAME_INTER && frame_type != CHUNK_FRAME_INTRA) return false;
    size_t frame_size = packet.read_u24();
    unsigned width = packet.read_u16();
    unsigned height = packet.read_u16();
    if (sizeOut) *sizeOut = frame_size;
    if (widthOut) *widthOut = width;
    if (heightOut) *heightOut = height;
    return true;
}

CPDecoderState::CPDecoderState(unsigned frame_width, unsigned frame_height)
: frame_mbWidth{frame_width/4},frame_mbHeight{frame_height/4} {
    frame = std::make_unique<CPYuvBlock[]>(frame_width*frame_height/4);
}

CP_API CPDecoderState *CP_create_decoder(unsigned frame_width, unsigned frame_height) {
    return new CPDecoderState(frame_width,frame_height);
}

CP_API void CP_destroy_decoder(CPDecoderState *dec) {
    delete dec;
}

CP_API void CP_set_decoder_debug(CPDecoderState *dec,uint32_t flags) {
    dec->debug_flags = flags;
}

static CPYuvBlock bullshit_code_v4(u8 i) {
    return {
        .u   = 64,
        .v   = clamp_u8(128+29 - ((i&15)*4)),
        .ytl = clamp_u8((i&0xF0)+8),
        .ytr = clamp_u8((i&0xF0)+8),
        .ybl = clamp_u8((i&0xF0)+8),
        .ybr = clamp_u8((i&0xF0)+8),
    };
}

static CPYuvBlock bullshit_code_v1(u8 i) {
    return {
        .u   = 192,
        .v   = clamp_u8(128+29 - ((i&15)*4)),
        .ytl = clamp_u8((i&0xF0)+8),
        .ytr = clamp_u8((i&0xF0)+8),
        .ybl = clamp_u8((i&0xF0)+8),
        .ybr = clamp_u8((i&0xF0)+8),
    };
}

void CPDecoderState::do_decode(const uint8_t *data,size_t data_size) {
    assert(data_size >= 16);
    PacketReader packet(data);
    u8 frame_type = packet.read_u8();
    assert(frame_type == CHUNK_FRAME_INTER || frame_type == CHUNK_FRAME_INTRA);
    size_t frame_size = packet.read_u24();
    assert(frame_size == data_size);
    uint frame_width = packet.read_u16();
    assert(frame_mbWidth*4 == frame_width);
    uint frame_height = packet.read_u16();
    assert(frame_mbHeight*4 == frame_height);
    uint strip_count = packet.read_u16();

    if (codes_v1.size() < strip_count) codes_v1.resize(strip_count);
    if (codes_v4.size() < strip_count) codes_v4.resize(strip_count);

    uint prev_ybottom = 0;

    for (uint stripno=0;stripno<strip_count;stripno++) {
        // Read strip header
        auto strip_begin = packet.ptr;
        u8 strip_type = packet.read_u8();
        assert(strip_type == CHUNK_STRIP_INTER || strip_type == CHUNK_STRIP_INTRA);
        size_t strip_size = packet.read_u24();
        uint ytop = packet.read_u16();
        uint xstart = packet.read_u16();
        uint ybottom = packet.read_u16();
        uint xend = packet.read_u16();
        assert(xstart < xend);
        assert(xstart < frame_mbWidth*4);
        assert(xend <= frame_mbWidth*4);
        assert(xstart%4 == 0);
        assert(xend%4 == 0);
        if (ytop == 0) {
            ytop = prev_ybottom;
            ybottom += ytop;
        }
        prev_ybottom = ybottom;
        assert(ytop < ybottom);
        assert(ytop < frame_mbHeight*4);
        assert(ybottom <= frame_mbHeight*4);
        assert(ytop%4 == 0);
        assert(ybottom%4 == 0);

        while(strip_begin+strip_size > packet.ptr) {
            auto chunk_begin = packet.ptr;
            u8 chunk_type = packet.read_u8();
            size_t chunk_size = packet.read_u24();
            switch(chunk_type) {
            case CHUNK_V4_COLOR_FULL   :
            case CHUNK_V4_COLOR_PARTIAL:
            case CHUNK_V1_COLOR_FULL   :
            case CHUNK_V1_COLOR_PARTIAL:
            case CHUNK_V4_MONO_FULL    :
            case CHUNK_V4_MONO_PARTIAL :
            case CHUNK_V1_MONO_FULL    :
            case CHUNK_V1_MONO_PARTIAL :
                // Codebook chunks...
            {
                auto &codebook = (chunk_type&CB_V1_MASK ? codes_v1 : codes_v4)[stripno];
                uint i = 0;
                BitstreamReader bitstream(packet);
                while (chunk_begin+chunk_size > packet.ptr) {
                    if (!(chunk_type&CB_PARTIAL_MASK) || bitstream.read_bit()) {
                        codebook[i].ytl = bitstream.read_u8();
                        codebook[i].ytr = bitstream.read_u8();
                        codebook[i].ybl = bitstream.read_u8();
                        codebook[i].ybr = bitstream.read_u8();
                        if (chunk_type&CB_MONO_MASK) {
                            codebook[i].u = 128;
                            codebook[i].v = 128;
                        } else {
                            codebook[i].u = bitstream.read_u8()^128;
                            codebook[i].v = bitstream.read_u8()^128;
                        }
                    }
                    i++;
                }
                assert(chunk_begin+chunk_size == packet.ptr);
            } break;
            case CHUNK_IMAGE_INTRA:
            case CHUNK_IMAGE_INTER:
            case CHUNK_IMAGE_V1:
            {
                BitstreamReader bitstream(packet);
                for (uint y=ytop/4;y<ybottom/4;y++) {
                    for (uint x=xstart/4;x<xend/4;x++) {
                        if (chunk_type != CHUNK_IMAGE_INTER || bitstream.read_bit()) {
                            // Not skipped
                            if (chunk_type == CHUNK_IMAGE_V1 || !bitstream.read_bit()) {
                                // V1 code
                                auto code = (debug_flags & CP_DECDEBUG_CRYPTOMATTE)
                                    ? bullshit_code_v1(bitstream.read_u8())
                                    : codes_v1[stripno][bitstream.read_u8()];
                                frame[blk_index(x*2+0,y*2+0)] = {
                                    .u = code.u,.v = code.v,.ytl = code.ytl,.ytr = code.ytl,.ybl = code.ytl,.ybr = code.ytl
                                };
                                frame[blk_index(x*2+1,y*2+0)] = {
                                    .u = code.u,.v = code.v,.ytl = code.ytr,.ytr = code.ytr,.ybl = code.ytr,.ybr = code.ytr,
                                };
                                frame[blk_index(x*2+0,y*2+1)] = {
                                    .u = code.u,.v = code.v,.ytl = code.ybl,.ytr = code.ybl,.ybl = code.ybl,.ybr = code.ybl,
                                };
                                frame[blk_index(x*2+1,y*2+1)] = {
                                    .u = code.u,.v = code.v,.ytl = code.ybr,.ytr = code.ybr,.ybl = code.ybr,.ybr = code.ybr,
                                };
                            } else {
                                
                                // V4 codes
                                if (debug_flags & CP_DECDEBUG_CRYPTOMATTE) {
                                    frame[blk_index(x*2+0,y*2+0)] = bullshit_code_v4(bitstream.read_u8());
                                    frame[blk_index(x*2+1,y*2+0)] = bullshit_code_v4(bitstream.read_u8());
                                    frame[blk_index(x*2+0,y*2+1)] = bullshit_code_v4(bitstream.read_u8());
                                    frame[blk_index(x*2+1,y*2+1)] = bullshit_code_v4(bitstream.read_u8());
                                } else {
                                    frame[blk_index(x*2+0,y*2+0)] = codes_v4[stripno][bitstream.read_u8()];
                                    frame[blk_index(x*2+1,y*2+0)] = codes_v4[stripno][bitstream.read_u8()];
                                    frame[blk_index(x*2+0,y*2+1)] = codes_v4[stripno][bitstream.read_u8()];
                                    frame[blk_index(x*2+1,y*2+1)] = codes_v4[stripno][bitstream.read_u8()];
                                }
                            }
                        } else {
                            // Do nothing
                        }
                    }
                }
                assert(chunk_begin+chunk_size == packet.ptr);
            } break;
            default:
                fprintf(stderr,"Bad chunk type %08X\n",chunk_type);
                assert(false);
                break;
            }
        }
    }
}

CP_API void CP_decode_frame(CPDecoderState *dec,const uint8_t *data,size_t data_size,CPColorType ctype,void *frameOut) {

    dec->do_decode(data,data_size);    

    switch (ctype) {
    case CP_RGB24:
        CP_yuv2rgb((u8 *)frameOut,dec->frame.get(),dec->frame_mbWidth*2,dec->frame_mbHeight*2);
        break;
    case CP_GRAY:
        CP_yuv2gray((u8 *)frameOut,dec->frame.get(),dec->frame_mbWidth*2,dec->frame_mbHeight*2);
        break;
    case CP_YUVBLOCK:
        memcpy(frameOut,dec->frame.get(),dec->frame_mbWidth*dec->frame_mbHeight*4*sizeof(CPYuvBlock));
    default:
        assert(false);
        break;
    }
}


