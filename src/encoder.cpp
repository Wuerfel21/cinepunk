#include "cinepunk_internal.hpp"
#include <cstdio>
#include <thread>
#include <optional>

CPEncoderState::CPEncoderState(unsigned frame_width, unsigned frame_height, unsigned max_strips)
: frame_mbWidth{frame_width/4},frame_mbHeight{frame_height/4},max_strips{max_strips},decode_state{frame_width,frame_height} {
    // Set defaults

    assert(frame_width%4 == 0 && frame_height%4 == 0);

    encoder_flags = 0;
    frame_count = 0;
    next_frame = std::make_unique<CPYuvBlock[]>(total_blocks());
    cur_frame  = std::make_unique<CPYuvBlock[]>(total_blocks());
    cur_frame_v1 = std::make_unique<CPYuvBlock[]>(total_macroblocks());
    skip_mb_distortion = std::make_unique<u32[]>(total_macroblocks());
    prev_codes_v4.resize(max_strips);
    prev_codes_v1.resize(max_strips);
}


CP_API CPEncoderState *CP_create_encoder(unsigned frame_width, unsigned frame_height, unsigned max_strips) {
    return new CPEncoderState(frame_width,frame_height,max_strips);
}

CP_API void CP_destroy_encoder(CPEncoderState *enc) {
    delete enc;
}

CP_API size_t CP_get_buffer_size(CPEncoderState *enc) {
    return CP_BUFFER_SIZE(enc->frame_mbWidth*4,enc->frame_mbHeight*4,enc->max_strips);
}

CP_API void CP_set_encflags(CPEncoderState *enc,uint32_t flags) {
    enc->encoder_flags |= flags;
}

CP_API void CP_clear_encflags(CPEncoderState *enc,uint32_t flags) {
    enc->encoder_flags &= ~flags;
}

CP_API void CP_set_quality(CPEncoderState *enc,uint32_t factor) {
    enc->quality_factor = factor;
}

CP_API bool CP_push_frame(CPEncoderState *enc,CPColorType ctype,const void *data) {
    if (enc->frames_pushed >= 2) return false;
    enc->frames_pushed++;
    std::swap(enc->cur_frame,enc->next_frame);
    if (data) {
        switch(ctype) {
        case CP_RGB24:
            if (enc->encoder_flags & CP_ENCFLAG_RGB2YUV_FAST) {
                CP_rgb2yuv_fast(enc->next_frame.get(),(uint8_t *)data,enc->frame_mbWidth*2,enc->frame_mbHeight*2);
            } else {
                CP_rgb2yuv_hq  (enc->next_frame.get(),(uint8_t *)data,enc->frame_mbWidth*2,enc->frame_mbHeight*2);
            }
            break;
        case CP_GRAY:
            CP_gray2yuv(enc->next_frame.get(),(uint8_t *)data,enc->frame_mbWidth*2,enc->frame_mbHeight*2);
        case CP_YUVBLOCK:
            memcpy(enc->next_frame.get(),data,4*sizeof(CPYuvBlock)*enc->total_macroblocks());
            break;
        default:
            assert(false);
            break;
        }
    } else {
        // Passing nullptr means "end of file"
        // .. in which case we repeat last frame
        // (is usually black, anyways...)
        memcpy(enc->next_frame.get(),enc->cur_frame.get(),4*sizeof(CPYuvBlock)*enc->total_macroblocks());
    }
    return true;
}

CP_API size_t CP_pull_frame(CPEncoderState *enc,uint8_t *buffer) {
    if (enc->frames_pushed < 2) return 0;
    enc->frames_pushed--;
    PacketWriter packet(buffer);
    enc->doFrame(packet);
    assert(packet.get_length() <= CP_BUFFER_SIZE(enc->frame_mbWidth*4,enc->frame_mbHeight*4,enc->max_strips));
    assert(packet.get_length() > 0);
    return packet.get_length();
}



