// VideoOverlay.cpp
// Theora/ogg video decoder loaded as a runtime overlay.
// This file must NOT link against the RetroEngine binary — all engine services
// come through the ResidentAPI table passed to Video_Load at dlopen time.

#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include <theora/theoradec.h>
#include <ogg/ogg.h>

#include "../../RSDK/Core/Overlay.hpp"
#include "VideoOverlay.hpp"

// -------------------------------------------------------
// ResidentAPI handle (set once in Video_Load)
// -------------------------------------------------------
static const ResidentAPI *s_api = NULL;

// -------------------------------------------------------
// Decoder state — mirrors VideoManager static members
// -------------------------------------------------------
static void            *s_fh           = NULL;  // FILE* via s_api->OpenFile
static ogg_sync_state   s_oy;
static ogg_page         s_og;
static ogg_packet       s_op;
static ogg_stream_state s_to;
static th_info          s_ti;
static th_comment       s_tc;
static th_dec_ctx      *s_td           = NULL;
static th_setup_info   *s_ts           = NULL;
static th_pixel_fmt     s_pixelFormat;
static ogg_int64_t      s_granulePos   = 0;
static int32_t          s_initializing = 0;
static int32_t          s_playing      = 0;

// Playback timing — mirrors engine.displayTime / engine.videoStartDelay
static double           s_displayTime  = 0.0;
static double           s_startDelay   = 0.0;

// Engine callback to fire on EOS or forced stop
static void           (*s_onFinished)(void) = NULL;

// -------------------------------------------------------
// VideoOverlay_Close — idempotent teardown of decode state
// Mirrors the "finished" block at the end of ProcessVideo.
// Does NOT touch engine state — that is handled by s_onFinished().
// -------------------------------------------------------
static void VideoOverlay_Close(void)
{
    if (!s_playing)
        return;

    if (s_fh) {
        s_api->CloseFile(s_fh);
        s_fh = NULL;
    }

    // Drain remaining pages so libogg internal state is consistent
    while (ogg_sync_pageout(&s_oy, &s_og) > 0)
        ogg_stream_pagein(&s_to, &s_og);

    ogg_stream_clear(&s_to);
    th_decode_free(s_td);  s_td = NULL;
    th_comment_clear(&s_tc);
    th_info_clear(&s_ti);
    ogg_sync_clear(&s_oy);

    s_playing     = 0;
    s_initializing = 0;
}

