/*
 * SPDX-FileCopyrightText: Copyright (c) 2025-2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "ucx_backend.h"
#include "ucx_proxy_engine.h"
#include "ucx_thread_engine.h"
#include "ucx_thread_pool_engine.h"
#include "ucx_backend_req.h"
#include "ucx_sgl.h"
#include "common/nixl_log.h"
#include "serdes/serdes.h"
#include "common/backend.h"
#include "common/configuration.h"
#include "common/nixl_log.h"
#include "tracing/backend_trace.h"

#include <array>
#include <bit>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <set>
#include <sstream>
#include <string.h>
#include "absl/strings/str_split.h"

#include "device/device_memview.h"
#include "device/device_ops.h"
#include "device/proxy/proxy_config.h"

namespace {
[[nodiscard]] bool
sglEnabledFromConfig() {
    // TODO: Remove NIXL_UCX_SGL_ENABLE once SGL has had enough field exposure.
    const auto configured = nixl::config::getValueOptional<bool>("NIXL_UCX_SGL_ENABLE");
#ifdef HAVE_UCX_SGL_API
    const bool enabled = configured.value_or(true);
    NIXL_DEBUG << "UCX SGL offload " << (enabled ? "enabled" : "disabled");
    return enabled;
#else
    if (configured.value_or(false)) {
        NIXL_WARN << "NIXL_UCX_SGL_ENABLE is set but NIXL was built without UCX SGL support";
    }
    return false;
#endif
}

[[nodiscard]] ucp_err_handling_mode_t
errHandlingModeFromParams(const nixl_b_params_t *custom_params) {
    const auto opt = nixl::getBackendParamOptional<std::string>(
        custom_params, std::string(nixl_ucx_err_handling_param_name));
    return opt ? ucx_err_mode_from_string(*opt) : UCP_ERR_HANDLING_MODE_PEER;
}
} // namespace

/****************************************
 * Constructor/Destructor
 *****************************************/

std::unique_ptr<nixlUcxEngine>
nixlUcxEngine::create(const nixlBackendInitParams &init_params) {
    nixlUcxEngine *engine;
    const size_t num_threads =
        nixl::getBackendParamDefaulted(init_params.customParams, "num_threads", 0u);

    nixl::proxyConfig proxy_config;
    if (nixl::parseProxyConfig(init_params, proxy_config) != NIXL_SUCCESS) {
        nixl::throwRuntimeError("invalid device proxy configuration");
    }

    if (proxy_config.enabled) {
        if (num_threads > 0) {
            nixl::throwRuntimeError(
                "num_threads is not supported with device_proxy=true; use proxy_thread_count");
        }
        engine = new nixl::ucx::proxyEngine(init_params, proxy_config);
    } else if (num_threads > 0) {
        engine = new nixlUcxThreadPoolEngine(init_params, num_threads);
    } else if (init_params.enableProgTh) {
        engine = new nixlUcxThreadEngine(init_params);
    } else {
        engine = new nixlUcxEngine(init_params);
    }
    return std::unique_ptr<nixlUcxEngine>(engine);
}

nixlUcxEngine::nixlUcxEngine(const nixlBackendInitParams &init_params, size_t num_dedicated_workers)
    : nixlBackendEngine(&init_params,
                        nixl_backend_capability_t::REMOTE | nixl_backend_capability_t::LOCAL |
                            nixl_backend_capability_t::NOTIF |
                            nixl_backend_capability_t::TRACE_CONTEXT),
      errHandlingMode_(errHandlingModeFromParams(init_params.customParams)),
      sharedWorkerIndex_(1),
      sglEnabled_(sglEnabledFromConfig()),
      traceSink_(init_params.traceSink) {
    std::vector<std::string> devs; /* Empty vector */
    nixl_b_params_t *custom_params = init_params.customParams;

    if (const auto opt = nixl::getBackendParamOptional<std::string>(custom_params, "device_list")) {
        devs = absl::StrSplit(*opt, ", ");
    }

    size_t num_workers = nixl::getBackendParamDefaulted(custom_params, "num_workers", 1u);
    if (num_workers <= num_dedicated_workers) {
        num_workers = num_dedicated_workers + 1;
    }
    numSharedWorkers_ = num_workers - num_dedicated_workers;

    const size_t num_device_channels =
        nixl::getBackendParamDefaulted(custom_params, "ucx_num_device_channels", 4u);

    nixl_ucx_vram_memtype_hint_t vram_memtype_hint_policy = nixl_ucx_vram_memtype_hint_t::AUTO;
    if (const auto opt = nixl::getBackendParamOptional<std::string>(
            custom_params, std::string(nixl_ucx_vram_memtype_hint_param_name))) {
        vram_memtype_hint_policy = ucx_vram_memtype_hint_from_string(*opt);
    }

    const auto engine_config =
        nixl::getBackendParamDefaulted(custom_params, "engine_config", std::string());

    uc = std::make_unique<nixlUcxContext>(devs,
                                          init_params.enableProgTh,
                                          num_workers,
                                          init_params.syncMode,
                                          num_device_channels,
                                          engine_config,
                                          localAgent,
                                          vram_memtype_hint_policy);

    uc->warnAboutHardwareSupportMismatch();

    workers_.reserve(num_workers);
    for (size_t i = 0; i < numSharedWorkers_; i++) {
        addWorker<nixlUcxWorker>();
    }

    auto &worker = workers_.front();
    workerAddr = worker->epAddr();
    worker->regAmCallback(nixl::ucx::am_cb_op_t::NOTIF_STR, notifAmCb, this);
    emitInitTelemetry();
}

