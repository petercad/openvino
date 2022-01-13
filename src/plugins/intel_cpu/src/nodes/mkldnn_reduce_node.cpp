// Copyright (C) 2018-2021 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//

#include "mkldnn_reduce_node.h"

#include "mkldnn_fake_quantize_node.h"
#include "mkldnn_eltwise_node.h"
#include <mkldnn.hpp>
#include <string>
#include <vector>
#include <set>
#include <mkldnn_types.h>
#include <mkldnn_extension_utils.h>
#include "utils/bfloat16.hpp"
#include "emitters/jit_bf16_emitters.hpp"
#include "ie_parallel.hpp"
#include <algorithm>

#include <cpu/x64/jit_generator.hpp>
#include <cpu/x64/jit_uni_eltwise.hpp>
#include <cpu/x64/injectors/jit_uni_depthwise_injector.hpp>
#include <cpu/x64/injectors/jit_uni_quantization_injector.hpp>
#include <cpu/x64/injectors/jit_uni_eltwise_injector.hpp>
#include <ngraph/opsets/opset1.hpp>
#include <ngraph/opsets/opset4.hpp>
#include <common/primitive_hashing_utils.hpp>

using namespace mkldnn;
using namespace MKLDNNPlugin;
using namespace InferenceEngine;
using namespace mkldnn::impl;
using namespace mkldnn::impl::cpu::x64;
using namespace mkldnn::impl::utils;
using namespace Xbyak;

#define SET_SRC_DIM_VALUE(batch, channel, depth, height, width) IB = batch;   \
                                                                IC = channel; \
                                                                ID = depth;   \
                                                                IH = height;  \
                                                                IW = width;
#define SET_DST_DIM_VALUE(batch, channel, depth, height, width) OB = batch;   \
                                                                OC = channel; \
                                                                OD = depth;   \
                                                                OH = height;  \
                                                                OW = width;

#define GET_OFF(field) offsetof(jit_reduce_call_args, field)
#define GET_OFF_POST(field) offsetof(jit_reduce_post_call_args, field)

#define GET_PTR_N_PLN              const uint8_t    *in_ptr_n      = in_ptr       + src_data_size * ib * IC * ID * IH * IW;               \
                                         uint8_t    *out_ptr_n     = out_ptr      + dst_data_size * ob * OC * OD * OH * OW;
#define GET_PTR_NC_PLN             const uint8_t    *in_ptr_nc     = in_ptr_n     + src_data_size * ic * ID * IH * IW;                    \
                                         uint8_t    *out_ptr_nc    = out_ptr_n    + dst_data_size * oc * OD * OH * OW;
#define GET_PTR_NCD_PLN            const uint8_t    *in_ptr_ncd    = in_ptr_nc    + src_data_size * id * IH * IW;                         \
                                         uint8_t    *out_ptr_ncd   = out_ptr_nc   + dst_data_size * od * OH * OW;
#define GET_PTR_NCDH_PLN           const uint8_t    *in_ptr_ncdh   = in_ptr_ncd   + src_data_size * ih * IW;                              \
                                         uint8_t    *out_ptr_ncdh  = out_ptr_ncd  + dst_data_size * oh * OW;
#define GET_PTR_NCD_BASE_PTR_N_PLN const uint8_t    *in_ptr_ncd    = in_ptr_n     + src_data_size * (ic * ID + id) * IH * IW;             \
                                         uint8_t    *out_ptr_ncd   = out_ptr_n    + dst_data_size * (oc * OD + od) * OH * OW;
#define GET_PTR_N_BLK              const uint8_t    *in_ptr_n      = in_ptr       + src_data_size * ib * ICB * ID * IH * IW * blk_size;   \
                                         uint8_t    *out_ptr_n     = out_ptr      + dst_data_size * ob * OCB * OD * OH * OW * blk_size;
#define GET_PTR_NC_BLK             const uint8_t    *in_ptr_nc     = in_ptr_n     + src_data_size * icb * ID * IH * IW * blk_size;        \
                                         uint8_t    *out_ptr_nc    = out_ptr_n    + dst_data_size * ocb * OD * OH * OW * blk_size;
#define GET_PTR_NCD_BLK            const uint8_t    *in_ptr_ncd    = in_ptr_nc    + src_data_size * id * IH * IW * blk_size;              \
                                         uint8_t    *out_ptr_ncd   = out_ptr_nc   + dst_data_size * od * OH * OW * blk_size;
#define GET_PTR_NCDH_BLK           const uint8_t    *in_ptr_ncdh   = in_ptr_ncd   + src_data_size * ih * IW * blk_size;                   \
                                         uint8_t    *out_ptr_ncdh  = out_ptr_ncd  + dst_data_size * oh * OW * blk_size;
#define GET_PTR_NCDHW_BLK          const uint8_t    *in_ptr_ncdhw  = in_ptr_ncdh  + src_data_size * iw * blk_size;                        \
                                         uint8_t    *out_ptr_ncdhw = out_ptr_ncdh + dst_data_size * ow * blk_size;
#define GET_PTR_NCD_BASE_PTR_N_BLK const uint8_t    *in_ptr_ncd    = in_ptr_n     + src_data_size * (icb * ID + id) * IH * IW * blk_size; \
                                         uint8_t    *out_ptr_ncd   = out_ptr_n    + dst_data_size * (ocb * OD + od) * OH * OW * blk_size;

namespace {
struct ReduceKey {
    jit_reduce_config_params jcp;
    mkldnn::post_ops postOps;

    size_t hash() const;
    bool operator==(const ReduceKey& rhs) const;
};

size_t ReduceKey::hash() const {
    using namespace dnnl::impl;
    using namespace dnnl::impl::primitive_hashing;

    size_t seed = 0;
    seed = hash_combine(seed, jcp.layout);
    seed = hash_combine(seed, jcp.reduce_mode);
    seed = hash_combine(seed, jcp.src_dt);
    seed = hash_combine(seed, jcp.dst_dt);
    seed = get_post_op_hash(seed, *postOps.get());

    return seed;
}

bool ReduceKey::operator==(const ReduceKey &rhs) const {
    return jcp.layout == rhs.jcp.layout && jcp.reduce_mode == rhs.jcp.reduce_mode &&
           jcp.src_dt == rhs.jcp.src_dt && jcp.dst_dt == rhs.jcp.dst_dt && *postOps.get() == *rhs.postOps.get();
}
} // namespace

// some utility functions
static inline bool isFloatCompatible(memory::data_type type) {
    return memory::data_type::f32 == type || memory::data_type::bf16 == type;
}

template <cpu_isa_t isa>
struct jit_uni_reduce_kernel_f32 : public jit_uni_reduce_kernel, public jit_generator {
    DECLARE_CPU_JIT_AUX_FUNCTIONS(jit_uni_reduce_kernel_f32)

    explicit jit_uni_reduce_kernel_f32(jit_reduce_config_params jcp)
    : jit_uni_reduce_kernel(jcp), jit_generator() {}

    void create_ker() override {
        jit_generator::create_kernel();
        ker_ = (decltype(ker_))jit_ker();
    }

    void generate() override {
        if (jcp_.reduce_mode == ReduceLogSumExp) {
            exp_injector = std::make_shared<jit_uni_eltwise_injector_f32<isa>>(this, alg_kind::eltwise_exp, 0.f, 0.f, 1);
        }

        if (!mayiuse(avx512_core_bf16) && mayiuse(avx512_core))
            emu_vcvtneps2bf16 = std::make_shared<jit_emu_vcvtneps2bf16>(this, isa);

        this->preamble();

        planar_layout = jcp_.layout == ReduceLayoutType::reduce_ncsp || jcp_.layout == ReduceLayoutType::reduce_nspc;

        mov(reg_src, ptr[reg_params + GET_OFF(src)]);
        mov(reg_dst, ptr[reg_params + GET_OFF(dst)]);
        mov(reg_work_amount, ptr[reg_params + GET_OFF(work_amount)]);
        mov(reg_work_batch, ptr[reg_params + GET_OFF(work_batch)]);
        if (planar_layout)
            mov(reg_reduce_w, ptr[reg_params + GET_OFF(reduce_w)]);

        if (jcp_.reduce_mode == ReduceAnd || jcp_.reduce_mode == ReduceL1 || jcp_.reduce_mode == ReduceMax ||
            jcp_.reduce_mode == ReduceMin || jcp_.reduce_mode == ReduceProd || jcp_.reduce_mode == ReduceOr) {
            mov(reg_table, l_table);
        }

        if (isa == cpu::x64::avx512_common || jcp_.reduce_mode == ReduceAnd || jcp_.reduce_mode == ReduceOr)
            uni_vpxor(vmm_zero, vmm_zero, vmm_zero);

        if ((isa == cpu::x64::avx512_common && jcp_.reduce_mode == ReduceAnd) || jcp_.reduce_mode == ReduceOr) {
            uni_vmovups(vmm_aux, table_val(0));
        }

        reduce_main();
        reduce_tail();

        this->postamble();

        if (!mayiuse(avx512_core_bf16) && mayiuse(avx512_core))
            emu_vcvtneps2bf16->emit_data();

        if (jcp_.reduce_mode == ReduceAnd || jcp_.reduce_mode == ReduceL1 || jcp_.reduce_mode == ReduceMax ||
            jcp_.reduce_mode == ReduceMin || jcp_.reduce_mode == ReduceProd || jcp_.reduce_mode == ReduceOr) {
            prepare_aux_table();
        } else if (jcp_.reduce_mode == ReduceLogSumExp) {
            exp_injector->prepare_table();
        }
    }

private:
    using Vmm = typename conditional3<isa == cpu::x64::sse41, Xbyak::Xmm, isa == cpu::x64::avx2,
            Xbyak::Ymm, Xbyak::Zmm>::type;
    size_t vlen = cpu_isa_traits<isa>::vlen;
    bool planar_layout = false;

    Xbyak::Address table_val(int index) { return ptr[reg_table + index * vlen]; }

    Xbyak::Reg64 reg_src = r8;
    Xbyak::Reg64 reg_dst = r9;
    Xbyak::Reg64 reg_idx = rdx;
    Xbyak::Reg64 reg_work_amount = r10;
    Xbyak::Reg64 reg_reduce_w = r11;
    Xbyak::Reg64 reg_reduce_stride = r12;
    Xbyak::Reg64 reg_work_batch = r13;
    Xbyak::Reg64 reg_table = r14;
    Xbyak::Reg64 reg_params = abi_param1;

    Xbyak::Reg8 reg_tmp_8 = r15b;
    Xbyak::Reg32 reg_tmp_32 = r15d;
    Xbyak::Reg64 reg_tmp_64 = r15;

    Xbyak::Reg64 reg_src_aux = rax;
    Xbyak::Reg64 reg_work_batch_aux = rbx;

    Vmm vmm_aux = Vmm(0);
    Xmm xmm_aux = Xmm(0);
    Vmm vmm_src = Vmm(1);
    Xmm xmm_src = Xmm(1);
    Vmm vmm_dst = Vmm(2);
    Xmm xmm_dst = Xmm(2);
    Vmm vmm_zero = Vmm(3);
    Xmm xmm_zero = Xmm(3);
    Vmm vmm_dst_aux = Vmm(4);
    Xmm xmm_aux1 = Xmm(5);
    Xmm xmm_aux2 = Xmm(6);
    Xmm xmm_aux3 = Xmm(7);
    Vmm vmm_idx = Vmm(8);
    Vmm vmm_mask = Vmm(9);

    const Xbyak::Opmask k_mask = Xbyak::Opmask(1);

    Xbyak::Label l_table;

    std::shared_ptr<jit_emu_vcvtneps2bf16> emu_vcvtneps2bf16;
    std::shared_ptr<jit_uni_eltwise_injector_f32<isa>> exp_injector;

    inline void reduce_main() {
        // ================================================================
        // ***isa: AVX512***
        // ReduceAnd (Logical And)
        // step 1: init dst 0x3f800000 (1.0f)
        //              aux 0x3f800000 (1.0f)
        //             zero 0x00000000 (0.0f)
        // step 2: if src equals 0, set mask bit 0, else set mask bit 1
        // step 3: src = mask bit == 0 ? zero : aux
        // step 4: dst = dst & src
        //                  src    mask_bit    new_src    dst    new_dst
        //         case 1    ~0        1         1.0f     1.0f     1.0f
        //         case 2     0        0         0.0f     1.0f     0.0f
        //         case 3    ~0        1         1.0f     0.0f     0.0f
        //         case 4     0        0         0.0f     0.0f     0.0f
        // step 5: loop: offset src, and do step 2 and step 3
        //
        // ReduceOr (Logical Or)
        // step 1: init dst 0x00000000 (0.0f)
        //              aux 0x3f800000 (1.0f)
        //             zero 0x00000000 (0.0f)
        // step 2: if src equals 0, set mask bit 0, else set mask bit 1
        // step 3: src = mask bit == 0 ? zero : aux
        // step 4: dst = dst | src
        //                  src    mask_bit    new_src    dst    new_dst
        //         case 1     0        0         0.0f     0.0f     0.0f
        //         case 2    ~0        1         1.0f     0.0f     1.0f
        //         case 3     0        0         0.0f     1.0f     1.0f
        //         case 4    ~0        1         1.0f     1.0f     1.0f
        // step 5: loop: offset src, and do step 2 and step 3
        // ================================================================
        // ***isa: OTHER***
        // ReduceAnd (Logical And)
        // step 1: init dst 0x3f800000 (1.0f)
        // step 2: if src equals 0, set it 0x00000000, else set 0xffffffff
        // step 3: dst = dst & src
        //         0x3f800000 = 0x3f800000 & 0xffffffff (result: 1.0f)
        //         0x00000000 = 0x3f800000 & 0x00000000 (result: 0.0f)
        //         0x00000000 = 0x00000000 & 0xffffffff (result: 0.0f)
        //         0x00000000 = 0x00000000 & 0x00000000 (result: 0.0f)
        // step 4: loop: offset src, and do step 2 and step 3
        //
        // ReduceOr (Logical Or)
        // step 1: init dst 0x00000000 (0.0f)
        //              aux 0x3f800000 (1.0f)
        // step 2: dst = dst | src
        //         0x00000000 = 0x00000000 | 0x00000000
        //                  A = 0x00000000 | A
        //                  A =          A | 0x00000000
        //                  C =          A | B
        // (A, B stand for number other than 0x00000000)
        // step 3: loop: offset src, and do step 2
        // step 4: if dst equals 0, set it 0x00000000, else set 0xffffffff
        // step 5: dst = dst & aux
        //         0x00000000 = 0x00000000 & 0x3f800000 (result: 0.0f)
        //         0x3f800000 = 0xffffffff & 0x3f800000 (result: 1.0f)
        // ================================================================
        Xbyak::Label reduce_to_vector_label;
        Xbyak::Label reduce_to_scalar_label;
        Xbyak::Label reduce_to_gather_label;
        Xbyak::Label reduce_main_end_label;
        if (planar_layout) {
            cmp(reg_work_batch, 0);
            je(reduce_to_gather_label, T_NEAR);

            cmp(reg_reduce_w, 1); // planar layout reducing W
            je(reduce_to_scalar_label, T_NEAR);
        }

        // store vmm_dst directly into memory after reducing
        // cases: [planar layout reducing other dimensions but W] [blocked layout]
        L(reduce_to_vector_label);
        {
            int step = vlen / sizeof(float) < 8 ? 8 : vlen / sizeof(float);
            cmp(reg_work_amount, step);
            jl(reduce_main_end_label, T_NEAR); //avoid illegal loading and storing

            if (jcp_.reduce_mode == ReduceL1) {
                uni_vmovups(vmm_aux, table_val(1));
            }

            // load
            load_dst_vector();

            // reduce
            reduce_kernel();

            // store
            store_dst_vector();

            jmp(reduce_main_end_label, T_NEAR);
        }

        // reduce vector in vmm_dst to be a scalar before store into memory
        // cases: [planar layout reducing W]
        L(reduce_to_scalar_label);
        {
            // init dst, dst loading is embedded in horiz_reduce_store
            switch (jcp_.reduce_mode) {
                case ReduceAnd:
                case ReduceProd:
                    uni_vmovups(vmm_dst, table_val(0));
                    break;
                case ReduceL1:
                    uni_vmovups(vmm_aux, table_val(1));
                    uni_vpxor(vmm_dst, vmm_dst, vmm_dst);
                    break;
                case ReduceL2:
                case ReduceLogSum:
                case ReduceLogSumExp:
                case ReduceMean:
                case ReduceOr:
                case ReduceSum:
                case ReduceSumSquare:
                    uni_vpxor(vmm_dst, vmm_dst, vmm_dst);
                    break;
                case ReduceMax:
                    if (isFloatCompatible(jcp_.dst_dt))
                        uni_vmovups(vmm_dst, table_val(2));
                    else
                        uni_vmovups(vmm_dst, table_val(4));
                    break;
                case ReduceMin:
                    if (isFloatCompatible(jcp_.dst_dt))
                        uni_vmovups(vmm_dst, table_val(3));
                    else
                        uni_vmovups(vmm_dst, table_val(5));
                    break;
                default:
                    assert(!"unsupported reduce mode");
            }
            // reduce
            reduce_main_loop();
            if (jcp_.reduce_mode == ReduceOr && isa != cpu::x64::avx512_common) {
                uni_cmpneqps(vmm_dst, vmm_dst, vmm_zero);
                uni_vandps(vmm_dst, vmm_dst, vmm_aux);
            }
            // store
            // store after horizontal calculation and calculation with loaded original ptr[reg_dst]
            horiz_reduce_store(vmm_dst, jcp_.dst_dt, true);

            jmp(reduce_main_end_label, T_NEAR);
        }

        // load vmm_src with gather, then store vmm_dst directly into memory after reducing
        // cases: [planar layout reducing small W]
        L(reduce_to_gather_label);
        {
            int step = 1;
            cmp(reg_work_amount, step);
            jl(reduce_main_end_label, T_NEAR); //avoid illegal loading and storing

            mov(reg_idx, ptr[reg_params + GET_OFF(idx)]);
            uni_vmovdqu(vmm_idx, ptr[reg_idx]);

            if (jcp_.reduce_mode == ReduceL1) {
                uni_vmovups(vmm_aux, table_val(1));
            }

            // load
            load_dst_vector();

            // reduce
            Xbyak::Label reduce_loop_label;
            Xbyak::Label reduce_loop_end_label;
            L(reduce_loop_label);
            {
                cmp(reg_work_amount, step);
                jl(reduce_loop_end_label, T_NEAR);

                reduce_gather(vmm_dst, 0);
                if (isa == cpu::x64::sse41) {
                    reduce_gather(vmm_dst_aux, 4 * jcp_.src_data_size);
                }

                add(reg_src, step * jcp_.src_data_size);
                sub(reg_work_amount, step);
                jmp(reduce_loop_label, T_NEAR);
            }
            L(reduce_loop_end_label);

            // store
            store_dst_vector();

            jmp(reduce_main_end_label, T_NEAR);
        }

        L(reduce_main_end_label);
    }

