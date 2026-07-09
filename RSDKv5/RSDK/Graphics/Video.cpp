#include "RSDK/Core/RetroEngine.hpp"
#include "../../Overlays/Video/VideoOverlay.hpp"

using namespace RSDK;

// Restores engine state when video playback ends (passed as onFinished to the overlay).
static void VideoFinishedCallback(void)
{
    Overlay_Unload(&g_videoOverlaySlot);
    videoSettings.shaderID    = engine.storedShaderID;
    videoSettings.screenCount = 1;

    if (ENGINE_VERSION == 5)
        sceneInfo.state = engine.storedState;
#if RETRO_REV0U
    else if (ENGINE_VERSION == 3)
        RSDK::Legacy::gameMode = engine.storedState;
#endif
}

FileInfo VideoManager::file;

ogg_sync_state VideoManager::oy;
ogg_page VideoManager::og;
ogg_packet VideoManager::op;
ogg_stream_state VideoManager::vo;
ogg_stream_state VideoManager::to;
th_info VideoManager::ti;
th_comment VideoManager::tc;
th_dec_ctx *VideoManager::td    = NULL;
th_setup_info *VideoManager::ts = NULL;

th_pixel_fmt VideoManager::pixelFormat;
ogg_int64_t VideoManager::granulePos = 0;
bool32 VideoManager::initializing    = false;

bool32 RSDK::LoadVideo(const char *filename, double startDelay, bool32 (*skipCallback)())
{
    if (ENGINE_VERSION == 5 && sceneInfo.state == ENGINESTATE_VIDEOPLAYBACK)
        return false;
#if RETRO_REV0U
    if (ENGINE_VERSION == 3 && RSDK::Legacy::gameMode == RSDK::Legacy::v3::ENGINE_VIDEOWAIT)
        return false;
#endif

    char fullFilePath[0x80];
    sprintf_s(fullFilePath, sizeof(fullFilePath), "Data/Video/%s", filename);

        // Load video overlay once per engine init. Re-entrant: Overlay_Load unloads first if already live.
#if RETRO_PLATFORM == RETRO_WIN
    static const char videoOverlayPath[] = "VideoOverlay.dll";
#else
    static const char videoOverlayPath[] = "VideoOverlay.so";
#endif
    if (Overlay_Load(&g_videoOverlaySlot, videoOverlayPath))
        g_videoOverlayAPI = (VideoOverlayAPI *)Overlay_GetSymbol(&g_videoOverlaySlot, "g_videoAPI");
    else
        g_videoOverlayAPI = nullptr;


    if (g_videoOverlayAPI) {

        PrintLog(PRINT_ERROR, "[Overlay] Trying to use Overlay");
        engine.storedShaderID = videoSettings.shaderID;
        if (ENGINE_VERSION == 5)
            engine.storedState = sceneInfo.state;
#if RETRO_REV0U
        else if (ENGINE_VERSION == 3)
            engine.storedState = RSDK::Legacy::gameMode;
#endif

        double effectiveDelay = (AudioDevice::audioState == 1) ? startDelay : 0.0;
        if (!g_videoOverlayAPI->PlayVideo(fullFilePath, effectiveDelay, VideoFinishedCallback)){
            PrintLog(PRINT_ERROR, "[Overlay] Failed calling Play Video returning false");
            return false;
        }

        switch (g_videoOverlayAPI->GetPixelFmt()) {
            default: break;
            case TH_PF_420: videoSettings.shaderID = SHADER_YUV_420; break;
            case TH_PF_422: videoSettings.shaderID = SHADER_YUV_422; break;
            case TH_PF_444: videoSettings.shaderID = SHADER_YUV_444; break;
        }

        videoSettings.screenCount = 0;

        // First frame — mirrors the fallback path's ProcessVideo() call in LoadVideo.
        engine.skipCallback = NULL;
        if (g_videoOverlayAPI->IsPlaying())
            g_videoOverlayAPI->UpdateFrame();
        engine.skipCallback = skipCallback;

        changedVideoSettings = false;
        if (ENGINE_VERSION == 5)
            sceneInfo.state = ENGINESTATE_VIDEOPLAYBACK;
#if RETRO_REV0U
        else if (ENGINE_VERSION == 3)
            RSDK::Legacy::gameMode = RSDK::Legacy::v3::ENGINE_VIDEOWAIT;
#endif
        return true;
    }
    return false;

}
