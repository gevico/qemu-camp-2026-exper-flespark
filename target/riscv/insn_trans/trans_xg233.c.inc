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