    inline void reduce_tail() {
        if (jcp_.reduce_mode == ReduceL1) {
            uni_vmovups(xmm_aux, table_val(1));
        }

        Xbyak::Label tail_dst_shifted_label;
        Xbyak::Label tail_dst_fixed_label;
        Xbyak::Label reduce_tail_end_label;
        if (planar_layout) {
            cmp(reg_reduce_w, 1);  // planar layout reducing W
            je(tail_dst_fixed_label, T_NEAR);
        }

        // each src scalar reduce to each dst scalar (X1, X2, X3, ...) -> (Y1, Y2, Y3, ...)
        // cases: [planar layout reducing other dimensions but W] [blocked layout concern padding]
        L(tail_dst_shifted_label);
        {
            reduce_kernel_tail();

            jmp(reduce_tail_end_label, T_NEAR);
        }

        // each src scalar reduce to the same dst scalar (X1, X2, X3, ...) -> (Y1)
        // cases: [planar layout reducing W]
        L(tail_dst_fixed_label);
        {
            // load
            load_scalar(xmm_dst, ptr[reg_dst], jcp_.dst_dt);

            Xbyak::Label reduce_loop_label;
            Xbyak::Label reduce_loop_end_label;

            // reduce
            int step = 1;
            L(reduce_loop_label);
            {
                cmp(reg_work_amount, step);
                jl(reduce_loop_end_label, T_NEAR);

                load_scalar(xmm_src, ptr[reg_src], jcp_.src_dt);

                reduce_kernel_scalar(xmm_src, xmm_dst);
                if (jcp_.reduce_mode == ReduceOr) {
                    uni_cmpneqps(xmm_dst, xmm_dst, xmm_zero);
                    uni_vandps(xmm_dst, xmm_dst, xmm_aux);
                }

                add(reg_src, step * jcp_.src_data_size);
                sub(reg_work_amount, step);

                jmp(reduce_loop_label, T_NEAR);
            }
            L(reduce_loop_end_label);

            // store
            store_scalar(ptr[reg_dst], xmm_dst, jcp_.dst_dt);
        }

        L(reduce_tail_end_label);
    }

    inline void init_reg_reduce_stride() {
        mov(reg_reduce_stride, ptr[reg_params + GET_OFF(reduce_stride)]);
        mul_by_const(reg_reduce_stride, reg_tmp_64, jcp_.src_data_size);
    }

    inline void reduce_kernel() {
        Xbyak::Label reduce_label;
        Xbyak::Label reduce_end_label;
        Xbyak::Label reduce_batch_label;
        Xbyak::Label reduce_batch_end_label;

        int step = vlen / sizeof(float) < 8 ? 8 : vlen / sizeof(float);
        cmp(reg_work_batch, 1);
        je(reduce_label, T_NEAR);

        init_reg_reduce_stride();

        L(reduce_batch_label);
        {
            cmp(reg_work_amount, step);
            jl(reduce_end_label, T_NEAR);

            reduce_batch();

            add(reg_src, step * jcp_.src_data_size);
            sub(reg_work_amount, step);
            jmp(reduce_batch_label, T_NEAR);
        }
        L(reduce_batch_end_label);

        L(reduce_label);
        {
            cmp(reg_work_amount, step);
            jl(reduce_end_label, T_NEAR);

            reduce_once();

            add(reg_src, step * jcp_.src_data_size);
            sub(reg_work_amount, step);
            jmp(reduce_label, T_NEAR);
        }
        L(reduce_end_label);
    }

    inline void reduce_once() {
        load_vector(vmm_src, ptr[reg_src], jcp_.src_dt);
        reduce_kernel(vmm_src, vmm_dst);

        if (isa == cpu::x64::sse41) {
            load_vector(vmm_src, ptr[reg_src + 4 * jcp_.src_data_size], jcp_.src_dt);
            reduce_kernel(vmm_src, vmm_dst_aux);
        }
    }

    inline void reduce_batch() {
        mov(reg_src_aux, reg_src);
        mov(reg_work_batch_aux, reg_work_batch);

        Xbyak::Label reduce_batch_loop_label;
        Xbyak::Label reduce_batch_loop_end_label;
        L(reduce_batch_loop_label);
        {
            cmp(reg_work_batch_aux, 1);
            jl(reduce_batch_loop_end_label, T_NEAR);

            load_vector(vmm_src, ptr[reg_src_aux], jcp_.src_dt);
            reduce_kernel(vmm_src, vmm_dst);
            if (isa == cpu::x64::sse41) {
                load_vector(vmm_src, ptr[reg_src_aux + 4 * jcp_.src_data_size], jcp_.src_dt);
                reduce_kernel(vmm_src, vmm_dst_aux);
            }

            add(reg_src_aux, reg_reduce_stride);
            sub(reg_work_batch_aux, 1);
            jmp(reduce_batch_loop_label, T_NEAR);
        }
        L(reduce_batch_loop_end_label);
    }

    inline void reduce_gather(Vmm vmm_dst, int offset) {
        switch (jcp_.src_dt) {
            case memory::data_type::f32:
            case memory::data_type::s32:
                if (isa == cpu::x64::avx512_common) {
                    kxnord(k_mask, k_mask, k_mask);
                    vgatherdps(vmm_src | k_mask, ptr[reg_src + offset + vmm_idx]);
                } else if (isa == cpu::x64::avx2) {
                    uni_vpcmpeqd(vmm_mask, vmm_mask, vmm_mask);
                    vgatherdps(vmm_src, ptr[reg_src + offset + vmm_idx], vmm_mask);
                } else {
                    pack_gathered_vector(vmm_src, vmm_idx, offset, jcp_.src_dt);
                }
                break;
            case memory::data_type::bf16:
            case memory::data_type::s8:
            case memory::data_type::u8:
                pack_gathered_vector(vmm_src, vmm_idx, offset, jcp_.src_dt);
                break;
            default:
                assert(!"unknown src_dt");
        }
        reduce_kernel(vmm_src, vmm_dst);
    }

    inline void pack_gathered_vector(Vmm vmm_val, Vmm vmm_index, int offset, memory::data_type src_dt) {
        sub(rsp, vlen);
        uni_vmovdqu(ptr[rsp], vmm_index);
        int repeats = vlen / sizeof(float);
        for (size_t i = 0; i < repeats; i++) {
            mov(reg_tmp_64.cvt32(), ptr[rsp + i * sizeof(int)]);
            Xbyak::Address table_idx = ptr[reg_src + offset + reg_tmp_64];
            switch (src_dt) {
                case memory::data_type::f32:
                case memory::data_type::s32:
                    mov(reg_tmp_64.cvt32(), table_idx);
                    mov(ptr[rsp + i * sizeof(int)], reg_tmp_64.cvt32());
                    break;
                case memory::data_type::bf16:
                    mov(reg_tmp_64.cvt16(), table_idx);
                    mov(ptr[rsp + i * sizeof(MKLDNNPlugin::bfloat16_t)], reg_tmp_64.cvt16());
                    break;
                case memory::data_type::s8:
                case memory::data_type::u8:
                    mov(reg_tmp_64.cvt8(), table_idx);
                    mov(ptr[rsp + i * sizeof(char)], reg_tmp_64.cvt8());
                    break;
                default:
                    assert(!"unknown src_dt");
            }
        }

        switch (src_dt) {
            case memory::data_type::f32:
            case memory::data_type::s32:
                uni_vmovups(vmm_val, ptr[rsp]);
                break;
            case memory::data_type::bf16:
                uni_vpmovzxwd(vmm_val, ptr[rsp]);
                uni_vpslld(vmm_val, vmm_val, 16);
            break;
            case memory::data_type::s8:
                uni_vpmovsxbd(vmm_val, ptr[rsp]);
                break;
            case memory::data_type::u8:
                uni_vpmovzxbd(vmm_val, ptr[rsp]);
                break;
            default:
                assert(!"unknown src_dt");
        }
        add(rsp, vlen);
    }

    inline void reduce_kernel_tail() {
        Xbyak::Label reduce_label;
        Xbyak::Label reduce_end_label;
        Xbyak::Label reduce_batch_label;
        Xbyak::Label reduce_batch_end_label;

        int step = 1;
        cmp(reg_work_batch, 1);
        je(reduce_label, T_NEAR);

        init_reg_reduce_stride();

        L(reduce_batch_label);
        {
            cmp(reg_work_amount, step);
            jl(reduce_end_label, T_NEAR);

            // load
            load_scalar(xmm_dst, ptr[reg_dst], jcp_.dst_dt);

            // reduce
            reduce_batch_tail();

            // store
            store_scalar(ptr[reg_dst], xmm_dst, jcp_.dst_dt);

            add(reg_dst, step * jcp_.dst_data_size);
            add(reg_src, step * jcp_.src_data_size);
            sub(reg_work_amount, step);

            jmp(reduce_batch_label, T_NEAR);
        }
        L(reduce_batch_end_label);

        L(reduce_label);
        {
            cmp(reg_work_amount, step);
            jl(reduce_end_label, T_NEAR);

            // load
            load_scalar(xmm_dst, ptr[reg_dst], jcp_.dst_dt);

            // reduce
            reduce_batch_tail();

            // store
            store_scalar(ptr[reg_dst], xmm_dst, jcp_.dst_dt);

            add(reg_dst, step * jcp_.dst_data_size);
            add(reg_src, step * jcp_.src_data_size);
            sub(reg_work_amount, step);

            jmp(reduce_label, T_NEAR);
        }
        L(reduce_end_label);
    }

    inline void reduce_once_tail() {
        load_scalar(xmm_src, ptr[reg_src], jcp_.src_dt);
        reduce_kernel_scalar(xmm_src, xmm_dst);
        if (jcp_.reduce_mode == ReduceOr) {
            uni_cmpneqps(xmm_dst, xmm_dst, xmm_zero);
            uni_vandps(xmm_dst, xmm_dst, xmm_aux);
        }
    }

    inline void reduce_batch_tail() {
        mov(reg_src_aux, reg_src);
        mov(reg_work_batch_aux, reg_work_batch);

        Xbyak::Label reduce_batch_loop_label;
        Xbyak::Label reduce_batch_loop_end_label;
        L(reduce_batch_loop_label);
        {
            cmp(reg_work_batch_aux, 1);
            jl(reduce_batch_loop_end_label, T_NEAR);

            load_scalar(xmm_src, ptr[reg_src_aux], jcp_.src_dt);
            reduce_kernel_scalar(xmm_src, xmm_dst);
            if (jcp_.reduce_mode == ReduceOr) {
                uni_cmpneqps(xmm_dst, xmm_dst, xmm_zero);
                uni_vandps(xmm_dst, xmm_dst, xmm_aux);
            }

            add(reg_src_aux, reg_reduce_stride);
            sub(reg_work_batch_aux, 1);
            jmp(reduce_batch_loop_label, T_NEAR);
        }
        L(reduce_batch_loop_end_label);
    }

    inline void reduce_main_loop() {
        Xbyak::Label reduce_loop_label;
        Xbyak::Label reduce_loop_end_label;

        int step = vlen / sizeof(float) < 8 ? 8 : vlen / sizeof(float);
        L(reduce_loop_label);
        {
            cmp(reg_work_amount, step);
            jl(reduce_loop_end_label, T_NEAR);

            load_vector(vmm_src, ptr[reg_src], jcp_.src_dt);
            reduce_kernel(vmm_src, vmm_dst);

            if (isa == cpu::x64::sse41) {
                load_vector(vmm_src, ptr[reg_src + 4 * jcp_.src_data_size], jcp_.src_dt);
                reduce_kernel(vmm_src, vmm_dst);
            }

            add(reg_src, step * jcp_.src_data_size);
            sub(reg_work_amount, step);

            jmp(reduce_loop_label, T_NEAR);
        }
        L(reduce_loop_end_label);
    }

    inline void reduce_kernel(Vmm vmm_src, Vmm vmm_dst) {
        switch (jcp_.reduce_mode) {
            case ReduceAnd:
                if (isa == cpu::x64::avx512_common) {
                    vcmpps(k_mask, vmm_src, vmm_zero, _cmp_neq_uq);
                    vblendmps(vmm_src | k_mask, vmm_zero, vmm_aux);
                } else {
                    uni_cmpneqps(vmm_src, vmm_src, vmm_zero);
                }
                uni_vandps(vmm_dst, vmm_dst, vmm_src);
                break;
            case ReduceL1:
                uni_vandps(vmm_src, vmm_src, vmm_aux);
                uni_vaddps(vmm_dst, vmm_dst, vmm_src);
                break;
            case ReduceLogSum:
            case ReduceMean:
            case ReduceSum:
                uni_vaddps(vmm_dst, vmm_dst, vmm_src);
                break;
            case ReduceMax:
                uni_vmaxps(vmm_dst, vmm_dst, vmm_src);
                break;
            case ReduceMin:
                uni_vminps(vmm_dst, vmm_dst, vmm_src);
                break;
            case ReduceL2:
            case ReduceSumSquare:
                uni_vmulps(vmm_src, vmm_src, vmm_src);
                uni_vaddps(vmm_dst, vmm_dst, vmm_src);
                break;
            case ReduceLogSumExp:
                exp_injector->compute_vector_range(vmm_src.getIdx(), vmm_src.getIdx() + 1);
                uni_vaddps(vmm_dst, vmm_dst, vmm_src);
                break;
            case ReduceOr:
                if (isa == cpu::x64::avx512_common) {
                    vcmpps(k_mask, vmm_src, vmm_zero, _cmp_neq_uq);
                    vblendmps(vmm_src | k_mask, vmm_zero, vmm_aux);
                }
                uni_vorps(vmm_dst, vmm_dst, vmm_src);
                break;
            case ReduceProd:
                uni_vmulps(vmm_dst, vmm_dst, vmm_src);
                break;
            default:
                assert(!"unsupported reduce mode");
        }
    }

    inline void reduce_kernel_scalar(Xmm xmm_src, Xmm xmm_dst) {
        switch (jcp_.reduce_mode) {
            case ReduceAnd:
                uni_cmpneqps(xmm_src, xmm_src, xmm_zero);
                uni_vandps(xmm_dst, xmm_dst, xmm_src);
                break;
            case ReduceL1:
                uni_vandps(xmm_src, xmm_src, xmm_aux);
                uni_vaddps(xmm_dst, xmm_dst, xmm_src);
                break;
            case ReduceLogSum:
            case ReduceMean:
            case ReduceSum:
                uni_vaddps(xmm_dst, xmm_dst, xmm_src);
                break;
            case ReduceMax:
                uni_vmaxps(xmm_dst, xmm_dst, xmm_src);
                break;
            case ReduceMin:
                uni_vminps(xmm_dst, xmm_dst, xmm_src);
                break;
            case ReduceL2:
            case ReduceSumSquare:
                uni_vmulps(xmm_src, xmm_src, xmm_src);
                uni_vaddps(xmm_dst, xmm_dst, xmm_src);
                break;
            case ReduceLogSumExp:
                exp_injector->compute_vector_range(xmm_src.getIdx(), xmm_src.getIdx() + 1);
                uni_vaddps(xmm_dst, xmm_dst, xmm_src);
                break;
            case ReduceOr:
                uni_vorps(xmm_dst, xmm_dst, xmm_src);
                break;
            case ReduceProd:
                uni_vmulps(xmm_dst, xmm_dst, xmm_src);
                break;
            default:
                assert(!"unsupported reduce mode");
        }
    }

