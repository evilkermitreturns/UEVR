/*
 * VRto3DBridge.hpp - UEVR <-> VRto3D Communication Bridge (Cooperative Mode)
 *
 * VERSION: 3.3 (matches uevr_vrto3d_protocol.h)
 * PURPOSE: Bidirectional shared memory IPC between UEVR and VRto3D
 *
 * KEY DESIGN:
 *   - Sends MULTIPLIERS instead of absolute values
 *   - VRto3D's profile remains the authority for depth/convergence
 *   - UEVR says "reduce depth by X%" during zoom
 *
 * DEBUG: Set config().debug_logging = true for verbose output
 *
 * For: oneup03's VRto3D - Cooperative, not competitive!
 */

#pragma once

#include <Windows.h>
#include <cstdint>
#include <atomic>
#include <mutex>
#include <string>

// Forward-declare DepthMode from GameFOV.hpp to avoid circular include
namespace vrmod { enum class DepthMode : uint8_t; }

namespace vrmod {

// ============================================================================
// PROTOCOL CONSTANTS
// ============================================================================

constexpr uint32_t VRTO3D_BRIDGE_MAGIC = 0x55455652;  // "UEVR" in ASCII
constexpr uint32_t VRTO3D_BRIDGE_VERSION = 3;         // Protocol v3 (v3.1 is backward-compat)
constexpr const char* VRTO3D_SHARED_MEM_NAME = "UEVR_VRto3D_SharedData";

// Scene types for depth adjustments
enum class SceneType : uint8_t {
    Normal = 0,
    Cutscene = 1,
    Menu = 2,
    Vehicle = 3,
    Loading = 4
};

// Camera/zoom modes
enum class ZoomMode : uint8_t {
    None = 0,
    AimDownSights = 1,
    Scope = 2
};

// Feature flags
constexpr uint32_t FLAG_MULTIPLIER_MODE = 0x01;  // Uses multipliers, not absolutes
constexpr uint32_t FLAG_SCENE_AWARE     = 0x02;  // Sends scene type
constexpr uint32_t FLAG_FOV_COMP        = 0x04;  // Supports FOV compensation
constexpr uint32_t FLAG_MODIFIERS       = 0x08;  // v3.1: Supports profile modifiers
constexpr uint32_t FLAG_AIM_CORRECTION  = 0x10;  // v3.2: stereo_aim_correction field

// ============================================================================
// SHARED MEMORY STRUCTURE v3.3 (256 bytes)
// MUST match VRto3D's uevr_receiver.hpp EXACTLY!
// See uevr_vrto3d_protocol.h for canonical layout and changelog.
// Profile modifiers use 48 bytes from original reserved area.
// Older receivers see new fields as zeroes and ignore them safely.
// ============================================================================

#pragma pack(push, 1)
struct UEVR_VRto3D_SharedData {
    // ----- HEADER (16 bytes) -----
    uint32_t magic;              // 0x55455652 ("UEVR")      [4]  offset 0
    uint32_t version;            // 3                        [4]  offset 4
    uint32_t struct_size;        // 256                      [4]  offset 8
    uint32_t flags;              // Feature flags            [4]  offset 12
    // Subtotal: 16 bytes (offset 16)

    // ----- UEVR -> VRTO3D: FOV/ZOOM (24 bytes) -----
    float game_fov;              // Game camera FOV (degrees)  [4]  offset 16
    float base_fov;              // Calibrated normal FOV      [4]  offset 20
    float fov_scale;             // Projection scale           [4]  offset 24
    float zoom_factor;           // Magnification              [4]  offset 28
    uint8_t is_zooming;          // 1 if zoomed                [1]  offset 32
    uint8_t is_valid;            // 1 if FOV reading valid     [1]  offset 33
    uint8_t zoom_mode;           // ZoomMode enum              [1]  offset 34
    uint8_t _pad1[5];            // Padding to 24 bytes        [5]  offset 35
    // Subtotal: 24 bytes (offset 40)

    // ----- UEVR -> VRTO3D: DEPTH CONTROL (16 bytes) -----
    float depth_multiplier;      // 0.05-1.0                  [4]  offset 40
    float convergence_multiplier;// Reserved (usually 1.0)   [4]  offset 44
    uint8_t scene_type;          // SceneType enum            [1]  offset 48
    uint8_t auto_depth_request;  // 1 = UEVR wants auto-depth [1]  offset 49
    uint8_t _pad2[2];            // Padding                   [2]  offset 50
    float world_scale;           // UEVR world scale          [4]  offset 52
    // Subtotal: 16 bytes (offset 56)

    // ----- UEVR -> VRTO3D: TIMING (16 bytes) -----
    uint64_t uevr_timestamp;     //                           [8]  offset 56
    uint32_t uevr_frame_count;   //                           [4]  offset 64
    float uevr_frametime;        //                           [4]  offset 68
    // Subtotal: 16 bytes (offset 72)

