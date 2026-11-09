/* qsv_common.c
 *
 * Copyright (c) 2003-2026 HandBrake Team
 * This file is part of the HandBrake source code.
 * Homepage: <http://handbrake.fr/>.
 * It may be used under the terms of the GNU General Public License v2.
 * For full terms see the file COPYING file or visit http://www.gnu.org/licenses/gpl-2.0.html
 */

#if defined(_WIN32) || defined(__MINGW32__)
#define COBJMACROS
#endif

#include "handbrake/handbrake.h"
#include "handbrake/project.h"

#if HB_PROJECT_FEATURE_QSV

#include "handbrake/hwaccel.h"
#include "handbrake/hb_dict.h"
#include "handbrake/qsv_common.h"
#include "handbrake/hbffmpeg.h"
#include "libavutil/avstring.h"

#if defined(_WIN32) || defined(__MINGW32__)
#include "libavutil/hwcontext_d3d11va.h"
#else
#include <unistd.h>
#include "libavutil/hwcontext_vaapi.h"
#endif

/*
 * Everything Quick Sync specific is done by FFmpeg: the adapters are
 * opened with av_hwdevice_ctx_create(), the encoders are probed with
 * avcodec_open2() and the decoder support is read from the D3D11/VAAPI
 * device that the QSV device is derived from.
 */

#define HB_QSV_MAX_ADAPTERS             16
#define HB_QSV_ASYNC_DEPTH_DEFAULT      4
#define HB_QSV_AVC_DECODER_WIDTH_MAX    4096
#define HB_QSV_AVC_DECODER_HEIGHT_MAX   4096

// codec bits used for the encoder and decoder capabilities
#define QSV_CODEC_H264      (1 << 0)
#define QSV_CODEC_H265      (1 << 1)
#define QSV_CODEC_H265_10   (1 << 2)
#define QSV_CODEC_AV1       (1 << 3)
#define QSV_CODEC_VVC       (1 << 4)

typedef struct
{
    int  index;         // DXGI adapter index or DRM render node offset
    int  integrated;    // 1 integrated, 0 discrete, -1 unknown
    char name[128];
    int  encoders;      // QSV_CODEC_* encoders that can be opened
    int  lowpower;      // QSV_CODEC_* encoders that can be opened with low_power=1
    int  icq;           // QSV_CODEC_* encoders that can be opened in ICQ mode
    int  decoders;      // QSV_CODEC_* decoders exposed by the driver
} hb_qsv_adapter_t;

static hb_qsv_adapter_t g_adapters[HB_QSV_MAX_ADAPTERS];
static int              g_adapters_count       = 0;
static int              g_default_adapter_index = 0;
static hb_list_t      * g_adapters_list         = NULL;
static int              qsv_init_done           = 0;
static int              qsv_init_result         = 0;

static const hb_qsv_adapter_t * get_adapter(int adapter_index)
{
    for (int i = 0; i < g_adapters_count; i++)
    {
        if (g_adapters[i].index == adapter_index)
        {
            return &g_adapters[i];
        }
    }
    return NULL;
}

static int codec_from_vcodec(int vcodec)
{
    switch (vcodec)
    {
        case HB_VCODEC_FFMPEG_QSV_H264:
            return QSV_CODEC_H264;
        case HB_VCODEC_FFMPEG_QSV_H265:
            return QSV_CODEC_H265;
        case HB_VCODEC_FFMPEG_QSV_H265_10BIT:
            return QSV_CODEC_H265_10;
        case HB_VCODEC_FFMPEG_QSV_AV1:
        case HB_VCODEC_FFMPEG_QSV_AV1_10BIT:
            return QSV_CODEC_AV1;
        default:
            return 0;
    }
}

static int probe_encoder(AVBufferRef *device, const char *codec_name,
                         enum AVPixelFormat pix_fmt, const char *profile,
                         int low_power, int icq)
{
    const AVCodec *codec = avcodec_find_encoder_by_name(codec_name);
    if (codec == NULL)
    {
        return 0;
    }

    AVCodecContext *context = avcodec_alloc_context3(codec);
    AVDictionary   *av_opts = NULL;
    if (context == NULL)
    {
        return 0;
    }

    context->width         = 640;
    context->height        = 480;
    context->pix_fmt       = pix_fmt;
    context->time_base     = (AVRational){1, 25};
    context->framerate     = (AVRational){25, 1};
    context->hw_device_ctx = av_buffer_ref(device);
    if (icq)
    {
        context->global_quality = 25;
    }
    else
    {
        context->bit_rate = 2000000;
    }
    if (low_power)
    {
        av_dict_set(&av_opts, "low_power", "1", 0);
    }
    if (profile)
    {
        av_dict_set(&av_opts, "profile", profile, 0);
    }

    int ret = hb_avcodec_open(context, codec, &av_opts, 1);
    av_dict_free(&av_opts);
    hb_avcodec_free_context(&context);

    return ret == 0;
}