    inline void load_dst_vector() {
        load_vector(vmm_dst, ptr[reg_dst], jcp_.dst_dt);
        if (isa == cpu::x64::sse41)
            load_vector(vmm_dst_aux, ptr[reg_dst + 4 * jcp_.dst_data_size], jcp_.dst_dt);
    }

    inline void store_dst_vector() {
        if (jcp_.reduce_mode == ReduceOr && isa != cpu::x64::avx512_common) {
            uni_cmpneqps(vmm_dst, vmm_dst, vmm_zero);
            uni_vandps(vmm_dst, vmm_dst, vmm_aux);

            if (isa == cpu::x64::sse41) {
                uni_cmpneqps(vmm_dst_aux, vmm_dst_aux, vmm_zero);
                uni_vandps(vmm_dst_aux, vmm_dst_aux, vmm_aux);
            }
        }
        store_vector(ptr[reg_dst], vmm_dst, jcp_.dst_dt);
        if (isa == cpu::x64::sse41)
            store_vector(ptr[reg_dst + 4 * jcp_.dst_data_size], vmm_dst_aux, jcp_.dst_dt);
    }

    inline void load_vector(Vmm vmm_src, const Xbyak::Address &op, memory::data_type src_dt) {
        switch (src_dt) {
            case memory::data_type::f32:
            case memory::data_type::s32:
                uni_vmovups(vmm_src, op);
                break;
            case memory::data_type::bf16:
                uni_vpmovzxwd(vmm_src, op);
                uni_vpslld(vmm_src, vmm_src, 16);
                break;
            case memory::data_type::s8:
                uni_vpmovsxbd(vmm_src, op);
                break;
            case memory::data_type::u8:
                uni_vpmovzxbd(vmm_src, op);
                break;
            default:
                assert(!"unknown src_dt");
        }

        if (!isFloatCompatible(src_dt))
            uni_vcvtdq2ps(vmm_src, vmm_src);
    }

    inline void load_scalar(Xmm xmm_src, const Xbyak::Address &op, memory::data_type src_dt) {
        switch (src_dt) {
            case memory::data_type::f32:
            case memory::data_type::s32:
                uni_vmovss(xmm_src, op);
                break;
            case memory::data_type::bf16:
                uni_vpinsrw(xmm_src, xmm_src, op, 0x0);
                uni_vpslld(xmm_src, xmm_src, 16);
                break;
            case memory::data_type::s8:
                movsx(reg_tmp_32, op);
                uni_vmovq(xmm_src, reg_tmp_64);
                break;
            case memory::data_type::u8:
                movzx(reg_tmp_32, op);
                uni_vmovq(xmm_src, reg_tmp_64);
                break;
            default:
                assert(!"unknown src_dt");
        }

        if (!isFloatCompatible(src_dt)) {
            uni_vcvtdq2ps(xmm_src, xmm_src);
        }
    }

    inline void store_vector(const Xbyak::Address &op, Vmm vmm_dst, memory::data_type dst_dt) {
        Xmm xmm_dst = Xmm(vmm_dst.getIdx());
        Ymm ymm_dst = Ymm(vmm_dst.getIdx());

        if (!isFloatCompatible(dst_dt)) {
            uni_vcvtps2dq(vmm_dst, vmm_dst);
        }

        switch (dst_dt) {
            case memory::data_type::f32:
            case memory::data_type::s32:
                uni_vmovups(op, vmm_dst);
                break;
            case memory::data_type::bf16:
                if (mayiuse(avx512_core_bf16))
                    vcvtneps2bf16(ymm_dst, vmm_dst);
                else
                    emu_vcvtneps2bf16->emit_code({static_cast<size_t>(vmm_dst.getIdx())}, {static_cast<size_t>(ymm_dst.getIdx())});
                vmovdqu16(op, ymm_dst);
                break;
            case memory::data_type::s8:
                if (isa == cpu::x64::avx512_common) {
                    vmaxps(vmm_dst, vmm_zero, vmm_dst);
                    vpmovsdb(op, vmm_dst);
                } else {
                    uni_vpackssdw(vmm_dst, vmm_dst, vmm_dst);
                    if (isa != cpu::x64::sse41)
                        vpermq(ymm_dst, ymm_dst, 0x08);
                    uni_vpacksswb(vmm_dst, vmm_dst, vmm_dst);
                    if (isa != cpu::x64::sse41)
                        vmovq(op, xmm_dst);
                    else
                        uni_vmovd(op, xmm_dst);
                }
                break;
            case memory::data_type::u8:
                if (isa == cpu::x64::avx512_common) {
                    vpmovusdb(op, vmm_dst);
                } else {
                    uni_vpackusdw(vmm_dst, vmm_dst, vmm_dst);
                    if (isa != cpu::x64::sse41)
                        vpermq(ymm_dst, ymm_dst, 0x08);
                    uni_vpackuswb(vmm_dst, vmm_dst, vmm_dst);
                    if (isa != cpu::x64::sse41)
                        vmovq(op, xmm_dst);
                    else
                        uni_vmovd(op, xmm_dst);
                }
                break;
            default:
                assert(!"unknown dst_dt");
        }
    }

    inline void store_scalar(const Xbyak::Address &op, Xmm xmm_dst, memory::data_type dst_dt) {
        if (!isFloatCompatible(dst_dt)) {
            uni_vcvtps2dq(xmm_dst, xmm_dst);
        }

        switch (dst_dt) {
            case memory::data_type::f32:
            case memory::data_type::s32:
                uni_vmovss(op, xmm_dst);
                break;
            case memory::data_type::bf16:
                uni_vpsrld(xmm_dst, xmm_dst, 16);
                uni_vpextrw(op, xmm_dst, 0x0);
                break;
            case memory::data_type::s8:
                uni_vpackssdw(xmm_dst, xmm_dst, xmm_dst);
                uni_vpacksswb(xmm_dst, xmm_dst, xmm_dst);
                uni_vmovq(reg_tmp_64, xmm_dst);
                mov(op, reg_tmp_8);
                break;
            case memory::data_type::u8:
                uni_vpackusdw(xmm_dst, xmm_dst, xmm_dst);
                uni_vpackuswb(xmm_dst, xmm_dst, xmm_dst);
                uni_vmovq(reg_tmp_64, xmm_dst);
                mov(op, reg_tmp_8);
                break;
            default:
                assert(!"unknown dst_dt");
        }
    }

    inline void horiz_reduce_store(Vmm vmm_dst, memory::data_type dst_dt, bool load_embedded = false) {
        if (isa == cpu::x64::sse41) {
            horiz_store(vmm_dst, dst_dt, load_embedded);
        } else if (isa == cpu::x64::avx2) {
            Xbyak::Ymm ymm_dst = Xbyak::Ymm(vmm_dst.getIdx());
            vextractf128(xmm_aux1, ymm_dst, 0);
            vextractf128(xmm_aux2, ymm_dst, 1);
            horiz_ps(xmm_aux1, xmm_aux2);
            horiz_store(xmm_aux1, dst_dt, load_embedded);
        } else {
            Xbyak::Zmm zmm_dst = Xbyak::Zmm(vmm_dst.getIdx());
            vextractf32x4(xmm_aux1, zmm_dst, 0);
            vextractf32x4(xmm_aux2, zmm_dst, 1);
            horiz_ps(xmm_aux1, xmm_aux2);
            vextractf32x4(xmm_aux2, zmm_dst, 2);
            vextractf32x4(xmm_aux3, zmm_dst, 3);
            horiz_ps(xmm_aux2, xmm_aux3);
            horiz_ps(xmm_aux1, xmm_aux2);
            horiz_store(xmm_aux1, dst_dt, load_embedded);
        }
    }

    inline void horiz_store(Xbyak::Xmm xmm_dst, memory::data_type dst_dt, bool load_embedded) {
        uni_vmovshdup(xmm_aux3, xmm_dst);          // dst:1,2,3,4; aux3:2,2,4,4
        horiz_ps(xmm_dst, xmm_aux3);               // dst:f(1,2),f(2,2),f(3,4),f(4,4)
        uni_vmovhlps(xmm_aux3, xmm_aux3, xmm_dst); // aux3:f(3,4),f(4,4),4,4
        horiz_ps(xmm_dst, xmm_aux3);               // dst:f(1,2,3,4),...
        if (load_embedded) {
            load_scalar(xmm_aux3, ptr[reg_dst], dst_dt);
            horiz_ps(xmm_dst, xmm_aux3);
        }
        store_scalar(ptr[reg_dst], xmm_dst, dst_dt);
    }

    inline void horiz_ps(const Xmm& xmm, const Operand& op) {
        switch (jcp_.reduce_mode) {
            case ReduceAnd:
                uni_vandps(xmm, xmm, op);
                break;
            case ReduceL1:
            case ReduceL2:
            case ReduceLogSum:
            case ReduceMean:
            case ReduceSum:
            case ReduceSumSquare:
            case ReduceLogSumExp:
                uni_vaddps(xmm, xmm, op);
                break;
            case ReduceMax:
                uni_vmaxps(xmm, xmm, op);
                break;
            case ReduceMin:
                uni_vminps(xmm, xmm, op);
                break;
            case ReduceOr:
                uni_vorps(xmm, xmm, op);
                break;
            case ReduceProd:
                uni_vmulps(xmm, xmm, op);
                break;
            default:
                assert(!"unsupported reduce mode");
        }
    }

    void prepare_aux_table() {
        auto broadcast_int = [&](int val) {
            for (size_t d = 0; d < vlen / sizeof(float); ++d) {
                dd(val);
            }
        };

        align(64);
        L(l_table);

        broadcast_int(aux_vals.float_one);
        broadcast_int(aux_vals.float_abs);
        broadcast_int(aux_vals.float_min);
        broadcast_int(aux_vals.float_max);
        broadcast_int(aux_vals.int32_min);
        broadcast_int(aux_vals.int32_max);
    }

    const struct aux_vals_type {
        int float_one = 0x3f800000; // 1.0f
        int float_abs = 0x7fffffff; // mask to make positive
        int float_min = 0xff7fffff; // float minimum
        int float_max = 0x7f7fffff; // float maximum
        int int32_min = 0xcf000000; // -2^31 presented in float
        int int32_max = 0x4effffff; // 2^31-1 presented in float
    } aux_vals;
};

template <cpu_isa_t isa>
struct jit_uni_reduce_post_kernel_f32 : public jit_uni_reduce_post_kernel, public jit_generator {
    DECLARE_CPU_JIT_AUX_FUNCTIONS(jit_uni_reduce_post_kernel_f32)

    explicit jit_uni_reduce_post_kernel_f32(jit_reduce_config_params jcp, const mkldnn_primitive_attr &attr)
    : jit_uni_reduce_post_kernel(jcp, attr), jit_generator() {}

    void create_ker() override {
        jit_generator::create_kernel();
        ker_ = (decltype(ker_))jit_ker();
    }

    void generate() override {
        const auto &p = attr_.post_ops_;
        for (int i = 0; i < p.len(); i++) {
            auto &post_op = p.entry_[i];
            if (post_op.is_eltwise()) {
                eltwise_injectors.push_back(std::make_shared<jit_uni_eltwise_injector_f32<isa>>(
                        this, post_op.eltwise.alg, post_op.eltwise.alpha, post_op.eltwise.beta, post_op.eltwise.scale));
            } else if (post_op.is_depthwise()) {
                depthwise_injectors.push_back(std::make_shared<jit_uni_depthwise_injector_f32<isa>>(
                        this, post_op.depthwise.alg));
            } else if (post_op.is_quantization()) {
                quantization_injectors.push_back(std::make_shared<jit_uni_quantization_injector_f32<isa>>(
                        this, post_op, vmm_d_weights, vmm_d_bias, reg_d_weights, reg_d_bias));
            }
        }

        if (jcp_.reduce_mode == ReduceLogSum || jcp_.reduce_mode == ReduceLogSumExp) {
            log_injector = std::make_shared<jit_uni_eltwise_injector_f32<isa>>(this, alg_kind::eltwise_log, 0.f, 0.f, 1.f);
        }

        if (!mayiuse(avx512_core_bf16) && mayiuse(avx512_core))
            emu_vcvtneps2bf16 = std::make_shared<jit_emu_vcvtneps2bf16>(this, isa);

        this->preamble();

        planar_layout = jcp_.layout == ReduceLayoutType::reduce_ncsp || jcp_.layout == ReduceLayoutType::reduce_nspc;

        mov(reg_dst, ptr[reg_params + GET_OFF_POST(dst)]);
        mov(reg_work_amount, ptr[reg_params + GET_OFF_POST(work_amount)]);
        mov(reg_channel_size, ptr[reg_params + GET_OFF_POST(channel_size)]);
        mov(reg_divisor, ptr[reg_params + GET_OFF_POST(divisor)]);
        if (!planar_layout)
            mov(reg_reduce_c, ptr[reg_params + GET_OFF_POST(reduce_c)]);
        if (attr_.post_ops_.len() != 0) {
            mov(reg_post_ops_data, ptr[reg_params + GET_OFF_POST(post_op_data)]);
            mov(reg_oc_off, ptr[reg_params + GET_OFF_POST(oc_off)]);
        }

        if (isa == cpu::x64::avx512_common)
            uni_vpxor(vmm_zero, vmm_zero, vmm_zero);

        if (jcp_.layout == ReduceLayoutType::reduce_blocked) {
            reduce_post_main();
        } else if (jcp_.layout == ReduceLayoutType::reduce_nspc && attr_.post_ops_.len() != 0) {
            // the tail of channel dimension should always be concerned during post ops fusing for nspc layout
            Xbyak::Label reduce_nspc_loop_label;
            Xbyak::Label reduce_nspc_loop_end_label;
            mov(reg_total_work_amount, reg_work_amount);
            L(reduce_nspc_loop_label);
            {
                cmp(reg_total_work_amount, 0);
                jle(reduce_nspc_loop_end_label, T_NEAR);

                mov(reg_oc_off, 0);
                mov(reg_work_amount, reg_channel_size);
                reduce_post_main();
                reduce_post_tail();

                sub(reg_total_work_amount, reg_channel_size);
                jmp(reduce_nspc_loop_label, T_NEAR);
            }
            L(reduce_nspc_loop_end_label);
        } else {
            reduce_post_main();
            reduce_post_tail();
        }

        this->postamble();

        if (!mayiuse(avx512_core_bf16) && mayiuse(avx512_core))
            emu_vcvtneps2bf16->emit_data();

        if (jcp_.reduce_mode == ReduceLogSum || jcp_.reduce_mode == ReduceLogSumExp) {
            log_injector->prepare_table();
        }

        for (auto& inj : eltwise_injectors)
            inj->prepare_table();
    }

private:
    using Vmm = typename conditional3<isa == cpu::x64::sse41, Xbyak::Xmm, isa == cpu::x64::avx2,
            Xbyak::Ymm, Xbyak::Zmm>::type;
    size_t vlen = cpu_isa_traits<isa>::vlen;
    bool planar_layout = false;

    Xbyak::Reg64 reg_dst = r8;
    Xbyak::Reg64 reg_work_amount = r9;
    Xbyak::Reg64 reg_total_work_amount = r10;
    Xbyak::Reg64 reg_channel_size = r11;
    Xbyak::Reg64 reg_divisor = r12;
    Xbyak::Reg64 reg_reduce_c = r13;
    Xbyak::Reg64 reg_params = abi_param1;

    Xbyak::Reg8 reg_tmp_8 = r14b;
    Xbyak::Reg32 reg_tmp_32 = r14d;
    Xbyak::Reg64 reg_tmp_64 = r14;

    Xbyak::Reg64 reg_oc_off = rax;
    Xbyak::Reg64 reg_d_weights = rbx;
    Xbyak::Reg64 reg_d_bias = rdx;
    Xbyak::Reg64 reg_post_ops_data = r15;

    Vmm vmm_aux = Vmm(0);
    Xmm xmm_aux = Xmm(0);
    Vmm vmm_dst = Vmm(1);
    Xmm xmm_dst = Xmm(1);
    Vmm vmm_zero = Vmm(2);
    Vmm vmm_dst_aux = Vmm(3);
    Xbyak::Xmm xmm_aux1 = Xbyak::Xmm(4);
    Xbyak::Xmm xmm_aux2 = Xbyak::Xmm(5);
    Xbyak::Xmm xmm_aux3 = Xbyak::Xmm(6);

    Vmm vmm_d_weights = Vmm(7);
    Vmm vmm_d_bias = Vmm(8);

    std::shared_ptr<jit_emu_vcvtneps2bf16> emu_vcvtneps2bf16;
    std::shared_ptr<jit_uni_eltwise_injector_f32<isa>> log_injector;

