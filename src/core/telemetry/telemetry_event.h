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
#ifndef NIXL_SRC_CORE_TELEMETRY_TELEMETRY_EVENT_H
#define NIXL_SRC_CORE_TELEMETRY_TELEMETRY_EVENT_H

#include <array>
#include <cstdint>
#include <string_view>

#include "nixl_types.h"

constexpr char TELEMETRY_BUFFER_SIZE_VAR[] = "NIXL_TELEMETRY_BUFFER_SIZE";
constexpr char TELEMETRY_RUN_INTERVAL_VAR[] = "NIXL_TELEMETRY_RUN_INTERVAL";
constexpr char TELEMETRY_ENABLED_METRICS_VAR[] = "NIXL_TELEMETRY_ENABLED_METRICS";

constexpr inline int TELEMETRY_VERSION = 7;

/**
 * @enum nixl_telemetry_event_type_t
 * @brief Enumerates all known telemetry event types.
 */
enum class nixl_telemetry_event_type_t : uint32_t {
    AGENT_TX_BYTES = 0,
    AGENT_RX_BYTES = 1,
    AGENT_TX_REQUESTS_NUM = 2,
    AGENT_RX_REQUESTS_NUM = 3,
    AGENT_MEMORY_REGISTERED = 4,
    AGENT_MEMORY_DEREGISTERED = 5,
    AGENT_XFER_TIME = 6,
    AGENT_XFER_POST_TIME = 7,
    AGENT_ERR_NOT_POSTED = 8,
    AGENT_ERR_INVALID_PARAM = 9,
    AGENT_ERR_BACKEND = 10,
    AGENT_ERR_NOT_FOUND = 11,
    AGENT_ERR_MISMATCH = 12,
    AGENT_ERR_NOT_ALLOWED = 13,
    AGENT_ERR_REPOST_ACTIVE = 14,
    AGENT_ERR_UNKNOWN = 15,
    AGENT_ERR_NOT_SUPPORTED = 16,
    AGENT_ERR_REMOTE_DISCONNECT = 17,
    AGENT_ERR_CANCELED = 18,
    AGENT_ERR_NO_TELEMETRY = 19,
    AGENT_TRACE_PHASES_DROPPED = 20,
    // Generic backend-agnostic system/connection/wire-time telemetry (v7 prototype)
    AGENT_DEVICE_ADDR_VALID = 21,
    AGENT_DEVICE_MTU_BYTES = 22,
    AGENT_DEVICE_AFFINITY = 23,
    AGENT_CONFIG_FLAG_STATE = 24,
    AGENT_CONN_ESTABLISHED = 25,
    AGENT_CONN_PATH_MODE = 26,
    AGENT_CONN_RAIL_COUNT = 27,
    AGENT_CONN_LOCALITY_MISMATCH = 28,
    AGENT_CONN_FALLBACK = 29,
    AGENT_XFER_WIRE_TIME = 30,
    AGENT_XFER_POLL_DELAY = 31,
    AGENT_XFER_INFLIGHT = 32,
    AGENT_TELEMETRY_EVENTS_DROPPED = 33,
};

// Bitmask constants for AGENT_CONFIG_FLAG_STATE (backend-generic bits)
enum : uint64_t {
    NIXL_CFG_FLAG_RELAXED_ORDERING = 1ULL << 0,
    NIXL_CFG_FLAG_SINGLE_NET_DEVICE = 1ULL << 1,
    NIXL_CFG_FLAG_PROTO_V2 = 1ULL << 2,
    NIXL_CFG_FLAG_MEMTYPE_CACHE_OFF = 1ULL << 3,
    NIXL_CFG_FLAG_ROCE_REACH_ALL = 1ULL << 4,
};

// Standard numeric encodings for AGENT_CONN_PATH_MODE gauge
enum class nixl_conn_path_mode_t : uint64_t {
    IPC_LOCAL = 0, // Intra-node IPC / SHM / CMA zero-copy
    DIRECT_RDMA = 1, // Direct inter-node zero-copy (RDMA / EFA / GDS)
    EMULATED = 2, // Software emulation / staged host copy
    FALLBACK_TCP = 3, // Silent TCP/sockets fallback
};