static void probe_encoders(AVBufferRef *device, hb_qsv_adapter_t *adapter)
{
    static const struct
    {
        int                codec;
        const char        *name;
        enum AVPixelFormat pix_fmt;
        const char        *profile;
    } encoders[] =
    {
        { QSV_CODEC_H264,    "h264_qsv", AV_PIX_FMT_NV12,  NULL,     },
        { QSV_CODEC_H265,    "hevc_qsv", AV_PIX_FMT_NV12,  NULL,     },
        { QSV_CODEC_H265_10, "hevc_qsv", AV_PIX_FMT_P010,  "main10", },
        { QSV_CODEC_AV1,     "av1_qsv",  AV_PIX_FMT_NV12,  NULL,     },
    };

    // the probes fail on purpose, don't let FFmpeg log the errors
    int log_level = av_log_get_level();
    av_log_set_level(AV_LOG_QUIET);

    for (int i = 0; i < sizeof(encoders) / sizeof(encoders[0]); i++)
    {
        const int   codec   = encoders[i].codec;
        const char *name    = encoders[i].name;
        const char *profile = encoders[i].profile;

        if (probe_encoder(device, name, encoders[i].pix_fmt, profile, 1, 1))
        {
            adapter->encoders |= codec;
            adapter->lowpower |= codec;
            adapter->icq      |= codec;
        }
        else if (probe_encoder(device, name, encoders[i].pix_fmt, profile, 0, 1))
        {
            adapter->encoders |= codec;
            adapter->icq      |= codec;
        }
        else if (probe_encoder(device, name, encoders[i].pix_fmt, profile, 0, 0))
        {
            adapter->encoders |= codec;
        }
    }

    av_log_set_level(log_level);
}

#if defined(_WIN32) || defined(__MINGW32__)

static const GUID hb_IID_IDXGIDevice =
    { 0x54ec77fa, 0x1377, 0x44e6, { 0x8c, 0x32, 0x88, 0xfd, 0x5f, 0x44, 0xc8, 0x4c } };

static const struct
{
    int  codec;
    GUID guid;
} d3d11_decoders[] =
{
    { QSV_CODEC_H264,    { 0x1b81be68, 0xa0c7, 0x11d3, { 0xb9, 0x84, 0x00, 0xc0, 0x4f, 0x2e, 0x73, 0xc5 } } },
    { QSV_CODEC_H265,    { 0x5b11d51b, 0x2f4c, 0x4452, { 0xbc, 0xc3, 0x09, 0xf2, 0xa1, 0x16, 0x0c, 0xc0 } } },
    { QSV_CODEC_H265_10, { 0x107af0e0, 0xef1a, 0x4d19, { 0xab, 0xa8, 0x67, 0xa1, 0x63, 0x07, 0x3d, 0x13 } } },
    { QSV_CODEC_AV1,     { 0xb8be4ccb, 0xcf53, 0x46ba, { 0x8d, 0x59, 0xd6, 0xb8, 0xa6, 0xda, 0x5d, 0x2a } } },
};

/*
 * Returns 0 if the adapter is an Intel adapter, -1 if it is not
 * and -2 past the last adapter.
 */