void CPEncoderState::doFrame(PacketWriter &packet) {

    auto frame_header = packet;
    auto frame_begin = packet.ptr;
    packet.skip(10);
    
    bool keyframe = false;
    if (inter_frames_left == 0 || frame_count == 0) {
        inter_frames_left = 60;
        keyframe = true;
    } else {
        inter_frames_left--;
    }

    // Compute bidirectional differences
    for (uint y=0;y<frame_mbHeight;y++) {
        for (uint x=0;x<frame_mbWidth;x++) {
            u32 forward = blockDistortion(cur_frame[blk_index(x*2+0,y*2+0)],next_frame[blk_index(x*2+0,y*2+0)])
                        + blockDistortion(cur_frame[blk_index(x*2+1,y*2+0)],next_frame[blk_index(x*2+1,y*2+0)])
                        + blockDistortion(cur_frame[blk_index(x*2+0,y*2+1)],next_frame[blk_index(x*2+0,y*2+1)])
                        + blockDistortion(cur_frame[blk_index(x*2+1,y*2+1)],next_frame[blk_index(x*2+1,y*2+1)]);
            u32 backward = keyframe ? UINT32_MAX 
                         : blockDistortion(cur_frame[blk_index(x*2+0,y*2+0)],decode_state.frame[blk_index(x*2+0,y*2+0)])
                         + blockDistortion(cur_frame[blk_index(x*2+1,y*2+0)],decode_state.frame[blk_index(x*2+1,y*2+0)])
                         + blockDistortion(cur_frame[blk_index(x*2+0,y*2+1)],decode_state.frame[blk_index(x*2+0,y*2+1)])
                         + blockDistortion(cur_frame[blk_index(x*2+1,y*2+1)],decode_state.frame[blk_index(x*2+1,y*2+1)]);
            
            skip_mb_distortion[mb_index(x,y)] =  backward;
            // TODO: Tune threshold
            // TODO: Re-enable
            u16 weight = 2;//forward < 16 ? 3 : 2;
            cur_frame[blk_index(x*2+0,y*2+0)].weight = weight;
            cur_frame[blk_index(x*2+1,y*2+0)].weight = weight;
            cur_frame[blk_index(x*2+0,y*2+1)].weight = weight;
            cur_frame[blk_index(x*2+1,y*2+1)].weight = weight;
        }
    } 

    // Create low-res copy of frame for V1 encoding
    CP_yuv_downscale_fast(cur_frame_v1.get(),cur_frame.get(),frame_mbWidth,frame_mbHeight,0);

    uint strips = max_strips;
    uint y1 = 0;
    auto strip_buffer = std::make_unique<StripEncoding[]>(strips);
    std::vector<std::thread> strip_threads;
    for (uint i=0;i<strips;i++) {
        auto height = std::min(frame_mbHeight/strips,frame_mbHeight-y1);
        auto strip_fun = [=,&strip_buffer](){
            strip_buffer[i] = tryStrip(y1,height,keyframe);
        };
        if (use_threads()) strip_threads.emplace_back(strip_fun);
        else strip_fun();
        y1+=height;
    }

    for (uint i=0;i<strips;i++) {
        if (i<strip_threads.size()) strip_threads[i].join();
        writeStrip(packet,strip_buffer[i]);
    }
    
    
    uint framesize = packet.ptr - frame_header.ptr;
    frame_header.write_u8(CHUNK_FRAME_INTRA);
    frame_header.write_u24(framesize);
    frame_header.write_u16(frame_mbWidth*4);
    frame_header.write_u16(frame_mbHeight*4);
    frame_header.write_u16(strips);

    // Decode packet back into buffer
    decode_state.do_decode(frame_begin,packet.ptr - frame_begin);
    frame_count++;
}

[[maybe_unused]]
static void printCBInfo(const std::vector<CPYuvBlock> codebook,const u8 *indices,const CPYuvBlock *frame,uint blocknum) {
        uint histogram[256] = {};
        u64 distortion_total[256] = {};
        for (uint i=0;i<blocknum;i++) {
            auto code = indices[i];
            histogram[code]++;
            distortion_total[code] += blockDistortion(frame[i],codebook[code]);
        }
        for(uint i=0;i<codebook.size();i++) {
            auto code = codebook[i];
            fprintf(stderr,"U: %3u V: %3u Y: %3u %3u %3u %3u D: %8llu H: %8u %s\n",code.u,code.v,code.ytl,code.ytr,code.ybl,code.ybr,distortion_total[i],histogram[i],histogram[i]==0?"DEAD!!!":"");
        }

}

