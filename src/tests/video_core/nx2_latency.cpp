// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "video_core/nx2_latency.h"

using VideoCommon::Nx2Latency::ParseSwitch;
using VideoCommon::Nx2Latency::UnlandedReleases;

TEST_CASE("Latency switches parse unset, off and on", "[video_core][nx2_latency]") {
    REQUIRE(ParseSwitch(nullptr) == -1);
    REQUIRE(ParseSwitch("") == -1);
    REQUIRE(ParseSwitch("0") == 0);
    REQUIRE(ParseSwitch("1") == 1);
    REQUIRE(ParseSwitch("yes") == 1);
}

TEST_CASE("Unlanded releases last until their own fence lands them", "[video_core][nx2_latency]") {
    UnlandedReleases releases;
    REQUIRE_FALSE(releases.Payload(0x1000).has_value());

    const u64 first = releases.Ran(0x1000, 5);
    // By 4-byte word.
    REQUIRE(releases.Payload(0x1002) == 5u);
    REQUIRE_FALSE(releases.Payload(0x1004).has_value());

    // A newer release to the word replaces it; the older one's landing leaves the newer.
    const u64 second = releases.Ran(0x1000, 6);
    REQUIRE(releases.Payload(0x1000) == 6u);
    releases.Landed(0x1000, first);
    REQUIRE(releases.Payload(0x1000) == 6u);
    releases.Landed(0x1000, second);
    REQUIRE_FALSE(releases.Payload(0x1000).has_value());

    // A write that lands at once decides over a release still in flight.
    const u64 third = releases.Ran(0x2000, 7);
    releases.Overwritten(0x2000);
    REQUIRE_FALSE(releases.Payload(0x2000).has_value());
    releases.Landed(0x2000, third);
    REQUIRE_FALSE(releases.Payload(0x2000).has_value());
}
