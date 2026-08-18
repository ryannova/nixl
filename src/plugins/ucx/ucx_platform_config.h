#ifndef NIXL_UCX_PLATFORM_CONFIG_H
#define NIXL_UCX_PLATFORM_CONFIG_H

#include "config.h"

#include <string>
#include <vector>
#include <unordered_map>

namespace nixl::ucx {

enum class Platform {
    UNKNOWN,
    A3_ULTRA, // a3-ultragpu-8g
    A4,       // a4-highgpu-8g
    A4X,      // a4x-highgpu-4g
    A3_MEGA,  // a3-megagpu-8g
    A3_HIGH   // a3-highgpu-8g
};

// -------------------------------------------------------------
// CONFIG FILE: Map platform identifiers to their UCX modifiers
// -------------------------------------------------------------
inline const std::unordered_map<Platform, std::vector<std::pair<std::string, std::string>>> platform_configs = {
    { Platform::A3_ULTRA, { 
        {"SINGLE_NET_DEVICE", "y"}, 
        {"MAX_RMA_RAILS", "2"} 
    }}, 
    { Platform::A4, { 
        {"SINGLE_NET_DEVICE", "y"}, 
        {"MAX_RMA_RAILS", "2"} 
    }}, 
    { Platform::A4X,      { } },
    { Platform::A3_MEGA,  { } },
    { Platform::A3_HIGH,  { } }
};

inline const std::unordered_map<std::string, Platform> platform_strings = {
    {"a3-ultragpu-8g", Platform::A3_ULTRA},
    {"a4-highgpu-8g",  Platform::A4},
    {"a4x-highgpu-4g", Platform::A4X},
    {"a3-megagpu-8g",  Platform::A3_MEGA},
    {"a3-highgpu-8g",  Platform::A3_HIGH}
};

// Detects cloud provider and platform, applying associated UCX overrides
void applyPlatformSpecificFlags(const config& config);

} // namespace nixl::ucx

#endif // NIXL_UCX_PLATFORM_CONFIG_H