static int open_adapter(int index, hb_qsv_adapter_t *adapter, AVBufferRef **child)
{
    static LUID luids[HB_QSV_MAX_ADAPTERS];
    char device[16];
    snprintf(device, sizeof(device), "%d", index);
    if (av_hwdevice_ctx_create(child, AV_HWDEVICE_TYPE_D3D11VA, device, NULL, 0) < 0)
    {
        return -1;
    }

    AVD3D11VADeviceContext *hwctx = ((AVHWDeviceContext *)(*child)->data)->hwctx;
    IDXGIDevice  *dxgi_device  = NULL;
    IDXGIAdapter *dxgi_adapter = NULL;
    DXGI_ADAPTER_DESC desc     = { 0 };

    if (SUCCEEDED(ID3D11Device_QueryInterface(hwctx->device, &hb_IID_IDXGIDevice, (void **)&dxgi_device)))
    {
        if (SUCCEEDED(IDXGIDevice_GetAdapter(dxgi_device, &dxgi_adapter)))
        {
            IDXGIAdapter_GetDesc(dxgi_adapter, &desc);
            IDXGIAdapter_Release(dxgi_adapter);
        }
        IDXGIDevice_Release(dxgi_device);
    }

    // FFmpeg falls back to the default adapter past the last one
    for (int i = 0; i < index; i++)
    {
        if (luids[i].LowPart  == desc.AdapterLuid.LowPart &&
            luids[i].HighPart == desc.AdapterLuid.HighPart)
        {
            av_buffer_unref(child);
            return -2;
        }
    }
    luids[index] = desc.AdapterLuid;

    if (desc.VendorId != 0x8086)
    {
        av_buffer_unref(child);
        return -1;
    }

    adapter->index = index;
    WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, adapter->name, sizeof(adapter->name), NULL, NULL);

    D3D11_FEATURE_DATA_D3D11_OPTIONS2 options2 = { 0 };
    adapter->integrated = SUCCEEDED(ID3D11Device_CheckFeatureSupport(hwctx->device, D3D11_FEATURE_D3D11_OPTIONS2,
                                                                     &options2, sizeof(options2))) ?
                          options2.UnifiedMemoryArchitecture != 0 : -1;

    UINT count = ID3D11VideoDevice_GetVideoDecoderProfileCount(hwctx->video_device);
    for (UINT i = 0; i < count; i++)
    {
        GUID guid;
        if (FAILED(ID3D11VideoDevice_GetVideoDecoderProfile(hwctx->video_device, i, &guid)))
        {
            continue;
        }
        for (int j = 0; j < sizeof(d3d11_decoders) / sizeof(d3d11_decoders[0]); j++)
        {
            if (IsEqualGUID(&guid, &d3d11_decoders[j].guid))
            {
                adapter->decoders |= d3d11_decoders[j].codec;
            }
        }
    }

    return 0;
}

#else // !_WIN32

static int va_has_decoder(VADisplay display, VAProfile *profiles, int count, VAProfile profile)
{
    for (int i = 0; i < count; i++)
    {
        if (profiles[i] != profile)
        {
            continue;
        }

        int entrypoints_count = vaMaxNumEntrypoints(display);
        VAEntrypoint *entrypoints = av_malloc_array(entrypoints_count, sizeof(VAEntrypoint));
        int found = 0;
        if (entrypoints != NULL &&
            vaQueryConfigEntrypoints(display, profile, entrypoints, &entrypoints_count) == VA_STATUS_SUCCESS)
        {
            for (int j = 0; j < entrypoints_count; j++)
            {
                found |= entrypoints[j] == VAEntrypointVLD;
            }
        }
        av_free(entrypoints);
        return found;
    }
    return 0;
}

/*
 * Returns 0 if the adapter is an Intel adapter, -1 otherwise.
 */
static int open_adapter(int index, hb_qsv_adapter_t *adapter, AVBufferRef **child)
{
    char device[32];
    snprintf(device, sizeof(device), "/dev/dri/renderD%d", 128 + index);
    if (access(device, R_OK | W_OK) != 0 ||
        av_hwdevice_ctx_create(child, AV_HWDEVICE_TYPE_VAAPI, device, NULL, 0) < 0)
    {
        return -1;
    }

    AVVAAPIDeviceContext *hwctx = ((AVHWDeviceContext *)(*child)->data)->hwctx;
    const char *vendor = vaQueryVendorString(hwctx->display);
    if (vendor == NULL || strstr(vendor, "Intel") == NULL)
    {
        av_buffer_unref(child);
        return -1;
    }

    adapter->index      = index;
    adapter->integrated = -1;
    snprintf(adapter->name, sizeof(adapter->name), "%s", vendor);

    int count = vaMaxNumProfiles(hwctx->display);
    VAProfile *profiles = av_malloc_array(count, sizeof(VAProfile));
    if (profiles != NULL &&
        vaQueryConfigProfiles(hwctx->display, profiles, &count) == VA_STATUS_SUCCESS)
    {
        if (va_has_decoder(hwctx->display, profiles, count, VAProfileH264High))
            adapter->decoders |= QSV_CODEC_H264;
        if (va_has_decoder(hwctx->display, profiles, count, VAProfileHEVCMain))
            adapter->decoders |= QSV_CODEC_H265;
        if (va_has_decoder(hwctx->display, profiles, count, VAProfileHEVCMain10))
            adapter->decoders |= QSV_CODEC_H265_10;
        if (va_has_decoder(hwctx->display, profiles, count, VAProfileAV1Profile0))
            adapter->decoders |= QSV_CODEC_AV1;
#if VA_CHECK_VERSION(1, 22, 0)
        if (va_has_decoder(hwctx->display, profiles, count, VAProfileVVCMain10))
            adapter->decoders |= QSV_CODEC_VVC;
#endif
    }
    av_free(profiles);

    return 0;
}