CPEncoderState::StripEncoding
CPEncoderState::tryStrip(uint ytop, uint height, bool keyframe) {
    CPEncoderState::StripEncoding strip(frame_mbWidth*height);
    strip.ytop = ytop;
    strip.height = height;
    uint strip_macroblocks = height*frame_mbWidth;

    auto image_v4 = cur_frame.get()+blk_index(0,ytop*2);
    auto image_v1 = cur_frame_v1.get()+mb_index(0,ytop);

    std::vector<uint> v4_idx,v1_idx;
    bool frame_skip = !keyframe;
    for (uint i=0;i<strip_macroblocks;i++) {
        strip.mb_types[i] = CPEncoderState::StripEncoding::MB_V4;
        auto skip_dist = skip_mb_distortion[ytop*frame_mbWidth+i];
        if (skip_dist>0) frame_skip = false;
        if (skip_dist>0 || keyframe || true) {
            // Funny optimization wherein we ignore unchanged macroblocks
            // TODO disabled because it destroys quality
            v1_idx.push_back(i);
            v4_idx.push_back(i*4+0);
            v4_idx.push_back(i*4+1);
            v4_idx.push_back(i*4+2);
            v4_idx.push_back(i*4+3);
        }
    }
    if (frame_skip) {
        // Special case for fully unchanged frame
        strip.strip_type = CPEncoderState::StripEncoding::MB_SKIP;
        for (uint i=0;i<strip_macroblocks;i++) strip.mb_types[i] = CPEncoderState::StripEncoding::MB_SKIP;
        v1_idx.clear();
        v4_idx.clear();
    } else {
        auto v1_proc = [&](){
            vq_fastpnn(strip.code_v1,256,image_v1,v1_idx,&strip.mb_v1);
            vq_elbg(strip.code_v1,256,image_v1,v1_idx,&strip.mb_v1);
        };
        std::optional<std::thread> v1_thread;
        if (use_threads()) v1_thread = std::thread(v1_proc);
        else v1_proc();
        vq_fastpnn(strip.code_v4,256,image_v4,v4_idx,&strip.blk_v4);
        vq_elbg(strip.code_v4,256,image_v4,v4_idx,&strip.blk_v4);
        if (v1_thread) v1_thread.value().join();

        v4_idx.clear();
        v1_idx.clear();
        // Like in weird card games, lower score is better
        u64 v1only_score_total = 0, intra_score_total = 0, inter_score_total = 0;
        u32 v1only_cost = 8*TOTAL_WEIGHT*quality_factor;
        u32 intra_v1_cost = 9*TOTAL_WEIGHT*quality_factor;
        u32 intra_v4_cost = 33*TOTAL_WEIGHT*quality_factor;
        u32 inter_v1_cost = 10*TOTAL_WEIGHT*quality_factor;
        u32 inter_v4_cost = 34*TOTAL_WEIGHT*quality_factor;
        u32 inter_skip_cost = 1*TOTAL_WEIGHT*quality_factor;
        for (uint y=0;y<height;y++) {
            for (uint x=0;x<frame_mbWidth;x++) {
                u32 v1_distortion = macroblockV1Distortion(
                    image_v4[blk_index(x*2+0,y*2+0)],
                    image_v4[blk_index(x*2+1,y*2+0)],
                    image_v4[blk_index(x*2+0,y*2+1)],
                    image_v4[blk_index(x*2+1,y*2+1)],
                    strip.code_v1[strip.mb_v1[mb_index(x,y)]]);
                u32 v4_distortion = blockDistortion(image_v4[blk_index(x*2+0,y*2+0)],strip.code_v4[strip.blk_v4[blk_index(x*2+0,y*2+0)]])
                                + blockDistortion(image_v4[blk_index(x*2+1,y*2+0)],strip.code_v4[strip.blk_v4[blk_index(x*2+1,y*2+0)]])
                                + blockDistortion(image_v4[blk_index(x*2+0,y*2+1)],strip.code_v4[strip.blk_v4[blk_index(x*2+0,y*2+1)]])
                                + blockDistortion(image_v4[blk_index(x*2+1,y*2+1)],strip.code_v4[strip.blk_v4[blk_index(x*2+1,y*2+1)]]);
                
                u32 skip_distortion = skip_mb_distortion[mb_index(x,ytop+y)];

                u32 v1only_score =       v1_distortion*QUALITY_SCALE + v1only_cost;
                u32 intra_v1_score =     v1_distortion*QUALITY_SCALE + intra_v1_cost;
                u32 intra_v4_score =     v4_distortion*QUALITY_SCALE + intra_v4_cost;
                u32 inter_v1_score =     v1_distortion*QUALITY_SCALE + inter_v1_cost;
                u32 inter_v4_score =     v4_distortion*QUALITY_SCALE + inter_v4_cost;
                u32 inter_skip_score = skip_distortion*QUALITY_SCALE + inter_skip_cost;

                if (!keyframe && inter_skip_score <= inter_v4_score && inter_skip_score <= inter_v1_score) {
                    if (inter_v1_score <= inter_v4_score) {
                        strip.mb_types[mb_index(x,y)] = CPEncoderState::StripEncoding::MB_SKIP_ELSE_V1;
                        intra_score_total += intra_v1_score;
                    } else {
                        strip.mb_types[mb_index(x,y)] = CPEncoderState::StripEncoding::MB_SKIP;
                        intra_score_total += intra_v4_score;
                    }
                    inter_score_total += inter_skip_score;
                } else if (inter_v1_score <= inter_v4_score) {
                    strip.mb_types[mb_index(x,y)] = CPEncoderState::StripEncoding::MB_V1;
                    v1_idx.push_back(mb_index(x,y));
                    intra_score_total += intra_v1_score;
                    inter_score_total += inter_v1_score;
                } else {
                    strip.mb_types[mb_index(x,y)] = CPEncoderState::StripEncoding::MB_V4;
                    v4_idx.push_back(blk_index(x*2+0,y*2+0));
                    v4_idx.push_back(blk_index(x*2+1,y*2+0));
                    v4_idx.push_back(blk_index(x*2+0,y*2+1));
                    v4_idx.push_back(blk_index(x*2+1,y*2+1));
                    intra_score_total += intra_v4_score;
                    inter_score_total += inter_v4_score;
                }
                v1only_score_total += v1only_score;
            }
        }
        // Evalutate scores
        if (v1only_score_total <= inter_score_total && (keyframe || v1only_score_total <= intra_score_total)) {
            // Convert to V1 only
            strip.strip_type = CPEncoderState::StripEncoding::MB_V1;
            v4_idx.clear();
            for (uint i=0;i<strip_macroblocks;i++) {
                if (strip.mb_types[i] != CPEncoderState::StripEncoding::MB_V1) {
                    strip.mb_types[i] = CPEncoderState::StripEncoding::MB_V1;
                    v1_idx.push_back(i);
                }
            }
        } else if (inter_score_total < intra_score_total && !keyframe) {
            // Use inter coding
            strip.strip_type = CPEncoderState::StripEncoding::MB_SKIP;
        } else {
            // Convert to intra coding
            strip.strip_type = CPEncoderState::StripEncoding::MB_V4;
            if (!keyframe) {
                // Keyframe already doesn't have inter coding
                for (uint i=0;i<strip_macroblocks;i++) {
                    if (strip.mb_types[i] == CPEncoderState::StripEncoding::MB_SKIP_ELSE_V1) {
                        strip.mb_types[i] = CPEncoderState::StripEncoding::MB_V1;
                        v1_idx.push_back(i);
                    } else if (strip.mb_types[i] == CPEncoderState::StripEncoding::MB_SKIP) {
                        strip.mb_types[i] = CPEncoderState::StripEncoding::MB_V4;
                        v4_idx.push_back(i*4);
                        v4_idx.push_back(i*4+1);
                        v4_idx.push_back(i*4+frame_mbWidth*2);
                        v4_idx.push_back(i*4+frame_mbWidth*2+1);
                    }
                }
            }
        }
    }
    fprintf(stderr,"V1: %u, V4 : %u, SKIP: %u, %s\n",uint(v1_idx.size()),uint(v4_idx.size()/4),uint(strip_macroblocks-(v1_idx.size()+v4_idx.size()/4)),keyframe ? "KEY" : "");
    if (v4_idx.empty()) {
        strip.code_v4.clear();
    } else {
        vq_elbg(strip.code_v4,256,image_v4,v4_idx,&strip.blk_v4);
    }
    if (v1_idx.empty()) {
        strip.code_v1.clear();
    } else {
        vq_elbg(strip.code_v1,256,image_v1,v1_idx,&strip.mb_v1);
    }

    #if 0
    fprintf(stderr,"V4 info:\n");
    printCBInfo(strip.code_v4,strip.blk_v4.data(),image_v4,strip_macroblocks*4);
    fprintf(stderr,"\nV1 info:\n");
    printCBInfo(strip.code_v4,strip.mb_v1.data(),image_v1,strip_macroblocks);
    fprintf(stderr,"\n");
    #endif


    
    return strip;
}