void
nixlUcxEngine::emitInitTelemetry() const {
    if (!enableTelemetry_) {
        return;
    }
    addTelemetryEvent(nixl_telemetry_event_type_t::AGENT_CONFIG_FLAG_STATE,
                      uc->getConfigFlagsBitmask());

    namespace fs = std::filesystem;
    bool found_ib = false;
    bool all_gids_valid = true;
    uint64_t min_mtu = 0;

    auto readTrimmed = [](const fs::path &p) -> std::string {
        std::ifstream f(p);
        if (!f) return {};
        std::string s;
        std::getline(f, s);
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) {
            s.pop_back();
        }
        return s;
    };

    std::error_code ec;
    const fs::path ib_root("/sys/class/infiniband");
    if (fs::exists(ib_root, ec) && fs::is_directory(ib_root, ec)) {
        for (const auto &dev : fs::directory_iterator(ib_root, ec)) {
            const fs::path port_dir = dev.path() / "ports" / "1";
            const std::string state = readTrimmed(port_dir / "state");
            if (state.find("ACTIVE") == std::string::npos) {
                continue;
            }
            found_ib = true;
            // Linux /sys/class/infiniband/<dev>/ports/1/ does not expose an `active_mtu` file on
            // standard rdma-core kernels. Resolve the RoCEv2 netdev MTU via gid_attrs/ndevs/3
            // (or device/net/<iface>/mtu) which reports jumbo Ethernet MTU (e.g. 8896) vs 1500.
            auto recordMtu = [&](const std::string &mtu_str) {
                if (mtu_str.empty()) return;
                const uint64_t mtu_val = std::strtoull(mtu_str.c_str(), nullptr, 10);
                if (mtu_val > 0 && (min_mtu == 0 || mtu_val < min_mtu)) {
                    min_mtu = mtu_val;
                }
            };
            const std::string ndev = readTrimmed(port_dir / "gid_attrs" / "ndevs" / "3");
            if (!ndev.empty()) {
                recordMtu(readTrimmed(fs::path("/sys/class/net") / ndev / "mtu"));
            } else {
                const fs::path net_dir = dev.path() / "device" / "net";
                if (fs::exists(net_dir, ec) && fs::is_directory(net_dir, ec)) {
                    for (const auto &iface : fs::directory_iterator(net_dir, ec)) {
                        recordMtu(readTrimmed(iface.path() / "mtu"));
                    }
                }
            }
            // A zeroed or missing RoCEv2 GID index 3 indicates an unbound/stale RDMA IP address.
            const std::string gid3 = readTrimmed(port_dir / "gids" / "3");
            if (gid3.empty() || gid3 == "0000:0000:0000:0000:0000:0000:0000:0000") {
                all_gids_valid = false;
                NIXL_WARN << "UCX telemetry: zero or missing RoCEv2 GID[3] on "
                          << dev.path().filename().string();
            }
        }
    }

    if (!found_ib) {
        // Fallback: inspect active non-loopback ethernet interface MTU via sysfs.
        const fs::path net_root("/sys/class/net");
        for (const auto &entry : fs::directory_iterator(net_root, ec)) {
            const std::string iface = entry.path().filename().string();
            if (iface == "lo") continue;
            const std::string oper = readTrimmed(entry.path() / "operstate");
            if (oper != "up" && oper != "unknown") continue;
            const std::string mtu_str = readTrimmed(entry.path() / "mtu");
            const uint64_t mtu_val = std::strtoull(mtu_str.c_str(), nullptr, 10);
            if (mtu_val > 0 && (min_mtu == 0 || mtu_val < min_mtu)) {
                min_mtu = mtu_val;
            }
        }
    }

    addTelemetryEvent(nixl_telemetry_event_type_t::AGENT_DEVICE_ADDR_VALID,
                      found_ib ? (all_gids_valid ? 1ULL : 0ULL) : (min_mtu > 0 ? 1ULL : 0ULL));
    if (min_mtu > 0) {
        addTelemetryEvent(nixl_telemetry_event_type_t::AGENT_DEVICE_MTU_BYTES, min_mtu);
    }
    addTelemetryEvent(
        nixl_telemetry_event_type_t::AGENT_DEVICE_AFFINITY,
        static_cast<uint64_t>(found_ib ? nixl_device_affinity_t::NODE :
                                         nixl_device_affinity_t::UNKNOWN));
}

void
nixlUcxEngine::emitEndpointTelemetry(const std::string &remote_agent, ucp_ep_h ep) const {
    if (!enableTelemetry_ || ep == nullptr) {
        return;
    }
    addTelemetryEvent(nixl_telemetry_event_type_t::AGENT_CONN_ESTABLISHED, 1);

    std::array<ucp_transport_entry_t, 16> entries{};
    ucp_ep_attr_t attr{};
    attr.field_mask = UCP_EP_ATTR_FIELD_TRANSPORTS;
    attr.transports.entries = entries.data();
    attr.transports.num_entries = entries.size();
    attr.transports.entry_size = sizeof(ucp_transport_entry_t);

    if (ucp_ep_query(ep, &attr) != UCS_OK) {
        return;
    }

    std::set<std::string> net_devices;
    bool has_rdma = false;
    bool has_tcp = false;
    bool has_cuda_ipc = false;
    bool has_shm = false;
    const unsigned count = std::min<unsigned>(attr.transports.num_entries, entries.size());
    for (unsigned i = 0; i < count; ++i) {
        const std::string tl = entries[i].transport_name ? entries[i].transport_name : "";
        const std::string dev = entries[i].device_name ? entries[i].device_name : "";
        if (tl.starts_with("rc_") || tl.starts_with("dc_") || tl.starts_with("ud_") ||
            tl.starts_with("srd")) {
            has_rdma = true;
            // Strip ":port" suffix so two QPs on mlx5_0:1 count as 1 unique rail.
            const auto colon = dev.find(':');
            net_devices.insert(colon == std::string::npos ? dev : dev.substr(0, colon));
        } else if (tl == "tcp") {
            has_tcp = true;
            if (!dev.empty() && dev != "memory") {
                net_devices.insert(dev);
            }
        } else if (tl.find("cuda_ipc") != std::string::npos) {
            has_cuda_ipc = true;
        } else if (tl == "cma" || tl == "posix" || tl == "sysv" || tl == "knem" || tl == "xpmem" ||
                   tl == "self") {
            has_shm = true;
        }
    }

    addTelemetryEvent(nixl_telemetry_event_type_t::AGENT_CONN_RAIL_COUNT, net_devices.size());

    nixl_conn_path_mode_t mode = nixl_conn_path_mode_t::IPC_LOCAL;
    if (has_rdma) {
        mode = nixl_conn_path_mode_t::DIRECT_RDMA;
    } else if (has_cuda_ipc || has_shm) {
        mode = nixl_conn_path_mode_t::IPC_LOCAL;
    } else if (has_tcp) {
        mode = nixl_conn_path_mode_t::FALLBACK_TCP;
        addTelemetryEvent(nixl_telemetry_event_type_t::AGENT_CONN_FALLBACK, 1);
    }
    lastPathMode_.store(static_cast<uint64_t>(mode), std::memory_order_relaxed);
    addTelemetryEvent(nixl_telemetry_event_type_t::AGENT_CONN_PATH_MODE,
                      static_cast<uint64_t>(mode));

    // Note: ucp_ep_query() reports all candidate device lanes wired on the endpoint
    // (e.g. all 8 NICs on a multi-rail host). Active per-GPU RMA rail count and cross-NUMA
    // mismatch are refined from the ..inf row in inspectWorkerRmaProtoTelemetry().
    lastActiveRails_.store(net_devices.size(), std::memory_order_relaxed);
    NIXL_DEBUG << "UCX telemetry connect peer=" << remote_agent
               << " rails=" << net_devices.size()
               << " path_mode=" << static_cast<uint64_t>(mode)
               << " transports=" << count;
}