// Standard numeric encodings for AGENT_DEVICE_AFFINITY gauge
enum class nixl_device_affinity_t : uint64_t {
    PIX = 0, // Same PCIe switch
    NODE = 1, // Same NUMA / PCIe root complex
    SYS = 2, // Cross-NUMA / cross-root socket
    UNKNOWN = 3, // CPU-only or unavailable
};

inline constexpr std::size_t nixl_telemetry_event_type_count =
    static_cast<std::size_t>(nixl_telemetry_event_type_t::AGENT_TELEMETRY_EVENTS_DROPPED) + 1;

// Per-event-type flag mask indexed by nixl_telemetry_event_type_t.
using nixl_telemetry_metric_mask_t = std::array<bool, nixl_telemetry_event_type_count>;

[[nodiscard]] nixl_telemetry_event_type_t
nixlTelemetryEventTypeForStatus(nixl_status_t s);

inline constexpr std::array telemetry_error_event_types = {
    nixl_telemetry_event_type_t::AGENT_ERR_NOT_POSTED,
    nixl_telemetry_event_type_t::AGENT_ERR_INVALID_PARAM,
    nixl_telemetry_event_type_t::AGENT_ERR_BACKEND,
    nixl_telemetry_event_type_t::AGENT_ERR_NOT_FOUND,
    nixl_telemetry_event_type_t::AGENT_ERR_MISMATCH,
    nixl_telemetry_event_type_t::AGENT_ERR_NOT_ALLOWED,
    nixl_telemetry_event_type_t::AGENT_ERR_REPOST_ACTIVE,
    nixl_telemetry_event_type_t::AGENT_ERR_UNKNOWN,
    nixl_telemetry_event_type_t::AGENT_ERR_NOT_SUPPORTED,
    nixl_telemetry_event_type_t::AGENT_ERR_REMOTE_DISCONNECT,
    nixl_telemetry_event_type_t::AGENT_ERR_CANCELED,
    nixl_telemetry_event_type_t::AGENT_ERR_NO_TELEMETRY,
};

inline constexpr std::array telemetry_metric_event_types = {
    nixl_telemetry_event_type_t::AGENT_TX_BYTES,
    nixl_telemetry_event_type_t::AGENT_RX_BYTES,
    nixl_telemetry_event_type_t::AGENT_TX_REQUESTS_NUM,
    nixl_telemetry_event_type_t::AGENT_RX_REQUESTS_NUM,
    nixl_telemetry_event_type_t::AGENT_MEMORY_REGISTERED,
    nixl_telemetry_event_type_t::AGENT_MEMORY_DEREGISTERED,
    nixl_telemetry_event_type_t::AGENT_XFER_TIME,
    nixl_telemetry_event_type_t::AGENT_XFER_POST_TIME,
    nixl_telemetry_event_type_t::AGENT_TRACE_PHASES_DROPPED,
    nixl_telemetry_event_type_t::AGENT_DEVICE_ADDR_VALID,
    nixl_telemetry_event_type_t::AGENT_DEVICE_MTU_BYTES,
    nixl_telemetry_event_type_t::AGENT_DEVICE_AFFINITY,
    nixl_telemetry_event_type_t::AGENT_CONFIG_FLAG_STATE,
    nixl_telemetry_event_type_t::AGENT_CONN_ESTABLISHED,
    nixl_telemetry_event_type_t::AGENT_CONN_PATH_MODE,
    nixl_telemetry_event_type_t::AGENT_CONN_RAIL_COUNT,
    nixl_telemetry_event_type_t::AGENT_CONN_LOCALITY_MISMATCH,
    nixl_telemetry_event_type_t::AGENT_CONN_FALLBACK,
    nixl_telemetry_event_type_t::AGENT_XFER_WIRE_TIME,
    nixl_telemetry_event_type_t::AGENT_XFER_POLL_DELAY,
    nixl_telemetry_event_type_t::AGENT_XFER_INFLIGHT,
    nixl_telemetry_event_type_t::AGENT_TELEMETRY_EVENTS_DROPPED,
};

static_assert(nixl_telemetry_event_type_count ==
                  telemetry_metric_event_types.size() + telemetry_error_event_types.size(),
              "AGENT_TELEMETRY_EVENTS_DROPPED must remain the last enumerator; "
              "nixl_telemetry_event_type_count is out of sync with the event-type enum");

// The error events share one family, so they have no per-type descriptor row.
inline constexpr const char *telemetry_error_family_name = "agent_errors_total";
inline constexpr const char *telemetry_error_family_help = "Cumulative error count by status";

