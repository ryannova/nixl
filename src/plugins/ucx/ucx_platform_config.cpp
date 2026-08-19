#include "ucx_platform_config.h"
#include "common/nixl_log.h"

#include <fstream>
#include <unistd.h>
#include <dirent.h>
#include <string.h>
#include <ctype.h>

namespace nixl::ucx {

enum { kMaxPathLength = 512, kMaxSysfsPathLength = 560, kMaxVendorLength = 32 };

// PCI Device ID Tables
const char* const kNvidiaVendorId = "0x10de";
const char* const kA3Id = "0x2331";      // A3
const char* const kA3MId = "0x2330";     // A3 Mega
const char* const kA3UId = "0x2335";     // A3 Ultra
const char* const kA4Id = "0x2901";      // A4
const char* const kA4XId = "0x2941";     // A4X
const char* const kA4XMaxId = "0x31c2";  // A4Xmax
const char* const kA5XId = "0x3041";     // A5X

static int ReadSysfsFile(const char* path, char* out_value, size_t out_size) {
    FILE* file = fopen(path, "r");
    if (file == NULL) {
        return 0;
    }

    if (fgets(out_value, out_size, file) == NULL) {
        fclose(file);
        return 0; // Read failed (e.g., empty file).
    }
    fclose(file);

    size_t len = strlen(out_value);
    if (len > 0 && out_value[len - 1] == '\n') {
        out_value[len - 1] = '\0';
        len--;
    }

    for (size_t i = 0; i < len; i++) {
        out_value[i] = tolower((unsigned char)out_value[i]);
    }

    return (len > 0);
}

static Platform DetectPlatformViaSysfs() {
    const char* pci_path = "/sys/bus/pci/devices";
    DIR* dir = opendir(pci_path);
    if (dir == NULL) {
        return Platform::UNKNOWN;
    }

    struct dirent* entry;
    Platform detected_platform = Platform::UNKNOWN;

    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_type != DT_DIR && entry->d_type != DT_LNK) {
            continue;
        }

        char device_path[kMaxPathLength];
        snprintf(device_path, sizeof(device_path), "%s/%s", pci_path, entry->d_name);

        char vendor_path[kMaxSysfsPathLength];
        snprintf(vendor_path, sizeof(vendor_path), "%s/vendor", device_path);
        char vendor[kMaxVendorLength];
        if (!ReadSysfsFile(vendor_path, vendor, sizeof(vendor))) {
            continue;
        }

        if (strcmp(vendor, kNvidiaVendorId) != 0) {
            continue;
        }

        char device_id_path[kMaxSysfsPathLength];
        snprintf(device_id_path, sizeof(device_id_path), "%s/device", device_path);
        char device[kMaxVendorLength];
        if (!ReadSysfsFile(device_id_path, device, sizeof(device))) {
            continue;
        }

        if (strcmp(device, kA3Id) == 0) { detected_platform = Platform::A3_HIGH; break; }
        else if (strcmp(device, kA3MId) == 0) { detected_platform = Platform::A3_MEGA; break; }
        else if (strcmp(device, kA3UId) == 0) { detected_platform = Platform::A3_ULTRA; break; }
        else if (strcmp(device, kA4Id) == 0) { detected_platform = Platform::A4; break; }
        else if (strcmp(device, kA4XId) == 0) { detected_platform = Platform::A4X; break; }
        else if (strcmp(device, kA4XMaxId) == 0) { detected_platform = Platform::A4X_MAX; break; }
        else if (strcmp(device, kA5XId) == 0) { detected_platform = Platform::A5X; break; }
    }
    closedir(dir);
    return detected_platform;
}

void applyPlatformSpecificFlags(const config& config) {
    Platform plat = DetectPlatformViaSysfs();
    if (plat != Platform::UNKNOWN) {
        NIXL_DEBUG << "Detected platform via sysfs, applying UCX overrides.";
        auto it = platform_configs.find(plat);
        if (it != platform_configs.end()) {
            for (const auto& kv : it->second) {
                config.modify(kv.first, kv.second);
            }
        }
    }
}

} // namespace nixl::ucx