void
nixlUcxEngine::inspectWorkerRmaProtoTelemetry(ucp_worker_h worker) const {
    if (!enableTelemetry_ || worker == nullptr) {
        return;
    }
    if (protoV2CudaInspected_.load(std::memory_order_relaxed)) {
        return;
    }
    // Check quickly once per worker for host-only, and keep inspecting until we see
    // the first CUDA rkey_config table if CUDA is active (rate-limited via atomic flag).
    static thread_local uint64_t check_count = 0;
    if (protoV2HostInspected_.load(std::memory_order_relaxed) && (++check_count % 64 != 1)) {
        return;
    }
    // Zero-disk-I/O in-memory proto_v2 check. Note: stock OpenUCX 1.23.0 populates
    // worker->rkey_config tables when UCX_PROTO_INFO=y or a glob (e.g. "*remote memory*"),
    // or always if openucx/ucx ucp_rkey.c:1306 passes show_all=1.
    char *buf = nullptr;
    size_t sz = 0;
    FILE *ms = ::open_memstream(&buf, &sz);
    if (ms == nullptr) {
        return;
    }
    ucp_worker_print_info(worker, ms);
    ::fclose(ms);
    if (buf == nullptr) {
        return;
    }

    auto readNuma = [](const std::string &ib_dev) -> int {
        std::ifstream f("/sys/class/infiniband/" + ib_dev + "/device/numa_node");
        int node = -1;
        if (f >> node) {
            return node;
        }
        return -1;
    };

    const std::string_view info(buf, sz);
    size_t search_pos = 0;
    bool saw_host_get = false;
    bool saw_cuda_get = false;
    bool host_emulated = false;
    bool host_emulated_tcp = false;
    bool host_zcopy_rdma = false;
    bool cuda_emulated = false;
    bool cuda_emulated_tcp = false;
    bool cuda_zcopy_rdma = false;
    std::set<std::string> cuda_rdma_devs;
    std::set<std::string> host_rdma_devs;
    std::set<std::string> cuda_tcp_devs;
    std::set<std::string> host_tcp_devs;
    int gpu_index_hint = -1;

    while (true) {
        const auto get_pos_r = info.find("ucp_get*", search_pos);
        const auto put_pos_r = info.find("ucp_put*", search_pos);
        const size_t get_pos = std::min(get_pos_r, put_pos_r);
        if (get_pos == std::string_view::npos) {
            break;
        }
        const auto header_eol = info.find('\n', get_pos);
        const auto header_line = (header_eol == std::string_view::npos) ?
            info.substr(get_pos) :
            info.substr(get_pos, header_eol - get_pos);
        // Match only true local-GPU CUDA tables ("cuda/GPU<N>"), never host-to-remote
        // control tables ("from host memory to cuda/dev[0]") which always pick mlx5_0.
        const auto gpu_tok = header_line.find("cuda/GPU");
        const bool is_cuda_table = (gpu_tok != std::string_view::npos);
        if (is_cuda_table) {
            saw_cuda_get = true;
            const int logical_gpu =
                std::atoi(std::string(header_line.substr(gpu_tok + 8, 4)).c_str());
            // Map logical CUDA index back to physical GPU index when CUDA_VISIBLE_DEVICES
            // pins the process to a numeric list (e.g. CUDA_VISIBLE_DEVICES=4 -> GPU0 is phys 4).
            int phys_gpu = logical_gpu;
            if (const char *cvd = std::getenv("CUDA_VISIBLE_DEVICES")) {
                if (cvd[0] != '\0') {
                    std::vector<std::string> toks = absl::StrSplit(cvd, ',');
                    if (logical_gpu >= 0 && static_cast<size_t>(logical_gpu) < toks.size()) {
                        const std::string &t = toks[static_cast<size_t>(logical_gpu)];
                        if (!t.empty() &&
                            std::all_of(t.begin(), t.end(), [](unsigned char c) {
                                return std::isdigit(c);
                            })) {
                            phys_gpu = std::atoi(t.c_str());
                        }
                    }
                }
            }
            gpu_index_hint = phys_gpu;
        } else {
            saw_host_get = true;
        }

        // Skip the 3 border lines (`# +---...`): under title, under column headers, bottom border.
        size_t end_pos = get_pos;
        for (int sep = 0; sep < 3 && end_pos != std::string_view::npos; ++sep) {
            end_pos = info.find("\n# +-", end_pos + 1);
        }
        const auto slice = info.substr(
            get_pos, end_pos == std::string_view::npos ? 1024 : (end_pos - get_pos));
        search_pos = (end_pos == std::string_view::npos) ? info.size() : end_pos;

        // Inspect ONLY the `..inf` row(s) — optimal rc_mlx5 tables legitimately use
        // `software emulation` for tiny 1..64B payloads and `zero-copy` for 65..inf!
        std::istringstream rows{std::string(slice)};
        std::string row;
        while (std::getline(rows, row)) {
            if (row.find("..inf") == std::string::npos) {
                continue;
            }
            bool &emul = is_cuda_table ? cuda_emulated : host_emulated;
            bool &emul_tcp = is_cuda_table ? cuda_emulated_tcp : host_emulated_tcp;
            bool &zcopy = is_cuda_table ? cuda_zcopy_rdma : host_zcopy_rdma;
            auto &devs = is_cuda_table ? cuda_rdma_devs : host_rdma_devs;
            if (row.find("software emulation") != std::string::npos) {
                if (row.find("tcp/") != std::string::npos) {
                    emul_tcp = true;
                } else {
                    emul = true;
                }
            } else if (row.find("zero-copy") != std::string::npos &&
                       row.find("mlx5_") != std::string::npos) {
                zcopy = true;
            }
            // Extract every distinct IB device name (e.g. mlx5_0, mlx5_1) on the ..inf row.
            size_t mpos = 0;
            while ((mpos = row.find("mlx5_", mpos)) != std::string::npos) {
                size_t mend = mpos + 5;
                while (mend < row.size() && std::isdigit(static_cast<unsigned char>(row[mend]))) {
                    ++mend;
                }
                if (mend > mpos + 5) {
                    devs.insert(row.substr(mpos, mend - mpos));
                }
                mpos = mend;
            }
            // Extract active TCP interface names (e.g. tcp/eth0) for accurate fallback rail count.
            auto &tdevs = is_cuda_table ? cuda_tcp_devs : host_tcp_devs;
            size_t tpos = 0;
            while ((tpos = row.find("tcp/", tpos)) != std::string::npos) {
                size_t tend = tpos + 4;
                while (tend < row.size() && row[tend] != ' ' && row[tend] != '|' &&
                       row[tend] != ',' && row[tend] != '\t' && row[tend] != '\r') {
                    ++tend;
                }
                if (tend > tpos + 4) {
                    tdevs.insert(row.substr(tpos + 4, tend - (tpos + 4)));
                }
                tpos = tend;
            }
        }
    }

    const bool use_cuda = saw_cuda_get;
    const bool inf_emulated = use_cuda ? cuda_emulated : host_emulated;
    const bool inf_emulated_tcp = use_cuda ? cuda_emulated_tcp : host_emulated_tcp;
    const bool inf_zcopy_rdma = use_cuda ? cuda_zcopy_rdma : host_zcopy_rdma;
    const auto &active_rdma_devs = use_cuda ? cuda_rdma_devs : host_rdma_devs;
    const auto &active_tcp_devs = use_cuda ? cuda_tcp_devs : host_tcp_devs;

    if (!active_rdma_devs.empty()) {
        const uint64_t rails = active_rdma_devs.size();
        const uint64_t prev_rails = lastActiveRails_.exchange(rails, std::memory_order_relaxed);
        if (prev_rails != rails) {
            addTelemetryEvent(nixl_telemetry_event_type_t::AGENT_CONN_RAIL_COUNT, rails);
        }
    } else if (inf_emulated_tcp && !active_tcp_devs.empty()) {
        const uint64_t rails = active_tcp_devs.size();
        const uint64_t prev_rails = lastActiveRails_.exchange(rails, std::memory_order_relaxed);
        if (prev_rails != rails) {
            addTelemetryEvent(nixl_telemetry_event_type_t::AGENT_CONN_RAIL_COUNT, rails);
        }
    }
    if (use_cuda && gpu_index_hint >= 0 && !active_rdma_devs.empty()) {
        const std::string expected_nic = "mlx5_" + std::to_string(gpu_index_hint);
        const bool exact_pair =
            (active_rdma_devs.size() == 1 && active_rdma_devs.count(expected_nic) == 1);
        if (!exact_pair) {
            const int chosen_numa = readNuma(*active_rdma_devs.begin());
            const int expected_numa = readNuma(expected_nic);
            const bool cross_numa =
                (chosen_numa >= 0 && expected_numa >= 0 && chosen_numa != expected_numa);
            addTelemetryEvent(nixl_telemetry_event_type_t::AGENT_DEVICE_AFFINITY,
                              static_cast<uint64_t>(cross_numa ? nixl_device_affinity_t::SYS :
                                                                 nixl_device_affinity_t::NODE));
            if (cross_numa || inf_emulated) {
                addTelemetryEvent(nixl_telemetry_event_type_t::AGENT_CONN_LOCALITY_MISMATCH, 1);
            }
        } else {
            addTelemetryEvent(nixl_telemetry_event_type_t::AGENT_DEVICE_AFFINITY,
                              static_cast<uint64_t>(nixl_device_affinity_t::PIX));
        }
    }

    if (inf_emulated) {
        NIXL_WARN << "UCX telemetry: RMA ucp_get* large-range (..inf) uses proto_v2 "
                     "'software emulation'";
        const uint64_t prev = lastPathMode_.exchange(
            static_cast<uint64_t>(nixl_conn_path_mode_t::EMULATED), std::memory_order_relaxed);
        addTelemetryEvent(nixl_telemetry_event_type_t::AGENT_CONN_PATH_MODE,
                          static_cast<uint64_t>(nixl_conn_path_mode_t::EMULATED));
        if (prev != static_cast<uint64_t>(nixl_conn_path_mode_t::EMULATED) &&
            prev != static_cast<uint64_t>(nixl_conn_path_mode_t::FALLBACK_TCP)) {
            addTelemetryEvent(nixl_telemetry_event_type_t::AGENT_CONN_FALLBACK, 1);
        }
    } else if (inf_zcopy_rdma) {
        addTelemetryEvent(nixl_telemetry_event_type_t::AGENT_CONN_PATH_MODE,
                          static_cast<uint64_t>(nixl_conn_path_mode_t::DIRECT_RDMA));
        lastPathMode_.store(static_cast<uint64_t>(nixl_conn_path_mode_t::DIRECT_RDMA),
                            std::memory_order_relaxed);
    } else if (inf_emulated_tcp) {
        NIXL_DEBUG << "UCX telemetry: RMA ucp_get* large-range uses software emulation over tcp";
    }

    if (saw_cuda_get) {
        protoV2CudaInspected_.store(true, std::memory_order_relaxed);
    }
    if (saw_host_get) {
        protoV2HostInspected_.store(true, std::memory_order_relaxed);
    }
    std::free(buf);
}

