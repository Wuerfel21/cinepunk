#include "cinepunk_internal.hpp"

// Dummy VQ algorithm. Just generates a codebook of grayscale blocks...
u64 CPEncoderState::vq_dummy(std::vector<CPYuvBlock> &codebook,uint target_codebook_size,const CPYuvBlock *data,const std::vector<uint> &applicable_indices,std::vector<u8> &closest_out) {

    assert(target_codebook_size>=2);
    codebook.clear();
    for (uint i=0;i<target_codebook_size;i++) {
        u8 y = (i*255)/(target_codebook_size-1);
        codebook.push_back({.ytl=y,.ytr=y,.ybl=y,.ybr=y,.u=128,.v=128});
    }

    u64 distortion = 0;

    for(uint i:applicable_indices) {
        u8 y = (data[i].ytl+data[i].ytr+data[i].ybl+data[i].ybr+2)>>2;
        u8 code = (y*(target_codebook_size-1)+128)/255;
        closest_out[i] = code;
        distortion += blockDistortion(data[i],codebook[code]);
    }
    return distortion;
}