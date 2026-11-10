/* qsv_common.h
 *
 * Copyright (c) 2003-2026 HandBrake Team
 * This file is part of the HandBrake source code.
 * Homepage: <http://handbrake.fr/>.
 * It may be used under the terms of the GNU General Public License v2.
 * For full terms see the file COPYING file or visit http://www.gnu.org/licenses/gpl-2.0.html
 */

#ifndef HANDBRAKE_QSV_COMMON_H
#define HANDBRAKE_QSV_COMMON_H

int hb_qsv_available(void);

#include "handbrake/project.h"

#if HB_PROJECT_FEATURE_QSV

// Public API
hb_list_t * hb_qsv_adapters_list(void);
int         hb_qsv_get_adapter_index(void);
int         hb_qsv_hyper_encode_available(int adapter_index);

#ifdef __LIBHB__
// Private API

#include "libavcodec/avcodec.h"

enum
{
    HB_QSV_FIELD_ORDER_PROGRESSIVE,
    HB_QSV_FIELD_ORDER_TFF,
    HB_QSV_FIELD_ORDER_BFF,
};

typedef struct hb_qsv_context_s
{
    int  la_is_enabled;
    int  system_memory;     // memory-type=system, no full QSV path
    int  field_order;       // HB_QSV_FIELD_ORDER_*
    char vpp_scale_mode[16];
} hb_qsv_context_t;

#define HB_QSV_FFMPEG_EXTRA_HW_FRAMES (60)

void hb_qsv_info_print(void);
int  hb_qsv_video_encoder_is_available(int encoder);
int  hb_qsv_decode_is_codec_supported(int adapter_index, int codec_id, int pix_fmt, int width, int height);

const char * hb_qsv_video_quality_get_name(uint32_t codec);
void         hb_qsv_video_quality_get_limits(uint32_t codec, float *low, float *high, float *granularity, int *direction);

int  hb_qsv_setup_job(hb_job_t *job);
int  hb_qsv_full_path_allowed(const hb_job_t *job);
int  hb_qsv_apply_encoder_options(hb_job_t *job, AVCodecContext *context, AVDictionary **av_opts);
void hb_qsv_set_frame_field_order(const hb_job_t *job, AVFrame *frame);

hb_qsv_context_t * hb_qsv_context_init(void);
hb_qsv_context_t * hb_qsv_context_dup(const hb_qsv_context_t *src);
void               hb_qsv_context_close(hb_qsv_context_t **_ctx);

#endif // __LIBHB__
#endif // HB_PROJECT_FEATURE_QSV
#endif // HANDBRAKE_QSV_COMMON_H