nixl_mem_list_t nixlUcxEngine::getSupportedMems () const {
    nixl_mem_list_t mems;
    mems.push_back(DRAM_SEG);
    mems.push_back(VRAM_SEG);
    return mems;
}

static std::unordered_map<const nixlUcxEngine *, size_t> &
tlsSharedWorkerMap() {
    static thread_local std::unordered_map<const nixlUcxEngine *, size_t> map;
    return map;
}

// Through parent destructor the unregister will be called.
nixlUcxEngine::~nixlUcxEngine() {
    tlsSharedWorkerMap().erase(this);
}

/****************************************
 * Connection management
*****************************************/

nixl_status_t nixlUcxEngine::getConnInfo(std::string &str) const {
    str = workerAddr;
    return NIXL_SUCCESS;
}

nixl_status_t nixlUcxEngine::connect(const std::string &remote_agent) {
    if(remote_agent == localAgent) {
        return loadRemoteConnInfo(remote_agent, workerAddr);
    }

    return (remoteConnMap.find(remote_agent) == remoteConnMap.end()) ? NIXL_ERR_NOT_FOUND :
                                                                       NIXL_SUCCESS;
}

nixl_status_t nixlUcxEngine::disconnect(const std::string &remote_agent) {
    const auto it = remoteConnMap.find(remote_agent);

    if (it == remoteConnMap.end()) {
        return NIXL_ERR_NOT_FOUND;
    }

    // thread safety?
    remoteConnMap.erase(it);
    return NIXL_SUCCESS;
}

nixl_status_t nixlUcxEngine::loadRemoteConnInfo (const std::string &remote_agent,
                                                 const std::string &remote_conn_info)
{
    size_t size = remote_conn_info.size();
    std::vector<char> addr(size);

    if(remoteConnMap.count(remote_agent)) {
        return NIXL_ERR_INVALID_PARAM;
    }

    nixlSerDes::_stringToBytes(addr.data(), remote_conn_info, size);
    std::shared_ptr<nixlUcxConnection> conn = std::make_shared<nixlUcxConnection>();
    for (const auto &uw : workers_) {
        std::unique_ptr<nixlUcxEp> ep = uw->connect(addr.data());
        if (!ep) {
            return NIXL_ERR_BACKEND;
        }
        if (conn->eps.empty() && remote_agent != localAgent) {
            emitEndpointTelemetry(remote_agent, ep->getEp());
        }
        conn->eps.push_back(std::move(ep));
    }

    remoteConnMap.insert({remote_agent, conn});

    return NIXL_SUCCESS;
}