#endif // _WIN32

static int bit_count(int value)
{
    int count = 0;
    for (; value; value &= value - 1)
    {
        count++;
    }
    return count;
}

static void make_adapters_list(void)
{
    int best_score = -1;

    for (int i = 0; i < HB_QSV_MAX_ADAPTERS; i++)
    {
        hb_qsv_adapter_t adapter = { 0 };
        AVBufferRef *child = NULL, *device = NULL;

        int ret = open_adapter(i, &adapter, &child);
        if (ret == -2)
        {
            break;
        }
        else if (ret < 0)
        {
            continue;
        }
        if (av_hwdevice_ctx_create_derived(&device, AV_HWDEVICE_TYPE_QSV, child, 0) == 0)
        {
            probe_encoders(device, &adapter);
            av_buffer_unref(&device);
        }
        av_buffer_unref(&child);

        if (adapter.encoders == 0)
        {
            continue;
        }

        // prefer the adapter that can open the most encoders
        int score = bit_count(adapter.encoders);
        if (score > best_score)
        {
            best_score              = score;
            g_default_adapter_index = adapter.index;
        }

        int *index = malloc(sizeof(int));
        *index = adapter.index;
        hb_list_add(g_adapters_list, index);
        g_adapters[g_adapters_count++] = adapter;
    }
}

/**
 * Check the actual availability of QSV implementations on the system
 * and collect GPU adapters capabilities.
 *
 * @returns encoder codec mask supported by QSV implementation,
 *      0 if QSV is not available, -1 if HB_PROJECT_FEATURE_QSV is not enabled
 */
int hb_qsv_available(void)
{
    if (hb_is_hardware_disabled())
    {
        return 0;
    }

    if (qsv_init_done != 0)
    {
        // This method gets called a lot. Don't probe hardware each time.
        return qsv_init_result;
    }

    qsv_init_done   = 1;
    g_adapters_list = hb_list_init();
    make_adapters_list();

    if (g_adapters_count == 0)
    {
        hb_log("qsv: not available on this system");
        qsv_init_result = 0;
        return qsv_init_result;
    }
    hb_log("qsv: is available on this system");

    static const int encoders[] =
    {
        HB_VCODEC_FFMPEG_QSV_H264,
        HB_VCODEC_FFMPEG_QSV_H265,
        HB_VCODEC_FFMPEG_QSV_H265_10BIT,
        HB_VCODEC_FFMPEG_QSV_AV1,
        HB_VCODEC_FFMPEG_QSV_AV1_10BIT,
    };
    for (int i = 0; i < sizeof(encoders) / sizeof(encoders[0]); i++)
    {
        if (hb_qsv_video_encoder_is_available(encoders[i]))
        {
            qsv_init_result |= encoders[i];
        }
    }
    return qsv_init_result;
}

hb_list_t * hb_qsv_adapters_list(void)
{
    return g_adapters_list;
}

int hb_qsv_get_adapter_index(void)
{
    return g_default_adapter_index;
}

int hb_qsv_video_encoder_is_available(int encoder)
{
    for (int i = 0; i < g_adapters_count; i++)
    {
        if (g_adapters[i].encoders & codec_from_vcodec(encoder))
        {
            return 1;
        }
    }
    return 0;
}

static void log_encoder(const hb_qsv_adapter_t *adapter, const char *name, int codec, int codec_10bit)
{
    if ((adapter->encoders & codec) == 0)
    {
        hb_log(" - %s encoder: no", name);
        return;
    }
    hb_log(" - %s encoder: yes%s%s%s", name,
           codec_10bit == 0                ? "" :
           adapter->encoders & codec_10bit ? " (8bit: yes, 10bit: yes)" : " (8bit: yes, 10bit: no)",
           adapter->lowpower & codec ? " lowpower" : "",
           adapter->icq      & codec ? " icq"      : "");
}

