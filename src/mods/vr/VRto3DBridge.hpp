// VRto3DBridge.hpp - Shared memory bridge to VRto3D (protocol v4.0)
#pragma once

#include <Windows.h>
#include <cstdint>
#include <atomic>
#include <mutex>
#include <string>

// Protocol struct (canonical in VRto3DLib, local copy for UEVR)
#include "ue3d_protocol.h"

// Forward-declare DepthMode from GameFOV.hpp to avoid circular include
namespace vrmod { enum class DepthMode : uint8_t; }

namespace vrmod {

// Protocol constants
constexpr uint32_t VRTO3D_BRIDGE_MAGIC = UE3D_MAGIC;
constexpr uint32_t VRTO3D_BRIDGE_VERSION = UE3D_VERSION;
constexpr const char* VRTO3D_SHARED_MEM_NAME = UE3D_SHMEM_NAME;

// Scene types
enum class SceneType : uint8_t {
    Normal = UE3D_SCENE_NORMAL,
    Cutscene = UE3D_SCENE_CUTSCENE,
    Menu = UE3D_SCENE_MENU,
    Vehicle = UE3D_SCENE_VEHICLE,
    Loading = UE3D_SCENE_LOADING
};

// Camera/zoom modes
enum class ZoomMode : uint8_t {
    None = UE3D_ZOOM_NONE,
    AimDownSights = UE3D_ZOOM_AIM_DOWN_SIGHT,
    Scope = UE3D_ZOOM_SCOPE
};

// Feature flags
constexpr uint32_t FLAG_MULTIPLIER_MODE = UE3D_FLAG_MULTIPLIER_MODE;
constexpr uint32_t FLAG_SCENE_AWARE     = UE3D_FLAG_SCENE_AWARE;
constexpr uint32_t FLAG_FOV_COMP        = UE3D_FLAG_FOV_COMP;
constexpr uint32_t FLAG_AIM_CORRECTION  = UE3D_FLAG_AIM_CORRECTION;

class VRto3DBridge {
public:
    // Singleton
    static VRto3DBridge& get() {
        static VRto3DBridge instance;
        return instance;
    }

    // Configuration
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

    // Lifecycle
    bool init();
    void shutdown();
    bool is_initialized() const { return m_data != nullptr; }

    // Connection status
    bool is_vrto3d_connected() const;
    bool is_vrto3d_listener_enabled() const;
    bool is_vrto3d_applying_depth() const;
    bool is_vrto3d_profile_loaded() const;

    // Write methods (UEVR -> VRto3D)

    void update_fov_state(
        float game_fov, float base_fov, float fov_scale, float zoom_factor,
        bool is_zooming, bool is_valid, ZoomMode zoom_mode = ZoomMode::None
    );

    void update_depth_multiplier(float multiplier, bool is_auto_depth = true);
    void update_scene(SceneType scene, float world_scale = 1.0f);
    void update_timing(float frametime);

    void update_all(
        float game_fov, float base_fov, float fov_scale, float zoom_factor,
        bool is_zooming, bool is_valid, ZoomMode zoom_mode,
        float depth_multiplier, SceneType scene,
        float world_scale = 1.0f,
        float convergence_mult = 1.0f
    );

    // Depth commands
    void request_calibration();         // cmd=2: Auto-calc from world_scale
    void request_depth_decrease();      // cmd=3: -3D  (reduce depth 20%)
    void request_depth_increase();      // cmd=4: +3D  (increase depth 20%)
    void request_depth_big_decrease();  // cmd=5: --3D  (reduce depth 40%)
    void request_depth_big_increase();  // cmd=6: ++3D  (increase depth 40%)
    void request_depth_huge_decrease(); // cmd=8: ---3D (reduce depth 60%)
    void request_depth_huge_increase(); // cmd=9: +++3D (increase depth 60%)

    // Read methods (VRto3D -> UEVR)
    float get_vrto3d_depth() const;
    float get_vrto3d_convergence() const;
    float get_vrto3d_fov_adjustment() const;
    bool get_is_monitor_display() const { return m_data && m_data->is_monitor_display; }

    // Leia LookAround (3DGameBridge -> UEVR via shared memory)
    void update_leia_tracking();      // per-frame: reads shared mem, smooths, writes MonitorState
    void reset_leia_calibration();    // recalibrate zero reference

    // Calculate zoom depth multiplier with adaptive curve
    float calculate_zoom_depth_multiplier(float fov_scale, float zoom_factor, DepthMode mode) const;

    // v3.0 compat overload
    float calculate_zoom_depth_multiplier(float fov_scale) const;

private:
    VRto3DBridge() = default;
    ~VRto3DBridge() { shutdown(); }
    VRto3DBridge(const VRto3DBridge&) = delete;
    VRto3DBridge& operator=(const VRto3DBridge&) = delete;

    void update_timestamp();
    void debug_log(const char* fmt, ...) const;
    void read_leia_eye_data();        // internal: shared mem -> calibrate -> smooth -> MonitorState

    HANDLE m_mapping = nullptr;
    UE3D_SharedData* m_data = nullptr;
    mutable std::mutex m_mutex;
    Config m_config{};
    std::atomic<uint32_t> m_frame_count{0};
    uint32_t m_command_seq{0};
    bool m_monitor_mode{false};

    // Leia smoothing state (not shared, bridge-local)
    float m_leia_smooth_x{0.0f};
    float m_leia_smooth_y{0.0f};
    float m_leia_smooth_z{0.0f};
    float m_leia_ref_x{0.0f};         // dynamic calibration zero reference (center-eye)
    float m_leia_ref_y{0.0f};
    float m_leia_ref_z{0.0f};
    // Per-eye Kooima calibration references + smoothing
    float m_leia_ref_lx{0.0f}, m_leia_ref_ly{0.0f};
    float m_leia_ref_rx{0.0f}, m_leia_ref_ry{0.0f};
    float m_leia_smooth_lx{0.0f}, m_leia_smooth_ly{0.0f};
    float m_leia_smooth_rx{0.0f}, m_leia_smooth_ry{0.0f};
    bool m_leia_calibrated{false};
    uint32_t m_leia_last_frame{0};     // last seen leia_frame_counter

public:
    void set_monitor_mode(bool enabled) { m_monitor_mode = enabled; }
};

} // namespace vrmod
