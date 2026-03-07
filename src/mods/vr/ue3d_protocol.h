/*
 * ue3d_protocol.h - UE3D Shared Memory Protocol Definition
 *
 * VERSION: 4.0
 *
 * SINGLE SOURCE OF TRUTH for 3-party shared memory (UEVR + VRto3D + 3DGameBridge).
 * All projects should include this EXACT file to prevent struct misalignment.
 *
 * UEVR includes local copy:      #include "ue3d_protocol.h"
 * VRto3D includes canonical:     #include "vrto3dlib/ue3d_protocol.h"
 * 3DGameBridge includes local:   #include "ue3d_protocol.h"
 *
 * LICENSE: This file is dual-licensed to be compatible with both:
 *   - UEVR (MIT License)
 *   - VRto3D (LGPL v3)
 *
 * CHANGELOG:
 *   v4.0 - Repurposed dead mod_* fields (48 bytes at offset 184) for Leia integration
 *        - Eye tracking: 6 eye position floats + tracking flag + frame counter (32 bytes)
 *        - Display info: physical width/height from SR::Display for auto calibration (8 bytes)
 *        - Added UE3D_FLAG_LEIA_EYES feature flag
 *        - Fields written by 3DGameBridge (Path 1), read by UEVR via VRto3DBridge
 *        - Wire-compatible with v3: old readers see has_modifiers=0 (ignored)
 *        - Leia coordinates: origin=display center, X=right, Y=up, Z=backward (mm)
 *        - Renamed from UEVR_VRto3D_* to UE3D_* (3-party naming)
 *   v3.3 - Added stereo_depth_hint field (4 bytes from reserved)
 *        - UEVR writes its stereo_depth so VRto3D can match overlay IPD to game
 *        - Wire-compatible with v3.2: new field occupies previously-zeroed area
 *   v3.2 - Added monitor_mode + is_monitor_display fields (2 bytes from reserved)
 *        - UEVR sets monitor_mode=1 when monitor mode is active
 *        - VRto3D sets is_monitor_display=1 when outputting to monitor (not HMD)
 *        - Wire-compatible with v3.1: new fields occupy previously-zeroed area
 *   v3.1 - Added profile modifier fields (48 bytes from reserved)
 *        - Added UE3D_FLAG_MODIFIERS feature flag
 *        - Reserved shrinks from 72 to 24 bytes
 *        - Wire-compatible with v3.0: modifiers occupy previously-zeroed area
 *        - Fixed _pad1 alignment (was 1 byte, now 5 bytes)
 *        - Made this a standalone header both projects share
 *   v3.0 - Multiplier mode (depth_multiplier instead of suggested_depth)
 *   v2.0 - Absolute values (deprecated)
 */

#ifndef UE3D_PROTOCOL_H
#define UE3D_PROTOCOL_H

#include <cstdint>

/* ========================================================================== */
/* PROTOCOL CONSTANTS                                                         */
/* ========================================================================== */

#define UE3D_MAGIC          0x55455652   /* "UEVR" in ASCII (unchanged)        */
#define UE3D_VERSION        4            /* Protocol version                   */
#define UE3D_STRUCT_SIZE    256          /* Total struct size in bytes          */
#define UE3D_SHMEM_NAME    "UE3D_SharedData"

/* Staleness threshold: data older than this (ms) is considered disconnected  */
#define UE3D_STALE_MS       1000

/* Feature flags (bitfield in SharedData.flags)                               */
#define UE3D_FLAG_MULTIPLIER_MODE  0x01   /* Uses multipliers, not absolutes  */
#define UE3D_FLAG_SCENE_AWARE      0x02   /* Sends scene_type field           */
#define UE3D_FLAG_FOV_COMP         0x04   /* Supports FOV compensation        */
#define UE3D_FLAG_MODIFIERS        0x08   /* v3.1: (deprecated, repurposed as LEIA_EYES in v4.0) */
#define UE3D_FLAG_AIM_CORRECTION   0x10   /* v3.2: stereo_aim_correction field */
#define UE3D_FLAG_LEIA_EYES        0x20   /* v4.0: Leia eye tracking + display fields */

/* ========================================================================== */
/* ENUMS                                                                       */
/* ========================================================================== */

