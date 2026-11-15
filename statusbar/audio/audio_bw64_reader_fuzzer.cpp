// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// libFuzzer harness for audio::parse_bw64. Feeds arbitrary bytes as a WAVE,
/// RF64 or BW64 file. The reader must not crash, hang, or read out of bounds
/// on any input; it may legally return any error. What we look for is
/// undefined behavior caught by ASan / UBSan, and a result that lies: every
/// view the reader hands back must lie inside the input, and the frame count
/// must fit the data it points at. size() reads no memory, so a span running
/// past the input would pass ASan untouched; the checks here trap on it, and
/// so catch the offset and length arithmetic as well as the crashes.

#include "statusbar/audio/audio_bw64_reader.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

using namespace statusbar;

namespace {

/// Whether [first, first + length) lies within the input, an empty view with a
/// null data pointer included (what an absent chunk comes back as).
[[nodiscard]] auto inside(uint8_t const* const first, size_t const length, std::span<uint8_t const> const input) noexcept -> bool
{
    if (length == 0) {
        return true;
    }
    return first >= input.data() && length <= input.size() && first <= input.data() + (input.size() - length);
}

[[nodiscard]] auto inside(std::span<uint8_t const> const view, std::span<uint8_t const> const input) noexcept -> bool
{
    return inside(view.data(), view.size(), input);
}

[[nodiscard]] auto inside(std::string_view const view, std::span<uint8_t const> const input) noexcept -> bool
{
    // Fuzzer harness: the view is bytes of the input, read back as bytes.
    return inside(
        reinterpret_cast<uint8_t const*>(view.data()), view.size(), input);  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
}

void require(bool const holds) noexcept
{
    if (!holds) {
        __builtin_trap();
    }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(uint8_t const* data, size_t size)
{
    std::span<uint8_t const> const input(data, size);
    auto const info = audio::parse_bw64(input);
    if (info) {
        require(inside(info->data, input));
        require(
            info->data_offset <= size && info->data.data() == (info->data.empty() ? info->data.data() : data + info->data_offset));
        require(info->data.size() <= info->data_size);
        require(info->format.block_align == 0 ? info->frames == 0 : info->frames * info->format.block_align <= info->data.size());
        require(inside(info->chna, input) && inside(info->axml, input) && inside(info->ixml, input));
        if (info->bext) {
            auto const& b = *info->bext;
            require(inside(b.description, input) && inside(b.originator, input) && inside(b.originator_reference, input));
            require(inside(b.origination_date, input) && inside(b.origination_time, input) && inside(b.coding_history, input));
            require(inside(b.umid, input));
        }
        (void)audio::bw64_channel_label(info->format.channel_mask, info->format.channels);
    }
    return 0;
}
