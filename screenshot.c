/*
 * This file is part of mplayer2.
 *
 * mplayer2 is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * mplayer2 is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with mplayer2; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <assert.h>

#include <libswscale/swscale.h>
#include <libavcodec/avcodec.h>

#include "config.h"
#include "talloc.h"
#include "screenshot.h"
#include "mp_core.h"
#include "mp_msg.h"
#include "path.h"
#include "libmpcodecs/img_format.h"
#include "libmpcodecs/mp_image.h"
#include "libmpcodecs/dec_video.h"
#include "libmpcodecs/vf.h"
#include "libvo/video_out.h"

#include "fmt-conversion.h"

//for sws_getContextFromCmdLine_hq and mp_sws_set_colorspace
#include "libmpcodecs/vf_scale.h"
#include "libvo/csputils.h"

typedef struct screenshot_ctx {
    AVFrame *pic;
    int full_window;
    int each_frame;
    int using_vf_screenshot;

    int frameno;
    char fname[102];
} screenshot_ctx;

static int destroy_ctx(void *ptr)
{
    struct screenshot_ctx *ctx = ptr;
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(54, 28, 0)
    avcodec_free_frame(&ctx->pic);
#else
    av_free(ctx->pic);
#endif
    return 0;
}

static screenshot_ctx *screenshot_get_ctx(MPContext *mpctx)
{
    if (!mpctx->screenshot_ctx) {
        struct screenshot_ctx *ctx = talloc_zero(mpctx, screenshot_ctx);
        talloc_set_destructor(ctx, destroy_ctx);
        ctx->pic = avcodec_alloc_frame();
        assert(ctx->pic);
        mpctx->screenshot_ctx = ctx;
    }
    return mpctx->screenshot_ctx;
}

static int write_png(screenshot_ctx *ctx, struct mp_image *image)
{
    char *fname = ctx->fname;
    FILE *fp = NULL;
    int success = 0;
    AVPacket packet = {0};

    struct AVCodec *png_codec = avcodec_find_encoder(AV_CODEC_ID_PNG);
    AVCodecContext *avctx = NULL;
    if (!png_codec)
        goto print_open_fail;
    avctx = avcodec_alloc_context3(png_codec);
    if (!avctx)
        goto print_open_fail;

    avctx->time_base = AV_TIME_BASE_Q;
    avctx->width = image->width;
    avctx->height = image->height;
    avctx->pix_fmt = PIX_FMT_RGB24;
    avctx->compression_level = 0;

    if (avcodec_open2(avctx, png_codec, NULL) < 0) {
     print_open_fail:
        mp_msg(MSGT_CPLAYER, MSGL_INFO, "Could not open libavcodec PNG encoder"
               " for saving screenshot\n");
        goto error_exit;
    }

    AVFrame *pic = ctx->pic;
    avcodec_get_frame_defaults(pic);
    for (int n = 0; n < 4; n++) {
        pic->data[n] = image->planes[n];
        pic->linesize[n] = image->stride[n];
    }
    int got_packet;
    int res = avcodec_encode_video2(avctx, &packet, pic, &got_packet);
    if (res < 0 || !got_packet)
        goto error_exit;

    fp = fopen(fname, "wb");
    if (fp == NULL) {
        mp_msg(MSGT_CPLAYER, MSGL_ERR, "\nPNG Error opening %s for writing!\n",
               fname);
        goto error_exit;
    }

    fwrite(packet.data, packet.size, 1, fp);
    fflush(fp);

    if (ferror(fp))
        goto error_exit;

    success = 1;
error_exit:
    if (avctx)
        avcodec_close(avctx);
    av_free(avctx);
    av_packet_unref(&packet);
    if (fp)
        fclose(fp);
    return success;
}

static int fexists(char *fname)
{
    return mp_path_exists(fname);
}

static void gen_fname(screenshot_ctx *ctx)
{
    do {
        snprintf(ctx->fname, 100, "shot%04d.png", ++ctx->frameno);
    } while (fexists(ctx->fname) && ctx->frameno < 100000);
    if (fexists(ctx->fname)) {
        ctx->fname[0] = '\0';
        return;
    }

    mp_msg(MSGT_CPLAYER, MSGL_INFO, "*** screenshot '%s' ***\n", ctx->fname);

}

void screenshot_save(struct MPContext *mpctx, struct mp_image *image)
{
    screenshot_ctx *ctx = screenshot_get_ctx(mpctx);
    struct mp_image *dst = alloc_mpi(image->w, image->h, IMGFMT_RGB24);

    struct SwsContext *sws = sws_getContextFromCmdLine_hq(image->width,
                                                          image->height,
                                                          image->imgfmt,
                                                          dst->width,
                                                          dst->height,
                                                          dst->imgfmt);

    struct mp_csp_details colorspace;
    get_detected_video_colorspace(mpctx->sh_video, &colorspace);
    // this is a property of the output device; images always use full-range RGB
    colorspace.levels_out = MP_CSP_LEVELS_PC;
    mp_sws_set_colorspace(sws, &colorspace);

    sws_scale(sws, (const uint8_t **)image->planes, image->stride, 0,
              image->height, dst->planes, dst->stride);

    gen_fname(ctx);
    write_png(ctx, dst);

    sws_freeContext(sws);
    free_mp_image(dst);
}

static void vf_screenshot_callback(void *pctx, struct mp_image *image)
{
    struct MPContext *mpctx = (struct MPContext *)pctx;
    screenshot_ctx *ctx = screenshot_get_ctx(mpctx);
    screenshot_save(mpctx, image);
    if (ctx->each_frame)
        screenshot_request(mpctx, 0, ctx->full_window);
}

void screenshot_request(struct MPContext *mpctx, bool each_frame,
                        bool full_window)
{
    if (mpctx->video_out && mpctx->video_out->config_ok) {
        screenshot_ctx *ctx = screenshot_get_ctx(mpctx);

        ctx->using_vf_screenshot = 0;

        if (each_frame) {
            ctx->each_frame = !ctx->each_frame;
            ctx->full_window = full_window;
            if (!ctx->each_frame)
                return;
        }

        struct voctrl_screenshot_args args = { .full_window = full_window };
        if (vo_control(mpctx->video_out, VOCTRL_SCREENSHOT, &args) == true) {
            screenshot_save(mpctx, args.out_image);
            free_mp_image(args.out_image);
        } else {
            mp_msg(MSGT_CPLAYER, MSGL_INFO, "No VO support for taking"
                   " screenshots, trying VFCTRL_SCREENSHOT!\n");
            ctx->using_vf_screenshot = 1;
            struct vf_ctrl_screenshot cmd = {
                .image_callback = vf_screenshot_callback,
                .image_callback_ctx = mpctx,
            };
            struct vf_instance *vfilter = mpctx->sh_video->vfilter;
            if (vfilter->control(vfilter, VFCTRL_SCREENSHOT, &cmd) !=
                    CONTROL_OK)
                mp_msg(MSGT_CPLAYER, MSGL_INFO,
                       "...failed (need --vf=screenshot?)\n");
        }
    }
}

void screenshot_flip(struct MPContext *mpctx)
{
    screenshot_ctx *ctx = screenshot_get_ctx(mpctx);

    if (!ctx->each_frame)
        return;

    // screenshot_flip is called when the VO presents a new frame. vf_screenshot
    // can behave completely different (consider filters inserted between
    // vf_screenshot and vf_vo, that add or remove frames), so handle this case
    // somewhere else.
    if (ctx->using_vf_screenshot)
        return;

    screenshot_request(mpctx, 0, ctx->full_window);
}