/****************************************
 * Memory management
*****************************************/
nixl_status_t nixlUcxEngine::registerMem (const nixlBlobDesc &mem,
                                          const nixl_mem_t &nixl_mem,
                                          nixlBackendMD* &out)
{
    auto priv = std::make_unique<nixlUcxPrivateMetadata>();

    // TODO: Add nixl_mem check?
    const int ret = uc->memReg((void*) mem.addr, mem.len, priv->mem, nixl_mem);
    if (ret) {
        return NIXL_ERR_BACKEND;
    }
    priv->rkeyStr = uc->packRkey(priv->mem);

    if (priv->rkeyStr.empty()) {
        return NIXL_ERR_BACKEND;
    }
    out = priv.release();
    return NIXL_SUCCESS;
}

nixl_status_t nixlUcxEngine::deregisterMem (nixlBackendMD* meta)
{
    nixlUcxPrivateMetadata *priv = (nixlUcxPrivateMetadata*) meta;
    uc->memDereg(priv->mem);
    delete priv;
    return NIXL_SUCCESS;
}

nixl_status_t nixlUcxEngine::getPublicData (const nixlBackendMD* meta,
                                            std::string &str) const {
    const nixlUcxPrivateMetadata *priv = (nixlUcxPrivateMetadata*) meta;
    str = priv->get();
    return NIXL_SUCCESS;
}

namespace {

[[nodiscard]] std::vector<nixl::ucx::rkey>
makePublicMetadataRkeys(const ucx_connection_ptr_t &conn, const size_t count, const void *buffer) {
    std::vector<nixl::ucx::rkey> result;
    result.reserve(count);

    for (size_t i = 0; i < count; ++i) {
        result.emplace_back(*conn->getEp(i), buffer);
    }
    return result;
}

} // namespace

nixlUcxPublicMetadata::nixlUcxPublicMetadata(const ucx_connection_ptr_t &conn,
                                             std::vector<nixl::ucx::rkey> &&rkeys)
    : nixlBackendMD(false),
      conn(conn),
      rkeys_(std::move(rkeys)) {}

nixl_status_t
nixlUcxEngine::internalMDHelper (const nixl_blob_t &blob,
                                 const std::string &agent,
                                 nixlBackendMD* &output) {
    try {
        const auto it = remoteConnMap.find(agent);

        if (it == remoteConnMap.end()) {
            // TODO: err: remote connection not found
            return NIXL_ERR_NOT_FOUND;
        }
        for (size_t i = 0; i < workers_.size(); ++i) {
            const nixl_status_t status = it->second->getEp(i)->checkTxState();
            if (status != NIXL_SUCCESS) {
                return status;
            }
        }
        // nixlSerDes::_stringToBytes() was used to "unpack" blob here.
        output = new nixlUcxPublicMetadata(
            it->second, makePublicMetadataRkeys(it->second, workers_.size(), blob.data()));
        return NIXL_SUCCESS;
    }
    catch (const std::runtime_error &e) {
        NIXL_ERROR << e.what();
        return NIXL_ERR_BACKEND;
    }
}

nixl_status_t
nixlUcxEngine::loadLocalMD (nixlBackendMD* input,
                            nixlBackendMD* &output)
{
    nixlUcxPrivateMetadata* input_md = (nixlUcxPrivateMetadata*) input;
    return internalMDHelper(input_md->rkeyStr, localAgent, output);
}

// To be cleaned up
nixl_status_t nixlUcxEngine::loadRemoteMD (const nixlBlobDesc &input,
                                           const nixl_mem_t &nixl_mem,
                                           const std::string &remote_agent,
                                           nixlBackendMD* &output)
{
    return internalMDHelper(input.metaInfo, remote_agent, output);
}

nixl_status_t nixlUcxEngine::unloadMD (nixlBackendMD* input) {

    nixlUcxPublicMetadata *md = (nixlUcxPublicMetadata*) input; //typecast?
    delete md;

    return NIXL_SUCCESS;
}

/****************************************
 * Data movement
*****************************************/

size_t
nixlUcxEngine::getSharedWorkerId(const nixl_opt_b_args_t *opt_args) const noexcept {
    if (opt_args) {
        const std::optional<size_t> worker_id = getWorkerIdFromOptArgs(*opt_args);
        if (worker_id) {
            return *worker_id;
        }
    }

    auto it = tlsSharedWorkerMap().find(this);
    if (it == tlsSharedWorkerMap().end()) {
        const size_t index = sharedWorkerIndex_.fetch_add(1) % getSharedWorkersSize();
        it = tlsSharedWorkerMap().emplace(this, index).first;
        NIXL_DEBUG << "engine " << this << " bound shared worker " << index << " to thread "
                   << std::this_thread::get_id();
    }
    return it->second;
}

std::optional<size_t>
nixlUcxEngine::getWorkerIdFromOptArgs(const nixl_opt_b_args_t &opt_args) const noexcept {
    constexpr std::string_view worker_id_key = "worker_id=";
    size_t pos = opt_args.customParam.find(worker_id_key);
    if (pos == std::string::npos) {
        return std::nullopt;
    }

    try {
        size_t worker_id = std::stoull(opt_args.customParam.substr(pos + worker_id_key.length()));

        if (worker_id >= getSharedWorkersSize()) {
            NIXL_WARN << "Invalid worker_id " << worker_id << " (must be < "
                      << getSharedWorkersSize() << ")";
            return std::nullopt;
        }

        return worker_id;
    }
    catch (const std::exception &e) {
        NIXL_WARN << "Failed to parse worker_id from customParam: " << e.what();
        return std::nullopt;
    }
}

namespace {

[[nodiscard]] std::optional<nixl::trace::TraceContext>
sampledTraceContext(const nixlBackendTraceSink *sink, const nixl_opt_b_args_t *opt_args) noexcept {
    if ((sink == nullptr) || (opt_args == nullptr) || (opt_args->traceContext == nullptr) ||
        !opt_args->traceContext->sampled()) {
        return std::nullopt;
    }
    return *opt_args->traceContext;
}

void
recordPhase(nixlBackendTraceSink *sink,
            nixl_trace_phase_t phase,
            const nixlUcxBackendReqH &handle) noexcept {
    if ((sink != nullptr) && handle.traceContext) {
        sink->recordPhase(phase,
                          {},
                          nixlTime::getUs(),
                          {},
                          &*handle.traceContext,
                          handle.traceRequestId,
                          handle.traceExecution);
    }
}

} // namespace