    std::vector<std::shared_ptr<jit_uni_eltwise_injector_f32<isa>>> eltwise_injectors;
    std::vector<std::shared_ptr<jit_uni_depthwise_injector_f32<isa>>> depthwise_injectors;
    std::vector<std::shared_ptr<jit_uni_quantization_injector_f32<isa>>> quantization_injectors;

    inline void reduce_post_main() {
        Xbyak::Label reduce_channel_label;
        Xbyak::Label reduce_map_label;
        if (planar_layout) {
            jmp(reduce_map_label, T_NEAR);
        } else {
            cmp(reg_reduce_c, 1);
            jne(reduce_map_label, T_NEAR);
        }

        // further reduce channel block since reduce channel batch has already been reduced
        // (X1, X2, X3, X4, X5, X6, X7, X8) -> (Y1, N/A, N/A, N/A, N/A, N/A, N/A, N/A)
        // cases: [blocked layout reducing channel dimensions]
        L(reduce_channel_label);
        {
            Xbyak::Label reduce_loop_label;
            Xbyak::Label reduce_loop_end_label;

            int step = vlen / sizeof(float) < 8 ? 8 : vlen / sizeof(float);
            L(reduce_loop_label);
            {
                cmp(reg_work_amount, step);
                jl(reduce_loop_end_label, T_NEAR);

                // load
                load_vector(vmm_dst, ptr[reg_dst], jcp_.dst_dt);
                if (isa == cpu::x64::sse41)
                    load_vector(vmm_dst_aux, ptr[reg_dst + 4 * jcp_.dst_data_size], jcp_.dst_dt);

                // reduce and store
                horiz_reduce_store(vmm_dst, jcp_.dst_dt);
                if (isa == cpu::x64::sse41)
                    horiz_reduce_store(vmm_dst_aux, jcp_.dst_dt, true);

                add(reg_dst, step * jcp_.dst_data_size);
                sub(reg_work_amount, step);

                jmp(reduce_loop_label, T_NEAR);
            }
            L(reduce_loop_end_label);

            mov(reg_dst, ptr[reg_params + GET_OFF_POST(dst)]);
            mov(reg_work_amount, ptr[reg_params + GET_OFF_POST(work_amount)]);
        }

        // reduce map for value in dst memory
        // cases: [ReduceL2] [ReduceLogSum] [ReduceLogSumExp] [ReduceMean]
        L(reduce_map_label);
        {
            if (jcp_.reduce_mode == ReduceL2 || jcp_.reduce_mode == ReduceMean ||
                jcp_.reduce_mode == ReduceLogSum || jcp_.reduce_mode == ReduceLogSumExp) {
                if (jcp_.reduce_mode == ReduceMean)
                    uni_vbroadcastss(vmm_aux, ptr[reg_divisor]);

                Xbyak::Label reduce_loop_label;
                Xbyak::Label reduce_loop_end_label;

                int step = vlen / sizeof(float) < 8 ? 8 : vlen / sizeof(float);
                L(reduce_loop_label);
                {
                    cmp(reg_work_amount, step);
                    jl(reduce_loop_end_label, T_NEAR);

                    load_vector(vmm_dst, ptr[reg_dst], jcp_.dst_dt);
                    reduce_map_kernel(vmm_dst);
                    if (attr_.post_ops_.len() != 0)
                        apply_post_ops(jcp_.dst_dt, jcp_.layout == ReduceLayoutType::reduce_ncsp);
                    store_vector(ptr[reg_dst], vmm_dst, jcp_.dst_dt);

                    if (isa == cpu::x64::sse41) {
                        load_vector(vmm_dst, ptr[reg_dst + 4 * jcp_.dst_data_size], jcp_.dst_dt);
                        reduce_map_kernel(vmm_dst);
                        if (attr_.post_ops_.len() != 0) {
                            if (jcp_.layout != ReduceLayoutType::reduce_ncsp)
                                add(reg_oc_off, 4 * sizeof(float));
                            apply_post_ops(jcp_.dst_dt, jcp_.layout == ReduceLayoutType::reduce_ncsp);
                            if (jcp_.layout != ReduceLayoutType::reduce_ncsp)
                                sub(reg_oc_off, 4 * sizeof(float));
                        }
                        store_vector(ptr[reg_dst + 4 * jcp_.dst_data_size], vmm_dst, jcp_.dst_dt);
                    }

                    add(reg_dst, step * jcp_.dst_data_size);
                    if (jcp_.layout == ReduceLayoutType::reduce_nspc && attr_.post_ops_.len() != 0)
                        add(reg_oc_off, step * sizeof(float));
                    sub(reg_work_amount, step);

                    jmp(reduce_loop_label, T_NEAR);
                }
                L(reduce_loop_end_label);
            } else {
                if (attr_.post_ops_.len() != 0) {
                    Xbyak::Label reduce_loop_label;
                    Xbyak::Label reduce_loop_end_label;

                    int step = vlen / sizeof(float) < 8 ? 8 : vlen / sizeof(float);
                    L(reduce_loop_label);
                    {
                        cmp(reg_work_amount, step);
                        jl(reduce_loop_end_label, T_NEAR);

                        load_vector(vmm_dst, ptr[reg_dst], jcp_.dst_dt);
                        apply_post_ops(jcp_.dst_dt, jcp_.layout == ReduceLayoutType::reduce_ncsp);
                        store_vector(ptr[reg_dst], vmm_dst, jcp_.dst_dt);

                        if (isa == cpu::x64::sse41) {
                            load_vector(vmm_dst, ptr[reg_dst + 4 * jcp_.dst_data_size], jcp_.dst_dt);
                            if (jcp_.layout != ReduceLayoutType::reduce_ncsp)
                                add(reg_oc_off, 4 * sizeof(float));
                            apply_post_ops(jcp_.dst_dt, jcp_.layout == ReduceLayoutType::reduce_ncsp);
                            if (jcp_.layout != ReduceLayoutType::reduce_ncsp)
                                sub(reg_oc_off, 4 * sizeof(float));
                            store_vector(ptr[reg_dst + 4 * jcp_.dst_data_size], vmm_dst, jcp_.dst_dt);
                        }

                        add(reg_dst, step * jcp_.dst_data_size);
                        if (jcp_.layout == ReduceLayoutType::reduce_nspc && attr_.post_ops_.len() != 0)
                            add(reg_oc_off, step * sizeof(float));
                        sub(reg_work_amount, step);

                        jmp(reduce_loop_label, T_NEAR);
                    }
                    L(reduce_loop_end_label);
                }
            }
        }
    }

    inline void reduce_post_tail() {
        // reduce map for tail in dst memory
        // cases: [ReduceL2] [ReduceLogSum] [ReduceLogSumExp] [ReduceMean] in planar layout
        if (jcp_.reduce_mode == ReduceL2 || jcp_.reduce_mode == ReduceMean ||
                jcp_.reduce_mode == ReduceLogSum || jcp_.reduce_mode == ReduceLogSumExp) {
            if (jcp_.reduce_mode == ReduceMean)
                uni_vbroadcastss(xmm_aux, ptr[reg_divisor]);

            Xbyak::Label reduce_loop_label;
            Xbyak::Label reduce_loop_end_label;

            int step = 1;
            L(reduce_loop_label);
            {
                cmp(reg_work_amount, step);
                jl(reduce_loop_end_label, T_NEAR);

                // load
                load_scalar(xmm_dst, ptr[reg_dst], jcp_.dst_dt);

                // reduce
                reduce_map_kernel_scalar(xmm_dst);

                // store
                if (attr_.post_ops_.len() != 0)
                    apply_post_ops(jcp_.dst_dt, jcp_.layout == ReduceLayoutType::reduce_ncsp);
                store_scalar(ptr[reg_dst], xmm_dst, jcp_.dst_dt);

                add(reg_dst, step * jcp_.dst_data_size);
                if (jcp_.layout == ReduceLayoutType::reduce_nspc && attr_.post_ops_.len() != 0)
                    add(reg_oc_off, step * sizeof(float));
                sub(reg_work_amount, step);

                jmp(reduce_loop_label, T_NEAR);
            }
            L(reduce_loop_end_label);
        } else {
            if (attr_.post_ops_.len() != 0) {
                Xbyak::Label reduce_loop_label;
                Xbyak::Label reduce_loop_end_label;

                int step = 1;
                L(reduce_loop_label);
                {
                    cmp(reg_work_amount, step);
                    jl(reduce_loop_end_label, T_NEAR);

                    // load
                    load_scalar(xmm_dst, ptr[reg_dst], jcp_.dst_dt);

                    // store
                    apply_post_ops(jcp_.dst_dt, jcp_.layout == ReduceLayoutType::reduce_ncsp);
                    store_scalar(ptr[reg_dst], xmm_dst, jcp_.dst_dt);

                    add(reg_dst, step * jcp_.dst_data_size);
                    if (jcp_.layout == ReduceLayoutType::reduce_nspc && attr_.post_ops_.len() != 0)
                        add(reg_oc_off, step * sizeof(float));
                    sub(reg_work_amount, step);

                    jmp(reduce_loop_label, T_NEAR);
                }
                L(reduce_loop_end_label);
            }
        }
    }

    void apply_post_ops(memory::data_type dst_dt, bool is_broadcast) {
        const auto &p = attr_.post_ops_;
        int eltwise_inj_idx = 0;
        int depthwise_inj_idx = 0;
        int quantization_inj_idx = 0;
        int post_ops_data_offset = 0;
        for (int i = 0; i < p.len(); i++) {
            auto& post_op = p.entry_[i];
            if (post_op.is_eltwise()) {
                eltwise_injectors[eltwise_inj_idx]->compute_vector_range(vmm_dst.getIdx(), vmm_dst.getIdx() + 1);
                eltwise_inj_idx++;
            } else if (post_op.is_depthwise()) {
                mov(reg_d_weights, ptr[reg_post_ops_data + post_ops_data_offset]);
                add(reg_d_weights, reg_oc_off);
                post_ops_data_offset += sizeof(float*);
                mov(reg_d_bias, ptr[reg_post_ops_data + post_ops_data_offset]);
                add(reg_d_bias, reg_oc_off);
                post_ops_data_offset += sizeof(float*);
                depthwise_injectors[depthwise_inj_idx]->compute_vector_range(vmm_dst.getIdx(), vmm_dst.getIdx() + 1, reg_d_weights, reg_d_bias, is_broadcast);
                depthwise_inj_idx++;
            } else if (post_op.is_quantization()) {
                bool do_dequantization = post_op.quantization.alg == alg_kind::quantization_quantize_dequantize;
                bool do_rounding = do_dequantization || isFloatCompatible(dst_dt) || i != p.len() - 1;

                int s_idx = vmm_dst.getIdx();

                quantization_injectors[quantization_inj_idx]->init_crop_ptrs(reg_post_ops_data + post_ops_data_offset, reg_oc_off);
                quantization_injectors[quantization_inj_idx]->compute_crop(s_idx, s_idx + 1, 0, 0, is_broadcast);

                quantization_injectors[quantization_inj_idx]->init_input_scale_shift_ptrs(reg_post_ops_data + post_ops_data_offset, reg_oc_off);
                quantization_injectors[quantization_inj_idx]->compute_input_scale_shift(s_idx, s_idx + 1, 0, do_rounding, 0, is_broadcast);

                if (do_dequantization) {
                    quantization_injectors[quantization_inj_idx]->init_output_scale_shift_ptrs(reg_post_ops_data + post_ops_data_offset, reg_oc_off);
                    quantization_injectors[quantization_inj_idx]->compute_output_scale_shift(s_idx, s_idx + 1, 0, 0, is_broadcast);
                }

                post_ops_data_offset += quantization_injectors[quantization_inj_idx]->memoryStep();
                quantization_inj_idx++;
            }
        }
    }

    inline void reduce_map_kernel(Vmm vmm_dst) {
        if (jcp_.reduce_mode == ReduceMean)
            uni_vdivps(vmm_dst, vmm_dst, vmm_aux);
        else if (jcp_.reduce_mode == ReduceL2)
            uni_vsqrtps(vmm_dst, vmm_dst);
        else if (jcp_.reduce_mode == ReduceLogSum || jcp_.reduce_mode == ReduceLogSumExp)
            log_injector->compute_vector_range(vmm_dst.getIdx(), vmm_dst.getIdx() + 1);
    }

    inline void reduce_map_kernel_scalar(Xmm xmm_dst) {
        if (jcp_.reduce_mode == ReduceMean)
            uni_vdivps(xmm_dst, xmm_dst, xmm_aux);
        else if (jcp_.reduce_mode == ReduceL2)
            uni_vsqrtps(xmm_dst, xmm_dst);
        else if (jcp_.reduce_mode == ReduceLogSum || jcp_.reduce_mode == ReduceLogSumExp)
            log_injector->compute_vector_range(xmm_dst.getIdx(), xmm_dst.getIdx() + 1);
    }

    inline void load_vector(Vmm vmm_src, const Xbyak::Address &op, memory::data_type src_dt) {
        switch (src_dt) {
            case memory::data_type::f32:
            case memory::data_type::s32:
                uni_vmovups(vmm_src, op);
                break;
            case memory::data_type::bf16:
                uni_vpmovzxwd(vmm_src, op);
                uni_vpslld(vmm_src, vmm_src, 16);
                break;
            case memory::data_type::s8:
                uni_vpmovsxbd(vmm_src, op);
                break;
            case memory::data_type::u8:
                uni_vpmovzxbd(vmm_src, op);
                break;
            default:
                assert(!"unknown src_dt");
        }

        if (!isFloatCompatible(src_dt))
            uni_vcvtdq2ps(vmm_src, vmm_src);
    }

    inline void load_scalar(Xmm xmm_src, const Xbyak::Address &op, memory::data_type src_dt) {
        switch (src_dt) {
            case memory::data_type::f32:
            case memory::data_type::s32:
                uni_vmovss(xmm_src, op);
                break;
            case memory::data_type::bf16:
                uni_vpinsrw(xmm_src, xmm_src, op, 0x0);
                uni_vpslld(xmm_src, xmm_src, 16);
                break;
            case memory::data_type::s8:
                movsx(reg_tmp_32, op);
                uni_vmovq(xmm_src, reg_tmp_64);
                break;
            case memory::data_type::u8:
                movzx(reg_tmp_32, op);
                uni_vmovq(xmm_src, reg_tmp_64);
                break;
            default:
                assert(!"unknown src_dt");
        }

        if (!isFloatCompatible(src_dt)) {
            uni_vcvtdq2ps(xmm_src, xmm_src);
        }
    }

    inline void store_vector(const Xbyak::Address &op, Vmm vmm_dst, memory::data_type dst_dt) {
        Xmm xmm_dst = Xmm(vmm_dst.getIdx());
        Ymm ymm_dst = Ymm(vmm_dst.getIdx());

        if (!isFloatCompatible(dst_dt)) {
            uni_vcvtps2dq(vmm_dst, vmm_dst);
        }

        switch (dst_dt) {
            case memory::data_type::f32:
            case memory::data_type::s32:
                uni_vmovups(op, vmm_dst);
                break;
            case memory::data_type::bf16:
                if (mayiuse(avx512_core_bf16))
                    vcvtneps2bf16(ymm_dst, vmm_dst);
                else
                    emu_vcvtneps2bf16->emit_code({static_cast<size_t>(vmm_dst.getIdx())}, {static_cast<size_t>(ymm_dst.getIdx())});
                vmovdqu16(op, ymm_dst);
                break;
            case memory::data_type::s8:
                if (isa == cpu::x64::avx512_common) {
                    vmaxps(vmm_dst, vmm_zero, vmm_dst);
                    vpmovsdb(op, vmm_dst);
                } else {
                    uni_vpackssdw(vmm_dst, vmm_dst, vmm_dst);
                    if (isa != cpu::x64::sse41)
                        vpermq(ymm_dst, ymm_dst, 0x08);
                    uni_vpacksswb(vmm_dst, vmm_dst, vmm_dst);
                    if (isa != cpu::x64::sse41)
                        vmovq(op, xmm_dst);
                    else
                        uni_vmovd(op, xmm_dst);
                }
                break;
            case memory::data_type::u8:
                if (isa == cpu::x64::avx512_common) {
                    vpmovusdb(op, vmm_dst);
                } else {
                    uni_vpackusdw(vmm_dst, vmm_dst, vmm_dst);
                    if (isa != cpu::x64::sse41)
                        vpermq(ymm_dst, ymm_dst, 0x08);
                    uni_vpackuswb(vmm_dst, vmm_dst, vmm_dst);
                    if (isa != cpu::x64::sse41)
                        vmovq(op, xmm_dst);
                    else
                        uni_vmovd(op, xmm_dst);
                }
                break;
            default:
                assert(!"unknown dst_dt");
        }
    }