    // ----- VRTO3D -> UEVR: CURRENT STATE (32 bytes) -----
    float vrto3d_depth;          // Current depth             [4]  offset 72
    float vrto3d_convergence;    // Current convergence       [4]  offset 76
    float vrto3d_fov;            // Current VRto3D FOV        [4]  offset 80
    float vrto3d_fov_adjustment; // FOV delta                 [4]  offset 84
    float vrto3d_aspect_ratio;   //                           [4]  offset 88
    float vrto3d_ipd;            //                           [4]  offset 92
    float vrto3d_hmd_height;     //                           [4]  offset 96
    uint8_t vrto3d_sbs_mode;     //                           [1]  offset 100
    uint8_t _pad3[3];            // Padding                   [3]  offset 101
    // Subtotal: 32 bytes (offset 104)

    // ----- VRTO3D -> UEVR: STATUS (16 bytes) -----
    uint8_t vrto3d_connected;        // 1 if VRto3D responding    [1]  offset 104
    uint8_t vrto3d_auto_depth_active;// 1 if applying multiplier  [1]  offset 105
    uint8_t vrto3d_profile_loaded;   // 1 if profile loaded       [1]  offset 106
    uint8_t vrto3d_listener_enabled; // 1 if listener on          [1]  offset 107
    uint8_t _pad4[4];                // Padding                   [4]  offset 108
    uint64_t vrto3d_timestamp;       //                           [8]  offset 112
    // Subtotal: 16 bytes (offset 120)

    // ----- PROFILE INFO (64 bytes) -----
    char uevr_profile_name[32];  // Current UEVR profile name [32] offset 120
    char game_exe_name[32];      // Game executable name      [32] offset 152
    // Subtotal: 64 bytes (offset 184)

    // ----- v3.1: VRTO3D -> UEVR: PROFILE MODIFIERS (48 bytes) -----
    // Sent by VRto3D when a game profile has an "uevr_modifiers" section.
    // All float fields use 0.0 as sentinel for "not overridden."
    // v3.0 receivers see zeroes here and ignore them safely.
    uint8_t has_modifiers;           // 1 if profile has uevr_modifiers [1]  offset 184
    uint8_t _pad5[3];               // Padding                          [3]  offset 185
    float mod_depth_strength;        // Override depth_strength          [4]  offset 188
    float mod_depth_min_floor;       // Override global min depth        [4]  offset 192
    float mod_ads_floor;             // Override ADS floor               [4]  offset 196
    float mod_scope_floor;           // Override scope floor             [4]  offset 200
    float mod_cutscene_floor;        // Override cutscene floor          [4]  offset 204
    float mod_base_power;            // Override depth_base_power        [4]  offset 208
    float mod_extra_power;           // Override depth_extra_power       [4]  offset 212
    float mod_dead_zone;             // Override dead_zone zoom factor   [4]  offset 216
    float mod_transition_speed;      // Override smoothing speed mult    [4]  offset 220
    float mod_zoom_threshold;        // Override zoom detect threshold   [4]  offset 224
    float mod_base_fov_override;     // Override base FOV (0 = auto)     [4]  offset 228
    // Subtotal: 48 bytes (offset 232)

    // ----- AIM + COMMANDS (12 bytes) -----
    float   stereo_aim_correction;   // v3.2: Lateral aim correction (zoom) [4]  offset 232
    float   stereo_aim_base;         // v3.2: Lateral aim correction (base) [4]  offset 236
    uint32_t command_seq;            // v3.2: Increments on each cmd 2-9 [4]  offset 240
    // Subtotal: 12 bytes (offset 244)

    // ----- MONITOR MODE (2 bytes) -----
    uint8_t monitor_mode;            // v3.2: 1 if UEVR monitor mode    [1]  offset 244
    uint8_t is_monitor_display;      // v3.2: 1 if VRto3D is monitor    [1]  offset 245
    // Subtotal: 2 bytes (offset 246)

    // ----- STEREO DEPTH HINT (4 bytes) -----
    float   stereo_depth_hint;       // v3.3: UEVR stereo depth for IPD [4]  offset 246
    // Subtotal: 4 bytes (offset 250)

    // ----- RESERVED (6 bytes) -----
    uint8_t reserved[6];             // future use                       [6]  offset 250
    // Subtotal: 6 bytes (offset 256)

    // TOTAL: 16+24+16+16+32+16+64+48+12+2+4+6 = 256 bytes
};
#pragma pack(pop)

static_assert(sizeof(UEVR_VRto3D_SharedData) == 256,
    "CRITICAL: SharedData must be exactly 256 bytes to match VRto3D!");

// ============================================================================
// PROFILE MODIFIERS STRUCT (convenient read-only view)
// ============================================================================

struct ProfileModifiers {
    bool active = false;
    float depth_strength = 0.0f;
    float depth_min_floor = 0.0f;
    float ads_floor = 0.0f;
    float scope_floor = 0.0f;
    float cutscene_floor = 0.0f;
    float base_power = 0.0f;
    float extra_power = 0.0f;
    float dead_zone = 0.0f;
    float transition_speed = 0.0f;
    float zoom_threshold = 0.0f;
    float base_fov_override = 0.0f;
};

// ============================================================================
// BRIDGE CLASS
// ============================================================================

class VRto3DBridge {
public:
    // ----- Singleton -----
    static VRto3DBridge& get() {
        static VRto3DBridge instance;
        return instance;
    }

