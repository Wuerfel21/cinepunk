#include "cinepunk_internal.hpp"

u64 CPEncoderState::vq_elbg(std::vector<CPYuvBlock> &codebook,uint target_codebook_size,const CPYuvBlock *data,const std::vector<uint> &applicable_indices,std::vector<u8> &closest_out) {

    assert(target_codebook_size>=1);
    assert(target_codebook_size<=256);

    uint iteration_left = codebook.size() == target_codebook_size ? 10 : 2;

    if (codebook.empty()) {
        // Generate initial codeword. Doesn't matter what it is.
        codebook.push_back({0});
    }

    for (;;) {
        std::vector<u64> code_distortion(codebook.size(),0);
        std::vector<std::vector<uint>> partition(codebook.size());
        // Do Voronoi Partition
        // i.e. find closest codeword to each vector
        for(uint i:applicable_indices) {
            auto vec = data[i];
            u8 best_code = 0; // Doesn't actually need initial value
            u32 lowest_distortion = UINT32_MAX;
            for (uint j=0;j<codebook.size();j++) {
                u32 distortion = blockDistortion(vec,codebook[j]);
                if (distortion<lowest_distortion) {
                    lowest_distortion = distortion;
                    best_code = j;
                }
            }
            closest_out[i] = best_code;
            code_distortion[best_code] += lowest_distortion;
            partition[best_code].push_back(i);
        }

        // Find centroid of each codeword's partition and move it there
        for(uint i=0;i<codebook.size();i++) {
            if (partition[i].empty()) continue;
            u32 ytl = 0,ytr = 0,ybl = 0,ybr = 0,u = 0,v = 0;
            u32 total_weight = 0;
            for (uint j:partition[i]) {
                auto block = data[j];
                ytl += block.ytl * block.weight;
                ytr += block.ytr * block.weight;
                ybl += block.ybl * block.weight;
                ybr += block.ybr * block.weight;
                u   += block.u   * block.weight;
                v   += block.v   * block.weight;
                total_weight += block.weight;
            }
            codebook[i] = {
                .ytl = u8((ytl+(total_weight>>1))/total_weight),
                .ytr = u8((ytr+(total_weight>>1))/total_weight),
                .ybl = u8((ybl+(total_weight>>1))/total_weight),
                .ybr = u8((ybr+(total_weight>>1))/total_weight),
                .u   = u8((u  +(total_weight>>1))/total_weight),
                .v   = u8((v  +(total_weight>>1))/total_weight),
            };
        }


        if (codebook.size()*2>target_codebook_size) {
            if (!(--iteration_left)) break;
        } else {
            if (--iteration_left) continue;
            // Split codewords to embiggen codebook;
            uint oldsize = codebook.size();
            for (uint i=0;i<oldsize;i++) {
                // TODO: better pertubation vector ?
                auto code = codebook[i];
                codebook.push_back({
                    .ytl = clamp_u8(code.ytl + 1),
                    .ytr = clamp_u8(code.ytr - 1),
                    .ybl = clamp_u8(code.ybl - 1),
                    .ybr = clamp_u8(code.ybr + 1),
                    .u   = clamp_u8(code.u   + 1),
                    .v   = clamp_u8(code.v   + 1),
                });
                codebook[i] = {
                    .ytl = clamp_u8(code.ytl - 1),
                    .ytr = clamp_u8(code.ytr + 1),
                    .ybl = clamp_u8(code.ybl + 1),
                    .ybr = clamp_u8(code.ybr - 1),
                    .u   = clamp_u8(code.u   - 1),
                    .v   = clamp_u8(code.v   - 1),
                };
            }
            iteration_left = codebook.size() == target_codebook_size ? 10 : 2;
        }   
        
    }

    // Compute final distortion?
    u64 distortion_total = 0;
    for(uint i:applicable_indices) {
        distortion_total += blockDistortion(data[i],codebook[closest_out[i]]);
    }
    return distortion_total;
}