// -------------------------------------------------------
// PlayVideo_Impl — mirrors LoadVideo's header-parse block.
// Engine-side state management (storedShaderID, sceneInfo.state,
// videoSettings, etc.) is intentionally left to Video.cpp's LoadVideo.
// -------------------------------------------------------
static int32_t PlayVideo_Impl(const char *path, double startDelay, void (*onFinished)(void))
{

    if (s_playing)
        VideoOverlay_Close();

    s_fh = s_api->OpenFile(path, "rb");
    if (!s_fh)
        return 0;

    s_onFinished = onFinished;
    s_startDelay = startDelay;
    s_displayTime = 0.0;

    ogg_sync_init(&s_oy);
    th_comment_init(&s_tc);
    th_info_init(&s_ti);

    int32_t theora_p = 0;
    char   *buffer   = NULL;

    // BOS page scan — find the theora stream
    int32_t finishedHeader = 0;
    while (!finishedHeader) {
        buffer = ogg_sync_buffer(&s_oy, 0x1000);
        int32_t ret = s_api->ReadFile(s_fh, buffer, 0x1000);
        ogg_sync_wrote(&s_oy, 0x1000);

        if (ret == 0)
            break;

        while (ogg_sync_pageout(&s_oy, &s_og) > 0) {
            ogg_stream_state test;

            if (!ogg_page_bos(&s_og)) {
                ogg_stream_pagein(&s_to, &s_og);
                finishedHeader = 1;
                break;
            }

            ogg_stream_init(&test, ogg_page_serialno(&s_og));
            ogg_stream_pagein(&test, &s_og);
            ogg_stream_packetout(&test, &s_op);

            if (!theora_p && th_decode_headerin(&s_ti, &s_tc, &s_ts, &s_op) >= 0) {
                memcpy(&s_to, &test, sizeof(test));
                theora_p = 1;
            }
            else {
                ogg_stream_clear(&test);
            }
        }
    }

    if (!theora_p)
        goto fail;

    // Parse remaining theora header packets (there are 3 total)
    s_ts     = NULL;
    theora_p = 1;
    while (theora_p && theora_p < 3) {
        int32_t ret;

        while (theora_p && theora_p < 3 && (ret = ogg_stream_packetout(&s_to, &s_op))) {
            if (ret < 0) {
#if !RETRO_USE_ORIGINAL_CODE
                fprintf(stderr, "[VideoOverlay] ERROR: failed to parse theora stream headers. corrupted stream?\n");
#endif
                theora_p = 0;
                break;
            }
            if (!th_decode_headerin(&s_ti, &s_tc, &s_ts, &s_op)) {
#if !RETRO_USE_ORIGINAL_CODE
                fprintf(stderr, "[VideoOverlay] ERROR: failed to parse theora stream headers. corrupted stream?\n");
#endif
                theora_p = 0;
                break;
            }
            theora_p++;
        }

        if (!theora_p)
            break;

        if (ogg_sync_pageout(&s_oy, &s_og) > 0) {
            ogg_stream_pagein(&s_to, &s_og);
        }
        else {
            buffer = ogg_sync_buffer(&s_oy, 0x1000);
            int32_t ret = s_api->ReadFile(s_fh, buffer, 0x1000);
            ogg_sync_wrote(&s_oy, 0x1000);
            if (ret == 0) {
#if !RETRO_USE_ORIGINAL_CODE
                fprintf(stderr, "[VideoOverlay] ERROR: Reached end of file while searching for codec headers.\n");
#endif
                theora_p = 0;
            }
        }
    }

    if (!theora_p)
        goto fail_headers;

    s_td = th_decode_alloc(&s_ti, s_ts);
    s_pixelFormat = s_ti.pixel_fmt;

    {
        int32_t ppLevelMax = 0;
        th_decode_ctl(s_td, TH_DECCTL_GET_PPLEVEL_MAX, &ppLevelMax, sizeof(int32_t));
        int32_t ppLevel = 0;
        th_decode_ctl(s_td, TH_DECCTL_SET_PPLEVEL, &ppLevel, sizeof(int32_t));
    }

    th_setup_free(s_ts);
    s_ts = NULL;

    s_granulePos  = 0;
    s_initializing = 1;
    s_playing      = 1;
    return 1;

fail_headers:
    th_info_clear(&s_ti);
    th_comment_clear(&s_tc);
    th_setup_free(s_ts);
    s_ts = NULL;
fail:
    ogg_sync_clear(&s_oy);
    s_api->CloseFile(s_fh);
    s_fh = NULL;
    return 0;
}

