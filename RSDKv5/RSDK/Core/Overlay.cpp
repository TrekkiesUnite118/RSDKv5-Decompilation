#include "Overlay.hpp"
#include "RetroEngine.hpp" // for engine state
#include "../Graphics/Drawing.hpp"
#include "../Audio/Audio.hpp"

using namespace RSDK;

#if defined(_WIN32)
#include <windows.h>
#define PLATFORM_DLOPEN(p)   (void *)LoadLibraryA(p)
#define PLATFORM_DLSYM(h, s) (void *)GetProcAddress((HMODULE)(h), s)
#define PLATFORM_DLCLOSE(h)  FreeLibrary((HMODULE)(h))
#else
#include <dlfcn.h>
#define PLATFORM_DLOPEN(p)   dlopen((p), RTLD_NOW | RTLD_LOCAL)
#define PLATFORM_DLSYM(h, s) dlsym((h), (s))
#define PLATFORM_DLCLOSE(h)  dlclose(h)
#endif

// -------------------------------------------------------
// Forward declarations for resident API implementations
// -------------------------------------------------------
static void *Resident_OpenFile(const char *path, const char *mode)
{
    (void)mode; // RSDK storage is read-only; mode is ignored
    FileInfo *info = new FileInfo();
    InitFileInfo(info);
    if (!LoadFile(info, path, FMODE_RB)) {
        delete info;
        return NULL;
    }
    return info;
}
static int  Resident_ReadFile(void *h, void *buf, int sz) { return (int)ReadBytes((FileInfo *)h, (uint8_t *)buf, sz); }
static void Resident_CloseFile(void *h) { CloseFile((FileInfo *)h); delete (FileInfo *)h; }
//static double Resident_GetTime(void) { return (double)SDL_GetTicks() / 1000.0; }
static double Resident_GetAudioClock(void) { return GetVideoStreamPos(); }

static void Resident_UploadYUVFrame(int fmt, int w, int h,
                                    uint8_t *y, uint8_t *u, uint8_t *v,
                                    int sY, int sU, int sV)
{
    switch (fmt) {
        case 2: RenderDevice::SetupVideoTexture_YUV444(w, h, y, u, v, sY, sU, sV); break; // TH_PF_444
        case 1: RenderDevice::SetupVideoTexture_YUV422(w, h, y, u, v, sY, sU, sV); break; // TH_PF_422
        case 0: RenderDevice::SetupVideoTexture_YUV420(w, h, y, u, v, sY, sU, sV); break; // TH_PF_420
        default: break;
    }
}

// Thin wrappers — you already have these in the engine,
// we just expose them through the table.
static void Resident_LockAudio(void) { /* SDL_LockAudioDevice or equivalent */ }
static void Resident_UnlockAudio(void) { /* SDL_UnlockAudioDevice or equivalent */ }
static bool Resident_QueueSamples(const int16_t *s, int n)
{
    // Hook into your existing audio mix buffer here.
    // For now, a stub so things link.
    (void)s;
    (void)n;
    return true;
}

// -------------------------------------------------------
// Global resident API table
// -------------------------------------------------------
ResidentAPI g_residentAPI;

void Overlay_Init(void)
{
    g_residentAPI.OpenFile          = Resident_OpenFile;
    g_residentAPI.ReadFile          = Resident_ReadFile;
    g_residentAPI.CloseFile         = Resident_CloseFile;
    //g_residentAPI.GetCurrentTime    = Resident_GetTime;
    g_residentAPI.LockAudioDevice   = Resident_LockAudio;
    g_residentAPI.UnlockAudioDevice = Resident_UnlockAudio;
    g_residentAPI.QueueAudioSamples = Resident_QueueSamples;
    g_residentAPI.GetAudioClock     = Resident_GetAudioClock;
    g_residentAPI.UploadYUVFrame    = Resident_UploadYUVFrame;
    // DrawRectangle / CopyFrameBuffer filled in once RenderDevice is up
}

void Overlay_Shutdown(void) { /* future: unload any live slots */ }

bool Overlay_Load(OverlaySlot *slot, const char *path)
{
    if (slot->loaded)
        Overlay_Unload(slot);

    void *handle = PLATFORM_DLOPEN(path);
    if (!handle) {
        PrintLog(PRINT_ERROR, "[Overlay] Failed to open '%s'", path);
        return false;
    }

    OverlayHeader *hdr = (OverlayHeader *)PLATFORM_DLSYM(handle, "OVERLAY_ENTRY");
    if (!hdr || hdr->magic != OVERLAY_MAGIC || hdr->version != OVERLAY_VERSION) {
        PrintLog(2, "[Overlay] Bad header in '%s', '%s'", path, hdr);
        PLATFORM_DLCLOSE(handle);
        return false;
    }

    if (!hdr->Load(&g_residentAPI)) {
        PrintLog(PRINT_ERROR, "[Overlay] Load() failed for '%s'", path);
        PLATFORM_DLCLOSE(handle);
        return false;
    }

    slot->platformHandle = handle;
    slot->header         = *hdr;
    slot->loaded         = true;
    PrintLog(PRINT_NORMAL, "[Overlay] Loaded '%s'", path);
    return true;
}

void *Overlay_GetSymbol(OverlaySlot *slot, const char *name)
{
    if (!slot->loaded || !slot->platformHandle)
        return NULL;
    return PLATFORM_DLSYM(slot->platformHandle, name);
}

void Overlay_Unload(OverlaySlot *slot)
{
    PrintLog(PRINT_ERROR, "[Overlay] Unloading Overlay");
    if (!slot->loaded)
        return;
    slot->header.Unload();
    PLATFORM_DLCLOSE(slot->platformHandle);
    slot->platformHandle = NULL;
    slot->loaded         = false;
}