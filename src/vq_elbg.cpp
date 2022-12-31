#include "cinepunk_internal.hpp"

constexpr uint lbg_iterations = 200;
constexpr uint split_iterations = 2;



static void __attribute__((noinline)) voronoi_partition(
    const std::vector<CPYuvBlock> &codebook,const CPYuvBlock *data,std::vector<uint> &applicable_indices,
    std::vector<u8> &closest_out, u64 *code_distortion, std::vector<std::vector<uint>> &partition
) {
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
}

static __attribute__((noinline)) CPYuvBlock calculate_centroid(
    const CPYuvBlock *data, const std::vector<uint> &partition
) {
    // Find centroid of each codeword's partition and move it there
    u32 ytl = 0,ytr = 0,ybl = 0,ybr = 0,u = 0,v = 0;
    u32 total_weight = 0;
    for (uint j:partition) {
        auto block = data[j];
        ytl += block.ytl * block.weight;
        ytr += block.ytr * block.weight;
        ybl += block.ybl * block.weight;
        ybr += block.ybr * block.weight;
        u   += block.u   * block.weight;
        v   += block.v   * block.weight;
        total_weight += block.weight;
    }
    return {
        .u   = u8((u  +(total_weight>>1))/total_weight),
        .v   = u8((v  +(total_weight>>1))/total_weight),
        .ytl = u8((ytl+(total_weight>>1))/total_weight),
        .ytr = u8((ytr+(total_weight>>1))/total_weight),
        .ybl = u8((ybl+(total_weight>>1))/total_weight),
        .ybr = u8((ybr+(total_weight>>1))/total_weight),
    };
}

#ifdef CINEPUNK_AVX2