nixl_status_t nixlUcxEngine::prepXfer (const nixl_xfer_op_t &operation,
                                       const nixl_meta_dlist_t &local,
                                       const nixl_meta_dlist_t &remote,
                                       const std::string &remote_agent,
                                       nixlBackendReqH* &handle,
                                       const nixl_opt_b_args_t* opt_args) const
{
    if (local.descCount() == 0 || remote.descCount() == 0) {
        NIXL_ERROR << "Local or remote descriptor list is empty";
        return NIXL_ERR_INVALID_PARAM;
    }

    const size_t worker_id = getSharedWorkerId(opt_args);
    /* TODO: try to get from a pool first */
    handle = new nixlUcxBackendReqH(getSharedWorker(worker_id).get());

#ifdef HAVE_UCX_SGL_API
    if (sglEnabled_ && operation == NIXL_WRITE) {
        return prepXferSgl(local, remote, handle);
    }
#endif

    return NIXL_SUCCESS;
}

nixl_status_t nixlUcxEngine::estimateXferCost (const nixl_xfer_op_t &operation,
                                               const nixl_meta_dlist_t &local,
                                               const nixl_meta_dlist_t &remote,
                                               const std::string &remote_agent,
                                               nixlBackendReqH* const &handle,
                                               std::chrono::microseconds &duration,
                                               std::chrono::microseconds &err_margin,
                                               nixl_cost_t &method,
                                               const nixl_opt_args_t* opt_args) const
{
    const auto int_handle = static_cast<nixlUcxBackendReqH *>(handle);
    const size_t worker_id = int_handle->getWorkerId();

    if (local.descCount() != remote.descCount()) {
        NIXL_ERROR << "Local (" << local.descCount() << ") and remote (" << remote.descCount()
                   << ") descriptor lists differ in size for cost estimation";
        return NIXL_ERR_MISMATCH;
    }

    duration = std::chrono::microseconds(0);
    err_margin = std::chrono::microseconds(0);

    if (local.descCount() == 0) {
        // Nothing to do, use a default value
        method = nixl_cost_t::ANALYTICAL_BACKEND;
        return NIXL_SUCCESS;
    }

    for (int i = 0; i < local.descCount(); i++) {
        const size_t lsize = local[i].len;
        const size_t rsize = remote[i].len;

        const auto lmd = static_cast<nixlUcxPrivateMetadata *>(local[i].metadataP);
        const auto rmd = static_cast<nixlUcxPublicMetadata *>(remote[i].metadataP);

        NIXL_ASSERT(lmd && rmd) << "No metadata found in descriptor lists at index " << i << " during cost estimation";
        NIXL_ASSERT(lsize == rsize) << "Local size (" << lsize << ") != Remote size (" << rsize
                                    << ") at index " << i << " during cost estimation";

        std::chrono::microseconds msg_duration;
        std::chrono::microseconds msg_err_margin;
        nixl_cost_t msg_method;
        const nixl_status_t ret = rmd->conn->getEp(worker_id)->estimateCost(
            lsize, msg_duration, msg_err_margin, msg_method);
        if (ret != NIXL_SUCCESS) {
            NIXL_ERROR << "Worker failed to estimate cost for segment " << i << " status: " << ret;
            return ret;
        }

        duration += msg_duration;
        err_margin += msg_err_margin;
        method = msg_method;
    }

    return NIXL_SUCCESS;
}

#ifdef HAVE_UCX_SGL_API
nixl_status_t
nixlUcxEngine::prepXferSgl(const nixl_meta_dlist_t &local,
                           const nixl_meta_dlist_t &remote,
                           nixlBackendReqH *handle) const {
    NIXL_ASSERT(local.descCount() == remote.descCount());

    const auto int_handle = static_cast<nixlUcxBackendReqH *>(handle);
    int_handle->sgl.emplace(local, remote, int_handle->getWorkerId(), 0, local.descCount());
    return NIXL_SUCCESS;
}

nixl_status_t
nixlUcxEngine::sendXferSgl(nixlBackendReqH *handle) const {
    const auto int_handle = static_cast<nixlUcxBackendReqH *>(handle);
    NIXL_ASSERT(int_handle->sgl);
    auto &sgl = *int_handle->sgl;

    const ucx_connection_ptr_t &conn = sgl.conn();

    auto &ep = conn->getEp(int_handle->getWorkerId());
    int_handle->init(conn, *ep);

    nixlUcxReq req;
    const nixl_status_t post_ret = sgl.post(*ep, req);
    if (int_handle->append(post_ret, req) != NIXL_SUCCESS) {
        return post_ret;
    }

    return int_handle->flush();
}
#endif

nixl_status_t
nixlUcxEngine::sendXferRange(const nixl_xfer_op_t &operation,
                             const nixl_meta_dlist_t &local,
                             const nixl_meta_dlist_t &remote,
                             const std::string &remote_agent,
                             nixlBackendReqH *handle,
                             size_t start_idx,
                             size_t end_idx) const {
    const auto int_handle = static_cast<nixlUcxBackendReqH *>(handle);
    const size_t worker_id = int_handle->getWorkerId();

    if (operation != NIXL_WRITE && operation != NIXL_READ) {
        return NIXL_ERR_INVALID_PARAM;
    }

#ifdef HAVE_UCX_SGL_API
    if (sglEnabled_ && operation == NIXL_WRITE) {
        if (!int_handle->sgl) {
            int_handle->sgl.emplace(local, remote, worker_id, start_idx, end_idx);
        }
        return sendXferSgl(handle);
    }
#endif

    const ucx_connection_ptr_t &conn =
        static_cast<nixlUcxPublicMetadata *>(remote[start_idx].metadataP)->conn;
    auto &ep = conn->getEp(worker_id);
    int_handle->init(conn, *ep);

    nixl_status_t status = NIXL_SUCCESS;
    nixlUcxReq pending_req = nullptr;

    for (size_t i = start_idx; i < end_idx; ++i) {
        void *laddr = (void *)local[i].addr;
        size_t lsize = local[i].len;
        uint64_t raddr = static_cast<uint64_t>(remote[i].addr);
        NIXL_ASSERT(lsize == remote[i].len);

        const auto lmd = static_cast<nixlUcxPrivateMetadata *>(local[i].metadataP);
        const auto rmd = static_cast<nixlUcxPublicMetadata *>(remote[i].metadataP);
        NIXL_ASSERT(rmd->conn->getEp(worker_id).get() == ep.get());

        nixlUcxReq req;
        const nixl_status_t ret = operation == NIXL_READ ?
            ep->read(raddr, rmd->getRkey(worker_id), laddr, lmd->mem, lsize, req) :
            ep->write(laddr, lmd->mem, raddr, rmd->getRkey(worker_id), lsize, req);

        if (ret == NIXL_IN_PROG) {
            if (pending_req != nullptr) [[likely]] {
                ucp_request_free(pending_req);
            }
            pending_req = req;
        } else if (ret != NIXL_SUCCESS) {
            status = ret;
            if (pending_req != nullptr) {
                ucp_request_free(pending_req);
                pending_req = nullptr;
            }
            break;
        }
    }

    if (status == NIXL_SUCCESS && pending_req) {
        status = NIXL_IN_PROG;
    }

    if (int_handle->append(status, pending_req) != NIXL_SUCCESS) {
        return status;
    }

    /*
     * Flush keeps int_handle non-empty until the operation is actually
     * completed, which can happen after local requests completion.
     */
    return int_handle->flush();
}

