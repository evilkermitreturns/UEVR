// UE3D_Bridge.hpp - Shared memory bridge to VRto3D (protocol v4.0)
#pragma once

#include <Windows.h>
#include <cstdint>
#include <atomic>
#include <mutex>
#include <string>

// Protocol struct (canonical in VRto3DLib, local copy for UEVR)
#include "ue3d_protocol.h"

namespace vrmod {

// Protocol constants
constexpr uint32_t VRTO3D_BRIDGE_MAGIC = UE3D_MAGIC;
constexpr uint32_t VRTO3D_BRIDGE_VERSION = UE3D_VERSION;
constexpr const char* VRTO3D_SHARED_MEM_NAME = UE3D_SHMEM_NAME;

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

class UE3D_Bridge {
public:
    // Singleton
    static UE3D_Bridge& get() {
        static UE3D_Bridge instance;
        return instance;
    }

    // Configuration
    struct Config {
        bool enabled = true;
        bool use_fov_compensation = true;
        bool debug_logging = false;
    };

    Config& config() { return m_config; }
    const Config& config() const { return m_config; }

    // Lifecycle
    bool init();
    void shutdown();
    bool is_initialized() const { return m_data != nullptr; }

    // Connection status
    bool is_vrto3d_connected() const;
    bool is_vrto3d_profile_loaded() const;

    // Write methods (UEVR -> VRto3D)

    void refresh_timestamp();  // Keep-alive: prevents staleness timeout

    void update_all(
        float game_fov, float base_fov, float fov_scale, float zoom_factor,
        bool is_zooming, bool is_valid, ZoomMode zoom_mode,
        float world_scale = 1.0f
    );

    // Depth commands
    void request_calibration();         // cmd=2: Auto-calc from world_scale
    void request_depth_decrease();      // cmd=3: -3D  (reduce depth 20%)
    void request_depth_increase();      // cmd=4: +3D  (increase depth 20%)
    void request_depth_big_decrease();  // cmd=5: --3D  (reduce depth 40%)
    void request_depth_big_increase();  // cmd=6: ++3D  (increase depth 40%)

    // Read methods (VRto3D -> UEVR)
    float get_vrto3d_depth() const;
    bool get_is_monitor_display() const { return m_data && m_data->is_monitor_display; }

    // Leia LookAround (3DGameBridge -> UEVR via shared memory)
    void update_leia_tracking();      // per-frame: reads shared mem, smooths, writes MonitorState
    void reset_leia_calibration();    // recalibrate zero reference

private:
    UE3D_Bridge() = default;
    ~UE3D_Bridge() { shutdown(); }
    UE3D_Bridge(const UE3D_Bridge&) = delete;
    UE3D_Bridge& operator=(const UE3D_Bridge&) = delete;

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
