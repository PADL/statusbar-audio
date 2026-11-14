// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/audio/audio_bw64_reader.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>

namespace statusbar::audio {

namespace {

constexpr uint16_t WAVE_FORMAT_PCM = 0x0001;
constexpr uint16_t WAVE_FORMAT_IEEE_FLOAT = 0x0003;
constexpr uint16_t WAVE_FORMAT_EXTENSIBLE = 0xFFFE;
constexpr uint32_t SIZE_IN_DS64 = 0xFFFFFFFFU;
constexpr size_t FMT_BASE_SIZE = 16;
constexpr size_t FMT_EXTENSIBLE_SIZE = 40;  // 16 + cbSize(2) + 22
constexpr size_t BEXT_FIXED_SIZE = 602;
constexpr size_t DS64_FIXED_SIZE = 28;  // riffSize(8) dataSize(8) sampleCount(8) tableLength(4)
constexpr size_t DS64_ENTRY_SIZE = 12;  // chunkId(4) chunkSize(8)
constexpr size_t CHANNEL_LABEL_COUNT = 18;

/// KSDATAFORMAT_SUBTYPE_*: the format tag in the first two octets, then this.
constexpr std::array<uint8_t, 14> SUBFORMAT_TAIL{
    0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71};

constexpr std::array<std::string_view, CHANNEL_LABEL_COUNT> CHANNEL_LABELS{
    "FL", "FR", "FC", "LFE", "BL", "BR", "FLC", "FRC", "BC", "SL", "SR", "TC", "TFL", "TFC", "TFR", "TBL", "TBR", "TBC"};

[[nodiscard]] auto malformed() noexcept -> std::unexpected<std::error_code>
{
    return failure(make_error_code(AudioError::MalformedFile));
}

[[nodiscard]] auto unsupported() noexcept -> std::unexpected<std::error_code>
{
    return failure(make_error_code(AudioError::UnsupportedFormat));
}

/// Little-endian loads out of a span. Each caller has checked the length.
[[nodiscard]] auto le16(std::span<uint8_t const> const b, size_t const at) noexcept -> uint16_t
{
    return static_cast<uint16_t>(b[at] | (b[at + 1] << 8));
}

[[nodiscard]] auto le32(std::span<uint8_t const> const b, size_t const at) noexcept -> uint32_t
{
    return static_cast<uint32_t>(le16(b, at)) | (static_cast<uint32_t>(le16(b, at + 2)) << 16);
}

[[nodiscard]] auto le64(std::span<uint8_t const> const b, size_t const at) noexcept -> uint64_t
{
    return static_cast<uint64_t>(le32(b, at)) | (static_cast<uint64_t>(le32(b, at + 4)) << 32);
}

[[nodiscard]] auto fourcc_is(std::span<uint8_t const> const b, size_t const at, char const (&id)[5]) noexcept -> bool
{
    return std::memcmp(b.data() + at, id, 4) == 0;
}

/// A fixed-width ASCII field without its trailing NULs.
[[nodiscard]] auto padded_text(std::span<uint8_t const> const b, size_t const at, size_t const width) noexcept -> std::string_view
{
    std::string_view text{
        reinterpret_cast<char const*>(b.data() + at), width};  // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
    auto const end = text.find_last_not_of('\0');
    return end == std::string_view::npos ? std::string_view{} : text.substr(0, end + 1);
}

/// The chunk sizes a ds64 chunk carries (EBU Tech 3306 3.5).
struct Ds64
{
    uint64_t riff_size{0};
    uint64_t data_size{0};
    uint64_t sample_count{0};
    std::span<uint8_t const> table{};  ///< the chunkId/chunkSize entries