/* Scene type - tells VRto3D what context UEVR is in */
enum UE3D_SceneType : uint8_t {
    UE3D_SCENE_NORMAL   = 0,
    UE3D_SCENE_CUTSCENE = 1,
    UE3D_SCENE_MENU     = 2,
    UE3D_SCENE_VEHICLE  = 3,
    UE3D_SCENE_LOADING  = 4
};

/* Zoom mode - tells VRto3D what kind of zoom UEVR detected */
enum UE3D_ZoomMode : uint8_t {
    UE3D_ZOOM_NONE           = 0,
    UE3D_ZOOM_AIM_DOWN_SIGHT = 1,
    UE3D_ZOOM_SCOPE          = 2
};

/* ========================================================================== */
/* SHARED MEMORY STRUCTURE - 256 BYTES, PACKED                                 */
/*                                                                             */
/* ALL PARTIES MUST USE THIS EXACT LAYOUT.                                     */
/*                                                                             */
/* Offset map:                                                                 */
/*   0-15    HEADER             (16 bytes)                                     */
/*   16-39   FOV / ZOOM         (24 bytes)  UEVR -> VRto3D                    */
/*   40-55   DEPTH CONTROL      (16 bytes)  UEVR -> VRto3D                    */
/*   56-71   TIMING             (16 bytes)  UEVR -> VRto3D                    */
/*   72-103  VRTO3D STATE       (32 bytes)  VRto3D -> UEVR                    */
/*   104-119 VRTO3D STATUS      (16 bytes)  VRto3D -> UEVR                    */
/*   120-183 PROFILE INFO       (64 bytes)  UEVR -> VRto3D                    */
/*   184-215 LEIA EYE TRACKING  (32 bytes)  3DGameBridge -> UEVR  [v4.0]     */
/*   216-223 LEIA DISPLAY INFO  (8 bytes)   3DGameBridge -> UEVR  [v4.0]     */
/*   224-231 LEIA RESERVED      (8 bytes)   Future Leia fields                */
/*   232-243 AIM + COMMANDS     (12 bytes)  UEVR -> VRto3D  [v3.2]           */
/*   244-245 MONITOR MODE       (2 bytes)   Bidirectional   [v3.2]           */
/*   246-249 STEREO DEPTH HINT  (4 bytes)   UEVR -> VRto3D  [v3.3]           */
/*   250-255 RESERVED           (6 bytes)   Future use                         */
/*                                                                             */
/* MONITOR MODE FIELD STATUS (for cleanup reference):                          */
/*                                                                             */
/* ALIVE (system breaks without):                                              */
/*   monitor_mode, is_monitor_display, stereo_depth_hint,                      */
/*   fov_scale, zoom_mode, depth_multiplier, scene_type, world_scale,          */
/*   vrto3d_connected, auto_depth_request, command_seq,                        */
/*   magic, version, struct_size, flags, is_valid, timestamps                  */
/*                                                                             */
/* ALIVE (Leia -- Path 1, written by 3DGameBridge, read by UEVR):             */
/*   leia_tracking_active, leia_left_eye_x/y/z, leia_right_eye_x/y/z,         */
/*   leia_frame_counter, leia_display_width_cm, leia_display_height_cm         */
/*                                                                             */
/* DEAD IN MONITOR MODE (UEVR handles internally):                             */
/*   game_fov, base_fov (VRto3D doesn't act on these)                          */
/*   convergence_multiplier (always 1.0, UEVR owns convergence)                */
/*   stereo_aim_correction, stereo_aim_base (VR-only feature)                  */
/*   uevr_frametime (never consumed)                                           */
/*   is_zooming (VRto3D derives from fov_scale < 0.99)                         */
/*                                                                             */
/* DIAGNOSTIC ONLY (read for UI display, no logic depends on them):            */
/*   vrto3d_depth, vrto3d_convergence, vrto3d_fov, vrto3d_fov_adjustment,      */
/*   vrto3d_aspect_ratio, vrto3d_ipd, vrto3d_hmd_height, vrto3d_sbs_mode      */
/* ========================================================================== */

