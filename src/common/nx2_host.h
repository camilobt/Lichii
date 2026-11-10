// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <cstring>

#if defined(__linux__) && !defined(__ANDROID__)
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

#include "common/common_types.h"
#include "common/logging.h"

// Hosts where the host GPU is far slower than the CPU side expects and shares four cores with it:
// an integrated AMD GPU on RADV (Steam Deck, handheld APUs). Switch 2 titles sync the guest CPU,
// the two GPU threads and the host GPU many times a frame, and there every wait for the host GPU
// lasts milliseconds. The switches below default on only on such a host, and each one can be
// forced with its variable (1 on, 0 off) for A/B runs.
namespace Common::Nx2Host {

/// Set once by Vulkan::Device, before the renderer's caches exist.
inline std::atomic<bool> g_integrated_radv{false};

[[nodiscard]] inline bool IsIntegratedRadv() {
    return g_integrated_radv.load(std::memory_order_relaxed);
}

/// An on/off variable: 1 when set to anything but 0, 0 when it starts with '0', -1 when unset.
[[nodiscard]] inline int EnvSwitch(const char* name) {
    const char* const value = std::getenv(name);
    if (value == nullptr || value[0] == '\0') {
        return -1;
    }
    return value[0] == '0' ? 0 : 1;
}

/// What a switch read with EnvSwitch decides: its value when set, else on for integrated RADV.
[[nodiscard]] inline bool OnForIntegratedRadv(int forced) {
    return forced >= 0 ? forced == 1 : IsIntegratedRadv();
}

namespace Detail {

#if defined(__linux__) && !defined(__ANDROID__)
/// The kernel's struct sched_attr up to its utilization clamps (SCHED_ATTR_SIZE_VER1). Declared
/// here: glibc only has it, and sched_setattr, since 2.41.
struct SchedAttr {
    u32 size;
    u32 sched_policy;
    u64 sched_flags;
    s32 sched_nice;
    u32 sched_priority;
    u64 sched_runtime;
    u64 sched_deadline;
    u64 sched_period;
    u32 sched_util_min;
    u32 sched_util_max;
};
static_assert(sizeof(SchedAttr) == 56);

inline void ApplyThreadBoost() {
    // Timed waits end up to 50 us late with the default slack; the handoffs between these threads
    // are shorter than that.
    const bool slack = prctl(PR_SET_TIMERSLACK, 1UL, 0, 0, 0) == 0;
    // Each of these threads works in short bursts between waits for another one, so none of them
    // looks busy to the scheduler, and schedutil keeps their cores at a low frequency. A minimum
    // utilization clamp asks for the highest frequency while the thread runs, and costs nothing
    // while it sleeps. Needs CONFIG_UCLAMP_TASK and the schedutil governor; no privileges.
    bool clamp = false;
    int clamp_error = ENOSYS;
#ifdef SYS_sched_setattr
    constexpr u64 KeepPolicy = 0x08;
    constexpr u64 KeepParams = 0x10;
    constexpr u64 UtilClampMin = 0x20;
    SchedAttr attr{};
    attr.size = sizeof(SchedAttr);
    attr.sched_flags = KeepPolicy | KeepParams | UtilClampMin;
    attr.sched_util_min = 1024;
    clamp = syscall(SYS_sched_setattr, 0, &attr, 0) == 0;
    clamp_error = clamp ? 0 : errno;
#endif
    static std::atomic<bool> logged{};
    if (!logged.exchange(true)) {
        LOG_INFO(Common,
                 "Thread boost (EDEN_NX2_THREAD_BOOST): timer slack {}, utilization clamp {}{}",
                 slack ? "1 ns" : "unchanged", clamp ? "1024" : "unavailable: ",
                 clamp ? "" : std::strerror(clamp_error));
    }
}
#endif

} // namespace Detail

/// Called by the threads a frame is handed between (guest cores, HostTiming, the GPU thread, the
/// split's back, the Vulkan worker, the fencing thread) where they are about to wait: applies the
/// boost to the calling thread once it is wanted. EDEN_NX2_THREAD_BOOST=1 forces it on, =0 off;
/// unset, it is on for an integrated AMD GPU on RADV. Linux only.
inline void PollThreadBoost() {
#if defined(__linux__) && !defined(__ANDROID__)
    static thread_local bool applied = false;
    if (applied) [[likely]] {
        return;
    }
    static const int forced = EnvSwitch("EDEN_NX2_THREAD_BOOST");
    if (!OnForIntegratedRadv(forced)) {
        return;
    }
    applied = true;
    Detail::ApplyThreadBoost();
#endif
}

} // namespace Common::Nx2Host