    inline void store_scalar(const Xbyak::Address &op, Xmm xmm_dst, memory::data_type dst_dt) {
        if (!isFloatCompatible(dst_dt)) {
            uni_vcvtps2dq(xmm_dst, xmm_dst);
        }

        switch (dst_dt) {
            case memory::data_type::f32:
            case memory::data_type::s32:
                uni_vmovss(op, xmm_dst);
                break;
            case memory::data_type::bf16:
                uni_vpsrld(xmm_dst, xmm_dst, 16);
                uni_vpextrw(op, xmm_dst, 0x0);
                break;
            case memory::data_type::s8:
                uni_vpackssdw(xmm_dst, xmm_dst, xmm_dst);
                uni_vpacksswb(xmm_dst, xmm_dst, xmm_dst);
                uni_vmovq(reg_tmp_64, xmm_dst);
                mov(op, reg_tmp_8);
                break;
            case memory::data_type::u8:
                uni_vpackusdw(xmm_dst, xmm_dst, xmm_dst);
                uni_vpackuswb(xmm_dst, xmm_dst, xmm_dst);
                uni_vmovq(reg_tmp_64, xmm_dst);
                mov(op, reg_tmp_8);
                break;
            default:
                assert(!"unknown dst_dt");
        }
    }

    inline void horiz_reduce_store(Vmm vmm_dst, memory::data_type dst_dt, bool load_embedded = false) {
        if (isa == cpu::x64::sse41) {
            horiz_store(vmm_dst, dst_dt, load_embedded);
        } else if (isa == cpu::x64::avx2) {
            Xbyak::Ymm ymm_dst = Xbyak::Ymm(vmm_dst.getIdx());
            vextractf128(xmm_aux1, ymm_dst, 0);
            vextractf128(xmm_aux2, ymm_dst, 1);
            horiz_ps(xmm_aux1, xmm_aux2);
            horiz_store(xmm_aux1, dst_dt, load_embedded);
        } else {
            Xbyak::Zmm zmm_dst = Xbyak::Zmm(vmm_dst.getIdx());
            vextractf32x4(xmm_aux1, zmm_dst, 0);
            vextractf32x4(xmm_aux2, zmm_dst, 1);
            horiz_ps(xmm_aux1, xmm_aux2);
            vextractf32x4(xmm_aux2, zmm_dst, 2);
            vextractf32x4(xmm_aux3, zmm_dst, 3);
            horiz_ps(xmm_aux2, xmm_aux3);
            horiz_ps(xmm_aux1, xmm_aux2);
            horiz_store(xmm_aux1, dst_dt, load_embedded);
        }
    }

    inline void horiz_store(Xbyak::Xmm xmm_dst, memory::data_type dst_dt, bool load_embedded) {
        uni_vmovshdup(xmm_aux3, xmm_dst);          // dst:1,2,3,4; aux3:2,2,4,4
        horiz_ps(xmm_dst, xmm_aux3);               // dst:f(1,2),f(2,2),f(3,4),f(4,4)
        uni_vmovhlps(xmm_aux3, xmm_aux3, xmm_dst); // aux3:f(3,4),f(4,4),4,4
        horiz_ps(xmm_dst, xmm_aux3);               // dst:f(1,2,3,4),...
        if (load_embedded) {
            load_scalar(xmm_aux3, ptr[reg_dst], dst_dt);
            horiz_ps(xmm_dst, xmm_aux3);
        }
        store_scalar(ptr[reg_dst], xmm_dst, dst_dt);
    }

    inline void horiz_ps(const Xmm& xmm, const Operand& op) {
        switch (jcp_.reduce_mode) {
            case ReduceAnd:
                uni_vandps(xmm, xmm, op);
                break;
            case ReduceL1:
            case ReduceL2:
            case ReduceLogSum:
            case ReduceMean:
            case ReduceSum:
            case ReduceSumSquare:
            case ReduceLogSumExp:
                uni_vaddps(xmm, xmm, op);
                break;
            case ReduceMax:
                uni_vmaxps(xmm, xmm, op);
                break;
            case ReduceMin:
                uni_vminps(xmm, xmm, op);
                break;
            case ReduceOr:
                uni_vorps(xmm, xmm, op);
                break;
            case ReduceProd:
                uni_vmulps(xmm, xmm, op);
                break;
            default:
                assert(!"unsupported reduce mode");
        }
    }
};

const std::map<const ngraph::DiscreteTypeInfo, std::function<void(const std::shared_ptr<ngraph::Node>&, MKLDNNReduceNode&)>> MKLDNNReduceNode::initializers = {
    {ngraph::opset4::ReduceL1::get_type_info_static(), [](const std::shared_ptr<ngraph::Node>& op, MKLDNNReduceNode& node) {
        node.algorithm = ReduceL1;
    }},
    {ngraph::opset4::ReduceL2::get_type_info_static(), [](const std::shared_ptr<ngraph::Node>& op, MKLDNNReduceNode& node) {
        node.algorithm = ReduceL2;
    }},
    {ngraph::opset1::ReduceLogicalAnd::get_type_info_static(), [](const std::shared_ptr<ngraph::Node>& op, MKLDNNReduceNode& node) {
        node.algorithm = ReduceAnd;
    }},
    {ngraph::opset1::ReduceLogicalOr::get_type_info_static(), [](const std::shared_ptr<ngraph::Node>& op, MKLDNNReduceNode& node) {
        node.algorithm = ReduceOr;
    }},
    {ngraph::opset1::ReduceMax::get_type_info_static(), [](const std::shared_ptr<ngraph::Node>& op, MKLDNNReduceNode& node) {
        node.algorithm = ReduceMax;
    }},
    {ngraph::opset1::ReduceMean::get_type_info_static(), [](const std::shared_ptr<ngraph::Node>& op, MKLDNNReduceNode& node) {
        node.algorithm = ReduceMean;
    }},
    {ngraph::opset1::ReduceMin::get_type_info_static(), [](const std::shared_ptr<ngraph::Node>& op, MKLDNNReduceNode& node) {
        node.algorithm = ReduceMin;
    }},
    {ngraph::opset1::ReduceProd::get_type_info_static(), [](const std::shared_ptr<ngraph::Node>& op, MKLDNNReduceNode& node) {
        node.algorithm = ReduceProd;
    }},
    {ngraph::opset1::ReduceSum::get_type_info_static(), [](const std::shared_ptr<ngraph::Node>& op, MKLDNNReduceNode& node) {
        node.algorithm = ReduceSum;
    }}
};

bool MKLDNNReduceNode::isSupportedOperation(const std::shared_ptr<const ngraph::Node>& op, std::string& errorMessage) noexcept {
    try {
        if (std::dynamic_pointer_cast<const ngraph::op::util::ArithmeticReductionKeepDims>(op) == nullptr &&
                std::dynamic_pointer_cast<const ngraph::op::util::LogicalReductionKeepDims>(op) == nullptr) {
            errorMessage = "Reduce node with name " + op->get_friendly_name() + " is not derived from ArithmeticReductionKeepDims or LogicalReductionKeepDims";
            return false;
        }
        if (const auto reduce = std::dynamic_pointer_cast<const ngraph::op::util::ArithmeticReductionKeepDims>(op)) {
            auto reduceConst = std::dynamic_pointer_cast<const ngraph::opset1::Constant>(reduce->get_input_node_shared_ptr(REDUCE_INDEXES));
            if (!reduceConst) {
                errorMessage = "Second tensor is not constant";
                return false;
            }
        }
        if (const auto reduce = std::dynamic_pointer_cast<const ngraph::op::util::LogicalReductionKeepDims>(op)) {
            auto reduceConst = std::dynamic_pointer_cast<const ngraph::opset1::Constant>(reduce->get_input_node_shared_ptr(REDUCE_INDEXES));
            if (!reduceConst) {
                errorMessage = "Second tensor is not constant";
                return false;
            }
        }
        if (initializers.find(op->get_type_info()) == initializers.end()) {
            errorMessage = "Doesn't support Reduce algorithm: " +  std::string(op->get_type_info().name);
            return false;
        }
        if (std::dynamic_pointer_cast<ngraph::opset1::Constant>(op->get_input_node_shared_ptr(REDUCE_INDEXES)) == nullptr) {
            errorMessage = "Only const 'reduce_indexes' input is supported";
            return false;
        }
    } catch (...) {
        return false;
    }
    return true;
}

MKLDNNReduceNode::MKLDNNReduceNode(const std::shared_ptr<ngraph::Node>& op, const mkldnn::engine& eng, MKLDNNWeightsSharing::Ptr &cache)
        : MKLDNNNode(op, eng, cache) {
    std::string errorMessage;
    if (isSupportedOperation(op, errorMessage)) {
        errorPrefix = "Reduce node with name '" + getName() + "'";
        initializers.at(op->get_type_info())(op, *this);
        if (const auto reduce = std::dynamic_pointer_cast<ngraph::op::util::ArithmeticReductionKeepDims>(op)) {
            keep_dims = reduce->get_keep_dims();
            auto reduceConst = std::dynamic_pointer_cast<const ngraph::opset1::Constant>(reduce->get_input_node_shared_ptr(REDUCE_INDEXES));
            if (!reduceConst)
                IE_THROW() << errorPrefix << " second tensor is not constant!";
            raw_axes = reduceConst->cast_vector<int>();
        } else if (const auto reduce = std::dynamic_pointer_cast<ngraph::op::util::LogicalReductionKeepDims>(op)) {
            keep_dims = reduce->get_keep_dims();
            auto reduceConst = std::dynamic_pointer_cast<const ngraph::opset1::Constant>(reduce->get_input_node_shared_ptr(REDUCE_INDEXES));
            if (!reduceConst)
                IE_THROW() << errorPrefix << " second tensor is not constant!";
            raw_axes = reduceConst->cast_vector<int>();
        }
        setJITBeyond5D();
    } else {
        IE_THROW(NotImplemented) << errorMessage;
    }
}

void MKLDNNReduceNode::getSupportedDescriptors() {
    if (!descs.empty())
        return;

    if (getParentEdges().size() != 2)
        IE_THROW() << errorPrefix << " gets incorrect number of input edges!";
    if (getChildEdges().empty())
        IE_THROW() << errorPrefix << " gets incorrect number of output edges!";

    if (getInputShapeAtPort(REDUCE_INDEXES).getRank() != 1) {
        IE_THROW() << errorPrefix << " gets incorrect index vector dimension! Index vector should be 1 dimension.";
    }

    if (keep_dims) {
        if (getInputShapeAtPort(REDUCE_DATA).getRank() != getOutputShapeAtPort(0).getRank())
            IE_THROW() << errorPrefix << " gets incorrect number of input/output dimensions!";
    } else {
        // In fact, after the Reduce operation, the shape must be a scalar if the previous one was 1d.
        // But for now, 0d tensor (scalar) is emulated as 1d tensor. Skip checking in such cases.
        bool is_emulated_0d_as_1d = getInputShapeAtPort(REDUCE_DATA).getRank() == 1 && getOutputShapeAtPort(0).getRank() == 1;
        if (getInputShapeAtPort(REDUCE_DATA).getRank() <= getOutputShapeAtPort(0).getRank() && !is_emulated_0d_as_1d)
            IE_THROW() << errorPrefix << "gets incorrect number of input/output dimensions!";
    }
}

void MKLDNNReduceNode::initSupportedPrimitiveDescriptors() {
    if (!supportedPrimitiveDescriptors.empty())
        return;

    input_prec = getOriginalInputPrecisionAtPort(REDUCE_DATA);
    output_prec = getOriginalOutputPrecisionAtPort(0);

    if (!fusedWith.empty()) {
        output_prec = fusedWith[fusedWith.size() - 1]->getOriginalOutputPrecisionAtPort(0);
    }

    jit_mode = canApplyJIT(input_prec, output_prec);

    if (jit_mode) {
        // Since in jit mode we use the output memory as an intermediate accumulator for certain reduce modes, we can't use BF16 output precision due to
        // the possible accuracy loss. Therefore, for such mods, we will change the output precision to FP32.
        if (Precision::BF16 == output_prec) {
            if (!mayiuse(avx512_core)) {
                    output_prec = Precision::FP32;
            } else if (algorithm != ReduceAnd && algorithm != ReduceOr &&
                       algorithm != ReduceMin && algorithm != ReduceMax) {
                            output_prec = Precision::FP32;
            }
        }
    }

    src_data_size = input_prec.size();
    dst_data_size = output_prec.size();

    NodeConfig config;
    config.dynBatchSupport = false;
    config.inConfs.resize(2);
    config.outConfs.resize(1);
    config.inConfs[REDUCE_DATA].constant = false;
    config.inConfs[REDUCE_INDEXES].constant = false;
    config.outConfs[0].constant = false;
    config.inConfs[REDUCE_DATA].inPlace = -1;
    config.inConfs[REDUCE_INDEXES].inPlace = -1;
    config.outConfs[0].inPlace = -1;

    auto& creatorsMap = BlockedDescCreator::getCommonCreators();

    auto pushDesc = [&](LayoutType inFormat, LayoutType outFormat, InferenceEngine::Precision inPrecision,
            InferenceEngine::Precision outPrecision, impl_desc_type impl_type) {
        config.inConfs[REDUCE_DATA].desc = creatorsMap.at(inFormat)->createSharedDesc(inPrecision, getInputShapeAtPort(REDUCE_DATA));
        config.inConfs[REDUCE_INDEXES].desc = creatorsMap.at(LayoutType::ncsp)->createSharedDesc(InferenceEngine::Precision::I32,
                                                                                                 getInputShapeAtPort(REDUCE_INDEXES));
        config.outConfs[0].desc = creatorsMap.at(outFormat)->createSharedDesc(outPrecision, getOutputShapeAtPort(0));
        supportedPrimitiveDescriptors.push_back({config, impl_type});
    };

    if (jit_mode) {
        impl_desc_type impl_type = impl_desc_type::jit_sse42;
        if (mayiuse(cpu::x64::avx512_common)) {
            impl_type = impl_desc_type::jit_avx512;
        } else if (mayiuse(cpu::x64::avx2)) {
            impl_type = impl_desc_type::jit_avx2;
        }

        pushDesc(LayoutType::ncsp, LayoutType::ncsp, input_prec, output_prec, impl_type);
        if ((getInputShapeAtPort(REDUCE_DATA).getRank() == 4 || getInputShapeAtPort(REDUCE_DATA).getRank() == 5) &&
                getInputShapeAtPort(REDUCE_DATA).getMinDims()[1] > 1) {
            if (keep_dims) {
                if (mayiuse(cpu::x64::avx512_common)) {
                    pushDesc(LayoutType::nspc, LayoutType::nspc, input_prec, output_prec, impl_type);
                    pushDesc(LayoutType::nCsp16c, LayoutType::nCsp16c, input_prec, output_prec, impl_type);
                } else if (mayiuse(cpu::x64::avx2) || mayiuse(cpu::x64::sse41)) {
                    pushDesc(LayoutType::nspc, LayoutType::nspc, input_prec, output_prec, impl_type);
                    pushDesc(LayoutType::nCsp8c, LayoutType::nCsp8c, input_prec, output_prec, impl_type);
                }
            } else {
                if (mayiuse(cpu::x64::avx512_common)) {
                    pushDesc(LayoutType::nspc, LayoutType::ncsp, input_prec, output_prec, impl_type);
                    pushDesc(LayoutType::nCsp16c, LayoutType::ncsp, input_prec, output_prec, impl_type);
                } else if (mayiuse(cpu::x64::avx2) || mayiuse(cpu::x64::sse41)) {
                    pushDesc(LayoutType::nspc, LayoutType::ncsp, input_prec, output_prec, impl_type);
                    pushDesc(LayoutType::nCsp8c, LayoutType::ncsp, input_prec, output_prec, impl_type);
                }
            }
        }
    } else {
        pushDesc(LayoutType::ncsp, LayoutType::ncsp, InferenceEngine::Precision::FP32, InferenceEngine::Precision::FP32, impl_desc_type::ref);
    }
}

bool MKLDNNReduceNode::isExecutable() const {
    return !isInputTensorAtPortEmpty(REDUCE_DATA);
}

void MKLDNNReduceNode::prepareParams() {
    src_dims = getParentEdgesAtPort(REDUCE_DATA)[0]->getMemory().getDesc().getShape().getDims();
    std::vector<int> reduce_axes;
    if (jit_mode && jit_beyond_5D) {
        reduce_axes = update_src_dims();
    } else {
        reduce_axes = raw_axes;
    }

    auto &dstMemPtr = getChildEdgeAt(0)->getMemoryPtr();
    const SizeVector &dst_dims = dstMemPtr->getDesc().getShape().getDims();
    dst_size = dstMemPtr->GetSize();
    calc_process_dst_dims(reduce_axes, dst_dims);
    if (jit_mode) {
        set_reduce_dim_flags();
    }

    auto builder = [&](const ReduceKey& key) -> std::shared_ptr<jit_uni_reduce_post_kernel> {
        std::shared_ptr<jit_uni_reduce_post_kernel> post_kernel;

        if (mayiuse(cpu::x64::avx512_common)) {
            post_kernel.reset(new jit_uni_reduce_post_kernel_f32<cpu::x64::avx512_common>(key.jcp, *attr.get()));
        } else if (mayiuse(cpu::x64::avx2)) {
            post_kernel.reset(new jit_uni_reduce_post_kernel_f32<cpu::x64::avx2>(key.jcp, *attr.get()));
        } else if (mayiuse(cpu::x64::sse41)) {
            post_kernel.reset(new jit_uni_reduce_post_kernel_f32<cpu::x64::sse41>(key.jcp, *attr.get()));
        }
        if (post_kernel)
            post_kernel->create_ker();

        return post_kernel;
    };

    if (compile_post_kernel) {
        setPostOps(attr, dst_dims, true);

        ReduceKey key = {jcp, attr.get_post_ops()};
        auto cache = getRuntimeCache();
        auto result = cache->getOrCreate(key, builder);
        if (!result.first) {
            IE_THROW() << errorPrefix << " has not found jit_uni_reduce_post_kernel_f32.";
        }

        reduce_post_kernel = result.first;
        jit_mode = jit_mode && reduce_post_kernel;

        if (!isDynamicNode()) {
            compile_post_kernel = false;
        }
    }
}