nixl_status_t
nixlUcxEngine::postXfer(const nixl_xfer_op_t &operation,
                        const nixl_meta_dlist_t &local,
                        const nixl_meta_dlist_t &remote,
                        const std::string &remote_agent,
                        nixlBackendReqH *&handle,
                        const nixl_opt_b_args_t *opt_args) const {
    const size_t lcnt = local.descCount();
    const size_t rcnt = remote.descCount();
    const auto int_handle = static_cast<nixlUcxBackendReqH *>(handle);
    nixl_status_t ret;

    if (lcnt != rcnt) {
        NIXL_ERROR << "Local (" << lcnt << ") and remote (" << rcnt
                   << ") descriptor lists differ in size";
        return NIXL_ERR_INVALID_PARAM;
    }

    int_handle->traceContext = sampledTraceContext(traceSink_, opt_args);
    int_handle->traceRequestId = int_handle->traceContext ? opt_args->traceRequestId : 0;
    int_handle->traceExecution = int_handle->traceContext ? opt_args->traceExecution : 0;
    recordPhase(traceSink_, nixl_trace_phase_t::SUBMIT, *int_handle);

    // TODO: assert that handle is empty/completed, as we can't post request before completion

    // TODO: build the notification lazily, after the data is posted (slightly faster).
    // Revisit with the serdes optimization and UCX memory pool for notifications.
    std::string notif;
    if (opt_args && opt_args->hasNotif) {
        notif = buildNotif(opt_args->notifMsg,
                           opt_args->traceContext,
                           int_handle->traceRequestId,
                           int_handle->traceExecution);
    }
    int_handle->setNotif(std::move(notif));
    if (enableTelemetry_) {
        int_handle->enableWireTiming(static_cast<uint64_t>(nixlTime::getUs()));
    }

    ret = sendXferRange(operation, local, remote, remote_agent, handle, 0, lcnt);
    if (ret != NIXL_SUCCESS) {
        return ret;
    }

    recordPhase(traceSink_, nixl_trace_phase_t::WIRE_SUBMITTED, *int_handle);
    const nixl_status_t post_status =
        int_handle->hasPendingRequests() ? NIXL_IN_PROG : int_handle->status();
    if (enableTelemetry_ && post_status == NIXL_SUCCESS) {
        uint64_t wire_us = 0, poll_delay_us = 0;
        if (int_handle->takeWireTiming(
                static_cast<uint64_t>(nixlTime::getUs()), wire_us, poll_delay_us)) {
            addTelemetryEvent(nixl_telemetry_event_type_t::AGENT_XFER_WIRE_TIME, wire_us);
            addTelemetryEvent(nixl_telemetry_event_type_t::AGENT_XFER_POLL_DELAY, poll_delay_us);
        }
        inspectWorkerRmaProtoTelemetry(int_handle->getWorker()->get());
    }
    return post_status;
}

nixl_status_t nixlUcxEngine::checkXfer (nixlBackendReqH* handle) const
{
    const auto int_handle = static_cast<nixlUcxBackendReqH *>(handle);
    const nixl_status_t st = int_handle->status();
    if (enableTelemetry_ && st == NIXL_SUCCESS) {
        uint64_t wire_us = 0, poll_delay_us = 0;
        if (int_handle->takeWireTiming(
                static_cast<uint64_t>(nixlTime::getUs()), wire_us, poll_delay_us)) {
            addTelemetryEvent(nixl_telemetry_event_type_t::AGENT_XFER_WIRE_TIME, wire_us);
            addTelemetryEvent(nixl_telemetry_event_type_t::AGENT_XFER_POLL_DELAY, poll_delay_us);
        }
        inspectWorkerRmaProtoTelemetry(int_handle->getWorker()->get());
    }
    return st;
}

nixl_status_t nixlUcxEngine::releaseReqH(nixlBackendReqH* handle) const
{
    const auto int_handle = static_cast<nixlUcxBackendReqH *>(handle);
    int_handle->release();

    /* TODO: return to a pool instead. */
    delete int_handle;

    return NIXL_SUCCESS;
}

unsigned
nixlUcxEngine::progress() {
    // TODO: add listen for connection handling if necessary
    unsigned ret = 0;
    for (const auto &uw : getSharedWorkers()) {
        ret += uw->progress();
    }
    return ret;
}

void
nixlUcxEngine::progressLoop() {
    while (progress() != 0)
        ;
}

/****************************************
 * Notifications
*****************************************/

namespace {

static_assert(std::endian::native == std::endian::little);

struct requestTag {
    std::uint64_t requestId;
    std::uint64_t execution;
};

void
addRequestTag(nixlSerDes &ser_des, std::uint64_t request_id, std::uint64_t execution) {
    const requestTag tag{request_id, execution};
    ser_des.addBuf("treq", &tag, sizeof(tag));
}

void
readRequestTag(nixlSerDes &ser_des, nixl::ucx::notifMessage &notif) {
    requestTag tag;
    if ((ser_des.peekBufLen("treq") == sizeof(tag)) &&
        (ser_des.getBuf("treq", &tag, sizeof(tag)) == NIXL_SUCCESS) && (tag.requestId != 0)) {
        notif.traceRequestId = tag.requestId;
        notif.traceExecution = tag.execution;
    }
}

} // namespace

std::string
nixl::ucx::serializeNotif(const std::string &agent,
                          const std::string &msg,
                          const nixl::trace::TraceContext *trace_context,
                          std::uint64_t request_id,
                          std::uint64_t execution) {
    nixlSerDes ser_des;

    ser_des.addStr("name", agent);
    ser_des.addStr("msg", msg);
    if ((trace_context != nullptr) && trace_context->sampled()) {
        std::array<std::uint8_t, nixl::trace::traceContextWireSize> record;
        if (const auto size = nixl::trace::encodeTraceContext(*trace_context, record)) {
            ser_des.addBuf("tctx", record.data(), *size);
            if (request_id != 0) {
                addRequestTag(ser_des, request_id, execution);
            }
        }
    }
    return std::move(ser_des).exportMoveStr();
}