void hb_qsv_info_print(void)
{
    if (hb_qsv_available() <= 0)
    {
        hb_log("Intel Quick Sync Video support: no");
        return;
    }

    char gpu_list[128] = "";
    for (int i = 0; i < g_adapters_count; i++)
    {
        char index[16];
        snprintf(index, sizeof(index), "%s%d", i ? ", " : "", g_adapters[i].index);
        av_strlcat(gpu_list, index, sizeof(gpu_list));
    }
    hb_log("Intel Quick Sync Video support: yes, gpu list: %s", gpu_list);

    for (int i = 0; i < g_adapters_count; i++)
    {
        const hb_qsv_adapter_t *adapter = &g_adapters[i];
        hb_log("Intel Quick Sync Video %s adapter with index %d: %s",
               adapter->integrated == 1 ? "integrated" :
               adapter->integrated == 0 ? "discrete"   : "unknown",
               adapter->index, adapter->name);
        hb_log(" - Decode support:%s%s%s%s",
               adapter->decoders & QSV_CODEC_H264    ? " h264" : "",
               adapter->decoders & QSV_CODEC_H265_10 ? " hevc (8bit: yes, 10bit: yes)" :
               adapter->decoders & QSV_CODEC_H265    ? " hevc (8bit: yes, 10bit: no)"  : "",
               adapter->decoders & QSV_CODEC_AV1     ? " av1"  : "",
               adapter->decoders & QSV_CODEC_VVC     ? " vvc"  : "");
        log_encoder(adapter, "H.264", QSV_CODEC_H264, 0);
        log_encoder(adapter, "H.265", QSV_CODEC_H265, QSV_CODEC_H265_10);
        log_encoder(adapter, "AV1",   QSV_CODEC_AV1,  0);
    }
}

int hb_qsv_decode_is_codec_supported(int adapter_index, int codec_id, int pix_fmt, int width, int height)
{
    const hb_qsv_adapter_t *adapter = get_adapter(adapter_index);
    if (adapter == NULL)
    {
        return 0;
    }

    int is_8bit  = pix_fmt == AV_PIX_FMT_NV12    ||
                   pix_fmt == AV_PIX_FMT_YUV420P ||
                   pix_fmt == AV_PIX_FMT_YUVJ420P;
    int is_10bit = pix_fmt == AV_PIX_FMT_P010LE  ||
                   pix_fmt == AV_PIX_FMT_YUV420P10;

    switch (codec_id)
    {
        case AV_CODEC_ID_H264:
            // QSV decode for AVC does not support higher video resolutions
            if (width > HB_QSV_AVC_DECODER_WIDTH_MAX || height > HB_QSV_AVC_DECODER_HEIGHT_MAX)
            {
                return 0;
            }
            return is_8bit && (adapter->decoders & QSV_CODEC_H264);
        case AV_CODEC_ID_HEVC:
            return (is_8bit  && (adapter->decoders & QSV_CODEC_H265)) ||
                   (is_10bit && (adapter->decoders & QSV_CODEC_H265_10));
        case AV_CODEC_ID_AV1:
            return (is_8bit || is_10bit) && (adapter->decoders & QSV_CODEC_AV1);
        case AV_CODEC_ID_VVC:
            return (is_8bit || is_10bit) && (adapter->decoders & QSV_CODEC_VVC);
        default:
            return 0;
    }
}

static int has_icq(uint32_t codec)
{
    const hb_qsv_adapter_t *adapter = get_adapter(g_default_adapter_index);
    return adapter != NULL && (adapter->icq & codec_from_vcodec(codec));
}

const char * hb_qsv_video_quality_get_name(uint32_t codec)
{
    return has_icq(codec) ? "ICQ" : "QP";
}

void hb_qsv_video_quality_get_limits(uint32_t codec, float *low, float *high,
                                     float *granularity, int *direction)
{
    *direction   = 1;
    *granularity = 1.;
    *low         = has_icq(codec) ? 1. : 0.;
    *high        = 51.;
}

/*
 * The options that configure the pipeline rather than the encoder,
 * they are consumed here before any filter or decoder is initialized.
 */
static void parse_pipeline_options(hb_job_t *job)
{
    if (job->encoder_options == NULL || *job->encoder_options == '\0')
    {
        return;
    }

    hb_dict_t *options = hb_encopts_to_dict(job->encoder_options, job->vcodec);
    hb_dict_iter_t iter;
    for (iter  = hb_dict_iter_init(options);
         iter != HB_DICT_ITER_DONE;
         iter  = hb_dict_iter_next(options, iter))
    {
        const char *key = hb_dict_iter_key(iter);
        char *value = hb_value_get_string_xform(hb_dict_iter_value(iter));

        if (!strcasecmp(key, "gpu"))
        {
            int index = atoi(value);
            if (get_adapter(index) != NULL)
            {
                job->hw_device_index = index;
            }
            else
            {
                hb_log("qsv: adapter with index %d not found", index);
            }
        }
        else if (!strcasecmp(key, "async-depth"))
        {
            job->hw_device_async_depth = atoi(value);
        }
        else if (!strcasecmp(key, "scalingmode") || !strcasecmp(key, "vpp-sm"))
        {
            snprintf(job->qsv_ctx->vpp_scale_mode, sizeof(job->qsv_ctx->vpp_scale_mode), "%s", value);
        }
        free(value);
    }
    hb_dict_free(&options);
}