    /// The 64-bit size the table gives a chunk, if it has one.
    [[nodiscard]] auto size_of(std::span<uint8_t const> const id) const noexcept -> std::optional<uint64_t>
    {
        for (size_t at = 0; at + DS64_ENTRY_SIZE <= table.size(); at += DS64_ENTRY_SIZE) {
            if (std::memcmp(table.data() + at, id.data(), 4) == 0) {
                return le64(table, at + 4);
            }
        }
        return std::nullopt;
    }
};

[[nodiscard]] auto parse_ds64(std::span<uint8_t const> const body) noexcept -> StatusValue<Ds64>
{
    if (body.size() < DS64_FIXED_SIZE) {
        return malformed();
    }
    Ds64 ds64{.riff_size = le64(body, 0), .data_size = le64(body, 8), .sample_count = le64(body, 16)};
    size_t const table_bytes = static_cast<size_t>(le32(body, 24)) * DS64_ENTRY_SIZE;
    if (DS64_FIXED_SIZE + table_bytes > body.size()) {
        return malformed();
    }
    ds64.table = body.subspan(DS64_FIXED_SIZE, table_bytes);
    return ds64;
}

/// The sample layout of a tag and width; nullopt for one the reader leaves alone.
[[nodiscard]] auto sample_of(uint16_t const tag, uint16_t const bits) noexcept -> std::optional<Bw64Sample>
{
    if (tag == WAVE_FORMAT_PCM) {
        switch (bits) {
            case 16:
                return Bw64Sample::pcm16;
            case 24:
                return Bw64Sample::pcm24;
            case 32:
                return Bw64Sample::pcm32;
            default:
                return std::nullopt;
        }
    }
    if (tag == WAVE_FORMAT_IEEE_FLOAT && bits == 32) {
        return Bw64Sample::float32;
    }
    return std::nullopt;
}

void read_extensible_fields(std::span<uint8_t const> const body, Bw64Format& f) noexcept
{
    f.extensible = true;
    f.valid_bits = le16(body, 18);
    f.channel_mask = le32(body, 20);
    f.format_tag = le16(body, 24);  // the SubFormat's first two octets are the tag it stands for
}

/// The WAVE_FORMAT_EXTENSIBLE tail: wValidBitsPerSample, dwChannelMask, SubFormat.
[[nodiscard]] auto parse_extensible(std::span<uint8_t const> const body, Bw64Format& f) noexcept -> Status
{
    if (body.size() < FMT_EXTENSIBLE_SIZE || le16(body, 16) < 22) {
        return malformed();
    }
    if (std::memcmp(body.data() + 26, SUBFORMAT_TAIL.data(), SUBFORMAT_TAIL.size()) != 0) {
        return unsupported();  // a SubFormat outside the KSDATAFORMAT family
    }
    read_extensible_fields(body, f);
    return success();
}

[[nodiscard]] auto parse_fmt(std::span<uint8_t const> const body) noexcept -> StatusValue<Bw64Format>
{
    if (body.size() < FMT_BASE_SIZE) {
        return malformed();
    }
    Bw64Format f{
        .format_tag = le16(body, 0),
        .channels = le16(body, 2),
        .sample_rate = le32(body, 4),
        .byte_rate = le32(body, 8),
        .block_align = le16(body, 12),
        .bits_per_sample = le16(body, 14),
    };
    f.valid_bits = f.bits_per_sample;
    if (f.format_tag == WAVE_FORMAT_EXTENSIBLE) {
        if (auto const st = parse_extensible(body, f); !st) {
            return failure(st.error());
        }
    }
    return f;
}

/// The checks that make a fmt chunk one the data can be read by.
[[nodiscard]] auto check_fmt(Bw64Format& f) noexcept -> Status
{
    auto const sample = sample_of(f.format_tag, f.bits_per_sample);
    if (!sample) {
        return unsupported();
    }
    f.sample = *sample;
    bool const consistent = f.channels != 0 && f.sample_rate != 0 && f.valid_bits != 0 && f.valid_bits <= f.bits_per_sample &&
        f.block_align == f.channels * (f.bits_per_sample / 8);
    return consistent ? success() : malformed();
}

[[nodiscard]] auto parse_bext(std::span<uint8_t const> const body) noexcept -> StatusValue<Bw64Bext>
{
    if (body.size() < BEXT_FIXED_SIZE) {
        return malformed();
    }
    Bw64Bext bext{
        .description = padded_text(body, 0, 256),
        .originator = padded_text(body, 256, 32),
        .originator_reference = padded_text(body, 288, 32),
        .origination_date = padded_text(body, 320, 10),
        .origination_time = padded_text(body, 330, 8),
        .time_reference = le64(body, 338),
        .version = le16(body, 346),
        .umid = body.subspan(348, 64),
        .loudness_value = static_cast<int16_t>(le16(body, 412)),
        .loudness_range = static_cast<int16_t>(le16(body, 414)),
        .max_true_peak_level = static_cast<int16_t>(le16(body, 416)),
        .max_momentary_loudness = static_cast<int16_t>(le16(body, 418)),
        .max_short_term_loudness = static_cast<int16_t>(le16(body, 420)),
        .coding_history = padded_text(body, BEXT_FIXED_SIZE, body.size() - BEXT_FIXED_SIZE),
    };
    return bext;
}

/// One chunk header as the walk sees it.
struct Chunk
{
    std::span<uint8_t const> id;  ///< the four octets
    size_t body_offset{0};
    uint64_t size{0};  ///< declared, 64-bit where the ds64 says so
};

/// The reader's walk over the chunk list.
class Walk
{
  public:
    explicit Walk(std::span<uint8_t const> const file) noexcept
        : file_{file}
    {}

    [[nodiscard]] auto run() noexcept -> StatusValue<Bw64Info>;

  private:
    [[nodiscard]] auto header() noexcept -> Status;
    [[nodiscard]] auto next_chunk() noexcept -> std::optional<Chunk>;
    [[nodiscard]] auto take(Chunk const& c) noexcept -> Status;
    [[nodiscard]] auto take_data(Chunk const& c) noexcept -> Status;
    [[nodiscard]] auto take_whole(Chunk const& c) noexcept -> Status;
    [[nodiscard]] auto finish() noexcept -> StatusValue<Bw64Info>;

