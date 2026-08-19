#ifndef NIXL_UCX_PLATFORM_CONFIG_H
#define NIXL_UCX_PLATFORM_CONFIG_H

#include "config.h"

#include <string>
#include <vector>
#include <unordered_map>

namespace nixl::ucx {

struct GCPPlatform {
    enum Type {
        UNKNOWN,
        A3_ULTRA, // a3-ultragpu-8g
        A4,       // a4-highgpu-8g
        A4X,      // a4x-highgpu-4g
        A3_MEGA,  // a3-megagpu-8g
        A3_HIGH,  // a3-highgpu-8g
        A4X_MAX,  // a4x-maxgpu-4g-metal
        A5X       // a5x
    };

    // PCI Device ID Tables
    static constexpr const char* kNvidiaVendorId = "0x10de";
    static constexpr const char* kA3Id = "0x2331";      // A3
    static constexpr const char* kA3MId = "0x2330";     // A3 Mega
    static constexpr const char* kA3UId = "0x2335";     // A3 Ultra
    static constexpr const char* kA4Id = "0x2901";      // A4
    static constexpr const char* kA4XId = "0x2941";     // A4X
    static constexpr const char* kA4XMaxId = "0x31c2";  // A4Xmax
    static constexpr const char* kA5XId = "0x3041";     // A5X
};

// -------------------------------------------------------------
// CONFIG FILE: Map platform identifiers to their UCX modifiers
// -------------------------------------------------------------
inline const std::unordered_map<GCPPlatform::Type, std::vector<std::pair<std::string, std::string>>> gcp_platform_config = {
    { GCPPlatform::A3_ULTRA, { 
        {"SINGLE_NET_DEVICE", "y"}, 
        {"MAX_RMA_RAILS", "2"} 
    }}, 
    { GCPPlatform::A4, { 
        {"SINGLE_NET_DEVICE", "y"}, 
        {"MAX_RMA_RAILS", "2"} 
    }}, 
    { GCPPlatform::A4X,      { } },
    { GCPPlatform::A3_MEGA,  { } },
    { GCPPlatform::A3_HIGH,  { } },
    { GCPPlatform::A4X_MAX,  { } },
    { GCPPlatform::A5X,      { } }
};


// Detects cloud provider and platform, applying associated UCX overrides
void applyPlatformSpecificFlags(const config& config);

} // namespace nixl::ucx

#endif // NIXL_UCX_PLATFORM_CONFIG_H
