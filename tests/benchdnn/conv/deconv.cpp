/*******************************************************************************
* Copyright 2018-2022 Intel Corporation
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
*     http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*******************************************************************************/

#include <utility>

#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "oneapi/dnnl/dnnl.h"

#include "utils/parallel.hpp"

#include "dnnl_common.hpp"
#include "dnnl_memory.hpp"

#include "binary/binary.hpp"
#include "conv/deconv.hpp"
#include "prelu/prelu.hpp"
using namespace conv;

namespace deconv {

int transpose_data_wei(
        const prb_t *prb, const dnn_mem_t &wei, const dnn_mem_t &wei_tr) {
    benchdnn_parallel_nd(prb->g, prb->oc / prb->g, prb->ic / prb->g, prb->kd,
            prb->kh, prb->kw,
            [&](int64_t g, int64_t oc, int64_t ic, int64_t kd, int64_t kh,
                    int64_t kw) {
                int64_t ch_idx
                        = (g * prb->ic / prb->g + ic) * prb->oc / prb->g + oc;
                int64_t idx = ((ch_idx * prb->kd + kd) * prb->kh + kh) * prb->kw
                        + kw;
                ((float *)wei_tr)[idx]
                        = ((float *)wei)[wei_off_f(prb, g, oc, ic, kd, kh, kw)];
            });

    return OK;
}

dnnl_status_t init_pd(dnnl_engine_t engine, const prb_t *prb,
        dnnl_primitive_desc_t &dpd, res_t *res, dir_t dir,
        const_dnnl_primitive_desc_t hint) {
    dnnl_deconvolution_desc_t cd;

    dnnl_dims_t src_1d_dims = {prb->mb, prb->ic, prb->iw};
    dnnl_dims_t src_2d_dims = {prb->mb, prb->ic, prb->ih, prb->iw};
    dnnl_dims_t src_3d_dims = {prb->mb, prb->ic, prb->id, prb->ih, prb->iw};
    dnnl_dim_t *src_dims = prb->ndims == 5
            ? src_3d_dims
            : prb->ndims == 4 ? src_2d_dims : src_1d_dims;

    dnnl_dims_t wei_1d_dims
            = {prb->g, prb->oc / prb->g, prb->ic / prb->g, prb->kw};
    dnnl_dims_t wei_2d_dims
            = {prb->g, prb->oc / prb->g, prb->ic / prb->g, prb->kh, prb->kw};
    dnnl_dims_t wei_3d_dims = {prb->g, prb->oc / prb->g, prb->ic / prb->g,
            prb->kd, prb->kh, prb->kw};
    dnnl_dim_t *wei_dims = prb->ndims == 5
            ? &wei_3d_dims[!prb->has_groups]
            : prb->ndims == 4 ? &wei_2d_dims[!prb->has_groups]
                              : &wei_1d_dims[!prb->has_groups];

    dnnl_dims_t bia_dims = {prb->oc};

    dnnl_dims_t dst_1d_dims = {prb->mb, prb->oc, prb->ow};
    dnnl_dims_t dst_2d_dims = {prb->mb, prb->oc, prb->oh, prb->ow};
    dnnl_dims_t dst_3d_dims = {prb->mb, prb->oc, prb->od, prb->oh, prb->ow};
    dnnl_dim_t *dst_dims = prb->ndims == 5
            ? dst_3d_dims
            : prb->ndims == 4 ? dst_2d_dims : dst_1d_dims;

    auto src_d = dnn_mem_t::init_md(
            prb->ndims, src_dims, prb->cfg[SRC].dt, prb->stag);
    auto wei_d = dnn_mem_t::init_md(prb->ndims + prb->has_groups, wei_dims,
            prb->cfg[WEI].dt, prb->wtag);
    auto bia_d = dnn_mem_t::init_md(1, bia_dims, prb->cfg[BIA].dt, tag::any);
    auto dst_d = dnn_mem_t::init_md(
            prb->ndims, dst_dims, prb->cfg[DST].dt, prb->dtag);

    dnnl_dim_t strides_nd[] = {prb->sd, prb->sh, prb->sw};
    dnnl_dim_t dilates_nd[] = {prb->dd, prb->dh, prb->dw};
    dnnl_dim_t padding_nd[] = {prb->pd, prb->ph, prb->pw};
    dnnl_dim_t padding_r_nd[] = {prb->pd_r, prb->ph_r, prb->pw_r};

    dnnl_dim_t *strides = strides_nd + (5 - prb->ndims);
    dnnl_dim_t *dilates = dilates_nd + (5 - prb->ndims);
    dnnl_dim_t *padding = padding_nd + (5 - prb->ndims);
    dnnl_dim_t *padding_r = padding_r_nd + (5 - prb->ndims);

    dnnl_alg_kind_t alg = dnnl_deconvolution_direct;
    if (prb->alg == WINO) alg = dnnl_deconvolution_winograd;

    switch (prb->dir) {
        case FWD_D:
        case FWD_B:
        case FWD_I:
            DNN_SAFE_STATUS(dnnl_dilated_deconvolution_forward_desc_init(&cd,
                    prb->dir == FWD_I ? dnnl_forward_inference
                                      : dnnl_forward_training,
                    alg, &src_d, &wei_d, prb->dir == FWD_B ? &bia_d : nullptr,
                    &dst_d, strides, dilates, padding, padding_r));
            break;
        case BWD_D:
            DNN_SAFE_STATUS(dnnl_dilated_deconvolution_backward_data_desc_init(
                    &cd, alg, &src_d, &wei_d, &dst_d, strides, dilates, padding,
                    padding_r));
            break;
        case BWD_W:
        case BWD_WB:
            DNN_SAFE_STATUS(
                    dnnl_dilated_deconvolution_backward_weights_desc_init(&cd,
                            alg, &src_d, &wei_d,
                            prb->dir == BWD_W ? nullptr : &bia_d, &dst_d,
                            strides, dilates, padding, padding_r));
            break;
        default: DNN_SAFE_STATUS(dnnl_invalid_arguments);
    }

    DNN_SAFE_STATUS(cd.accum_data_type == prb->cfg[ACC].dt
                    ? dnnl_success
                    : dnnl_unimplemented);

    attr_args_t attr_args;
    attr_args.prepare_output_scales(prb->attr, prb->scales, prb->oc);
    attr_args.prepare_post_ops_mds(prb->attr, prb->ndims, dst_dims);
    auto dnnl_attr = make_benchdnn_dnnl_wrapper(
            create_dnnl_attr(prb->attr, attr_args));

    return dnnl_primitive_desc_create(&dpd, &cd, dnnl_attr, engine, nullptr);
}

int init_prim_ref(
        benchdnn_dnnl_wrapper_t<dnnl_primitive_t> &prim_ref, const prb_t *prb) {
    if (!(is_bench_mode(CORR) && is_gpu() && fast_ref_gpu)) return OK;

    // Create a new copy of prb to avoid potentially corrupting the test by
    // modifying prb in place.
    // DIRECT algorithm is used to prevent fallback  to the slow benchdnn
    // reference implementation.
    auto cpu_attr = prb->attr;
    update_cpu_ref_attrs(cpu_attr);
    prb_t prb_cpu {*prb, prb->dir, conf_f32, tag::abx, tag::abx, tag::abx,
            DIRECT, cpu_attr, prb->mb, prb->is_deconv};
    dnnl_primitive_desc_t pd_ref_ {};
    init_pd(get_cpu_engine(), &prb_cpu, pd_ref_, nullptr, prb->dir, nullptr);
    auto pd_ref = make_benchdnn_dnnl_wrapper(pd_ref_);

    dnnl_primitive_t prim_ref_ {};
    if (pd_ref) {
        DNN_SAFE(dnnl_primitive_create(&prim_ref_, pd_ref), WARN);
        BENCHDNN_PRINT(
                5, "%s\n", "benchdnn: use CPU primitive as the reference");
    }
    prim_ref.reset(prim_ref_);
    return OK;
}

void check_known_skipped_case(const prb_t *prb, res_t *res) {
    check_known_skipped_case_common(
            {prb->cfg[SRC].dt, prb->cfg[WEI].dt, prb->cfg[DST].dt}, prb->dir,
            res);
    if (res->state == SKIPPED) return;

    // GPU:
    //     * BWD: doesn't support any attributes
    //     * FWD: support only post ops and all but x8s8bf16 cfg
    if (is_gpu()) {
        const bool only_non_default_post_ops = prb->attr.oscale.is_def()
                && prb->attr.scales.is_def() && prb->attr.zero_points.is_def();
        const bool is_x8s8bf16_cfg
                = prb->cfg[WEI].dt == dnnl_s8 && prb->cfg[DST].dt == dnnl_bf16;
        const bool fwd_ok = !is_x8s8bf16_cfg
                && IMPLICATION(
                        (prb->dir & FLAG_FWD), only_non_default_post_ops);
        const bool bwd_ok
                = IMPLICATION((prb->dir & FLAG_BWD), prb->attr.is_def());
        if (!fwd_ok || !bwd_ok) {
            res->state = SKIPPED, res->reason = CASE_NOT_SUPPORTED;
            return;
        }
    }
}

int doit(const prb_t *prb, res_t *res) {
    if (bench_mode == LIST) return res->state = LISTED, OK;

    check_known_skipped_case(prb, res);
    check_sum_post_ops(prb->attr, res);
    if (res->state == SKIPPED) return OK;

    benchdnn_dnnl_wrapper_t<dnnl_primitive_t> prim;
    SAFE(init_prim(prim, init_pd, prb, res), WARN);
    if (res->state == SKIPPED || res->state == UNIMPLEMENTED) return OK;

    auto const_pd = query_pd(prim);

    if (check_mem_size(const_pd) != OK) {
        return res->state = SKIPPED, res->reason = NOT_ENOUGH_RAM, OK;
    }

    const auto &src_md = prb->dir == BWD_D
            ? query_md(const_pd, DNNL_ARG_DIFF_SRC)
            : query_md(const_pd, DNNL_ARG_SRC);
    const auto &wei_md = prb->dir & FLAG_WEI
            ? query_md(const_pd, DNNL_ARG_DIFF_WEIGHTS)
            : query_md(const_pd, DNNL_ARG_WEIGHTS);
    const auto &bia_md = prb->dir & FLAG_WEI
            ? query_md(const_pd, DNNL_ARG_DIFF_BIAS)
            : query_md(const_pd, DNNL_ARG_BIAS);
    const auto &dst_md = prb->dir & FLAG_BWD
            ? query_md(const_pd, DNNL_ARG_DIFF_DST)
            : query_md(const_pd, DNNL_ARG_DST);
    const auto &scratchpad_md = query_md(const_pd, DNNL_ARG_SCRATCHPAD);
    auto wei_tr_md = wei_md;

    const bool with_groups = true;
    std::swap(wei_tr_md.dims[with_groups + 0], wei_tr_md.dims[with_groups + 1]);

    const auto fp = dnnl_f32;
    const auto src_tag = tag::abx;
    const auto wei_tag = tag::abx;

    // Use CPU prim as the reference in GPU testing to reduce testing time.
    benchdnn_dnnl_wrapper_t<dnnl_primitive_t> prim_ref;
    SAFE(init_prim_ref(prim_ref, prb), WARN);

    const auto &test_engine = get_test_engine();
    const auto &ref_engine = get_cpu_engine();

    dnn_mem_t src_dt(src_md, test_engine);
    dnn_mem_t wei_dt(wei_md, test_engine);
    dnn_mem_t dst_dt(dst_md, test_engine);
    dnn_mem_t bia_dt(bia_md, test_engine);
    dnn_mem_t scratchpad_dt(scratchpad_md, test_engine);
    std::vector<dnn_mem_t> binary_po_fp, binary_po_dt;
    std::vector<int> binary_po_args;
    SAFE(binary::setup_binary_po(
                 const_pd, binary_po_args, binary_po_dt, binary_po_fp),
            WARN);
    std::vector<dnn_mem_t> prelu_po_fp, prelu_po_dt;
    std::vector<int> prelu_po_args;
    SAFE(prelu::setup_prelu_po(
                 const_pd, prelu_po_args, prelu_po_fp, prelu_po_dt),
            WARN);

    dnn_mem_t src_fp(src_md, fp, src_tag, ref_engine);
    dnn_mem_t wei_fp(wei_md, fp, wei_tag, ref_engine);
    dnn_mem_t dst_fp(dst_md, fp, src_tag, ref_engine);
    dnn_mem_t wei_tr_fp(wei_tr_md, fp, wei_tag, ref_engine);
    dnn_mem_t bia_fp(bia_md, fp, tag::x, ref_engine);
    dnn_mem_t scratchpad_fp;

    if (prim_ref)
        scratchpad_fp = dnn_mem_t(
                query_md(query_pd(prim_ref), DNNL_ARG_SCRATCHPAD), ref_engine);

    dnn_mem_t src_zero_points_m;
    dnn_mem_t dst_zero_points_m;

    /* fill memory + reorders <-> */
    if (need_dst_init(prb)) SAFE(fill_dst(prb, dst_dt, dst_fp, res), WARN);
    if (need_src_init(prb)) SAFE(fill_src(prb, src_dt, src_fp, res), WARN);
    if (need_wei_init(prb)) {
        SAFE(fill_wei(prb, wei_dt, wei_fp, res), WARN);
        SAFE(transpose_data_wei(prb, wei_fp, wei_tr_fp), WARN);
    }
    if (need_bia_init(prb)) SAFE(fill_bia(prb, bia_dt, bia_fp, res), WARN);

    args_t args, ref_args;

    if (prb->dir & FLAG_FWD) {
        maybe_prepare_runtime_zero_points(src_zero_points_m, prb->attr,
                DNNL_ARG_SRC, prb->ic, prb->src_zp);
        maybe_prepare_runtime_zero_points(dst_zero_points_m, prb->attr,
                DNNL_ARG_DST, prb->oc, prb->dst_zp);

        args.set(DNNL_ARG_SRC, src_dt);
        args.set(DNNL_ARG_WEIGHTS, wei_dt);
        args.set(DNNL_ARG_BIAS, bia_dt);
        args.set(DNNL_ARG_DST, dst_dt);
        args.set(DNNL_ARG_SCRATCHPAD, scratchpad_dt);
        args.set(binary_po_args, binary_po_dt);
        args.set(prelu_po_args, prelu_po_dt);
        args.set(DNNL_ARG_ATTR_ZERO_POINTS | DNNL_ARG_SRC, src_zero_points_m);
        args.set(DNNL_ARG_ATTR_ZERO_POINTS | DNNL_ARG_DST, dst_zero_points_m);

        SAFE(execute_and_wait(prim, args, res), WARN);

        if (is_bench_mode(CORR)) {
            ref_args.set(DNNL_ARG_SRC, src_fp);
            ref_args.set(DNNL_ARG_WEIGHTS, wei_fp);
            ref_args.set(DNNL_ARG_BIAS, bia_fp);
            ref_args.set(DNNL_ARG_DST, dst_fp);
            ref_args.set(DNNL_ARG_DIFF_WEIGHTS, wei_tr_fp); // Hack. See ref.
            ref_args.set(DNNL_ARG_SCRATCHPAD, scratchpad_fp);
            ref_args.set(binary_po_args, binary_po_fp);
            ref_args.set(prelu_po_args, prelu_po_fp);

            check_correctness(
                    prb, {DST}, args, ref_args, conv::setup_cmp, res, prim_ref);
        }
    } else if (prb->dir == BWD_D) {
        args.set(DNNL_ARG_DIFF_DST, dst_dt);
        args.set(DNNL_ARG_WEIGHTS, wei_dt);
        args.set(DNNL_ARG_DIFF_SRC, src_dt);
        args.set(DNNL_ARG_SCRATCHPAD, scratchpad_dt);

        SAFE(execute_and_wait(prim, args, res), WARN);

        if (is_bench_mode(CORR)) {
            ref_args.set(DNNL_ARG_DIFF_SRC, src_fp);
            ref_args.set(DNNL_ARG_WEIGHTS, wei_fp);
            ref_args.set(DNNL_ARG_DIFF_DST, dst_fp);
            ref_args.set(DNNL_ARG_DIFF_WEIGHTS, wei_tr_fp); // Hack. See ref.
            ref_args.set(DNNL_ARG_SCRATCHPAD, scratchpad_fp);

            check_correctness(
                    prb, {SRC}, args, ref_args, conv::setup_cmp, res, prim_ref);
        }
    } else if (prb->dir & FLAG_BWD && prb->dir & FLAG_WEI) {
        args.set(DNNL_ARG_SRC, src_dt);
        args.set(DNNL_ARG_DIFF_DST, dst_dt);
        args.set(DNNL_ARG_DIFF_WEIGHTS, wei_dt);
        args.set(DNNL_ARG_DIFF_BIAS, bia_dt);
        args.set(DNNL_ARG_SCRATCHPAD, scratchpad_dt);

        SAFE(execute_and_wait(prim, args, res), WARN);

        if (is_bench_mode(CORR)) {
            ref_args.set(DNNL_ARG_SRC, src_fp);
            ref_args.set(DNNL_ARG_WEIGHTS, wei_tr_fp); // Hack. See ref.
            ref_args.set(DNNL_ARG_DIFF_DST, dst_fp);
            ref_args.set(DNNL_ARG_DIFF_WEIGHTS, wei_fp);
            ref_args.set(DNNL_ARG_DIFF_BIAS, bia_fp);
            ref_args.set(DNNL_ARG_SCRATCHPAD, scratchpad_fp);

            check_correctness(prb, {WEI, BIA}, args, ref_args, conv::setup_cmp,
                    res, prim_ref);
        }
    } else {
        SAFE(FAIL, CRIT);
    }

    return measure_perf(res, prim, args);
}

} // namespace deconv
