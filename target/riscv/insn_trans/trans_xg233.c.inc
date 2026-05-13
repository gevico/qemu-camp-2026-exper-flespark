#include "qemu/typedefs.h"
#define REQUIRE_XG233(ctx) do {                  \
    if (!ctx->cfg_ptr->ext_xg233) {              \
        return false;                            \
    }                                            \
} while (0);

static bool trans_dma(DisasContext *ctx, arg_r *a)
{
    REQUIRE_XG233(ctx);
    TCGv src = get_gpr(ctx, a->rs1, EXT_NONE);
    TCGv dst = get_gpr(ctx, a->rd, EXT_NONE);
    TCGv gran = get_gpr(ctx, a->rs2, EXT_NONE);

    gen_helper_xg233_dma(tcg_env, src, dst, gran);

    return true;
}

static bool trans_sort(DisasContext *ctx, arg_r *a)
{
    REQUIRE_XG233(ctx);
    TCGv src = get_gpr(ctx, a->rs1, EXT_NONE);
    TCGv size = get_gpr(ctx, a->rs2, EXT_NONE);
    TCGv cnt = get_gpr(ctx, a->rd, EXT_NONE);

    gen_helper_xg233_sort(tcg_env, src, size, cnt);

    return true;
}

static bool trans_crush(DisasContext *ctx, arg_r *a)
{
    REQUIRE_XG233(ctx);
    TCGv src = get_gpr(ctx, a->rs1, EXT_NONE);
    TCGv len = get_gpr(ctx, a->rs2, EXT_NONE);
    TCGv dst = get_gpr(ctx, a->rd, EXT_NONE);

    gen_helper_xg233_crush(tcg_env, src, dst, len);

    return true;
}

static bool trans_expand(DisasContext *ctx, arg_r *a)
{
    REQUIRE_XG233(ctx);
    TCGv src = get_gpr(ctx, a->rs1, EXT_NONE);
    TCGv len = get_gpr(ctx, a->rs2, EXT_NONE);
    TCGv dst = get_gpr(ctx, a->rd, EXT_NONE);

    gen_helper_xg233_expand(tcg_env, src, dst, len);

    return true;
}

static bool trans_vdot(DisasContext *ctx, arg_r *a)
{
    REQUIRE_XG233(ctx);
    TCGv out = tcg_temp_new();
    TCGv vec1 = get_gpr(ctx, a->rs1, EXT_NONE);
    TCGv vec2 = get_gpr(ctx, a->rs2, EXT_NONE);

    gen_helper_xg233_vdot(out, tcg_env, vec1, vec2);
    gen_set_gpr(ctx, a->rd, out);

    return true;
}

static bool trans_vrelu(DisasContext *ctx, arg_r *a)
{
    REQUIRE_XG233(ctx);
    TCGv src = get_gpr(ctx, a->rs1, EXT_NONE);
    TCGv len = get_gpr(ctx, a->rs2, EXT_NONE);
    TCGv dst = get_gpr(ctx, a->rd, EXT_NONE);

    gen_helper_xg233_vrelu(tcg_env, src, dst, len);

    return true;
}

static bool trans_vscale(DisasContext *ctx, arg_r *a)
{
    REQUIRE_XG233(ctx);
    TCGv src = get_gpr(ctx, a->rs1, EXT_NONE);
    TCGv factor = get_gpr(ctx, a->rs2, EXT_NONE);
    TCGv dst = get_gpr(ctx, a->rd, EXT_NONE);

    gen_helper_xg233_vscale(tcg_env, src, dst, factor);

    return true;
}

static bool trans_vmax(DisasContext *ctx, arg_r *a)
{
    REQUIRE_XG233(ctx);
    TCGv src = get_gpr(ctx, a->rs1, EXT_NONE);
    TCGv len = get_gpr(ctx, a->rs2, EXT_NONE);
    TCGv out = tcg_temp_new();

    gen_helper_xg233_vmax(out, tcg_env, src, len);
    gen_set_gpr(ctx, a->rd, out);

    return true;
}

static bool trans_gemm(DisasContext *ctx, arg_r *a)
{
    REQUIRE_XG233(ctx);
    TCGv A = get_gpr(ctx, a->rs1, EXT_NONE);
    TCGv B = get_gpr(ctx, a->rs2, EXT_NONE);
    TCGv C = get_gpr(ctx, a->rd, EXT_NONE);

    gen_helper_xg233_gemm(tcg_env, A, B, C);

    return true;
}

static bool trans_vadd(DisasContext *ctx, arg_r *a)
{
    REQUIRE_XG233(ctx);
    TCGv A = get_gpr(ctx, a->rs1, EXT_NONE);
    TCGv B = get_gpr(ctx, a->rs2, EXT_NONE);
    TCGv C = get_gpr(ctx, a->rd, EXT_NONE);

    gen_helper_xg233_vadd(tcg_env, A, B, C);

    return true;
}