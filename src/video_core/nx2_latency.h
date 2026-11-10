// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <mutex>
#include <optional>
#include <unordered_map>

#include "common/common_types.h"

// Switches for hosts where every wait for the host GPU is long: on an integrated AMD GPU (the
// Steam Deck's Van Gogh on RADV) the GPU runs ~7 ms behind the GPU thread when a CPU-side wait
// starts, against ~1 ms on desktop cards, and the threads around it idle meanwhile (GPU ~39%
// busy, no host thread at 100%). Each switch is EDEN_NX2_<NAME>: "0" off, any other value on,
// unset on only with an integrated GPU on RADV.
namespace VideoCommon::Nx2Latency {

enum class Switch : u32 {
    /// EDEN_NX2_FENCE_VIEW: a GPU semaphore acquire is met by a fence-flagged release the GPU
    /// thread already ran, without waiting for the host GPU to land it in memory.
    FenceView,
    /// EDEN_NX2_POLL_PUBLISH: the split's front publishes the words it holds before it polls
    /// suspended channels, instead of leaving them to the 1 ms valve drain.
    PollPublish,
    Count,
};

inline constexpr std::array<const char*, static_cast<size_t>(Switch::Count)> switch_names{
    "EDEN_NX2_FENCE_VIEW",
    "EDEN_NX2_POLL_PUBLISH",
};

/// Set by the Vulkan renderer once its device is known: an integrated GPU on RADV.
inline std::atomic<bool> g_integrated_radv{false};

/// -1 unset, 0 off, 1 on.
[[nodiscard]] inline int ParseSwitch(const char* value) {
    if (value == nullptr || value[0] == '\0') {
        return -1;
    }
    return value[0] == '0' ? 0 : 1;
}

/// Read once, at the first use (gameplay, after the frontend loaded nx2_switches.txt).
[[nodiscard]] inline int Forced(Switch which) {
    static const std::array<int, static_cast<size_t>(Switch::Count)> forced = [] {
        std::array<int, static_cast<size_t>(Switch::Count)> values{};
        for (size_t i = 0; i < values.size(); ++i) {
            values[i] = ParseSwitch(std::getenv(switch_names[i]));
        }
        return values;
    }();
    return forced[static_cast<size_t>(which)];
}

[[nodiscard]] inline bool Enabled(Switch which) {
    const int forced = Forced(which);
    return forced >= 0 ? forced == 1 : g_integrated_radv.load(std::memory_order_relaxed);
}

/// Fence-flagged payload releases (QueryPropertiesFlags::IsAFence) the GPU thread ran whose fence
/// has not written them to memory yet. The query cache writes such a release only once the host
/// GPU passed its fence (FenceManager::SignalFence, delayed for query reports); a GPU acquire on
/// it then waited that long although the host runs every channel on one queue, in the order the
/// GPU thread recorded them. Keyed by device word address.
class UnlandedReleases {
public:
    /// Past this, an entry whose landing never came is ignored (device loss, shutdown).
    static constexpr std::chrono::milliseconds MaxAge{250};
    static constexpr size_t PruneAt = 4096;

    static UnlandedReleases& Get() {
        static UnlandedReleases instance;
        return instance;
    }

    /// The GPU thread ran a fence-flagged release of `payload` to the word at device_address.
    /// Returns the ticket its fence operation passes to Landed.
    u64 Ran(u64 device_address, u32 payload) {
        const auto now = std::chrono::steady_clock::now();
        std::scoped_lock lock{mutex};
        if (entries.size() >= PruneAt) {
            std::erase_if(entries, [now](const auto& entry) { return now - entry.second.ran > MaxAge; });
        }
        const u64 ticket = ++next_ticket;
        entries.insert_or_assign(device_address & ~u64{3}, Entry{payload, ticket, now});
        count.store(entries.size(), std::memory_order_release);
        return ticket;
    }

    /// The release's fence operation ran: its value is in memory, or the report was dropped.
    /// A newer release to the same word (another ticket) stays.
    void Landed(u64 device_address, u64 ticket) {
        if (count.load(std::memory_order_acquire) == 0) {
            return;
        }
        std::scoped_lock lock{mutex};
        const auto it = entries.find(device_address & ~u64{3});
        if (it != entries.end() && it->second.ticket == ticket) {
            entries.erase(it);
            count.store(entries.size(), std::memory_order_release);
        }
    }

    /// The word got a write that is not a fence-flagged release (it lands at once): the newest
    /// write decides, so an older release in flight no longer tells what the word holds.
    void Overwritten(u64 device_address) {
        if (count.load(std::memory_order_acquire) == 0) {
            return;
        }
        std::scoped_lock lock{mutex};
        if (entries.erase(device_address & ~u64{3}) != 0) {
            count.store(entries.size(), std::memory_order_release);
        }
    }

    /// The payload of the newest release to the word still waiting for its fence, if any.
    [[nodiscard]] std::optional<u32> Payload(u64 device_address) const {
        if (count.load(std::memory_order_acquire) == 0) {
            return std::nullopt;
        }
        const auto now = std::chrono::steady_clock::now();
        std::scoped_lock lock{mutex};
        const auto it = entries.find(device_address & ~u64{3});
        if (it == entries.end() || now - it->second.ran > MaxAge) {
            return std::nullopt;
        }
        return it->second.payload;
    }

private:
    struct Entry {
        u32 payload;
        u64 ticket;
        std::chrono::steady_clock::time_point ran;
    };

    mutable std::mutex mutex;
    std::unordered_map<u64, Entry> entries;
    u64 next_ticket{};
    std::atomic<size_t> count{};
};

} // namespace VideoCommon::Nx2Latency