#pragma pack(push, 1)
typedef struct UE3D_SharedData {

    /* ----- HEADER (16 bytes) -------------------------------------------- */
    uint32_t magic;                  /* Must be UE3D_MAGIC                   */
    uint32_t version;                /* UE3D_VERSION                         */
    uint32_t struct_size;            /* sizeof(UE3D_SharedData) = 256        */
    uint32_t flags;                  /* Bitfield: UE3D_FLAG_*                */

    /* ----- UEVR -> VRTO3D: FOV / ZOOM (24 bytes) ----------------------- */
    float    game_fov;               /* Game camera FOV in degrees           */
    float    base_fov;               /* Calibrated "normal" FOV              */
    float    fov_scale;              /* Projection scale (0.5 = 2x zoom)    */
    float    zoom_factor;            /* Magnification (2.0 = 2x zoom)       */
    uint8_t  is_zooming;             /* 1 if zoom is active                  */
    uint8_t  is_valid;               /* 1 if FOV reading is trustworthy      */
    uint8_t  zoom_mode;              /* UE3D_ZoomMode enum                   */
    uint8_t  _pad1[5];              /* Align to 24 bytes for this section   */

    /* ----- UEVR -> VRTO3D: DEPTH CONTROL (16 bytes) -------------------- */
    float    depth_multiplier;       /* 0.05 - 1.0 (1.0 = no change)        */
    float    convergence_multiplier; /* Reserved, usually 1.0                */
    uint8_t  scene_type;             /* UE3D_SceneType enum                  */
    uint8_t  auto_depth_request;     /* 1 if UEVR wants auto-depth applied   */
    uint8_t  _pad2[2];              /* Padding                              */
    float    world_scale;            /* UEVR world scale                     */

    /* ----- UEVR -> VRTO3D: TIMING (16 bytes) --------------------------- */
    uint64_t uevr_timestamp;         /* GetTickCount64() from UEVR           */
    uint32_t uevr_frame_count;       /* UEVR frame counter                   */
    float    uevr_frametime;         /* Frame time in seconds                */

    /* ----- VRTO3D -> UEVR: CURRENT STATE (32 bytes) -------------------- */
    float    vrto3d_depth;           /* VRto3D's current depth               */
    float    vrto3d_convergence;     /* VRto3D's current convergence         */
    float    vrto3d_fov;             /* VRto3D's current FOV                 */
    float    vrto3d_fov_adjustment;  /* FOV delta from convergence           */
    float    vrto3d_aspect_ratio;    /* Display aspect ratio                 */
    float    vrto3d_ipd;             /* IPD setting                          */
    float    vrto3d_hmd_height;      /* HMD height                           */
    uint8_t  vrto3d_sbs_mode;       /* 0 = TaB, 1 = SbS                    */
    uint8_t  _pad3[3];              /* Padding                              */

    /* ----- VRTO3D -> UEVR: STATUS (16 bytes) --------------------------- */
    uint8_t  vrto3d_connected;       /* 1 if VRto3D is reading this memory   */
    uint8_t  vrto3d_auto_depth_active; /* 1 if applying depth multiplier     */
    uint8_t  vrto3d_profile_loaded;  /* 1 if VRto3D has a game profile       */
    uint8_t  vrto3d_listener_enabled;/* 1 if Ctrl+F11 auto-depth is ON       */
    uint8_t  _pad4[4];              /* Padding                              */
    uint64_t vrto3d_timestamp;       /* GetTickCount64() from VRto3D         */

    /* ----- PROFILE INFO (64 bytes) -------------------------------------- */
    char     uevr_profile_name[32];  /* Current UEVR profile name            */
    char     game_exe_name[32];      /* Game executable name                 */

    /* ----- v4.0: LEIA EYE TRACKING (32 bytes) 3DGameBridge -> UEVR ------ */
    /*                                                                       */
    /* Eye positions from LeiaSR SDK, written by 3DGameBridge (Path 1).      */
    /* Coordinates: Leia space (origin=display center, mm).                  */
    /*   X = right, Y = up, Z = backward (positive away from user).         */
    /* UE conversion: FVector(-Z, X, Y), divide mm by 10 for cm.            */
    /* Both eyes provided; consumer averages for center-eye head offset.     */
    /* v3.x peers see has_modifiers=0 (leia_tracking_active=0 on init),     */
    /* so mod fields read as "not overridden" -- backward compatible.        */
    /* Guard: (flags & UE3D_FLAG_LEIA_EYES) && leia_tracking_active.        */
    /*                                                                       */
    uint8_t  leia_tracking_active;   /* 1 if face tracked, 0 if not         */
    uint8_t  _leia_pad1[3];          /* Alignment padding                   */
    float    leia_left_eye_x;        /* Left eye X (mm, right)              */
    float    leia_left_eye_y;        /* Left eye Y (mm, up)                 */
    float    leia_left_eye_z;        /* Left eye Z (mm, backward)           */
    float    leia_right_eye_x;       /* Right eye X (mm, right)             */
    float    leia_right_eye_y;       /* Right eye Y (mm, up)                */
    float    leia_right_eye_z;       /* Right eye Z (mm, backward)          */
    uint32_t leia_frame_counter;     /* Writer increments each update       */

    /* ----- v4.0: LEIA DISPLAY INFO (8 bytes) 3DGameBridge -> UEVR ------- */
    /*                                                                       */
    /* Physical display dimensions from SR::Display class.                   */
    /* Enables automatic stereo calibration: stereo_depth = IPD / width.     */
    /* Replaces UE3D_MonitorDetect EDID heuristic with exact hardware data.  */
    /* 0.0 = not provided (non-Leia display or 3DGameBridge not running).    */
    /*                                                                       */
    float    leia_display_width_cm;  /* Physical display width (cm)         */
    float    leia_display_height_cm; /* Physical display height (cm)        */

    /* ----- v4.0: RESERVED FOR LEIA (8 bytes) ---------------------------- */
    uint8_t  _leia_reserved[8];      /* Zero-filled, future Leia fields     */

    /* ----- v3.2: AIM CORRECTION + COMMANDS (12 bytes) UEVR -> VRto3D ---- */
    float    stereo_aim_correction;  /* Zoom-scaled lateral aim correction    */
    float    stereo_aim_base;        /* Constant lateral aim correction base  */
    uint32_t command_seq;            /* Sequence number for depth commands    */

    /* ----- v3.2: MONITOR MODE (2 bytes) --------------------------------- */
    /*                                                                       */
    /* monitor_mode: set by UEVR when monitor mode is active.                */
    /* is_monitor_display: set by VRto3D when it's a monitor driver          */
    /*   (not a real VR HMD). UEVR reads this to auto-enable bMonitorMode.   */
    /* v3.1 peers see zeroes here and are unaffected.                        */
    /*                                                                       */
    uint8_t  monitor_mode;           /* 1 if UEVR monitor mode is active     */
    uint8_t  is_monitor_display;     /* 1 if VRto3D is outputting to monitor  */

    /* ----- v3.3: STEREO DEPTH HINT (4 bytes) UEVR -> VRto3D ------------ */
    /*                                                                       */
    /* UEVR's stereo_depth (IPD/screen_width calibration value).             */
    /* In monitor mode, VRto3D uses this as base depth instead of            */
    /* config.depth so overlay IPD matches game world stereo separation.     */
    /* 0.0 = not provided (v3.2 peers see zero, use config.depth).           */
    /*                                                                       */
    float    stereo_depth_hint;      /* UEVR stereo depth for overlay IPD    */

    /* ----- RESERVED (6 bytes) ------------------------------------------- */
    uint8_t  reserved[6];            /* Zero-filled, for future fields       */

} UE3D_SharedData;
#pragma pack(pop)