// -------------------------------------------------------
// UpdateFrame_Impl — mirrors ProcessVideo's decode loop.
// Skip-callback logic is intentionally absent: the engine
// checks the skip callback in RetroEngine.cpp before calling
// UpdateFrame, so this function only decodes and presents.
// -------------------------------------------------------
static void UpdateFrame_Impl(void)
{
    int32_t finished = 0;

    if (!s_initializing) {
        double streamPos = s_api->GetAudioClock();

        if (streamPos <= -1.0)
            s_displayTime += (1.0 / 60.0);
        else
            s_displayTime = streamPos;
    }

    double curTime = th_granule_time(s_td, s_granulePos);

    if (s_initializing || s_displayTime >= s_startDelay + curTime) {
        // Pump the bitstream until we get a decodeable packet
        while (ogg_stream_packetout(&s_to, &s_op) <= 0) {
            char *buffer = ogg_sync_buffer(&s_oy, 0x1000);
            if (!s_api->ReadFile(s_fh, buffer, 0x1000) && !s_initializing) {
                finished = 1;
                break;
            }
            ogg_sync_wrote(&s_oy, 0x1000);
            while (ogg_sync_pageout(&s_oy, &s_og) > 0)
                ogg_stream_pagein(&s_to, &s_og);
        }

        if (!finished && !th_decode_packetin(s_td, &s_op, &s_granulePos)) {
            th_ycbcr_buffer yuv;
            th_decode_ycbcr_out(s_td, yuv);

            int32_t dataPos = (s_ti.pic_x & ~1) + (s_ti.pic_y & ~1) * yuv[0].stride;
            switch (s_pixelFormat) {
                default: break;

                case TH_PF_444:
                    s_api->UploadYUVFrame(TH_PF_444,
                        yuv[0].width, yuv[0].height,
                        yuv[0].data + dataPos,
                        yuv[1].data + dataPos,
                        yuv[2].data + dataPos,
                        yuv[0].stride, yuv[1].stride, yuv[2].stride);
                    break;

                case TH_PF_422:
                    s_api->UploadYUVFrame(TH_PF_422,
                        yuv[0].width, yuv[0].height,
                        yuv[0].data + dataPos,
                        yuv[1].data + yuv[1].stride * s_ti.pic_y + (s_ti.pic_x >> 1),
                        yuv[2].data + yuv[1].stride * s_ti.pic_y + (s_ti.pic_x >> 1),
                        yuv[0].stride, yuv[1].stride, yuv[2].stride);
                    break;

                case TH_PF_420:
                    s_api->UploadYUVFrame(TH_PF_420,
                        yuv[0].width, yuv[0].height,
                        yuv[0].data + dataPos,
                        yuv[1].data + yuv[1].stride * (s_ti.pic_y >> 1) + (s_ti.pic_x >> 1),
                        yuv[2].data + yuv[1].stride * (s_ti.pic_y >> 1) + (s_ti.pic_x >> 1),
                        yuv[0].stride, yuv[1].stride, yuv[2].stride);
                    break;
            }
        }

        s_initializing = 0;
    }

    if (finished) {
        void (*cb)(void) = s_onFinished;
        s_onFinished = NULL;
        VideoOverlay_Close();
        if (cb)
            cb();
    }
}

static void StopVideo_Impl(void)
{
    if (!s_playing)
        return;
    void (*cb)(void) = s_onFinished;
    s_onFinished = NULL;
    VideoOverlay_Close();
    if (cb)
        cb();
}

static int32_t IsPlaying_Impl(void)  { return s_playing; }
static int     GetPixelFmt_Impl(void) { return (int)s_pixelFormat; }

// -------------------------------------------------------
// Overlay lifecycle hooks — called by Overlay_Load / Overlay_Unload
// -------------------------------------------------------
static bool Video_Load(const ResidentAPI *api)
{
    s_api     = api;
    s_playing = 0;
    s_td      = NULL;
    s_ts      = NULL;
    s_fh      = NULL;
    return true;
}

static void Video_Unload(void)
{
    VideoOverlay_Close();
    s_api = NULL;
}

// -------------------------------------------------------
// Exported symbols.
// extern "C": suppresses C++ name mangling so GetProcAddress/dlsym
//             can find them by their plain string names.
// OVERLAY_EXPORT: puts them in the DLL export table (required on Windows;
//                 sets default visibility on ELF so dlsym can find them
//                 even when the module was built with -fvisibility=hidden).
// -------------------------------------------------------
#if defined(_WIN32)
#  define OVERLAY_EXPORT __declspec(dllexport)
#else
#  define OVERLAY_EXPORT __attribute__((visibility("default")))
#endif

extern "C" OVERLAY_EXPORT VideoOverlayAPI g_videoAPI = {
    .PlayVideo   = PlayVideo_Impl,
    .UpdateFrame = UpdateFrame_Impl,
    .StopVideo   = StopVideo_Impl,
    .IsPlaying   = IsPlaying_Impl,
    .GetPixelFmt = GetPixelFmt_Impl,
};

extern "C" OVERLAY_EXPORT OverlayHeader OVERLAY_ENTRY = {
    .magic   = OVERLAY_MAGIC,
    .version = OVERLAY_VERSION,
    .maxSize = 512 * 1024,
    .Load    = Video_Load,
    .Unload  = Video_Unload,
};
