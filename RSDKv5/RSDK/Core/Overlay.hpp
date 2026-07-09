#pragma once
#include <stdint.h>
#include <stdbool.h>

// -------------------------------------------------------
// ResidentAPI
// This is the table of engine functions that overlays are
// allowed to call back into. Populated at startup, passed
// to every overlay on load. Keep this minimal and stable —
// on Saturn this will be a fixed struct in ROM-resident RAM.
// -------------------------------------------------------
typedef struct ResidentAPI {
    // Drawing — overlays need to blit decoded frames
    void (*DrawRectangle)(int x, int y, int w, int h, uint32_t color, int alpha, int inkEffect, bool screenRelative);
    void (*CopyFrameBuffer)(void); // present current backbuffer

    // Audio — video overlay needs to push PCM
    void (*LockAudioDevice)(void);
    void (*UnlockAudioDevice)(void);
    bool (*QueueAudioSamples)(const int16_t *samples, int count);

    // File I/O — thin wrapper around RSDK's reader
    void *(*OpenFile)(const char *path, const char *mode);
    int (*ReadFile)(void *handle, void *buf, int size);
    void (*CloseFile)(void *handle);

    // Timing
    double (*GetCurrentTime)(void); // seconds since engine start
    double (*GetAudioClock)(void);  // audio stream position in seconds; -1 when unavailable

    // YUV frame upload — routes to the correct RenderDevice path.
    // pixelFmt mirrors th_pixel_fmt: TH_PF_420=0, TH_PF_422=1, TH_PF_444=2.
    void (*UploadYUVFrame)(int pixelFmt,
                           int w, int h,
                           uint8_t *y, uint8_t *u, uint8_t *v,
                           int strideY, int strideU, int strideV);
} ResidentAPI;

// -------------------------------------------------------
// OverlayHeader
// Every overlay shared library must export a symbol named
// "OVERLAY_ENTRY" of this type. It is the sole ABI contract
// between the engine and an overlay.
// -------------------------------------------------------
typedef struct OverlayHeader {
    uint32_t magic;   // must be 0x4F564C59 ('OVLY')
    uint32_t version; // overlay ABI version, currently 1
    uint32_t maxSize; // self-reported max RAM needed (for Saturn budget checks)

    // Lifecycle
    bool (*Load)(const ResidentAPI *api); // called after mapping; return false = abort
    void (*Unload)(void);                 // called before unmapping; must null all state
} OverlayHeader;

#define OVERLAY_MAGIC   0x4F564C59u
#define OVERLAY_VERSION 1u

// -------------------------------------------------------
// OverlaySlot
// The engine keeps one of these per overlay "socket".
// Phase 1: we only need one socket (for video).
// Later you add more slots for zones, menus, etc.
// -------------------------------------------------------
typedef struct OverlaySlot {
    void *platformHandle; // dlopen handle / NULL if not loaded
    OverlayHeader header; // copy of the overlay's header
    bool loaded;
} OverlaySlot;

// -------------------------------------------------------
// Public API — called from RetroEngine.cpp
// -------------------------------------------------------
void Overlay_Init(void);
void Overlay_Shutdown(void);

// Load an overlay from a shared library path. Returns true on success.
bool Overlay_Load(OverlaySlot *slot, const char *path);

// Unload an overlay, calling its Unload() hook first.
void Overlay_Unload(OverlaySlot *slot);

// The single global resident API table — filled in by Overlay_Init().
extern ResidentAPI g_residentAPI;

// Retrieve an exported symbol from a loaded overlay's platform handle.
// Returns NULL if the slot is not loaded or the symbol is not found.
void *Overlay_GetSymbol(OverlaySlot *slot, const char *name);