struct nixlTelemetryMetricDescriptor {
    const char *counterName;
    const char *counterHelp;
    const char *gaugeName;
    const char *gaugeHelp;
    const char *histogramName;
    const char *histogramHelp;
};

namespace nixlEnumStrings {
[[nodiscard]] constexpr std::string_view
telemetryEventTypeStr(const nixl_telemetry_event_type_t type) noexcept {
    switch (type) {
    case nixl_telemetry_event_type_t::AGENT_TX_BYTES:
        return "agent_tx_bytes";
    case nixl_telemetry_event_type_t::AGENT_RX_BYTES:
        return "agent_rx_bytes";
    case nixl_telemetry_event_type_t::AGENT_TX_REQUESTS_NUM:
        return "agent_tx_requests_num";
    case nixl_telemetry_event_type_t::AGENT_RX_REQUESTS_NUM:
        return "agent_rx_requests_num";
    case nixl_telemetry_event_type_t::AGENT_MEMORY_REGISTERED:
        return "agent_memory_registered";
    case nixl_telemetry_event_type_t::AGENT_MEMORY_DEREGISTERED:
        return "agent_memory_deregistered";
    case nixl_telemetry_event_type_t::AGENT_XFER_TIME:
        return "agent_xfer_time";
    case nixl_telemetry_event_type_t::AGENT_XFER_POST_TIME:
        return "agent_xfer_post_time";
    case nixl_telemetry_event_type_t::AGENT_ERR_NOT_POSTED:
        return "agent_err_not_posted";
    case nixl_telemetry_event_type_t::AGENT_ERR_INVALID_PARAM:
        return "agent_err_invalid_param";
    case nixl_telemetry_event_type_t::AGENT_ERR_BACKEND:
        return "agent_err_backend";
    case nixl_telemetry_event_type_t::AGENT_ERR_NOT_FOUND:
        return "agent_err_not_found";
    case nixl_telemetry_event_type_t::AGENT_ERR_MISMATCH:
        return "agent_err_mismatch";
    case nixl_telemetry_event_type_t::AGENT_ERR_NOT_ALLOWED:
        return "agent_err_not_allowed";
    case nixl_telemetry_event_type_t::AGENT_ERR_REPOST_ACTIVE:
        return "agent_err_repost_active";
    case nixl_telemetry_event_type_t::AGENT_ERR_UNKNOWN:
        return "agent_err_unknown";
    case nixl_telemetry_event_type_t::AGENT_ERR_NOT_SUPPORTED:
        return "agent_err_not_supported";
    case nixl_telemetry_event_type_t::AGENT_ERR_REMOTE_DISCONNECT:
        return "agent_err_remote_disconnect";
    case nixl_telemetry_event_type_t::AGENT_ERR_CANCELED:
        return "agent_err_canceled";
    case nixl_telemetry_event_type_t::AGENT_ERR_NO_TELEMETRY:
        return "agent_err_no_telemetry";
    case nixl_telemetry_event_type_t::AGENT_TRACE_PHASES_DROPPED:
        return "agent_trace_phases_dropped";
    case nixl_telemetry_event_type_t::AGENT_DEVICE_ADDR_VALID:
        return "agent_device_addr_valid";
    case nixl_telemetry_event_type_t::AGENT_DEVICE_MTU_BYTES:
        return "agent_device_mtu_bytes";
    case nixl_telemetry_event_type_t::AGENT_DEVICE_AFFINITY:
        return "agent_device_affinity";
    case nixl_telemetry_event_type_t::AGENT_CONFIG_FLAG_STATE:
        return "agent_config_flag_state";
    case nixl_telemetry_event_type_t::AGENT_CONN_ESTABLISHED:
        return "agent_conn_established";
    case nixl_telemetry_event_type_t::AGENT_CONN_PATH_MODE:
        return "agent_conn_path_mode";
    case nixl_telemetry_event_type_t::AGENT_CONN_RAIL_COUNT:
        return "agent_conn_rail_count";
    case nixl_telemetry_event_type_t::AGENT_CONN_LOCALITY_MISMATCH:
        return "agent_conn_locality_mismatch";
    case nixl_telemetry_event_type_t::AGENT_CONN_FALLBACK:
        return "agent_conn_fallback";
    case nixl_telemetry_event_type_t::AGENT_XFER_WIRE_TIME:
        return "agent_xfer_wire_time";
    case nixl_telemetry_event_type_t::AGENT_XFER_POLL_DELAY:
        return "agent_xfer_poll_delay";
    case nixl_telemetry_event_type_t::AGENT_XFER_INFLIGHT:
        return "agent_xfer_inflight";
    case nixl_telemetry_event_type_t::AGENT_TELEMETRY_EVENTS_DROPPED:
        return "agent_telemetry_events_dropped";
    }
    return "unknown_event";
}

[[nodiscard]] constexpr const char *
telemetryErrorStatusLabel(const nixl_telemetry_event_type_t type) noexcept {
    switch (type) {
    case nixl_telemetry_event_type_t::AGENT_ERR_NOT_POSTED:
        return "not_posted";
    case nixl_telemetry_event_type_t::AGENT_ERR_INVALID_PARAM:
        return "invalid_param";
    case nixl_telemetry_event_type_t::AGENT_ERR_BACKEND:
        return "backend";
    case nixl_telemetry_event_type_t::AGENT_ERR_NOT_FOUND:
        return "not_found";
    case nixl_telemetry_event_type_t::AGENT_ERR_MISMATCH:
        return "mismatch";
    case nixl_telemetry_event_type_t::AGENT_ERR_NOT_ALLOWED:
        return "not_allowed";
    case nixl_telemetry_event_type_t::AGENT_ERR_REPOST_ACTIVE:
        return "repost_active";
    case nixl_telemetry_event_type_t::AGENT_ERR_UNKNOWN:
        return "unknown";
    case nixl_telemetry_event_type_t::AGENT_ERR_NOT_SUPPORTED:
        return "not_supported";
    case nixl_telemetry_event_type_t::AGENT_ERR_REMOTE_DISCONNECT:
        return "remote_disconnect";
    case nixl_telemetry_event_type_t::AGENT_ERR_CANCELED:
        return "canceled";
    case nixl_telemetry_event_type_t::AGENT_ERR_NO_TELEMETRY:
        return "no_telemetry";
    case nixl_telemetry_event_type_t::AGENT_TX_BYTES:
    case nixl_telemetry_event_type_t::AGENT_RX_BYTES:
    case nixl_telemetry_event_type_t::AGENT_TX_REQUESTS_NUM:
    case nixl_telemetry_event_type_t::AGENT_RX_REQUESTS_NUM:
    case nixl_telemetry_event_type_t::AGENT_MEMORY_REGISTERED:
    case nixl_telemetry_event_type_t::AGENT_MEMORY_DEREGISTERED:
    case nixl_telemetry_event_type_t::AGENT_XFER_TIME:
    case nixl_telemetry_event_type_t::AGENT_XFER_POST_TIME:
    case nixl_telemetry_event_type_t::AGENT_TRACE_PHASES_DROPPED:
    case nixl_telemetry_event_type_t::AGENT_DEVICE_ADDR_VALID:
    case nixl_telemetry_event_type_t::AGENT_DEVICE_MTU_BYTES:
    case nixl_telemetry_event_type_t::AGENT_DEVICE_AFFINITY:
    case nixl_telemetry_event_type_t::AGENT_CONFIG_FLAG_STATE:
    case nixl_telemetry_event_type_t::AGENT_CONN_ESTABLISHED:
    case nixl_telemetry_event_type_t::AGENT_CONN_PATH_MODE:
    case nixl_telemetry_event_type_t::AGENT_CONN_RAIL_COUNT:
    case nixl_telemetry_event_type_t::AGENT_CONN_LOCALITY_MISMATCH:
    case nixl_telemetry_event_type_t::AGENT_CONN_FALLBACK:
    case nixl_telemetry_event_type_t::AGENT_XFER_WIRE_TIME:
    case nixl_telemetry_event_type_t::AGENT_XFER_POLL_DELAY:
    case nixl_telemetry_event_type_t::AGENT_XFER_INFLIGHT:
    case nixl_telemetry_event_type_t::AGENT_TELEMETRY_EVENTS_DROPPED:
        return nullptr;
    }
    return nullptr;
}

/**
 * @brief Exporter-side Prometheus series descriptor for a telemetry event.
 *
 * The Prometheus and DOCA/CollectX exporters
 * derive their series from this single mapping, so they emit the same metric
 * definitions. A null @c counterName, @c gaugeName, or @c histogramName means the
 * event has no cumulative counter, no last-operation gauge, or no distribution
 * histogram, respectively. Error events (@c AGENT_ERR_*) and any unmapped value
 * return an all-null descriptor.
 *
 * @param type Telemetry event type.
 * @return Counter/gauge series names and HELP strings for @p type.
 */
[[nodiscard]] constexpr nixlTelemetryMetricDescriptor
telemetryMetricDescriptor(const nixl_telemetry_event_type_t type) noexcept {
    switch (type) {
    case nixl_telemetry_event_type_t::AGENT_TX_BYTES:
        return {"agent_tx_bytes_total",
                "Number of bytes sent by the agent",
                "agent_tx_last_bytes",
                "Bytes sent by the last request",
                nullptr,
                nullptr};
    case nixl_telemetry_event_type_t::AGENT_RX_BYTES:
        return {"agent_rx_bytes_total",
                "Number of bytes received by the agent",
                "agent_rx_last_bytes",
                "Bytes received by the last request",
                nullptr,
                nullptr};
    case nixl_telemetry_event_type_t::AGENT_TX_REQUESTS_NUM:
        return {"agent_tx_requests_num_total",
                "Number of requests sent by the agent",
                nullptr,
                nullptr,
                nullptr,
                nullptr};
    case nixl_telemetry_event_type_t::AGENT_RX_REQUESTS_NUM:
        return {"agent_rx_requests_num_total",
                "Number of requests received by the agent",
                nullptr,
                nullptr,
                nullptr,
                nullptr};
    case nixl_telemetry_event_type_t::AGENT_MEMORY_REGISTERED:
        return {"agent_memory_registered_total",
                "Cumulative memory registered",
                "agent_memory_registered_last_bytes",
                "Memory registered by the last operation",
                nullptr,
                nullptr};
    case nixl_telemetry_event_type_t::AGENT_MEMORY_DEREGISTERED:
        return {"agent_memory_deregistered_total",
                "Cumulative memory deregistered",
                "agent_memory_deregistered_last_bytes",
                "Memory deregistered by the last operation",
                nullptr,
                nullptr};
    case nixl_telemetry_event_type_t::AGENT_XFER_TIME:
        return {"agent_xfer_time_total",
                "Cumulative sum of transfer time from start to completion",
                "agent_xfer_time",
                "Transfer time of the last request",
                "agent_xfer_time_us",
                "Distribution of transfer time from start to completion, in microseconds"};
    case nixl_telemetry_event_type_t::AGENT_XFER_POST_TIME:
        return {"agent_xfer_post_time_total",
                "Cumulative sum of time from start to posting to the back-end",
                "agent_xfer_post_time",
                "Post time of the last request",
                "agent_xfer_post_time_us",
                "Distribution of time from start to posting to the back-end, in microseconds"};
    case nixl_telemetry_event_type_t::AGENT_TRACE_PHASES_DROPPED:
        return {"agent_trace_phases_dropped_total",
                "Cumulative backend trace phases dropped because recording them failed",
                nullptr,
                nullptr,
                nullptr,
                nullptr};
    case nixl_telemetry_event_type_t::AGENT_DEVICE_ADDR_VALID:
        return {nullptr,
                nullptr,
                "agent_device_addr_valid",
                "Device address/GID/IP validity state (1=valid, 0=missing/zero)",
                nullptr,
                nullptr};
    case nixl_telemetry_event_type_t::AGENT_DEVICE_MTU_BYTES:
        return {nullptr,
                nullptr,
                "agent_device_mtu_bytes",
                "Active link MTU in bytes for the selected network device",
                nullptr,
                nullptr};
    case nixl_telemetry_event_type_t::AGENT_DEVICE_AFFINITY:
        return {nullptr,
                nullptr,
                "agent_device_affinity",
                "GPU-to-device topology class (0=PIX, 1=NODE, 2=SYS, 3=unknown)",
                nullptr,
                nullptr};
    case nixl_telemetry_event_type_t::AGENT_CONFIG_FLAG_STATE:
        return {nullptr,
                nullptr,
                "agent_config_flag_state",
                "Packed bitmask of effective backend transport flags",
                nullptr,
                nullptr};
    case nixl_telemetry_event_type_t::AGENT_CONN_ESTABLISHED:
        return {"agent_conn_established_total",
                "Cumulative peer endpoints opened by backend",
                nullptr,
                nullptr,
                nullptr,
                nullptr};
    case nixl_telemetry_event_type_t::AGENT_CONN_PATH_MODE:
        return {nullptr,
                nullptr,
                "agent_conn_path_mode",
                "Last connection path mode (0=ipc, 1=direct, 2=emulated, 3=fallback)",
                nullptr,
                nullptr};
    case nixl_telemetry_event_type_t::AGENT_CONN_RAIL_COUNT:
        return {nullptr,
                nullptr,
                "agent_conn_rail_count",
                "Distinct network devices bound to the last endpoint",
                nullptr,
                nullptr};
    case nixl_telemetry_event_type_t::AGENT_CONN_LOCALITY_MISMATCH:
        return {"agent_conn_locality_mismatch_total",
                "Cumulative connects selecting a non-local or cross-rail device",
                nullptr,
                nullptr,
                nullptr,
                nullptr};
    case nixl_telemetry_event_type_t::AGENT_CONN_FALLBACK:
        return {"agent_conn_fallback_total",
                "Cumulative endpoints downgraded off the direct zero-copy fast path",
                nullptr,
                nullptr,
                nullptr,
                nullptr};
    case nixl_telemetry_event_type_t::AGENT_XFER_WIRE_TIME:
        return {"agent_xfer_wire_time_total",
                "Cumulative sum of backend submit-to-completion wire time, in microseconds",
                "agent_xfer_wire_time",
                "Backend wire time of the last transfer, in microseconds",
                "agent_xfer_wire_time_us",
                "Distribution of backend submit-to-completion wire time, in microseconds"};
    case nixl_telemetry_event_type_t::AGENT_XFER_POLL_DELAY:
        return {"agent_xfer_poll_delay_total",
                "Cumulative sum of completion-to-poll latency, in microseconds",
                "agent_xfer_poll_delay",
                "Completion-to-poll latency of the last transfer, in microseconds",
                "agent_xfer_poll_delay_us",
                "Distribution of completion-to-poll latency, in microseconds"};
    case nixl_telemetry_event_type_t::AGENT_XFER_INFLIGHT:
        return {nullptr,
                nullptr,
                "agent_xfer_inflight",
                "Current number of active posted transfer requests",
                nullptr,
                nullptr};
    case nixl_telemetry_event_type_t::AGENT_TELEMETRY_EVENTS_DROPPED:
        return {"agent_telemetry_events_dropped_total",
                "Cumulative telemetry events dropped at the producer-side staging queue",
                nullptr,
                nullptr,
                nullptr,
                nullptr};
    case nixl_telemetry_event_type_t::AGENT_ERR_NOT_POSTED:
    case nixl_telemetry_event_type_t::AGENT_ERR_INVALID_PARAM:
    case nixl_telemetry_event_type_t::AGENT_ERR_BACKEND:
    case nixl_telemetry_event_type_t::AGENT_ERR_NOT_FOUND:
    case nixl_telemetry_event_type_t::AGENT_ERR_MISMATCH:
    case nixl_telemetry_event_type_t::AGENT_ERR_NOT_ALLOWED:
    case nixl_telemetry_event_type_t::AGENT_ERR_REPOST_ACTIVE:
    case nixl_telemetry_event_type_t::AGENT_ERR_UNKNOWN:
    case nixl_telemetry_event_type_t::AGENT_ERR_NOT_SUPPORTED:
    case nixl_telemetry_event_type_t::AGENT_ERR_REMOTE_DISCONNECT:
    case nixl_telemetry_event_type_t::AGENT_ERR_CANCELED:
    case nixl_telemetry_event_type_t::AGENT_ERR_NO_TELEMETRY:
        break;
    }
    return {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
}
} // namespace nixlEnumStrings

/**
 * @struct nixlTelemetryEvent
 * @brief A structure to hold individual telemetry event data for cyclic buffer storage
 */
struct nixlTelemetryEvent {
    nixl_telemetry_event_type_t eventType_; // Detailed event type/identifier
    uint64_t value_; // Numeric value associated with the event

    nixlTelemetryEvent() noexcept = default;

    nixlTelemetryEvent(nixl_telemetry_event_type_t event_type, uint64_t value) noexcept
        : eventType_(event_type),
          value_(value) {}
};

#endif