void CPEncoderState::writeCodebook(PacketWriter &packet,std::vector<CPYuvBlock> book,bool isV4) {
    auto header = packet;
    packet.skip(4);
    for (uint i=0;i<book.size();i++) {
        auto code = book[i];
        packet.write_u8(code.ytl);
        packet.write_u8(code.ytr);
        packet.write_u8(code.ybl);
        packet.write_u8(code.ybr);
        packet.write_u8(code.u^128);
        packet.write_u8(code.v^128);
    }
    uint size = packet.ptr - header.ptr;
    header.write_u8(isV4 ? CHUNK_V4_COLOR_FULL : CHUNK_V1_COLOR_FULL);
    header.write_u24(size);
}

void CPEncoderState::writeStrip(PacketWriter &packet,StripEncoding &strip) {
    // Write strip header later
    auto strip_header = packet;
    packet.skip(12);

    writeCodebook(packet,strip.code_v1,false);
    writeCodebook(packet,strip.code_v4,true);

    auto image_header = packet;
    packet.skip(4);
    BitstreamWriter bitstream(packet);
    for(uint y=0;y<strip.height;y++) {
        for(uint x=0;x<frame_mbWidth;x++) {
            switch (strip.mb_types[mb_index(x,y)]) {
            case CPEncoderState::StripEncoding::MB_V1:
                if (strip.strip_type >= CPEncoderState::StripEncoding::MB_SKIP) bitstream.put_bit(true);
                if (strip.strip_type >= CPEncoderState::StripEncoding::MB_V4) bitstream.put_bit(false);
                bitstream.write_u8(strip.mb_v1[mb_index(x,y)]);
                break;
            case CPEncoderState::StripEncoding::MB_V4:
                if (strip.strip_type >= CPEncoderState::StripEncoding::MB_SKIP) bitstream.put_bit(true);
                assert(strip.strip_type >= CPEncoderState::StripEncoding::MB_V4);
                bitstream.put_bit(true);
                bitstream.write_u8(strip.blk_v4[blk_index(x*2+0,y*2+0)]);
                bitstream.write_u8(strip.blk_v4[blk_index(x*2+1,y*2+0)]);
                bitstream.write_u8(strip.blk_v4[blk_index(x*2+0,y*2+1)]);
                bitstream.write_u8(strip.blk_v4[blk_index(x*2+1,y*2+1)]);
                break;
            case CPEncoderState::StripEncoding::MB_SKIP:
            case CPEncoderState::StripEncoding::MB_SKIP_ELSE_V1:
                assert(strip.strip_type >= CPEncoderState::StripEncoding::MB_SKIP);
                bitstream.put_bit(false);
                break;
            default:
                assert(false);
                break;
            }
        }
    }
    bitstream.flush();
    uint imgsize = packet.ptr - image_header.ptr;
    image_header.write_u8(strip.strip_type >= CPEncoderState::StripEncoding::MB_SKIP ? CHUNK_IMAGE_INTER :
        (strip.strip_type >= CPEncoderState::StripEncoding::MB_V4 ? CHUNK_IMAGE_INTRA : CHUNK_IMAGE_V1));
    image_header.write_u24(imgsize);

    uint stripsize = packet.ptr - strip_header.ptr;
    strip_header.write_u8(strip.strip_type >= CPEncoderState::StripEncoding::MB_SKIP ? CHUNK_STRIP_INTER : CHUNK_STRIP_INTRA);
    strip_header.write_u24(stripsize);
    strip_header.write_u16(strip.ytop*4);
    strip_header.write_u16(0);
    strip_header.write_u16((strip.ytop+strip.height)*4);
    strip_header.write_u16(frame_mbWidth*4);
}