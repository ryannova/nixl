#ifndef NIXL_UCX_PLATFORM_CONFIG_H
#define NIXL_UCX_PLATFORM_CONFIG_H

#include "config.h"

#include <string>
#include <vector>
#include <unordered_map>

namespace nixl::ucx {

enum class GCPPlatform {
    UNKNOWN,
    A3_ULTRA, // a3-ultragpu-8g
    A4,       // a4-highgpu-8g
    A4X,      // a4x-highgpu-4g
    A3_MEGA,  // a3-megagpu-8g
    A3_HIGH,  // a3-highgpu-8g
    A4X_MAX,  // a4x-maxgpu-4g-metal
    A5X       // a5x
};

// -------------------------------------------------------------
// CONFIG FILE: Map platform identifiers to their UCX modifiers
// -------------------------------------------------------------
inline const std::unordered_map<GCPPlatform, std::vector<std::pair<std::string, std::string>>> gcp_platform_config = {
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
