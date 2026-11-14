// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// libFuzzer harness for audio::parse_bw64. It may refuse any input; it may not crash or read out of
/// bounds, and what it accepts must lie within the bytes: the samples and every raw chunk body.

#include "statusbar/audio/audio_bw64_reader.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>

using namespace statusbar;

namespace {

/// Whether `inner` lies within `outer`.
[[nodiscard]] auto within(std::span<uint8_t const> const inner, std::span<uint8_t const> const outer) noexcept -> bool
{
    return inner.empty() || (inner.data() >= outer.data() && inner.data() + inner.size() <= outer.data() + outer.size());
}

void check(audio::Bw64Info const& info, std::span<uint8_t const> const bytes)
{
    bool const placed = info.data_offset + info.data_available <= bytes.size() &&
        info.frames * info.format.block_align <= info.data_available && info.format.block_align != 0;
    if (!placed || !within(info.chna, bytes) || !within(info.axml, bytes) || !within(info.ixml, bytes)) {
        std::abort();
    }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(uint8_t const* data, size_t size)
{
    std::span<uint8_t const> const bytes{data, size};
    auto const info = audio::parse_bw64(bytes);
    if (info) {
        check(*info, bytes);
    }
    return 0;
}