static void __attribute__((noinline)) voronoi_partition_AVX2(
    const std::vector<CPYuvBlock> &codebook,const CPYuvBlock *data,std::vector<uint> &applicable_indices,
    std::vector<u8> &closest_out, u64 *code_distortion, std::vector<std::vector<uint>> &partition
) {
    __m256i weights = _mm256_set_epi32(1,1,1,1,V_WEIGHT,U_WEIGHT,0,0);
    applicable_indices.reserve((applicable_indices.size()+7)&~7);
    for (uint i=0;i<applicable_indices.size();i+=8) {
        uint use = std::min(8u,uint(applicable_indices.size()-i));
        #if 1
        // Minor hack: remove branches by accessing vector beyond nominal size
        // (see reserve call above)
        __m256i pack10 = _mm256_cvtepu8_epi16(_mm_set_epi64x(
            block_packed(data[applicable_indices[i+1]]),
            block_packed(data[applicable_indices[i+0]])));
        __m256i pack32 = _mm256_cvtepu8_epi16(_mm_set_epi64x(
            block_packed(data[applicable_indices[i+3]]),
            block_packed(data[applicable_indices[i+2]])));
        __m256i pack54 = _mm256_cvtepu8_epi16(_mm_set_epi64x(
            block_packed(data[applicable_indices[i+5]]),
            block_packed(data[applicable_indices[i+4]])));
        __m256i pack76 = _mm256_cvtepu8_epi16(_mm_set_epi64x(
            block_packed(data[applicable_indices[i+7]]),
            block_packed(data[applicable_indices[i+6]])));
        #else
        __m256i pack10 = _mm256_cvtepu8_epi16(_mm_set_epi64x(
            use >  1 ? block_packed(data[applicable_indices[i+1]]) : 0,
                       block_packed(data[applicable_indices[i+0]])    ));
        __m256i pack32 = _mm256_cvtepu8_epi16(_mm_set_epi64x(
            use >  3 ? block_packed(data[applicable_indices[i+3]]) : 0,
            use >  2 ? block_packed(data[applicable_indices[i+2]]) : 0));
        __m256i pack54 = _mm256_cvtepu8_epi16(_mm_set_epi64x(
            use >  5 ? block_packed(data[applicable_indices[i+5]]) : 0,
            use >  4 ? block_packed(data[applicable_indices[i+4]]) : 0));
        __m256i pack76 = _mm256_cvtepu8_epi16(_mm_set_epi64x(
            use >  7 ? block_packed(data[applicable_indices[i+7]]) : 0,
            use >  6 ? block_packed(data[applicable_indices[i+6]]) : 0));
        #endif


        __m256i lowest_distortion = _mm256_set1_epi32(INT32_MAX);
        __m256i best_code = _mm256_set1_epi32(0);

        for (uint j=0;j<codebook.size();j++) {
            __m256i code_pack = _mm256_cvtepu8_epi16(_mm_set1_epi64x(block_packed(codebook[j])));
            __m256i diff10 = _mm256_sub_epi16(code_pack,pack10);
            __m256i diff32 = _mm256_sub_epi16(code_pack,pack32);
            __m256i diff54 = _mm256_sub_epi16(code_pack,pack54);
            __m256i diff76 = _mm256_sub_epi16(code_pack,pack76);
            diff10 = _mm256_mullo_epi16(diff10,diff10);
            diff32 = _mm256_mullo_epi16(diff32,diff32);
            diff54 = _mm256_mullo_epi16(diff54,diff54);
            diff76 = _mm256_mullo_epi16(diff76,diff76);

            __m256i diff0 = _mm256_mullo_epi32(_mm256_cvtepu16_epi32(_mm256_extractf128_si256(diff10,0)),weights);
            __m256i diff1 = _mm256_mullo_epi32(_mm256_cvtepu16_epi32(_mm256_extractf128_si256(diff10,1)),weights);
            __m256i diff2 = _mm256_mullo_epi32(_mm256_cvtepu16_epi32(_mm256_extractf128_si256(diff32,0)),weights);
            __m256i diff3 = _mm256_mullo_epi32(_mm256_cvtepu16_epi32(_mm256_extractf128_si256(diff32,1)),weights);
            __m256i diff4 = _mm256_mullo_epi32(_mm256_cvtepu16_epi32(_mm256_extractf128_si256(diff54,0)),weights);
            __m256i diff5 = _mm256_mullo_epi32(_mm256_cvtepu16_epi32(_mm256_extractf128_si256(diff54,1)),weights);
            __m256i diff6 = _mm256_mullo_epi32(_mm256_cvtepu16_epi32(_mm256_extractf128_si256(diff76,0)),weights);
            __m256i diff7 = _mm256_mullo_epi32(_mm256_cvtepu16_epi32(_mm256_extractf128_si256(diff76,1)),weights);

            __m256i sum10 = _mm256_hadd_epi32(diff0,diff1);
            __m256i sum32 = _mm256_hadd_epi32(diff2,diff3);
            __m256i sum54 = _mm256_hadd_epi32(diff4,diff5);
            __m256i sum76 = _mm256_hadd_epi32(diff6,diff7);
            // {0a+0b,0c+0d,1a+1b,1c+1d , 0e+0f,0g+0h,1e+1f,1g+1h}
            __m256i sum3210 = _mm256_hadd_epi32(sum10,sum32);
            __m256i sum7654 = _mm256_hadd_epi32(sum54,sum76);
            // {0a+0b+0c+0d,1a+1b+1c+1d,2a+2b+2c+2d,3a+3b+3c+3d , 0e+0f+0g+0h,1e+1f+1g+1h,2e+2f+2g+2h,3e+3f+3g+3h}
            __m128i sumlow = _mm_add_epi32(_mm256_extractf128_si256(sum3210,1),_mm256_extractf128_si256(sum3210,0));
            __m128i sumhi = _mm_add_epi32(_mm256_extractf128_si256(sum7654,1),_mm256_extractf128_si256(sum7654,0));
            __m256i distortion = _mm256_set_m128i(sumhi,sumlow);

            __m256i better_mask = _mm256_cmpgt_epi32(lowest_distortion,distortion);
            lowest_distortion = _mm256_blendv_epi8(lowest_distortion,distortion,better_mask);
            best_code = _mm256_blendv_epi8(best_code,_mm256_set1_epi32(j),better_mask);
        }
        for (uint sub=0;sub<use;sub++) {
            u32 code = _mm256_extract_epi32(best_code,sub);
            auto index = applicable_indices[i+sub];
            closest_out[index] = u8(code);
            code_distortion[code] += _mm256_extract_epi32(lowest_distortion,sub);
            partition[code].push_back(index);
        }
    }
}

