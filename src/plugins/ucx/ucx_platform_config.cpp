#include "ucx_platform_config.h"
#include "common/nixl_log.h"

#include <fstream>
#include <unistd.h>
#include <dirent.h>
#include <string.h>
#include <ctype.h>
#include <cstdlib>

namespace nixl::ucx {

enum { kMaxPathLength = 512, kMaxSysfsPathLength = 560, kMaxVendorLength = 32 };

// Helper function to read a single whitespace-delimited string from a sysfs file,
// converting it entirely to lowercase.
// Returns 1 on success, 0 on failure.
static int ReadSysfsFile(const char* path, char* out_value, size_t out_size) {
    FILE* file = fopen(path, "r");
    if (file == NULL) {
        return 0;
    }

    // Use fgets for safe reading, respecting the buffer size.
    if (fgets(out_value, out_size, file) == NULL) {
        fclose(file);
        return 0; // Read failed (e.g., empty file).
    }
    fclose(file);

    // Remove trailing newline character, if present.
    size_t len = strlen(out_value);
    if (len > 0 && out_value[len - 1] == '\n') {
        out_value[len - 1] = '\0';
        len--;
    }

    // Convert to lowercase.
    for (size_t i = 0; i < len; i++) {
        out_value[i] = tolower((unsigned char)out_value[i]);
    }

    return (len > 0);
}

// Scans /sys/bus/pci/devices, finds the first NVIDIA GPU, and determines the
// platform based on its PCI device ID matching against known GCP platforms.
static GCPPlatform DetectPlatformViaSysfs() {
    const char* pci_path = "/sys/bus/pci/devices";
    DIR* dir = opendir(pci_path);
    if (dir == NULL) {
        return GCPPlatform::UNKNOWN; // Fail silently
    }

    struct dirent* entry;
    GCPPlatform detected_platform = GCPPlatform::UNKNOWN;

    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_type != DT_DIR && entry->d_type != DT_LNK) {
            continue;
        }

        char device_path[kMaxPathLength];
        snprintf(device_path, sizeof(device_path), "%s/%s", pci_path, entry->d_name);

        // Check Vendor ID
        char vendor_path[kMaxSysfsPathLength];
        snprintf(vendor_path, sizeof(vendor_path), "%s/vendor", device_path);
        char vendor[kMaxVendorLength];
        if (!ReadSysfsFile(vendor_path, vendor, sizeof(vendor))) {
            continue;
        }

        // Check if it is an Nvidia device
        int vendor_id = strtol(vendor, NULL, 16);
        if (vendor_id != GCPPlatformId::kNvidiaVendorId) {
            continue;
        }

        // Check Device ID
        char device_id_path[kMaxSysfsPathLength];
        snprintf(device_id_path, sizeof(device_id_path), "%s/device", device_path);
        char device[kMaxVendorLength];
        if (!ReadSysfsFile(device_id_path, device, sizeof(device))) {
            continue;
        }

        // Determine specific platform based on known PCI tables
        int device_id = strtol(device, NULL, 16);
        switch (device_id) {
            case GCPPlatformId::kA3Id:      detected_platform = GCPPlatform::A3_HIGH; break;
            case GCPPlatformId::kA3MId:     detected_platform = GCPPlatform::A3_MEGA; break;
            case GCPPlatformId::kA3UId:     detected_platform = GCPPlatform::A3_ULTRA; break;
            case GCPPlatformId::kA4Id:      detected_platform = GCPPlatform::A4; break;
            case GCPPlatformId::kA4XId:     detected_platform = GCPPlatform::A4X; break;
            case GCPPlatformId::kA4XMaxId:  detected_platform = GCPPlatform::A4X_MAX; break;
            case GCPPlatformId::kA5XId:     detected_platform = GCPPlatform::A5X; break;
            default: break;
        }

        if (detected_platform != GCPPlatform::UNKNOWN) {
            break;
        }
    }
    closedir(dir);
    return detected_platform;
}

// Entrypoint for applying platform-specific environment/config overrides:
// First, it verifies the host is running on Google architecture using the DMI sys_vendor.
// Then, it detects the specific accelerator platform via the system's PCI devices.
// Finally, it applies the appropriate configuration flags if a known platform is found.
void applyPlatformSpecificFlags(const config& config) {
    std::ifstream sys_vendor_file("/sys/devices/virtual/dmi/id/sys_vendor");
    if (!sys_vendor_file.is_open()) return;

    std::string sys_vendor;
    if (std::getline(sys_vendor_file, sys_vendor)) {
        if (sys_vendor.find("Google") != std::string::npos) {
            GCPPlatform plat = DetectPlatformViaSysfs();
            if (plat != GCPPlatform::UNKNOWN) {
                NIXL_DEBUG << "Detected GCP platform via sysfs, applying UCX overrides.";
                auto it = gcp_platform_config.find(plat);
                if (it != gcp_platform_config.end()) {
                    for (const auto& kv : it->second) {
                        config.modify(kv.first, kv.second);
                    }
                }
            }
        }
    }
}

} // namespace nixl::ucx
