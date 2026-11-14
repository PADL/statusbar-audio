// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// libFuzzer harness for audio::parse_bw64. Feeds arbitrary bytes as a WAVE,
/// RF64 or BW64 file. The reader must not crash, hang, or read out of bounds
/// on any input; it may legally return any error. What we look for is
/// undefined behavior caught by ASan / UBSan.

#include "statusbar/audio/audio_bw64_reader.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

using namespace statusbar;

extern "C" int LLVMFuzzerTestOneInput(uint8_t const* data, size_t size)
{
    auto const info = audio::parse_bw64(std::span<uint8_t const>(data, size));
    if (info) {
        // Touch every view so the optimizer can't elide the parse.
        size_t volatile sink = info->data.size() + info->chna.size() + info->axml.size() + info->ixml.size() +
            static_cast<size_t>(info->frames) + (info->bext ? info->bext->coding_history.size() : 0);
        (void)sink;
        (void)audio::bw64_channel_label(info->format.channel_mask, info->format.channels);
    }
    return 0;
}