int hb_qsv_setup_job(hb_job_t *job)
{
    if (job->qsv_ctx == NULL)
    {
        return 1;
    }

    if (get_adapter(job->hw_device_index) == NULL)
    {
        job->hw_device_index = g_default_adapter_index;
    }
    parse_pipeline_options(job);
    if (job->hw_device_async_depth <= 0)
    {
        job->hw_device_async_depth = HB_QSV_ASYNC_DEPTH_DEFAULT;
    }
    hb_log("qsv: adapter with index %d has been selected", job->hw_device_index);

    // Make sure QSV Decode is only True if the selected QSV adapter supports decode
    if ((job->hw_decode & HB_DECODE_QSV) &&
        !hb_qsv_decode_is_codec_supported(job->hw_device_index,
                                          job->title->video_codec_param, job->input_pix_fmt,
                                          job->title->geometry.width, job->title->geometry.height))
    {
        job->hw_decode &= ~HB_DECODE_QSV;
    }

    return 0;
}

/*
 * HandBrake QSV option names that differ from the FFmpeg option names.
 * Anything not listed here, nor handled in hb_qsv_apply_encoder_options(),
 * is handed over to FFmpeg as is.
 */
static const char * const option_names[][2] =
{
    { "target-usage",    "preset"                  },
    { "tu",              "preset"                  },
    { "num-ref-frame",   "refs"                    },
    { "ref",             "refs"                    },
    { "gop-pic-size",    "g"                       },
    { "keyint",          "g"                       },
    { "b-pyramid",       "b_strategy"              },
    { "adaptive-i",      "adaptive_i"              },
    { "i-adapt",         "adaptive_i"              },
    { "adaptive-b",      "adaptive_b"              },
    { "b-adapt",         "adaptive_b"              },
    { "colorprim",       "color_primaries"         },
    { "transfer",        "color_trc"               },
    { "colormatrix",     "colorspace"              },
    { "lookahead-depth", "look_ahead_depth"        },
    { "la-depth",        "look_ahead_depth"        },
    { "lookahead-ds",    "look_ahead_downsampling" },
    { "la-ds",           "look_ahead_downsampling" },
    { "repeatpps",       "repeat_pps"              },
    { "palette",         "palette_mode"            },
    { NULL,              NULL                      },
};

static const char * option_name(const char *key)
{
    for (int i = 0; option_names[i][0] != NULL; i++)
    {
        if (!strcasecmp(key, option_names[i][0]))
        {
            return option_names[i][1];
        }
    }
    return key;
}

static int option_bool(const char *value)
{
    return !strcasecmp(value, "true") || !strcasecmp(value, "yes") || atoi(value) != 0;
}