    // ----- Configuration -----
    struct Config {
        bool enabled = true;
        bool use_fov_compensation = true;
        bool debug_logging = false;

        // v3.1: Adaptive depth curve parameters
        // These are the UEVR defaults; profile modifiers can override them.
        float depth_base_power = 0.8f;       // Power curve base exponent
        float depth_extra_power = 0.8f;      // Additional power at high zoom
        float depth_strength = 1.0f;         // Overall strength multiplier
        float depth_dead_zone = 1.15f;       // No correction below this zoom
        float min_depth_multiplier = 0.05f;  // Absolute minimum multiplier

        // v3.1: Per-mode floors
        float ads_min_depth = 0.20f;
        float scope_min_depth = 0.05f;
        float cutscene_min_depth = 0.30f;

        // v3.2: Aim correction
        float stereo_aim_correction = 0.0f;  // Zoom-scaled (only during ADS/scope)
        float stereo_aim_base = 0.0f;        // Constant base offset (always applied)
    };

    Config& config() { return m_config; }
    const Config& config() const { return m_config; }

    // ----- Lifecycle -----
    bool init();
    void shutdown();
    bool is_initialized() const { return m_data != nullptr; }

    // ----- Connection Status -----
    bool is_vrto3d_connected() const;
    bool is_vrto3d_listener_enabled() const;
    bool is_vrto3d_applying_depth() const;
    bool is_vrto3d_profile_loaded() const;

    // ----- v3.1: Profile Modifiers -----
    bool has_profile_modifiers() const;
    ProfileModifiers get_modifiers() const;

    // ----- UEVR -> VRto3D: Write Methods -----

    void update_fov_state(
        float game_fov, float base_fov, float fov_scale, float zoom_factor,
        bool is_zooming, bool is_valid, ZoomMode zoom_mode = ZoomMode::None
    );

    void update_depth_multiplier(float multiplier, bool is_auto_depth = true);
    void update_scene(SceneType scene, float world_scale = 1.0f);
    void set_profile_info(const std::string& profile_name, const std::string& exe_name);
    void update_timing(float frametime);

    void update_all(
        float game_fov, float base_fov, float fov_scale, float zoom_factor,
        bool is_zooming, bool is_valid, ZoomMode zoom_mode,
        float depth_multiplier, SceneType scene,
        float world_scale = 1.0f,
        float convergence_mult = 1.0f
    );

    // ----- VRto3D Commands (via auto_depth_request field) -----
    void request_calibration();         // cmd=2: Auto-calc from world_scale
    void request_depth_decrease();      // cmd=3: -3D  (reduce depth 20%)
    void request_depth_increase();      // cmd=4: +3D  (increase depth 20%)
    void request_depth_big_decrease();  // cmd=5: --3D  (reduce depth 40%)
    void request_depth_big_increase();  // cmd=6: ++3D  (increase depth 40%)
    void request_depth_huge_decrease(); // cmd=8: ---3D (reduce depth 60%)
    void request_depth_huge_increase(); // cmd=9: +++3D (increase depth 60%)

    // ----- VRto3D -> UEVR: Read Methods -----
    float get_vrto3d_depth() const;
    float get_vrto3d_convergence() const;
    float get_vrto3d_fov() const;
    float get_vrto3d_fov_adjustment() const;
    bool get_is_monitor_display() const { return m_data && m_data->is_monitor_display; }

    // ----- Calculated Values -----

    float get_compensated_fov_scale(float raw_fov_scale) const;

    /**
     * v3.1: Calculate depth multiplier with adaptive curve.
     *
     * @param fov_scale    Current FOV scale (0.5 = 2x zoom)
     * @param zoom_factor  Current magnification (2.0 = 2x zoom)
     * @param mode         Current depth mode for per-mode floors
     * @return Multiplier 0.05-1.0 to send to VRto3D
     */
    float calculate_zoom_depth_multiplier(float fov_scale, float zoom_factor, DepthMode mode) const;

    // Backward-compat overload (v3.0 callers)
    float calculate_zoom_depth_multiplier(float fov_scale) const;

private:
    VRto3DBridge() = default;
    ~VRto3DBridge() { shutdown(); }
    VRto3DBridge(const VRto3DBridge&) = delete;
    VRto3DBridge& operator=(const VRto3DBridge&) = delete;

    void update_timestamp();
    void debug_log(const char* fmt, ...) const;

    HANDLE m_mapping = nullptr;
    UEVR_VRto3D_SharedData* m_data = nullptr;
    mutable std::mutex m_mutex;
    Config m_config{};
    std::atomic<uint32_t> m_frame_count{0};
    uint32_t m_command_seq{0};  // v3.2: Incremented on each depth command (matches struct field type)
    bool m_monitor_mode{false};

public:
    void set_monitor_mode(bool enabled) { m_monitor_mode = enabled; }
};

} // namespace vrmod