void MKLDNNReduceNode::createPrimitive() {
    if (!isExecutable()) {
        return;
    }
    auto &dstMemPtr = getChildEdgeAt(0)->getMemoryPtr();
    auto &srcMemPtr = getParentEdgeAt(REDUCE_DATA)->getMemoryPtr();
    if (!dstMemPtr || !dstMemPtr->GetPrimitivePtr())
        IE_THROW() << errorPrefix << " has not allocated destination memory.";
    if (!srcMemPtr || !srcMemPtr->GetPrimitivePtr())
        IE_THROW() << errorPrefix << " has not allocate input memory.";
    if (getSelectedPrimitiveDescriptor() == nullptr)
        IE_THROW() << errorPrefix << " has nullable preferable primitive descriptor";

    if (srcMemPtr->getDesc().hasLayoutType(LayoutType::ncsp)) {
        layout = ReduceLayoutType::reduce_ncsp;
    } else if (srcMemPtr->getDesc().hasLayoutType(LayoutType::nspc)) {
        layout = ReduceLayoutType::reduce_nspc;
    } else {
        layout = ReduceLayoutType::reduce_blocked;
    }

    // hybrid layout: nspc/blocked layout for input and ncsp for output
    // !keep_dims is needed to avoid hybrid layout for cases eg. (A, B, C, D) reduce to (A, 1, 1, 1)
    if (!keep_dims && (layout == ReduceLayoutType::reduce_nspc || layout == ReduceLayoutType::reduce_blocked)) {
        is_hybrid_layout = dstMemPtr->getDesc().hasLayoutType(LayoutType::ncsp);
    }

    auto selectedPD = getSelectedPrimitiveDescriptor();
    jcp = jit_reduce_config_params();
    jcp.src_dt = MKLDNNExtensionUtils::IEPrecisionToDataType(selectedPD->getConfig().inConfs[REDUCE_DATA].desc->getPrecision());
    jcp.dst_dt = MKLDNNExtensionUtils::IEPrecisionToDataType(selectedPD->getConfig().outConfs[0].desc->getPrecision());
    jcp.src_data_size = MKLDNNExtensionUtils::sizeOfDataType(jcp.src_dt);
    jcp.dst_data_size = MKLDNNExtensionUtils::sizeOfDataType(jcp.dst_dt);
    jcp.layout = layout;
    jcp.reduce_mode = getAlgorithm();

    compile_post_kernel = true;

    if (inputShapesDefined()) {
        if (needPrepareParams())
            prepareParams();
        updateLastInputDims();
    }

    if (mayiuse(cpu::x64::avx512_common)) {
        reduce_kernel.reset(new jit_uni_reduce_kernel_f32<cpu::x64::avx512_common>(jcp));
        blk_size = 16;
    } else if (mayiuse(cpu::x64::avx2)) {
        reduce_kernel.reset(new jit_uni_reduce_kernel_f32<cpu::x64::avx2>(jcp));
        blk_size = 8;
    } else if (mayiuse(cpu::x64::sse41)) {
        reduce_kernel.reset(new jit_uni_reduce_kernel_f32<cpu::x64::sse41>(jcp));
        blk_size = 8;
    }
    if (reduce_kernel)
        reduce_kernel->create_ker();
    jit_mode = jit_mode && reduce_kernel;
}

void MKLDNNReduceNode::executeDynamicImpl(mkldnn::stream strm) {
    execute(strm);
}

void MKLDNNReduceNode::execute(mkldnn::stream strm) {
    auto &dstMemPtr = getChildEdgeAt(0)->getMemoryPtr();
    auto &srcMemPtr = getParentEdgeAt(REDUCE_DATA)->getMemoryPtr();

    const uint8_t *src_data = reinterpret_cast<const uint8_t *>(srcMemPtr->GetPtr());
    uint8_t *dst_data = reinterpret_cast<uint8_t *>(dstMemPtr->GetPtr());

    if (jit_mode) {
        if (is_hybrid_layout) {
            dst_data = reinterpret_cast<uint8_t *>(prc_mem->get_data_handle());
        }
        reduce_type(src_data, dst_data, dst_size);
    } else {
        if (layout == ReduceLayoutType::reduce_ncsp) {
            auto in_ptr = reinterpret_cast<const float *>(src_data);
            auto out_ptr = reinterpret_cast<float *>(dst_data);
            reduce_ref(in_ptr, out_ptr);
        } else {
            IE_THROW() << errorPrefix << " supports only plain layout on machine w/o sse42.";
        }
    }
}

void MKLDNNReduceNode::reduce_type(const uint8_t *in_ptr, uint8_t *out_ptr, size_t dst_size) {
    init_dst_data(out_ptr, dst_size);
    reduce_stride = IW;

    if (layout == ReduceLayoutType::reduce_ncsp || layout == ReduceLayoutType::reduce_nspc) {
        reduce_PLN(in_ptr, out_ptr);
    } else {
        if (ReduceC && (IC % blk_size)) {
            reduce_BLK_concern_padding(in_ptr, out_ptr);
        } else {
            reduce_BLK(in_ptr, out_ptr);
        }
    }

    if (is_hybrid_layout) {
        uint8_t *proc_ptr = out_ptr;
        auto &dstMemPtr = getChildEdgeAt(0)->getMemoryPtr();
        out_ptr = reinterpret_cast<uint8_t *>(dstMemPtr->GetPtr());
        if (layout == ReduceLayoutType::reduce_nspc) {
            nspc2ncsp(proc_ptr, out_ptr);
        } else {
            blocked2ncsp(proc_ptr, out_ptr);
        }
    }
}

void MKLDNNReduceNode::reduce_PLN(const uint8_t *in_ptr, uint8_t *out_ptr) {
    if (ReduceN && !ReduceC && !ReduceD && !ReduceH && !ReduceW) {
        size_t IA = IC * ID * IH * IW;
        reduce_stride = IA;
        parallel_for(IA / blk_size, [&](size_t iba){
            size_t oba = iba;
            reduce_kernel_process(in_ptr + iba * blk_size * src_data_size, out_ptr + oba * blk_size * dst_data_size,
                                  blk_size, 0, IB);
        });

        size_t tail_start = IA / blk_size * blk_size;
        reduce_kernel_process(in_ptr + tail_start * src_data_size, out_ptr + tail_start * dst_data_size,
                              IA - tail_start, 0, IB);
    } else {
        for (size_t ib = 0; ib < IB; ib++) {
            size_t ob = ReduceN ? 0 : ib; GET_PTR_N_PLN;
            if (!ReduceC && !ReduceD && ReduceW) {
                size_t work_amount = ReduceH ? IH * IW : IW;
                if (work_amount < blk_size && mayiuse(cpu::x64::avx2)) {
                    size_t outer_size = ReduceH ? IC * ID : IC * ID * IH;
                    size_t inner_size = ReduceH ? IH * IW : IW;
                    size_t output_inner_size = ReduceH ? OH * OW : OW;
                    size_t IK = outer_size / blk_size;
                    std::vector<int> index_buf(blk_size);
                    for (size_t i = 0; i < blk_size; i++) {
                        index_buf[i] = i * work_amount * src_data_size;
                    }
                    parallel_for(IK, [&](size_t ik) {
                        size_t ok = ik;
                        reduce_kernel_process(in_ptr_n + ik * blk_size * inner_size * src_data_size,
                                              out_ptr_n + ok * blk_size * output_inner_size * dst_data_size,
                                              work_amount, 1, 0, static_cast<int *>(&index_buf[0]));
                    });
                    size_t tail_start = IK * blk_size;
                    size_t IT = outer_size - tail_start;
                    parallel_for(IT, [&](size_t it) {
                        size_t ot = it;
                        reduce_kernel_process(in_ptr_n + (tail_start + it) * inner_size * src_data_size,
                                              out_ptr_n + (tail_start + ot) * output_inner_size * dst_data_size, work_amount, 1);
                    });
                } else {
                    if (ReduceH) {
                        parallel_for2d(IC, ID, [&](size_t ic, size_t id) {
                            size_t oc = ic, od = id; GET_PTR_NCD_BASE_PTR_N_PLN;
                            reduce_kernel_process(in_ptr_ncd, out_ptr_ncd, work_amount, 1);
                        });
                    } else {
                        parallel_for3d(IC, ID, IH, [&](size_t ic, size_t id, size_t ih) {
                            size_t oc = ic, od = id; GET_PTR_NCD_BASE_PTR_N_PLN;
                            size_t oh = ih; GET_PTR_NCDH_PLN;
                            reduce_kernel_process(in_ptr_ncdh, out_ptr_ncdh, work_amount, 1);
                        });
                    }
                }
            } else if (ReduceH && ReduceW) {
                for (size_t ic = 0; ic < IC; ic++) {
                    size_t oc = ReduceC ? 0 : ic; GET_PTR_NC_PLN;
                    for (size_t id = 0; id < ID; id++) {
                        size_t od = ReduceD ? 0 : id; GET_PTR_NCD_PLN;
                        reduce_kernel_process(in_ptr_ncd, out_ptr_ncd, IH * IW, 1);
                    }
                }
            } else if (!ReduceH && ReduceW) {
                for (size_t ic = 0; ic < IC; ic++) {
                    size_t oc = ReduceC ? 0 : ic; GET_PTR_NC_PLN;
                    for (size_t id = 0; id < ID; id++) {
                        size_t od = ReduceD ? 0 : id; GET_PTR_NCD_PLN;
                        parallel_for(IH, [&](size_t ih){
                            size_t oh = ih; GET_PTR_NCDH_PLN;
                            reduce_kernel_process(in_ptr_ncdh, out_ptr_ncdh, IW, 1);
                        });
                    }
                }
            } else if (ReduceW) {
                for (size_t ic = 0; ic < IC; ic++) {
                    size_t oc = ReduceC ? 0 : ic; GET_PTR_NC_PLN;
                    for (size_t id = 0; id < ID; id++) {
                        size_t od = ReduceD ? 0 : id; GET_PTR_NCD_PLN;
                        for (size_t ih = 0; ih < IH; ih++) {
                            size_t oh = ReduceH ? 0 : ih; GET_PTR_NCDH_PLN;
                            reduce_kernel_process(in_ptr_ncdh, out_ptr_ncdh, IW, 1);
                        }
                    }
                }
            } else if (!ReduceC && !ReduceD && ReduceH && !ReduceW) {
                parallel_for2d(IC, ID, [&](size_t ic, size_t id) {
                    size_t oc = ic, od = id; GET_PTR_NCD_BASE_PTR_N_PLN;
                    parallel_for(IW / blk_size, [&](size_t ibw){
                        size_t obw = ibw;
                        reduce_kernel_process(in_ptr_ncd + ibw * blk_size * src_data_size, out_ptr_ncd + obw * blk_size * dst_data_size,
                                              blk_size, 0, IH);
                    });
                    size_t tail_start = IW / blk_size * blk_size;
                    reduce_kernel_process(in_ptr_ncd + tail_start * src_data_size, out_ptr_ncd + tail_start * dst_data_size,
                                          IW - tail_start, 0, IH);
                });
            } else if (!ReduceC && ReduceD && ReduceH && !ReduceW) {
                parallel_for(IC, [&](size_t ic) {
                    size_t oc = ic; GET_PTR_NC_PLN;
                    parallel_for(IW / blk_size, [&](size_t ibw){
                        size_t obw = ibw;
                        reduce_kernel_process(in_ptr_nc + ibw * blk_size * src_data_size, out_ptr_nc + obw * blk_size * dst_data_size,
                                              blk_size, 0, ID * IH);
                    });
                    size_t tail_start = IW / blk_size * blk_size;
                    reduce_kernel_process(in_ptr_nc + tail_start * src_data_size, out_ptr_nc + tail_start * dst_data_size,
                                          IW - tail_start, 0, ID * IH);
                });
            } else if (ReduceC && ReduceD && ReduceH && !ReduceW) {
                parallel_for(IW / blk_size, [&](size_t ibw){
                    size_t obw = ibw;
                    reduce_kernel_process(in_ptr_n + ibw * blk_size * src_data_size, out_ptr_n + obw * blk_size * dst_data_size,
                                          blk_size, 0, IC * ID * IH);
                });

                size_t tail_start = IW / blk_size * blk_size;
                reduce_kernel_process(in_ptr_n + tail_start * src_data_size, out_ptr_n + tail_start * dst_data_size,
                                      IW - tail_start, 0, IC * ID * IH);
            } else if (ReduceC && !ReduceD && !ReduceH && !ReduceW) {
                size_t IS = ID * IH * IW;
                reduce_stride = IS;
                parallel_for(IS / blk_size, [&](size_t ibs){
                    size_t obs = ibs;
                    reduce_kernel_process(in_ptr_n + ibs * blk_size * src_data_size, out_ptr_n + obs * blk_size * dst_data_size,
                                          blk_size, 0, IC);
                });

                size_t tail_start = IS / blk_size * blk_size;
                reduce_kernel_process(in_ptr_n + tail_start * src_data_size, out_ptr_n + tail_start * dst_data_size,
                                      IS - tail_start, 0, IC);
            } else {
                for (size_t ic = 0; ic < IC; ic++) {
                    size_t oc = ReduceC ? 0 : ic; GET_PTR_NC_PLN;
                    for (size_t id = 0; id < ID; id++) {
                        size_t od = ReduceD ? 0 : id; GET_PTR_NCD_PLN;
                        for (size_t ih = 0; ih < IH; ih++) {
                            size_t oh = ReduceH ? 0 : ih; GET_PTR_NCDH_PLN;
                            for (size_t ibw = 0; ibw < IW / blk_size; ibw++) {
                                size_t obw = ibw;
                                reduce_kernel_process(in_ptr_ncdh + ibw * blk_size * src_data_size,
                                                      out_ptr_ncdh + obw * blk_size * dst_data_size, blk_size, 0);
                            }
                            size_t tail_start = IW / blk_size * blk_size;
                            reduce_kernel_process(in_ptr_ncdh + tail_start * src_data_size, out_ptr_ncdh + tail_start * dst_data_size, IW - tail_start, 0);
                        }
                    }
                }
            }
        }
    }

    reduce_kernel_post_process(out_ptr);
}

void MKLDNNReduceNode::reduce_BLK(const uint8_t *in_ptr, uint8_t *out_ptr) {
    size_t ICB = div_up(IC, blk_size);
    size_t OCB = div_up(OC, blk_size);

    for (size_t ib = 0; ib < IB; ib++) {
        size_t ob = ReduceN ? 0 : ib; GET_PTR_N_BLK;
        if (!ReduceC && !ReduceD && ReduceH && ReduceW) {
            parallel_for2d(ICB, ID, [&](size_t icb, size_t id) {
                size_t ocb = icb, od = id; GET_PTR_NCD_BASE_PTR_N_BLK;
                reduce_kernel_process(in_ptr_ncd, out_ptr_ncd, IH * IW * blk_size);
            });
        } else if (ReduceC && ReduceD && ReduceH && ReduceW) {
            if (input_prec != output_prec || getAlgorithm() == ReduceL2 ||
                 algorithm == ReduceLogSumExp || algorithm == ReduceSumSquare) {
                reduce_kernel_process(in_ptr_n, out_ptr_n, ICB * ID * IH * IW * blk_size);
            } else {
                // reduce parallelly
                // step1: !ReduceC && ReduceD && ReduceH && ReduceW
                size_t prc_size = ICB * blk_size * dst_data_size;
                std::vector<uint8_t> vec_prc(prc_size);
                init_dst_data(vec_prc.data(), prc_size);
                uint8_t *out_ptr_n_cp = out_ptr_n;
                out_ptr_n = vec_prc.data();
                parallel_for(ICB, [&](size_t icb) {
                    size_t ocb = icb; GET_PTR_NC_BLK;
                    reduce_kernel_process(in_ptr_nc, out_ptr_nc, ID * IH * IW * blk_size);
                });
                // step2: ReduceC
                reduce_kernel_process(out_ptr_n, out_ptr_n_cp, ICB * blk_size);
            }
        } else if (ReduceW) {
            for (size_t icb = 0; icb < ICB; icb++) {
                size_t ocb = ReduceC ? 0 : icb; GET_PTR_NC_BLK;
                for (size_t id = 0; id < ID; id++) {
                    size_t od = ReduceD ? 0 : id; GET_PTR_NCD_BLK;
                    for (size_t ih = 0; ih < IH; ih++) {
                        size_t oh = ReduceH ? 0 : ih; GET_PTR_NCDH_BLK;
                        reduce_kernel_process(in_ptr_ncdh, out_ptr_ncdh, IW * blk_size);
                    }
                }
            }
        } else if (ReduceC && !ReduceD && !ReduceH && !ReduceW) {
            reduce_stride = ID * IH * IW * blk_size;
            parallel_for3d(ID, IH, IW, [&](size_t id, size_t ih, size_t iw) {
                size_t icb = 0, ocb = 0; GET_PTR_NC_BLK;
                size_t od = id; GET_PTR_NCD_BLK;
                size_t oh = ih; GET_PTR_NCDH_BLK;
                size_t ow = iw; GET_PTR_NCDHW_BLK;
                reduce_kernel_process(in_ptr_ncdhw, out_ptr_ncdhw, blk_size, 0, ICB);
            });
        } else {
            for (size_t icb = 0; icb < ICB; icb++) {
                size_t ocb = ReduceC ? 0 : icb; GET_PTR_NC_BLK;
                for (size_t id = 0; id < ID; id++) {
                    size_t od = ReduceD ? 0 : id; GET_PTR_NCD_BLK;
                    for (size_t ih = 0; ih < IH; ih++) {
                        size_t oh = ReduceH ? 0 : ih; GET_PTR_NCDH_BLK;
                        parallel_for(IW, [&](size_t iw) {
                            size_t ow = iw; GET_PTR_NCDHW_BLK;
                            reduce_kernel_process(in_ptr_ncdhw, out_ptr_ncdhw, blk_size);
                        });
                    }
                }
            }
        }
    }

    reduce_kernel_post_process(out_ptr);
}

