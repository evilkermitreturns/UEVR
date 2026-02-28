/*
 * UE3D_MonitorDetect.hpp — Physical Monitor Size Auto-Detection
 *
 * VERSION: 8.1
 * PURPOSE: Detect physical monitor dimensions from EDID data in the Windows registry.
 *          Falls back to GetDeviceCaps if EDID is unavailable.
 *
 * USAGE:
 *   auto size = ue3d::detect_monitor_size();
 *   if (size) {
 *       float vertical_inches = size->height_cm / 2.54f;
 *       // Use it
 *   }
 *
 * NOTES:
 *   - Requires setupapi.lib (Windows SDK, automatically available)
 *   - Returns the LARGEST monitor found (heuristic: gaming display is typically biggest)
 *   - EDID bytes 21-22 contain physical width/height in cm (integer precision)
 *   - Called once on first D3D present when monitor mode activates
 */

#pragma once

#include <cstdint>
#include <optional>
#include <algorithm>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <setupapi.h>

// Link setupapi.lib for SetupDi* functions
#pragma comment(lib, "setupapi.lib")

// GUID_DEVINTERFACE_MONITOR: {E6F07B5F-EE97-4a90-B076-33F57BF4EAA7}
static const GUID GUID_DEVINTERFACE_MONITOR_LOCAL =
    { 0xE6F07B5F, 0xEE97, 0x4A90, { 0xB0, 0x76, 0x33, 0xF5, 0x7B, 0xF4, 0xEA, 0xA7 } };

namespace ue3d {

struct MonitorPhysicalSize {
    float width_cm;
    float height_cm;
};

// Parse physical dimensions from a 128+ byte EDID blob.
// Bytes 21-22 are horizontal and vertical size in centimeters.
inline std::optional<MonitorPhysicalSize> parse_edid(const uint8_t* edid, size_t len) {
    if (len < 128) return std::nullopt;

    // EDID header check: 00 FF FF FF FF FF FF 00
    if (edid[0] != 0x00 || edid[1] != 0xFF || edid[6] != 0xFF || edid[7] != 0x00) {
        return std::nullopt;
    }

    uint8_t width_cm = edid[21];
    uint8_t height_cm = edid[22];

    // Sanity: must be non-zero and reasonable (5-200 cm)
    if (width_cm < 5 || width_cm > 200 || height_cm < 5 || height_cm > 200) {
        return std::nullopt;
    }

    // v15.0: Try Detailed Timing Descriptor (DTD) for mm precision first.
    // DTD starts at byte 54. Valid if pixel clock (bytes 54-55) is non-zero.
    // Bytes 66-68 contain horizontal/vertical size in mm with 12-bit precision.
    // Falls through to coarse bytes 21-22 (cm) if DTD is unavailable or invalid.
    if (len >= 72 && (edid[54] | edid[55]) != 0) {
        uint16_t h_mm = edid[66] | (static_cast<uint16_t>((edid[68] >> 4) & 0x0F) << 8);
        uint16_t v_mm = edid[67] | (static_cast<uint16_t>(edid[68] & 0x0F) << 8);
        if (h_mm > 50 && h_mm < 2000 && v_mm > 50 && v_mm < 2000) {
            return MonitorPhysicalSize{
                static_cast<float>(h_mm) / 10.0f,   // mm → cm
                static_cast<float>(v_mm) / 10.0f
            };
        }
    }

    // Fallback: coarse cm precision from bytes 21-22
    return MonitorPhysicalSize{
        static_cast<float>(width_cm),
        static_cast<float>(height_cm)
    };
}

// Primary: Enumerate display devices via SetupDI and read EDID from registry.
// Returns the largest monitor found (by area).
inline std::optional<MonitorPhysicalSize> detect_monitor_size_edid() {
    HDEVINFO dev_info = SetupDiGetClassDevsW(
        &GUID_DEVINTERFACE_MONITOR_LOCAL, nullptr, nullptr,
        DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);

    if (dev_info == INVALID_HANDLE_VALUE) {
        return std::nullopt;
    }

    std::optional<MonitorPhysicalSize> best;
    float best_area = 0.0f;

    SP_DEVICE_INTERFACE_DATA iface_data{};
    iface_data.cbSize = sizeof(SP_DEVICE_INTERFACE_DATA);

    for (DWORD i = 0; SetupDiEnumDeviceInterfaces(dev_info, nullptr,
            &GUID_DEVINTERFACE_MONITOR_LOCAL, i, &iface_data); ++i)
    {
        SP_DEVINFO_DATA dev_data{};
        dev_data.cbSize = sizeof(SP_DEVINFO_DATA);

        if (!SetupDiEnumDeviceInfo(dev_info, i, &dev_data)) continue;

        HKEY reg_key = SetupDiOpenDevRegKey(
            dev_info, &dev_data, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_READ);

        if (reg_key == INVALID_HANDLE_VALUE) continue;

        uint8_t edid_buf[256]{};
        DWORD edid_size = sizeof(edid_buf);
        DWORD reg_type = 0;

        LONG result = RegQueryValueExW(
            reg_key, L"EDID", nullptr, &reg_type, edid_buf, &edid_size);

        RegCloseKey(reg_key);

        if (result != ERROR_SUCCESS || edid_size < 128) continue;

        auto parsed = parse_edid(edid_buf, edid_size);
        if (parsed) {
            float area = parsed->width_cm * parsed->height_cm;
            if (area > best_area) {
                best_area = area;
                best = parsed;
            }
        }
    }

    SetupDiDestroyDeviceInfoList(dev_info);
    return best;
}

// Fallback: Use GetDeviceCaps to get physical size.
// Less reliable (DPI scaling can lie), but works when EDID isn't available.
inline std::optional<MonitorPhysicalSize> detect_monitor_size_gdi() {
    HDC hdc = GetDC(nullptr);
    if (!hdc) return std::nullopt;

    int width_mm = GetDeviceCaps(hdc, HORZSIZE);
    int height_mm = GetDeviceCaps(hdc, VERTSIZE);
    ReleaseDC(nullptr, hdc);

    if (width_mm < 50 || height_mm < 50 || width_mm > 2000 || height_mm > 2000) {
        return std::nullopt;
    }

    return MonitorPhysicalSize{
        static_cast<float>(width_mm) / 10.0f,  // mm → cm
        static_cast<float>(height_mm) / 10.0f
    };
}

// Main entry point: try EDID first, fall back to GDI.
inline std::optional<MonitorPhysicalSize> detect_monitor_size() {
    auto edid_result = detect_monitor_size_edid();
    if (edid_result) return edid_result;

    return detect_monitor_size_gdi();
}

} // namespace ue3d