/* Compile-time size check - if this fires, the struct layout is wrong */
#ifndef __cplusplus
_Static_assert(sizeof(UE3D_SharedData) == 256,
    "UE3D_SharedData must be exactly 256 bytes");
#else
static_assert(sizeof(UE3D_SharedData) == 256,
    "UE3D_SharedData must be exactly 256 bytes");
#endif

/* ========================================================================== */
/* HELPERS                                                                     */
/* ========================================================================== */

/* Check if UEVR data is fresh */
static inline int ue3d_is_uevr_fresh(const UE3D_SharedData* d, uint64_t now_ms) {
    if (!d || d->magic != UE3D_MAGIC) return 0;
    if (!d->is_valid) return 0;
    return (now_ms - d->uevr_timestamp) < UE3D_STALE_MS;
}

/* Check if VRto3D data is fresh */
static inline int ue3d_is_vrto3d_fresh(const UE3D_SharedData* d, uint64_t now_ms) {
    if (!d || d->magic != UE3D_MAGIC) return 0;
    if (!d->vrto3d_connected) return 0;
    return (now_ms - d->vrto3d_timestamp) < UE3D_STALE_MS;
}

/* Check if Leia eye tracking data is present (v4.0) */
static inline int ue3d_has_leia_eyes(const UE3D_SharedData* d) {
    if (!d) return 0;
    return (d->flags & UE3D_FLAG_LEIA_EYES) && d->leia_tracking_active;
}

#endif /* UE3D_PROTOCOL_H */
