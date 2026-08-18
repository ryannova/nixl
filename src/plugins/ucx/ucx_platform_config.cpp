#include "ucx_platform_config.h"
#include "common/nixl_log.h"

#include <fstream>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>

namespace nixl::ucx {

// Fetch machine-type via raw TCP socket (1-second timeout)
static std::string getMetadataServerResponse() {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return "";

    struct timeval tv;
    tv.tv_sec = 1; tv.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tv, sizeof(tv));

    struct sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(80);
    inet_pton(AF_INET, "169.254.169.254", &server_addr.sin_addr);

    if (connect(sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        close(sock);
        return "";
    }

    std::string request = "GET /computeMetadata/v1/instance/machine-type HTTP/1.1\r\n"
                          "Host: metadata.google.internal\r\n"
                          "Metadata-Flavor: Google\r\n"
                          "Connection: close\r\n\r\n";
    
    if (send(sock, request.c_str(), request.length(), 0) < 0) {
        close(sock);
        return "";
    }

    char buffer[1024];
    std::string response;
    int bytes_read;
    while ((bytes_read = recv(sock, buffer, sizeof(buffer) - 1, 0)) > 0) {
        buffer[bytes_read] = '\0';
        response += buffer;
    }
    close(sock);
    return response;
}

static Platform detectGcpPlatform() {
    std::string search_target;

    // Phase A: Fast Hostname Check (Catch GKE format)
    char hostname[256];
    if (gethostname(hostname, sizeof(hostname)) == 0) {
        search_target = hostname;
    }

    auto checkTarget = [&](const std::string& target) {
        for (const auto& pair : platform_strings) {
            if (target.find(pair.first) != std::string::npos) {
                return pair.second;
            }
        }
        return Platform::UNKNOWN;
    };

    Platform detected = checkTarget(search_target);
    if (detected != Platform::UNKNOWN) {
        return detected;
    }

    // Phase B: Metadata Check fallback
    return checkTarget(getMetadataServerResponse());
}

void applyPlatformSpecificFlags(const config& config) {
    std::ifstream sys_vendor_file("/sys/devices/virtual/dmi/id/sys_vendor");
    if (!sys_vendor_file.is_open()) return;

    std::string sys_vendor;
    if (std::getline(sys_vendor_file, sys_vendor)) {
        if (sys_vendor.find("Google") != std::string::npos) {
            Platform plat = detectGcpPlatform();
            if (plat != Platform::UNKNOWN) {
                NIXL_DEBUG << "Detected GCP platform, applying UCX overrides.";
                
                auto it = platform_configs.find(plat);
                if (it != platform_configs.end()) {
                    for (const auto& kv : it->second) {
                        config.modify(kv.first, kv.second); 
                    }
                }
            }
        } 
        else if (sys_vendor.find("CoreWeave") != std::string::npos) {
            // CoreWeave fallback handling out of scope for now
        }
    }
}

} // namespace nixl::ucx