void MKLDNNReduceNode::reduce_BLK_concern_padding(const uint8_t *in_ptr, uint8_t *out_ptr) {
    size_t ICB = div_up(IC, blk_size);
    size_t OCB = div_up(OC, blk_size);

    auto reduceSkipPadding = [&](const uint8_t *in_ptr_ncd, uint8_t *out_ptr_ncd, size_t ic) {
        size_t blk_valid_size = IC - ic;
        for (size_t ih = 0; ih < IH; ih++) {
            size_t oh = ReduceH ? 0 : ih; GET_PTR_NCDH_BLK;
            for (size_t iw = 0; iw < IW; iw++) {
                size_t ow = ReduceW ? 0 : iw; GET_PTR_NCDHW_BLK;
                reduce_kernel_process(in_ptr_ncdhw, out_ptr_ncdhw, blk_valid_size);
            }
        }
    };

    for (size_t ib = 0; ib < IB; ib++) {
        size_t ob = ReduceN ? 0 : ib; GET_PTR_N_BLK;
        if (!ReduceD && ReduceH && ReduceW) {
            for (size_t icb = 0; icb < ICB; icb++) {
                size_t ocb = 0;;
                size_t ic = icb * blk_size;
                parallel_for(ID, [&](size_t id) {
                    size_t od = id; GET_PTR_NCD_BASE_PTR_N_BLK;
                    if (ic + blk_size <= IC) {
                        reduce_kernel_process(in_ptr_ncd, out_ptr_ncd, IH * IW * blk_size);
                    } else {
                        reduceSkipPadding(in_ptr_ncd, out_ptr_ncd, ic);
                    }
                });
            }
        } else if (ReduceD && ReduceH && ReduceW) {
            for (size_t icb = 0; icb < ICB; icb++) {
                size_t ocb = 0; GET_PTR_NC_BLK;
                size_t ic = icb * blk_size;
                if (ic + blk_size <= IC) {
                    reduce_kernel_process(in_ptr_nc, out_ptr_nc, ID * IH * IW * blk_size);
                } else {
                    for (size_t id = 0; id < ID; id++) {
                        size_t od = 0; GET_PTR_NCD_BLK;
                        reduceSkipPadding(in_ptr_ncd, out_ptr_ncd, ic);
                    }
                }
            }
        } else if (ReduceW) {
            for (size_t icb = 0; icb < ICB; icb++) {
                size_t ocb = 0; GET_PTR_NC_BLK;
                size_t ic = icb * blk_size;
                for (size_t id = 0; id < ID; id++) {
                    size_t od = ReduceD ? 0 : id; GET_PTR_NCD_BLK;
                    if (ic + blk_size <= IC) {
                        for (size_t ih = 0; ih < IH; ih++) {
                            size_t oh = ReduceH ? 0 : ih; GET_PTR_NCDH_BLK;
                            reduce_kernel_process(in_ptr_ncdh, out_ptr_ncdh, IW * blk_size);
                        }
                    } else {
                        reduceSkipPadding(in_ptr_ncd, out_ptr_ncd, ic);
                    }
                }
            }
        } else {
            for (size_t icb = 0; icb < ICB; icb++) {
                size_t ocb = 0; GET_PTR_NC_BLK;
                size_t ic = icb * blk_size;
                for (size_t id = 0; id < ID; id++) {
                    size_t od = ReduceD ? 0 : id; GET_PTR_NCD_BLK;
                    if (ic + blk_size <= IC) {
                        for (size_t ih = 0; ih < IH; ih++) {
                            size_t oh = ReduceH ? 0 : ih; GET_PTR_NCDH_BLK;
                            parallel_for(IW, [&](size_t iw) {
                                size_t ow = iw; GET_PTR_NCDHW_BLK;
                                reduce_kernel_process(in_ptr_ncdhw, out_ptr_ncdhw, blk_size);
                            });
                        }
                    } else {
                        reduceSkipPadding(in_ptr_ncd, out_ptr_ncd, ic);
                    }
                }
            }
        }
    }

    reduce_kernel_post_process(out_ptr);
}

inline void MKLDNNReduceNode::reduce_kernel_process(const uint8_t *in_p, uint8_t *out_p, size_t work_amount,
                                                    size_t reduce_w, size_t work_batch, const int *tab_idx) {
    auto arg = jit_reduce_call_args();
    arg.src = static_cast<const void *>(in_p);
    arg.idx = tab_idx;
    arg.dst = static_cast<void *>(out_p);
    arg.work_amount = work_amount;
    arg.work_batch = work_batch;
    arg.reduce_w = reduce_w;
    arg.reduce_stride = reduce_stride;

    (*reduce_kernel)(&arg);
}

inline void MKLDNNReduceNode::reduce_kernel_post_process(uint8_t *out_ptr) {
    const size_t integerDivisor = IB * IC * ID * IH * IW / (OB * OC * OD * OH * OW);
    const float divisor = static_cast<float>(integerDivisor);
    if (layout == ReduceLayoutType::reduce_ncsp || layout == ReduceLayoutType::reduce_nspc) {
        parallel_for2d(OB, OC, [&](size_t ob, size_t oc) {
            uint8_t *out_p = out_ptr + (ob * OC + oc) * OD * OH * OW * dst_data_size;
            auto arg = jit_reduce_post_call_args();
            arg.dst = static_cast<void *>(out_p);
            arg.oc_off = layout == ReduceLayoutType::reduce_nspc ? 0 : oc * sizeof(float);
            arg.channel_size = layout == ReduceLayoutType::reduce_nspc ? OW : OC; // OW is related to nspc-ncsp dimension reinterpret
            arg.work_amount = OD * OH * OW;
            arg.divisor = &divisor;
            arg.post_op_data = static_cast<const void **>(&postOpsDataPtrs[0]);
            (*reduce_post_kernel)(&arg);
        });
    } else {
        size_t OCB = div_up(OC, blk_size);
        parallel_for2d(OB, OCB, [&](size_t ob, size_t ocb) {
            uint8_t *out_p = out_ptr + (ob * OCB + ocb) * OD * OH * OW * blk_size * dst_data_size;
            auto arg = jit_reduce_post_call_args();
            arg.dst = static_cast<void *>(out_p);
            arg.reduce_c = ReduceC ? 1 : 0;
            arg.oc_off = ocb * blk_size * sizeof(float);
            arg.work_amount = OD * OH * OW * blk_size;
            arg.divisor = &divisor;
            arg.post_op_data = static_cast<const void **>(&postOpsDataPtrs[0]);
            (*reduce_post_kernel)(&arg);
        });
    }
}

void MKLDNNReduceNode::nspc2ncsp(uint8_t *proc_ptr, uint8_t *out_ptr) {
    // dimension reinterpret after nspc reusing routine reduce_PLN
    // demote -- nspc -- ncsp
    //  DIM0  --   B  --  B
    //  DIM1  --   C  --  W
    //  DIM2  --   D  --  C
    //  DIM3  --   H  --  D
    //  DIM4  --   W  --  H
    const size_t DIM0 = OB;
    const size_t DIM1 = OW;
    const size_t DIM2 = OC;
    const size_t DIM3 = OD;
    const size_t DIM4 = OH;
    const size_t stride1 = DIM2 * DIM3 * DIM4;
    const size_t stride0 = stride1 * DIM1;

    if (dst_data_size == 4) {
        auto src_data = reinterpret_cast<const float *>(proc_ptr);
        auto dst_data = reinterpret_cast<float *>(out_ptr);
        parallel_for2d(DIM0, stride1, [&](size_t b, size_t j) {
            auto src_off = b * stride0 + j * DIM1;
            auto dst_off = b * stride0 + j;
            for (size_t dim1 = 0; dim1 < DIM1; dim1++) {
                dst_data[dst_off] = src_data[src_off];
                src_off++;
                dst_off += stride1;
            }
        });
    } else if (dst_data_size == 2) {
        auto src_data = reinterpret_cast<const uint16_t *>(proc_ptr);
        auto dst_data = reinterpret_cast<uint16_t *>(out_ptr);
        parallel_for2d(DIM0, stride1, [&](size_t b, size_t j) {
            auto src_off = b * stride0 + j * DIM1;
            auto dst_off = b * stride0 + j;
            for (size_t dim1 = 0; dim1 < DIM1; dim1++) {
                dst_data[dst_off] = src_data[src_off];
                src_off++;
                dst_off += stride1;
            }
        });
    } else {
        auto src_data = reinterpret_cast<const uint8_t *>(proc_ptr);
        auto dst_data = reinterpret_cast<uint8_t *>(out_ptr);
        parallel_for2d(DIM0, stride1, [&](size_t b, size_t j) {
            auto src_off = b * stride0 + j * DIM1;
            auto dst_off = b * stride0 + j;
            for (size_t dim1 = 0; dim1 < DIM1; dim1++) {
                dst_data[dst_off] = src_data[src_off];
                src_off++;
                dst_off += stride1;
            }
        });
    }
}

void MKLDNNReduceNode::blocked2ncsp(uint8_t *proc_ptr, uint8_t *out_ptr) {
    const size_t DIM0 = OB;
    const size_t DIM1 = OC;
    const size_t DIM2 = OD;
    const size_t DIM3 = OH;
    const size_t DIM4 = OW;
    const size_t stride1 = DIM2 * DIM3 * DIM4;
    const size_t src_stride0 = stride1 * div_up(OC, blk_size) * blk_size;
    const size_t dst_stride0 = stride1 * DIM1;

    if (dst_data_size == 4) {
        auto src_data = reinterpret_cast<const float *>(proc_ptr);
        auto dst_data = reinterpret_cast<float *>(out_ptr);
        parallel_for2d(DIM0, stride1, [&](size_t b, size_t j) {
            auto src_off = b * src_stride0 + j * blk_size;
            auto dst_off = b * dst_stride0 + j;
            for (size_t dim1 = 0; dim1 + blk_size <= DIM1; dim1 += blk_size) {
                for (size_t k = 0; k < blk_size; k++) {
                    dst_data[dst_off] = src_data[src_off];
                    src_off++;
                    dst_off += stride1;
                }
                src_off += (stride1 - 1) * blk_size;
            }
            size_t tail = DIM1 % blk_size;
            for (size_t k = 0; k < tail; k++) {
                dst_data[dst_off] = src_data[src_off];
                src_off++;
                dst_off += stride1;
            }
        });
    } else if (dst_data_size == 2) {
        auto src_data = reinterpret_cast<const uint16_t *>(proc_ptr);
        auto dst_data = reinterpret_cast<uint16_t *>(out_ptr);
        parallel_for2d(DIM0, stride1, [&](size_t b, size_t j) {
            auto src_off = b * src_stride0 + j * blk_size;
            auto dst_off = b * dst_stride0 + j;
            for (size_t dim1 = 0; dim1 + blk_size <= DIM1; dim1 += blk_size) {
                for (size_t k = 0; k < blk_size; k++) {
                    dst_data[dst_off] = src_data[src_off];
                    src_off++;
                    dst_off += stride1;
                }
                src_off += (stride1 - 1) * blk_size;
            }
            size_t tail = DIM1 % blk_size;
            for (size_t k = 0; k < tail; k++) {
                dst_data[dst_off] = src_data[src_off];
                src_off++;
                dst_off += stride1;
            }
        });
    } else {
        auto src_data = reinterpret_cast<const uint8_t *>(proc_ptr);
        auto dst_data = reinterpret_cast<uint8_t *>(out_ptr);
        parallel_for2d(DIM0, stride1, [&](size_t b, size_t j) {
            auto src_off = b * src_stride0 + j * blk_size;
            auto dst_off = b * dst_stride0 + j;
            for (size_t dim1 = 0; dim1 + blk_size <= DIM1; dim1 += blk_size) {
                for (size_t k = 0; k < blk_size; k++) {
                    dst_data[dst_off] = src_data[src_off];
                    src_off++;
                    dst_off += stride1;
                }
                src_off += (stride1 - 1) * blk_size;
            }
            size_t tail = DIM1 % blk_size;
            for (size_t k = 0; k < tail; k++) {
                dst_data[dst_off] = src_data[src_off];
                src_off++;
                dst_off += stride1;
            }
        });
    }
}

inline void MKLDNNReduceNode::init_dst_data(uint8_t *out_ptr, size_t dst_size) {
    switch (algorithm) {
        case ReduceL1:
        case ReduceL2:
        case ReduceLogSum:
        case ReduceLogSumExp:
        case ReduceMean:
        case ReduceOr:
        case ReduceSum:
        case ReduceSumSquare:
            memset(out_ptr, 0, dst_size);
            break;
        case ReduceAnd:
        case ReduceProd:
            if (output_prec == Precision::FP32) {
                auto out_p = reinterpret_cast<float *>(out_ptr);
                parallel_for(dst_size / dst_data_size, [&](size_t i) { out_p[i] = static_cast<float>(1); });
            } else if (output_prec == Precision::I32) {
                auto out_p = reinterpret_cast<int32_t *>(out_ptr);
                parallel_for(dst_size / dst_data_size, [&](size_t i) { out_p[i] = static_cast<int32_t>(1); });
            } else if (output_prec == Precision::BF16) {
                auto out_p = reinterpret_cast<bfloat16_t*>(out_ptr);
                parallel_for(dst_size / dst_data_size, [&](size_t i) { out_p[i] = static_cast<bfloat16_t>(1); });
            } else if (output_prec == Precision::U8) {
                auto out_p = reinterpret_cast<uint8_t *>(out_ptr);
                parallel_for(dst_size / dst_data_size, [&](size_t i) { out_p[i] = static_cast<uint8_t>(1); });
            } else if (output_prec == Precision::I8) {
                auto out_p = reinterpret_cast<int8_t *>(out_ptr);
                parallel_for(dst_size / dst_data_size, [&](size_t i) { out_p[i] = static_cast<int8_t>(1); });
            }
            break;
        case ReduceMax:
            if (output_prec == Precision::FP32) {
                auto out_p = reinterpret_cast<float *>(out_ptr);
                parallel_for(dst_size / dst_data_size, [&](size_t i) { out_p[i] = std::numeric_limits<float>::lowest(); });
            } else if (output_prec == Precision::I32) {
                auto out_p = reinterpret_cast<int32_t *>(out_ptr);
                parallel_for(dst_size / dst_data_size, [&](size_t i) { out_p[i] = std::numeric_limits<int32_t>::min(); });
            } else if (output_prec == Precision::BF16) {
                auto out_p = reinterpret_cast<bfloat16_t*>(out_ptr);
                parallel_for(dst_size / dst_data_size, [&](size_t i) { out_p[i] = std::numeric_limits<bfloat16_t>::lowest(); });
            } else if (output_prec == Precision::U8) {
                auto out_p = reinterpret_cast<uint8_t *>(out_ptr);
                parallel_for(dst_size / dst_data_size, [&](size_t i) { out_p[i] = std::numeric_limits<uint8_t>::min(); });
            } else if (output_prec == Precision::I8) {
                auto out_p = reinterpret_cast<int8_t *>(out_ptr);
                parallel_for(dst_size / dst_data_size, [&](size_t i) { out_p[i] = std::numeric_limits<int8_t>::min(); });
            }
            break;
        case ReduceMin:
            if (output_prec == Precision::FP32) {
                auto out_p = reinterpret_cast<float *>(out_ptr);
                parallel_for(dst_size / dst_data_size, [&](size_t i) { out_p[i] = std::numeric_limits<float>::max(); });
            } else if (output_prec == Precision::I32) {
                auto out_p = reinterpret_cast<int32_t *>(out_ptr);
                parallel_for(dst_size / dst_data_size, [&](size_t i) { out_p[i] = std::numeric_limits<int32_t>::max(); });
            } else if (output_prec == Precision::BF16) {
                auto out_p = reinterpret_cast<bfloat16_t*>(out_ptr);
                parallel_for(dst_size / dst_data_size, [&](size_t i) { out_p[i] = std::numeric_limits<bfloat16_t>::max(); });
            } else if (output_prec == Precision::U8) {
                auto out_p = reinterpret_cast<uint8_t *>(out_ptr);
                parallel_for(dst_size / dst_data_size, [&](size_t i) { out_p[i] = std::numeric_limits<uint8_t>::max(); });
            } else if (output_prec == Precision::I8) {
                auto out_p = reinterpret_cast<int8_t *>(out_ptr);
                parallel_for(dst_size / dst_data_size, [&](size_t i) { out_p[i] = std::numeric_limits<int8_t>::max(); });
            }
            break;
        default:
            IE_THROW() << errorPrefix << " gets unsupported reduce mode.";
    }
}