    [[nodiscard]] auto declared_size(Chunk const& c, uint32_t const size32) const noexcept -> uint64_t
    {
        if (size32 != SIZE_IN_DS64) {
            return size32;
        }
        if (fourcc_is(c.id, 0, "data")) {
            return ds64_ ? ds64_->data_size : static_cast<uint64_t>(file_.size() - c.body_offset);  // streamed: to the end
        }
        return ds64_ ? ds64_->size_of(c.id).value_or(size32) : size32;
    }

    std::span<uint8_t const> file_;
    size_t pos_{0};
    Bw64Info info_{};
    std::optional<Ds64> ds64_{};
    bool have_fmt_{false};
    bool have_data_{false};
};

auto Walk::header() noexcept -> Status
{
    if (file_.size() < 12 || !fourcc_is(file_, 8, "WAVE")) {
        return malformed();
    }
    if (fourcc_is(file_, 0, "RIFF")) {
        info_.container = Bw64Container::riff;
    } else if (fourcc_is(file_, 0, "RF64")) {
        info_.container = Bw64Container::rf64;
    } else if (fourcc_is(file_, 0, "BW64")) {
        info_.container = Bw64Container::bw64;
    } else {
        return malformed();
    }
    pos_ = 12;
    return success();
}

auto Walk::next_chunk() noexcept -> std::optional<Chunk>
{
    if (pos_ + 8 > file_.size()) {
        return std::nullopt;
    }
    Chunk c{.id = file_.subspan(pos_, 4), .body_offset = pos_ + 8};
    c.size = declared_size(c, le32(file_, pos_ + 4));
    return c;
}

auto Walk::take_data(Chunk const& c) noexcept -> Status
{
    if (have_data_) {
        return malformed();
    }
    have_data_ = true;
    info_.data_offset = c.body_offset;
    info_.data_size = c.size;
    info_.data = file_.subspan(c.body_offset, static_cast<size_t>(std::min<uint64_t>(c.size, file_.size() - c.body_offset)));
    return success();
}

auto Walk::take_whole(Chunk const& c) noexcept -> Status
{
    auto const body = file_.subspan(c.body_offset, static_cast<size_t>(c.size));
    if (fourcc_is(c.id, 0, "fmt ")) {
        auto fmt = parse_fmt(body);
        if (!fmt) {
            return failure(fmt.error());
        }
        info_.format = *fmt;
        have_fmt_ = true;
    } else if (fourcc_is(c.id, 0, "ds64")) {
        auto ds64 = parse_ds64(body);
        if (!ds64) {
            return failure(ds64.error());
        }
        ds64_ = *ds64;
    } else if (fourcc_is(c.id, 0, "bext")) {
        auto bext = parse_bext(body);
        if (!bext) {
            return failure(bext.error());
        }
        info_.bext = *bext;
    } else if (fourcc_is(c.id, 0, "chna")) {
        info_.chna = body;
    } else if (fourcc_is(c.id, 0, "axml")) {
        info_.axml = body;
    } else if (fourcc_is(c.id, 0, "iXML")) {
        info_.ixml = body;
    }
    return success();
}

auto Walk::take(Chunk const& c) noexcept -> Status
{
    if (fourcc_is(c.id, 0, "data")) {
        return take_data(c);
    }
    // Every chunk but data has to be there whole; a RF64 size that overflows
    // size_t can only be past the end as well.
    if (c.size > file_.size() - c.body_offset) {
        return malformed();
    }
    return take_whole(c);
}

auto Walk::finish() noexcept -> StatusValue<Bw64Info>
{
    if (!have_fmt_ || !have_data_) {
        return malformed();
    }
    if (auto const st = check_fmt(info_.format); !st) {
        return failure(st.error());
    }
    info_.frames = info_.data.size() / info_.format.block_align;
    return info_;
}

auto Walk::run() noexcept -> StatusValue<Bw64Info>
{
    if (auto const st = header(); !st) {
        return failure(st.error());
    }
    for (auto c = next_chunk(); c; c = next_chunk()) {
        if (auto const st = take(*c); !st) {
            return failure(st.error());
        }
        if (fourcc_is(c->id, 0, "data") && !info_.complete()) {
            break;  // cut short: nothing can follow in the span
        }
        pos_ = c->body_offset + static_cast<size_t>(c->size) + (c->size & 1U);  // chunks are word-aligned
    }
    return finish();
}

}  // namespace

auto parse_bw64(std::span<uint8_t const> const file) noexcept -> StatusValue<Bw64Info>
{
    return Walk{file}.run();
}

auto bw64_channel_label(uint32_t const channel_mask, uint16_t const channel) noexcept -> std::string_view
{
    uint32_t mask = channel_mask;
    for (uint16_t skipped = 0; mask != 0; ++skipped) {
        auto const bit = static_cast<size_t>(std::countr_zero(mask));
        if (skipped == channel) {
            return bit < CHANNEL_LABEL_COUNT ? CHANNEL_LABELS[bit] : std::string_view{};
        }
        mask &= mask - 1;  // clear the lowest set bit
    }
    return {};
}

}  // namespace statusbar::audio