int hb_qsv_apply_encoder_options(hb_job_t *job, AVCodecContext *context, AVDictionary **av_opts)
{
    const hb_qsv_adapter_t *adapter = get_adapter(job->hw_device_index);
    const int codec = codec_from_vcodec(job->vcodec);

    int   icq            = adapter != NULL && (adapter->icq & codec);
    int   lookahead      = 0;
    int   cqp_offsets[3] = { 0, 2, 4 };
    int   vbv_maxrate    = 0;
    int   vbv_bufsize    = 0;
    float vbv_init       = 0;

#if defined(_WIN32) || defined(__MINGW32__)
    if (adapter != NULL && (adapter->lowpower & codec))
    {
        av_dict_set(av_opts, "low_power", "1", 0);
    }
#endif

    // a 2 seconds GOP, keyint overrides it
    context->gop_size = FFMIN((int)((double)job->orig_vrate.num / job->orig_vrate.den * 2 + 0.5), 120);

    if (job->encoder_profile != NULL && *job->encoder_profile &&
        strcasecmp(job->encoder_profile, "auto"))
    {
        av_dict_set(av_opts, "profile",
                    !strcasecmp(job->encoder_profile, "mainstillpicture") ? "mainsp" : job->encoder_profile, 0);
    }
    else if (job->vcodec == HB_VCODEC_FFMPEG_QSV_H265_10BIT)
    {
        av_dict_set(av_opts, "profile", "main10", 0);
    }

    if (job->hw_device_async_depth > 0)
    {
        av_dict_set_int(av_opts, "async_depth", job->hw_device_async_depth, 0);
    }

    hb_dict_t *options = NULL;
    if (job->encoder_options != NULL && *job->encoder_options)
    {
        options = hb_encopts_to_dict(job->encoder_options, job->vcodec);
    }

    hb_dict_iter_t iter;
    for (iter  = hb_dict_iter_init(options);
         iter != HB_DICT_ITER_DONE;
         iter  = hb_dict_iter_next(options, iter))
    {
        const char *key = hb_dict_iter_key(iter);
        char *str = hb_value_get_string_xform(hb_dict_iter_value(iter));
        const char *value = str;

        if (value == NULL || *value == '\0')
        {
            value = "1";
        }
        else if (!strcasecmp(value, "true"))
        {
            value = "1";
        }
        else if (!strcasecmp(value, "false"))
        {
            value = "0";
        }
        if (!strncasecmp(key, "no-", 3))
        {
            key  += 3;
            value = option_bool(value) ? "0" : "1";
        }

        if (!strcasecmp(key, "gpu")         || !strcasecmp(key, "async-depth") ||
            !strcasecmp(key, "scalingmode") || !strcasecmp(key, "vpp-sm"))
        {
            // already consumed by hb_qsv_setup_job()
        }
        else if (!strcasecmp(key, "force-cqp"))
        {
            icq = icq && !option_bool(value);
        }
        else if (!strcasecmp(key, "cqp-offset-i"))
        {
            cqp_offsets[0] = atoi(value);
        }
        else if (!strcasecmp(key, "cqp-offset-p"))
        {
            cqp_offsets[1] = atoi(value);
        }
        else if (!strcasecmp(key, "cqp-offset-b"))
        {
            cqp_offsets[2] = atoi(value);
        }
        else if (!strcasecmp(key, "vbv-maxrate"))
        {
            vbv_maxrate = atoi(value);
        }
        else if (!strcasecmp(key, "vbv-bufsize"))
        {
            vbv_bufsize = atoi(value);
        }
        else if (!strcasecmp(key, "vbv-init"))
        {
            vbv_init = atof(value);
        }
        else if (!strcasecmp(key, "lookahead") || !strcasecmp(key, "la"))
        {
            lookahead = option_bool(value);
        }
        else if (!strcasecmp(key, "lowpower"))
        {
            if (option_bool(value) && (adapter == NULL || (adapter->lowpower & codec) == 0))
            {
                hb_log("encavcodec: lowpower is not supported by this adapter");
            }
            else
            {
                av_dict_set(av_opts, "low_power", option_bool(value) ? "1" : "0", 0);
            }
        }
        else if (!strcasecmp(key, "gop-ref-dist"))
        {
            av_dict_set_int(av_opts, "bf", FFMAX(atoi(value), 1) - 1, 0);
        }
        else if (!strcasecmp(key, "scenecut"))
        {
            // no adaptive I/B frames placement gives a strict GOP
            if (!option_bool(value))
            {
                av_dict_set(av_opts, "adaptive_i", "0", 0);
                av_dict_set(av_opts, "adaptive_b", "0", 0);
            }
        }
        else if (!strcasecmp(key, "cabac"))
        {
            av_dict_set(av_opts, "cavlc", option_bool(value) ? "0" : "1", 0);
        }
        else
        {
            av_dict_set(av_opts, option_name(key), value, 0);
        }
        free(str);
    }
    hb_dict_free(&options);

    if (job->vquality > HB_INVALID_VIDEO_QUALITY)
    {
        context->bit_rate = 0;
        if (icq)
        {
            context->flags         &= ~AV_CODEC_FLAG_QSCALE;
            context->global_quality = av_clip((int)job->vquality, 1, 51);
            hb_log("encavcodec: encoding with ICQ %d%s", context->global_quality,
                   lookahead ? " and lookahead" : "");
        }
        else
        {
            // the P frames QP is the base, I and B frames are offset from it
            context->flags         |= AV_CODEC_FLAG_QSCALE;
            context->global_quality = FFMAX((int)job->vquality + cqp_offsets[1], 0) * FF_QP2LAMBDA;
            context->i_quant_factor = 1.0;
            context->i_quant_offset = cqp_offsets[0] - cqp_offsets[1];
            context->b_quant_factor = 1.0;
            context->b_quant_offset = cqp_offsets[2] - cqp_offsets[1];
            lookahead = 0;
            hb_log("encavcodec: encoding with CQP %d (I: %+d, B: %+d)", (int)job->vquality + cqp_offsets[1],
                   cqp_offsets[0] - cqp_offsets[1], cqp_offsets[2] - cqp_offsets[1]);
        }
    }
    else if (!lookahead)
    {
        // VBR, or CBR when the maximum rate is the average rate
        context->rc_max_rate = 1000LL * vbv_maxrate;
        if (vbv_bufsize > 0)
        {
            context->rc_buffer_size = 1000 * vbv_bufsize;
            context->rc_initial_buffer_occupancy = vbv_init > 1.0 ? 1000 * vbv_init :
                                                   1000 * vbv_bufsize * vbv_init;
        }
    }
    if (lookahead)
    {
        av_dict_set(av_opts, "look_ahead", "1", 0);
    }
    if (job->qsv_ctx != NULL)
    {
        job->qsv_ctx->la_is_enabled = lookahead;
    }

    return 0;
}