inline void MKLDNNReduceNode::create_working_memory() {
    auto rank = getInputShapeAtPort(REDUCE_DATA).getRank();
    memory::format_tag format = (layout == ReduceLayoutType::reduce_nspc) ? (rank == 4 ? memory::format_tag::nhwc : memory::format_tag::ndhwc)
                                        : (rank == 4 ? (mayiuse(cpu::x64::avx512_common) ? memory::format_tag::nChw16c : memory::format_tag::nChw8c)
                                                     : (mayiuse(cpu::x64::avx512_common) ? memory::format_tag::nCdhw16c : memory::format_tag::nCdhw8c));
    auto prc_dims = rank == 4 ? std::vector<size_t>{OB, OC, OH, OW} : std::vector<size_t>{OB, OC, OD, OH, OW};
    auto desc = mkldnn::memory::desc(MKLDNNExtensionUtils::convertToDnnlDims(prc_dims), MKLDNNExtensionUtils::IEPrecisionToDataType(output_prec), format);
    prc_mem = std::make_shared<mkldnn::memory>(desc, getEngine());
    dst_size = desc.get_size();
}

inline void MKLDNNReduceNode::calc_process_dst_dims(std::vector<int> &reduce_axes, const SizeVector &dst_dims) {
    std::set<size_t> axes;
    SizeVector out_dims;
    process_dst_dims.clear();
    axes_for_reduction.clear();
    for (auto &axis : reduce_axes) {
        if (axis < 0)
            axis += src_dims.size();
        if (static_cast<size_t>(axis) > src_dims.size())
            IE_THROW() << errorPrefix << " exceeds data tensor dimension on index to reduce";
        axes.insert(static_cast<size_t>(axis));
    }
    for (size_t i = 0; i < src_dims.size(); i++) {
        bool found = false;
        for (auto axis : axes) {
            if (i == axis) {
                found = true;
                break;
            }
        }
        if (found) {
            if (keep_dims) out_dims.push_back(1);
            process_dst_dims.push_back(1);
            axes_for_reduction.push_back(i);
        } else {
            out_dims.push_back(src_dims[i]);
            process_dst_dims.push_back(src_dims[i]);
        }
    }
    if (jit_mode && jit_beyond_5D) {
        if (std::accumulate(out_dims.begin(), out_dims.end(), 1, std::multiplies<double>()) !=
            std::accumulate(dst_dims.begin(), dst_dims.end(), 1, std::multiplies<double>()))
            IE_THROW() << errorPrefix << "gets incorrect number of output dimensions!";
    } else {
        for (size_t i = 0; i < std::min(out_dims.size(), dst_dims.size()); i++) {
            if (out_dims[i] != dst_dims[i])
                IE_THROW() << errorPrefix << "gets incorrect number of output dimensions!";
        }
    }
}

inline void MKLDNNReduceNode::set_reduce_dim_flags() {
    size_t dims_size = src_dims.size();
    if (dims_size == 5) {
        SET_SRC_DIM_VALUE(src_dims[0], src_dims[1], src_dims[2], src_dims[3], src_dims[4]);
        SET_DST_DIM_VALUE(process_dst_dims[0], process_dst_dims[1], process_dst_dims[2], process_dst_dims[3], process_dst_dims[4]);
    } else if (dims_size == 4) {
        SET_SRC_DIM_VALUE(src_dims[0], src_dims[1], 1, src_dims[2], src_dims[3]);
        SET_DST_DIM_VALUE(process_dst_dims[0], process_dst_dims[1], 1, process_dst_dims[2], process_dst_dims[3]);
    } else if (dims_size == 3) {
        SET_SRC_DIM_VALUE(1, src_dims[0], 1, src_dims[1], src_dims[2]);
        SET_DST_DIM_VALUE(1, process_dst_dims[0], 1, process_dst_dims[1], process_dst_dims[2]);
    } else if (dims_size == 2) {
        SET_SRC_DIM_VALUE(1, 1, 1, src_dims[0], src_dims[1]);
        SET_DST_DIM_VALUE(1, 1, 1, process_dst_dims[0], process_dst_dims[1]);
    } else {
        SET_SRC_DIM_VALUE(1, src_dims[0], 1, 1, 1);
        SET_DST_DIM_VALUE(1, process_dst_dims[0], 1, 1, 1);
    }

    // must be done before the following dimension change
    if (is_hybrid_layout) {
        create_working_memory();
    }

    // Reducing a dimesion in nspc layout can be treated as reducing another dimension in ncsp layout,
    // eg. reducing C in nspc can be treated as reducing W in ncsp layout, so that the routine reduce_PLN can be reused.
    // nspc -- ncsp
    //    D -- C
    //    H -- D
    //    W -- H
    //    C -- W
    if (layout == ReduceLayoutType::reduce_nspc) {
        size_t ITmp = IC; IC = ID; ID = IH; IH = IW; IW = ITmp;
        size_t OTmp = OC; OC = OD; OD = OH; OH = OW; OW = OTmp;
    }

    ReduceN = IB != OB && OB == 1;
    ReduceC = IC != OC && OC == 1;
    ReduceD = ID != OD && OD == 1;
    ReduceH = IH != OH && OH == 1;
    ReduceW = IW != OW && OW == 1;

    // suit for parallel
    if (ReduceH && IW == 1) {
        ReduceW = true;
    }
    if (ReduceC && ReduceH && ID == 1) {
        ReduceD = true;
    }
}

inline void MKLDNNReduceNode::reduce_ref(const float *in_ptr, float *out_ptr) {
    switch (algorithm) {
        case ReduceAnd:
            reduce_ref_process(in_ptr, out_ptr, 1, [](float x, float y)->float { return x && y; });
            break;
        case ReduceL1:
            reduce_ref_process(in_ptr, out_ptr, 0, [](float old, float y)->float { return old + (y >= 0 ? y : -y); });
            break;
        case ReduceL2:
            reduce_ref_process(in_ptr, out_ptr, 0, [](float old, float y)->float { return old + y * y; });
            break;
        case ReduceLogSum:
            reduce_ref_process(in_ptr, out_ptr, 0, [](float x, float y)->float { return x + y; });
            break;
        case ReduceLogSumExp:
            reduce_ref_process(in_ptr, out_ptr, 0, [](float old, float y)->float { return old + expf(y); });
            break;
        case ReduceMax:
            reduce_ref_process(in_ptr, out_ptr, std::numeric_limits<float>::lowest(),
                                                    [](float x, float y)->float { return x > y ? x : y; });
            break;
        case ReduceMean:
            reduce_ref_process(in_ptr, out_ptr, 0, [](float x, float y)->float { return x + y; });
            break;
        case ReduceMin:
            reduce_ref_process(in_ptr, out_ptr, std::numeric_limits<float>::max(),
                                                    [](float x, float y)->float { return x < y ? x : y; });
            break;
        case ReduceOr:
            reduce_ref_process(in_ptr, out_ptr, 0, [](float x, float y)->float { return x || y; });
            break;
        case ReduceProd:
            reduce_ref_process(in_ptr, out_ptr, 1, [](float x, float y)->float { return x * y; });
            break;
        case ReduceSum:
            reduce_ref_process(in_ptr, out_ptr, 0, [](float x, float y)->float { return x + y; });
            break;
        case ReduceSumSquare:
            reduce_ref_process(in_ptr, out_ptr, 0, [](float old, float y)->float { return old + y * y; });
            break;
    default:
        IE_THROW() << errorPrefix << "gets unsupported reduce mode.";
    }
}

void MKLDNNReduceNode::reduce_ref_process(const float *in_ptr, float *out_ptr, float init_value, std::function<float(float, float)> func) {
    size_t work_amount_dst = 1, reduced_dims_work_amount = 1;
    for (size_t i = 0; i < process_dst_dims.size(); i++)
        work_amount_dst *= process_dst_dims[i];
    for (size_t i = 0; i < src_dims.size(); i++)
        reduced_dims_work_amount *= src_dims[i];
    reduced_dims_work_amount /= work_amount_dst;

    SizeVector src_strides = getParentEdgeAt(REDUCE_DATA)->getMemory().GetDescWithType<BlockedMemoryDesc>()->getStrides();
    parallel_nt(0, [&](const int ithr, const int nthr) {
        int j;
        size_t i, start = 0, end = 0;
        SizeVector dst_counters(process_dst_dims.size(), 0);
        splitter(work_amount_dst, nthr, ithr, start, end);
        for (j = process_dst_dims.size() - 1, i = start; j >= 0; j--) {
            dst_counters[j] = i % process_dst_dims[j];
            i /= process_dst_dims[j];
        }
        for (size_t src_idx = 0, dst_idx = start; dst_idx < end; ++dst_idx) {
            float reduce_prod = init_value;
            bool update_idx = true;
            SizeVector src_counters = dst_counters;
            for (i = 0; i < reduced_dims_work_amount; ++i) {
                if (update_idx) {
                    src_idx = 0;
                    for (j = 0; j < static_cast<int>(src_dims.size()); ++j)
                        src_idx += (src_counters[j] % src_dims[j]) * src_strides[j];
                    update_idx = false;
                }
                reduce_prod = func(reduce_prod, in_ptr[src_idx]);
                for (j = axes_for_reduction.size() - 1; j >= 0; j--) {
                    src_counters[axes_for_reduction[j]]++;
                    if (src_counters[axes_for_reduction[j]] < src_dims[axes_for_reduction[j]]) {
                        src_idx += src_strides[axes_for_reduction[j]];
                        break;
                    } else {
                        src_counters[axes_for_reduction[j]] = 0;
                        update_idx = true;
                    }
                }
            }
            out_ptr[dst_idx] = reduce_prod;
            for (j = process_dst_dims.size() - 1; j >= 0; j--) {
                dst_counters[j]++;
                if (dst_counters[j] < process_dst_dims[j])
                    break;
                else
                    dst_counters[j] = 0;
            }
        }
    });

    reduce_ref_map(out_ptr, work_amount_dst, reduced_dims_work_amount);
}

inline void MKLDNNReduceNode::reduce_ref_map(float *out_ptr, size_t work_amount_dst, size_t reduced_dims_work_amount) {
    switch (algorithm) {
        case ReduceAnd:
        case ReduceL1:
        case ReduceMax:
        case ReduceMin:
        case ReduceOr:
        case ReduceProd:
        case ReduceSum:
        case ReduceSumSquare:
            break;
        case ReduceL2:
            parallel_for(work_amount_dst, [&](size_t i) {
                out_ptr[i] = std::sqrt(out_ptr[i]);
            });
            break;
        case ReduceLogSum:
        case ReduceLogSumExp:
            parallel_for(work_amount_dst, [&](size_t i) {
                out_ptr[i] = logf(out_ptr[i]);
            });
            break;
        case ReduceMean:
            parallel_for(work_amount_dst, [&](size_t i) {
                out_ptr[i] /= reduced_dims_work_amount;
            });
            break;
        default:
            IE_THROW() << errorPrefix << "gets unsupported reduce mode.";
    }
}

void MKLDNNReduceNode::setPostOps(mkldnn::primitive_attr &attr, const VectorDims &postOpDims, bool initWeights) {
    mkldnn::post_ops ops;
    for (auto &node : fusedWith) {
        auto* fakeQuantizeNode = dynamic_cast<MKLDNNFakeQuantizeNode *>(node.get());
        if (fakeQuantizeNode) {
            fakeQuantizeNode->appendPostOps(ops);
            continue;
        }

        auto* eltwiseNode = dynamic_cast<MKLDNNEltwiseNode *>(node.get());
        if (eltwiseNode) {
            eltwiseNode->appendPostOps(ops, postOpDims);
            continue;
        }
        IE_THROW() << "Fusing of " << NameFromType(node->getType()) << " operation to " << NameFromType(this->getType()) << " node is not implemented";
    }

    postOpsDataPtrs.clear();
    for (int i = 0; i < ops.len(); ++i) {
        auto &post_op = ops.get()->entry_[i];
        if (post_op.is_quantization()) {
            auto &data = post_op.quantization.data;
            postOpsDataPtrs.insert(postOpsDataPtrs.end(), std::begin(data), std::end(data));
            memset(data, 0, sizeof(data));
        } else if (post_op.is_depthwise()) {
            auto &weights = post_op.depthwise.weights_data;
            auto &biases = post_op.depthwise.biases_data;
            postOpsDataPtrs.push_back(weights);
            postOpsDataPtrs.push_back(biases);
            weights = 0;
            biases = 0;
        }
    }
    attr.set_post_ops(ops);
}

void MKLDNNReduceNode::setJITBeyond5D() {
    jit_beyond_5D = false;
    if (getInputShapeAtPort(REDUCE_DATA).getRank() > 5) {
        for (auto &axis : raw_axes) {
            if (axis < 0)
                axis += static_cast<int>(getInputShapeAtPort(REDUCE_DATA).getRank());
        }

        if (raw_axes.size() <= 1) {
            jit_beyond_5D = true;
        } else {
            for (size_t i = 1; i < raw_axes.size(); i++) {
                if (raw_axes[i] != raw_axes[i - 1] + 1) {
                    jit_beyond_5D = false;
                    break;
                }
                jit_beyond_5D = true;
            }
        }
    }
}

std::vector<int> MKLDNNReduceNode::update_src_dims() {
    std::vector<int> reduce_axes = raw_axes;

    if (reduce_axes.size() < 1)
        return reduce_axes;

    size_t axis_dim = 1;
    size_t outer_dim = 1;
    size_t inner_dim = 1;
    int outer_end = reduce_axes[0];
    int inner_start = reduce_axes[reduce_axes.size() - 1];
    for (size_t i = 0; i < src_dims.size(); i++) {
        if (i < outer_end) {
            outer_dim *= src_dims[i];
        } else if (i > inner_start) {
            inner_dim *= src_dims[i];
        } else {
            axis_dim *= src_dims[i];
        }
    }

    reduce_axes.clear();
    reduce_axes.push_back(1);

    src_dims.clear();
    src_dims.push_back(outer_dim);
    src_dims.push_back(axis_dim);
    src_dims.push_back(inner_dim);

    return reduce_axes;
}

bool MKLDNNReduceNode::canApplyJIT(const Precision &input_prec, const Precision &output_prec) const {
    static const Precision supportedPrecisions[] = {
            Precision::FP32,
            Precision::BF16,
            Precision::I32,
            Precision::I8,
            Precision::U8
    };

    return (mayiuse(cpu::x64::sse41)) && (getInputShapeAtPort(REDUCE_DATA).getRank() <= 5 || jit_beyond_5D) &&
           std::find(std::begin(supportedPrecisions), std::end(supportedPrecisions), input_prec) != std::end(supportedPrecisions) &&
           std::find(std::begin(supportedPrecisions), std::end(supportedPrecisions), output_prec) != std::end(supportedPrecisions);
}

bool MKLDNNReduceNode::canFuse(const MKLDNNNodePtr& node) const {
    Precision input_prec = getOriginalInputPrecisionAtPort(REDUCE_DATA);
    Precision output_prec = getOriginalOutputPrecisionAtPort(0);
    if (!canApplyJIT(input_prec, output_prec) || jit_beyond_5D || algorithm == ReduceAnd || algorithm == ReduceOr) {
        return false;
    }

    // In jit mode we use the output memory as an intermediate accumulator for certain reduce modes.
    // If the post ops node has a lower precision for such modes, post ops fusing won't be supposted, in order to avoid accuracy loss.
    if (output_prec == Precision::FP32 &&
        !node->getOriginalOutputPrecisions().empty() && node->getOriginalOutputPrecisionAtPort(0) != Precision::FP32) {
        if (algorithm != ReduceAnd && algorithm != ReduceOr &&
            algorithm != ReduceMin && algorithm != ReduceMax) {
            return false;
        }
    }

    return canFuseSimpleOperation(node);
}

bool MKLDNNReduceNode::created() const {
    return getType() == Reduce;
}

REG_MKLDNN_PRIM_FOR(MKLDNNReduceNode, Reduce);
