#pragma once
#include <stdint.h>

// Self-contained ABI header — no RetroEngine.hpp dependency.
// Include this anywhere you need to call into the video overlay.

typedef struct VideoOverlayAPI {
    // Open a video file, parse theora headers, allocate decoder.
    // startDelay: audio-sync offset in seconds (0 when audio unavailable).
    // onFinished: engine callback fired on natural EOS or forced stop.
    // Returns non-zero on success; pixel format is queryable via GetPixelFmt().
    int32_t (*PlayVideo)(const char *path, double startDelay, void (*onFinished)(void));

    // Decode and upload one frame. Engine calls this each ENGINESTATE_VIDEOPLAYBACK tick.
    // The skip-callback check happens engine-side before this is called.
    void (*UpdateFrame)(void);

    // Force-stop; fires onFinished before returning.
    void (*StopVideo)(void);

    int32_t (*IsPlaying)(void);

    // th_pixel_fmt of the current stream: TH_PF_420=0, TH_PF_422=1, TH_PF_444=2.
    // Valid after a successful PlayVideo call, until StopVideo or natural EOS.
    int (*GetPixelFmt)(void);
} VideoOverlayAPI;

// g_videoAPI is located at runtime via Overlay_GetSymbol — not linked statically.