hb_qsv_context_t * hb_qsv_context_init(void)
{
    if (hb_qsv_available() <= 0)
    {
        return NULL;
    }
    return av_mallocz(sizeof(hb_qsv_context_t));
}

hb_qsv_context_t * hb_qsv_context_dup(const hb_qsv_context_t *src)
{
    if (src == NULL)
    {
        return NULL;
    }

    hb_qsv_context_t *ctx = av_malloc(sizeof(hb_qsv_context_t));
    if (ctx)
    {
        memcpy(ctx, src, sizeof(hb_qsv_context_t));
    }
    return ctx;
}

void hb_qsv_context_close(hb_qsv_context_t **_ctx)
{
    av_freep(_ctx);
}

static int are_filters_supported(hb_list_t *filters)
{
#if defined(_WIN32) || defined(__MINGW32__)
    int num_sw_filters = 0;
    for (int i = 0; i < hb_list_count(filters); i++)
    {
        hb_filter_object_t *filter = hb_list_item(filters, i);
        switch (filter->id)
        {
            case HB_FILTER_ADAPTER:
            // pixel format conversion is done via VPP filter
            case HB_FILTER_FORMAT:
            // cropping and scaling always done via VPP filter
            case HB_FILTER_CROP_SCALE:
            case HB_FILTER_ROTATE:
            case HB_FILTER_AVFILTER:
                break;
            case HB_FILTER_VFR:
            {
                // Frame drop mode 0 doesn't require access to the frame data
                int mode = hb_dict_get_int(filter->settings, "frame-drop-mode");
                if (mode == 0)
                {
                    break;
                }
            }
            default:
                // count only filters with access to frame data
                num_sw_filters++;
                break;
        }
    }
    return num_sw_filters == 0;
#else // other OS
    return 0;
#endif
}

static void * find_decoder(int codec_param)
{
    switch (codec_param)
    {
        case AV_CODEC_ID_H264:
            return (void *)avcodec_find_decoder_by_name("h264_qsv");
        case AV_CODEC_ID_HEVC:
            return (void *)avcodec_find_decoder_by_name("hevc_qsv");
        case AV_CODEC_ID_MPEG2VIDEO:
            return (void *)avcodec_find_decoder_by_name("mpeg2_qsv");
        case AV_CODEC_ID_AV1:
            return (void *)avcodec_find_decoder_by_name("av1_qsv");
        case AV_CODEC_ID_VVC:
            return (void *)avcodec_find_decoder_by_name("vvc_qsv");
        default:
            return NULL;
    }
}

static const int qsv_encoders[] =
{
    HB_VCODEC_FFMPEG_QSV_H264,
    HB_VCODEC_FFMPEG_QSV_H265,
    HB_VCODEC_FFMPEG_QSV_H265_10BIT,
    HB_VCODEC_FFMPEG_QSV_AV1,
    HB_VCODEC_FFMPEG_QSV_AV1_10BIT,
    HB_VCODEC_INVALID
};

hb_hwaccel_t hb_hwaccel_qsv =
{
    .id           = HB_DECODE_QSV,
    .name         = "qsv",
    .encoders     = qsv_encoders,
    .type         = AV_HWDEVICE_TYPE_QSV,
    .hw_pix_fmt   = AV_PIX_FMT_QSV,
    .can_filter   = are_filters_supported,
    .find_decoder = find_decoder,
    .caps         = HB_HWACCEL_CAP_ROTATE | HB_HWACCEL_CAP_COLOR_RANGE | HB_HWACCEL_CAP_SYSTEM_MEMORY
};

#else // HB_PROJECT_FEATURE_QSV

int hb_qsv_available(void)
{
    return -1;
}

#endif // HB_PROJECT_FEATURE_QSV