static CPYuvBlock __attribute__((noinline)) calculate_centroid_AVX2(
    const CPYuvBlock *data, const std::vector<uint> &partition
) {
    // Find centroid of each codeword's partition and move it there
    __m256i total = _mm256_set1_epi32(0);
    for (uint j:partition) {
        auto block = _mm256_cvtepu8_epi32(_mm_set_epi64x(0,block_packed(data[j])));
        auto weight = _mm256_broadcastd_epi32(_mm256_extractf128_si256(block,0));
        block = _mm256_insert_epi32(block,1,0);
        total = _mm256_add_epi32(total,_mm256_mullo_epi32(block,weight));
    }
    int32_t total_weight = _mm256_extract_epi32(total,0);
    total = _mm256_add_epi32(total,_mm256_srai_epi32(_mm256_broadcastd_epi32(_mm256_extractf128_si256(total,0)),1));
    return {
        .u   = u8(_mm256_extract_epi32(total,2)/total_weight),
        .v   = u8(_mm256_extract_epi32(total,3)/total_weight),
        .ytl = u8(_mm256_extract_epi32(total,4)/total_weight),
        .ytr = u8(_mm256_extract_epi32(total,5)/total_weight),
        .ybl = u8(_mm256_extract_epi32(total,6)/total_weight),
        .ybr = u8(_mm256_extract_epi32(total,7)/total_weight),
    };
}

#endif


u64 CPEncoderState::vq_elbg(std::vector<CPYuvBlock> &codebook,uint target_codebook_size,const CPYuvBlock *data, std::vector<uint> &applicable_indices,std::vector<u8> &closest_out) {

    assert(target_codebook_size>=1);
    assert(target_codebook_size<=256);

    uint iteration_left = (codebook.size()*2>target_codebook_size) ? lbg_iterations : split_iterations;

    if (codebook.empty()) {
        // Generate initial codeword. Doesn't matter what it is.
        codebook.push_back({0});
    }

    std::vector<std::vector<uint>> partition;
    for (;;) {
        partition.resize(codebook.size());
        for (auto p : partition) {
            p.clear();
            p.reserve((applicable_indices.size()/codebook.size())*2);
        }
        u64 code_distortion[256] = {0};
        
        voronoi_partition_AVX2(codebook,data,applicable_indices,closest_out,code_distortion,partition);

        for(uint i=0;i<codebook.size();i++) {
            if (partition[i].empty()) continue;
            codebook[i] = calculate_centroid_AVX2(data,partition[i]);
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
                    .u   = clamp_u8(code.u   + 1),
                    .v   = clamp_u8(code.v   + 1),
                    .ytl = clamp_u8(code.ytl + 1),
                    .ytr = clamp_u8(code.ytr - 1),
                    .ybl = clamp_u8(code.ybl - 1),
                    .ybr = clamp_u8(code.ybr + 1),
                });
                codebook[i] = {
                    .u   = clamp_u8(code.u   - 1),
                    .v   = clamp_u8(code.v   - 1),
                    .ytl = clamp_u8(code.ytl - 1),
                    .ytr = clamp_u8(code.ytr + 1),
                    .ybl = clamp_u8(code.ybl + 1),
                    .ybr = clamp_u8(code.ybr - 1),
                };
            }
            iteration_left = (codebook.size()*2>target_codebook_size) ? lbg_iterations : split_iterations;
        }   
        
    }

    // Compute final distortion?
    u64 distortion_total = 0;
    for(uint i:applicable_indices) {
        distortion_total += blockDistortion(data[i],codebook[closest_out[i]]);
    }
    return distortion_total;
}