nixl::ucx::notifMessage
nixl::ucx::deserializeNotif(const std::string &buffer, bool with_trace_context) {
    nixlSerDes ser_des;
    ser_des.importStr(buffer);

    notifMessage notif;
    notif.agent = ser_des.getStr("name");
    notif.msg = ser_des.getStr("msg");
    if (with_trace_context && (ser_des.peekBufLen("tctx") == nixl::trace::traceContextWireSize)) {
        std::array<std::uint8_t, nixl::trace::traceContextWireSize> record;
        nixl::trace::TraceContext context;
        if ((ser_des.getBuf("tctx", record.data(), record.size()) == NIXL_SUCCESS) &&
            (nixl::trace::decodeTraceContext(record, context) ==
             nixl::trace::WireDecodeResult::Ok)) {
            notif.traceContext = context;
            readRequestTag(ser_des, notif);
        }
    }
    return notif;
}

std::string
nixlUcxEngine::buildNotif(const std::string &msg,
                          const nixl::trace::TraceContext *trace_context,
                          std::uint64_t request_id,
                          std::uint64_t execution) const {
    // TODO: replace with mpool for performance
    return nixl::ucx::serializeNotif(localAgent, msg, trace_context, request_id, execution);
}

nixl_status_t
nixlUcxEngine::notifSendPriv(const std::string &remote_agent,
                             const std::string &msg,
                             const nixlUcxEp &ep,
                             nixlUcxReq *req) const {
    return ep.sendAm(
        nixl::ucx::am_cb_op_t::NOTIF_STR, buildNotif(msg, nullptr), UCP_AM_SEND_FLAG_EAGER, req);
}

ucx_connection_ptr_t
nixlUcxEngine::getConnection(const std::string &remote_agent) const {
    const auto it = remoteConnMap.find(remote_agent);
    return (it != remoteConnMap.end()) ? it->second : nullptr;
}

void
nixlUcxEngine::appendNotif(std::string &&remote_name, std::string &&msg) {
    // In the "no progress thread" case the lock in nixlAgent is sufficient.
    notifList_.emplace_back(std::move(remote_name), std::move(msg));
}

ucs_status_t
nixlUcxEngine::notifAmCb(void *arg, const void *header,
                         size_t header_length, void *data,
                         size_t length,
                         const ucp_am_recv_param_t *param)
{
    std::string ser_str( (char*) data, length);
    nixlUcxEngine* engine = (nixlUcxEngine*) arg;

    // send_am should be forcing EAGER protocol
    NIXL_ASSERT(!(param->recv_attr & UCP_AM_RECV_ATTR_FLAG_RNDV));
    NIXL_ASSERT(header_length == 0) << "header_length " << header_length;

    nixl::ucx::notifMessage notif =
        nixl::ucx::deserializeNotif(ser_str, engine->traceSink_ != nullptr);
    if ((engine->traceSink_ != nullptr) && notif.traceContext && notif.traceContext->sampled()) {
        const std::array attrs{nixlBackendTraceAttr{"nixl.remote_agent", notif.agent}};
        engine->traceSink_->recordPhase(nixl_trace_phase_t::NOTIF_RECEIVED,
                                        {},
                                        nixlTime::getUs(),
                                        attrs,
                                        &*notif.traceContext,
                                        notif.traceRequestId,
                                        notif.traceExecution);
    }

    engine->appendNotif(std::move(notif.agent), std::move(notif.msg));
    return UCS_OK;
}

nixl_status_t
nixlUcxEngine::getNotifs(notif_list_t &notif_list) {
    if (!notif_list.empty()) {
        return NIXL_ERR_INVALID_PARAM;
    }

    progressLoop();

    // In the "no progress thread" case the lock in nixlAgent is sufficient.
    notifList_.swap(notif_list);
    return NIXL_SUCCESS;
}

nixl_status_t
nixlUcxEngine::genNotif(const std::string &remote_agent, const std::string &msg) const {
    const auto conn = getConnection(remote_agent);
    if (!conn) {
        return NIXL_ERR_NOT_FOUND;
    }

    const nixlUcxEp &ep = *conn->getEp(getSharedWorkerId());
    const nixl_status_t ret = notifSendPriv(remote_agent, msg, ep);
    if (ret == NIXL_IN_PROG) {
        return NIXL_SUCCESS;
    }
    return ret;
}

nixl_status_t
nixlUcxEngine::prepMemView(const nixl_remote_meta_dlist_t &dlist,
                           nixlMemViewH &mvh,
                           const nixl_opt_b_args_t *opt_args) const {
    return prepMemViewImpl(dlist, mvh, opt_args, "remote");
}

nixl_status_t
nixlUcxEngine::prepMemView(const nixl_meta_dlist_t &dlist,
                           nixlMemViewH &mvh,
                           const nixl_opt_b_args_t *opt_args) const {
    return prepMemViewImpl(dlist, mvh, opt_args, "local");
}

template<typename DlistT>
nixl_status_t
nixlUcxEngine::prepMemViewImpl(const DlistT &dlist,
                               nixlMemViewH &mvh,
                               const nixl_opt_b_args_t *opt_args,
                               const char *kind) const {
    nixl::deviceOps *ops = nixl::getDeviceOps();
    if (ops == nullptr) {
        NIXL_ERROR << "No device operations implementation is loaded";
        return NIXL_ERR_NOT_SUPPORTED;
    }

    nixlMemViewH backend_mvh = nullptr;
    const size_t worker_id = getSharedWorkerId(opt_args);
    try {
        backend_mvh = nixl::ucx::createMemList(dlist, *getSharedWorker(worker_id));
    }
    catch (const std::exception &e) {
        NIXL_ERROR << "Failed to prepare " << kind << " memory view: " << e.what();
        return NIXL_ERR_BACKEND;
    }

    // Device code takes the backend handle out of a tagged wrapper: the tag picks the
    // implementation at run time, or checks a mode a caller fixed at compile time.
    const nixl_status_t status =
        nixl::deviceMemViewAllocate(*ops, nixl_device_exec_mode_t::UCX_DIRECT, backend_mvh, mvh);
    if (status != NIXL_SUCCESS) {
        nixl::ucx::releaseMemList(backend_mvh);
    }
    return status;
}

void
nixlUcxEngine::releaseMemView(nixlMemViewH mem_view) const {
    if (mem_view == nullptr) {
        return;
    }

    // prepMemView allocated the wrapper through the same process-wide instance.
    nixl::deviceOps &ops = *nixl::getDeviceOps();
    nixlMemViewH backend_mvh = nullptr;
    if (nixl::deviceMemViewGetBackend(ops, mem_view, backend_mvh) != NIXL_SUCCESS) {
        NIXL_ERROR << "Failed to read device memview wrapper for handle " << mem_view;
        nixl::deviceMemViewFree(ops, mem_view);
        return;
    }

    nixl::ucx::releaseMemList(backend_mvh);
    nixl::deviceMemViewFree(ops, mem_view);
}
