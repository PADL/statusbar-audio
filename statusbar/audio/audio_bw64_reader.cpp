// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

#include "statusbar/audio/audio_bw64_reader.hpp"

#include "statusbar/audio/audio_error.hpp"
#include "statusbar/buffer/buffer_deserializer_builder.hpp"
#include "statusbar/buffer/span_utils.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace statusbar::audio {

namespace {

using FourCc = std::array<uint8_t, 4>;

[[nodiscard]] constexpr auto fourcc(std::string_view const id) noexcept -> FourCc
{
    return {static_cast<uint8_t>(id[0]), static_cast<uint8_t>(id[1]), static_cast<uint8_t>(id[2]), static_cast<uint8_t>(id[3])};
}

constexpr FourCc RIFF_ID = fourcc("RIFF");
constexpr FourCc RF64_ID = fourcc("RF64");
constexpr FourCc BW64_ID = fourcc("BW64");
constexpr FourCc WAVE_ID = fourcc("WAVE");
constexpr FourCc DS64_ID = fourcc("ds64");
constexpr FourCc FMT_ID = fourcc("fmt ");
constexpr FourCc DATA_ID = fourcc("data");
constexpr FourCc BEXT_ID = fourcc("bext");
constexpr FourCc CHNA_ID = fourcc("chna");
constexpr FourCc AXML_ID = fourcc("axml");
constexpr FourCc IXML_ID = fourcc("iXML");

constexpr uint32_t SIZE_IN_DS64 = 0xFFFFFFFFU;  ///< EBU Tech 3306: the real size is in ds64
constexpr size_t CHUNK_HEADER_SIZE = 8;
constexpr size_t FORM_HEADER_SIZE = 12;
constexpr size_t DS64_MIN_SIZE = 28;           ///< riffSize, dataSize, sampleCount, tableLength
constexpr size_t FMT_MIN_SIZE = 16;            ///< WAVEFORMAT plus wBitsPerSample
constexpr size_t EXTENSIBLE_TAIL_SIZE = 24;    ///< cbSize, wValidBitsPerSample, dwChannelMask, SubFormat
constexpr size_t BEXT_FIXED_SIZE = 602;        ///< EBU Tech 3285 before CodingHistory
constexpr size_t BEXT_UMID_TO_RESERVED = 254;  ///< UMID, the loudness fields and Reserved
constexpr uint16_t WAVE_FORMAT_PCM = 0x0001;
constexpr uint16_t WAVE_FORMAT_IEEE_FLOAT = 0x0003;
constexpr uint16_t WAVE_FORMAT_EXTENSIBLE = 0xFFFE;

/// KSDATAFORMAT_SUBTYPE_PCM and _IEEE_FLOAT after their leading 16-bit format code (RFC 2361).
constexpr std::array<uint8_t, 14> SUBTYPE_TAIL{0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71};

/// WAVEFORMATEXTENSIBLE's speaker positions, SPEAKER_FRONT_LEFT (bit 0) to SPEAKER_TOP_BACK_RIGHT (bit 17).
constexpr std::array<std::string_view, 18> SPEAKER_LABELS{
    "L", "R", "C", "LFE", "Ls", "Rs", "Lc", "Rc", "Cs", "Lsd", "Rsd", "Ts", "Tfl", "Tfc", "Tfr", "Tbl", "Tbc", "Tbr"};

[[nodiscard]] auto malformed() noexcept
{
    return failure(AudioError::MalformedFile);
}

[[nodiscard]] auto unsupported() noexcept
{
    return failure(AudioError::UnsupportedFormat);
}

/// Read a little-endian unsigned integer and advance; false when too few bytes remain.
template <std::unsigned_integral T>
[[nodiscard]] auto read_le(BufferDeserializer& in, T& value) noexcept -> bool
{
    if (!in.parse(&value)) {
        return false;
    }
    if constexpr (std::endian::native == std::endian::big) {
        value = std::byteswap(value);
    }
    return true;
}

template <size_t N>
[[nodiscard]] auto read_bytes(BufferDeserializer& in, std::array<uint8_t, N>& bytes) noexcept -> bool
{
    return static_cast<bool>(in.parse(&bytes));
}

/// `text` up to its first NUL, trailing spaces trimmed.
[[nodiscard]] auto trimmed(std::string_view text) noexcept -> std::string_view
{
    text = text.substr(0, text.find('\0'));
    auto const last = text.find_last_not_of(' ');
    return last == std::string_view::npos ? std::string_view{} : text.substr(0, last + 1);
}

/// A fixed-size text field of `size` bytes, trimmed(); false when too few bytes remain.
[[nodiscard]] auto read_text(BufferDeserializer& in, size_t const size, std::string_view& text) noexcept -> bool
{
    auto const bytes = in.peek(size);
    text = trimmed(as_string_view(bytes));
    return bytes.size() == size && in.consume(size);
}

/// Skip the pad byte after an odd-sized chunk body, when the file has it.
void skip_pad(BufferDeserializer& in, uint64_t const size) noexcept
{
    if ((size & 1U) != 0 && in.available() > 0) {
        (void)in.consume(1);
    }
}

[[nodiscard]] auto container_of(FourCc const& id) noexcept -> std::optional<Bw64Container>
{
    if (id == RIFF_ID) {
        return Bw64Container::riff;
    }
    if (id == RF64_ID) {
        return Bw64Container::rf64;
    }
    return id == BW64_ID ? std::optional{Bw64Container::bw64} : std::nullopt;
}

[[nodiscard]] auto coding_of(uint16_t const code) noexcept -> std::optional<Bw64Coding>
{
    if (code == WAVE_FORMAT_PCM) {
        return Bw64Coding::pcm;
    }
    return code == WAVE_FORMAT_IEEE_FLOAT ? std::optional{Bw64Coding::ieee_float} : std::nullopt;
}

/// Whether `format`'s sample sizes are ones `coding` is read at: PCM 16, 24 or 32 bits, float 32.
[[nodiscard]] auto bits_fit(Bw64Coding const coding, Bw64Format const& format) noexcept -> bool
{
    auto const bits = format.bits_per_sample;
    return coding == Bw64Coding::ieee_float ? bits == 32 : (bits == 16 || bits == 24 || bits == 32);
}

[[nodiscard]] auto read_wave_format(BufferDeserializer& in, Bw64Format& format) noexcept -> bool
{
    uint32_t bytes_per_second{0};
    bool const read = read_le(in, format.format_tag) && read_le(in, format.channels) && read_le(in, format.sample_rate) &&
        read_le(in, bytes_per_second) && read_le(in, format.block_align) && read_le(in, format.bits_per_sample);
    format.valid_bits = format.bits_per_sample;
    return read;
}

/// WAVEFORMATEXTENSIBLE's fields after WAVEFORMATEX.
struct ExtensibleFields
{
    uint16_t extension_size{0};
    uint16_t valid_bits{0};
    uint32_t channel_mask{0};
    uint16_t code{0};  ///< the SubFormat GUID's leading format code
    std::array<uint8_t, 14> tail{};
};

[[nodiscard]] auto read_extensible_fields(BufferDeserializer& in, ExtensibleFields& fields) noexcept -> bool
{
    return in.available() >= EXTENSIBLE_TAIL_SIZE && read_le(in, fields.extension_size) && read_le(in, fields.valid_bits) &&
        read_le(in, fields.channel_mask) && read_le(in, fields.code) && read_bytes(in, fields.tail);
}

/// WAVEFORMATEXTENSIBLE's fields into `format`: the SubFormat's format code.
[[nodiscard]] auto read_extensible(BufferDeserializer& in, Bw64Format& format) noexcept -> StatusValue<uint16_t>
{
    ExtensibleFields fields{};
    if (!read_extensible_fields(in, fields)) {
        return malformed();
    }
    format.valid_bits = fields.valid_bits == 0 ? format.bits_per_sample : fields.valid_bits;  // 0: left unset
    format.channel_mask = fields.channel_mask;
    if (fields.extension_size < EXTENSIBLE_TAIL_SIZE - 2 || format.valid_bits > format.bits_per_sample) {
        return malformed();  // cbSize short of the extension, or more valid bits than the container holds
    }
    return fields.tail == SUBTYPE_TAIL ? StatusValue<uint16_t>{fields.code} : unsupported();
}

/// Whether a frame is exactly one sample of each channel.
[[nodiscard]] auto aligned(Bw64Format const& format) noexcept -> bool
{
    return uint32_t{format.block_align} == uint32_t{format.channels} * format.bytes_per_sample();
}

/// `format` with its coding resolved from format code `code`, checked.
[[nodiscard]] auto checked_format(Bw64Format format, uint16_t const code) noexcept -> StatusValue<Bw64Format>
{
    if (format.channels == 0 || format.sample_rate == 0) {
        return malformed();
    }
    auto const coding = coding_of(code);
    if (!coding || format.bits_per_sample % 8 != 0 || !bits_fit(*coding, format)) {
        return unsupported();
    }
    format.coding = *coding;
    return aligned(format) ? StatusValue<Bw64Format>{format} : malformed();
}

[[nodiscard]] auto parse_fmt(std::span<uint8_t const> const body) noexcept -> StatusValue<Bw64Format>
{
    BufferDeserializer in{body};
    Bw64Format format{};
    if (body.size() < FMT_MIN_SIZE || !read_wave_format(in, format)) {
        return malformed();
    }
    auto const code =
        format.format_tag == WAVE_FORMAT_EXTENSIBLE ? read_extensible(in, format) : StatusValue<uint16_t>{format.format_tag};
    return code ? checked_format(format, *code) : forward_failure(code);
}

[[nodiscard]] auto read_bext_text(BufferDeserializer& in, Bw64Bext& bext) noexcept -> bool
{
    return read_text(in, 256, bext.description) && read_text(in, 32, bext.originator) &&
        read_text(in, 32, bext.originator_reference) && read_text(in, 10, bext.origination_date) &&
        read_text(in, 8, bext.origination_time);
}

[[nodiscard]] auto read_bext_numbers(BufferDeserializer& in, Bw64Bext& bext) noexcept -> bool
{
    uint32_t low{0};
    uint32_t high{0};
    bool const read = read_le(in, low) && read_le(in, high) && read_le(in, bext.version);
    bext.time_reference = (uint64_t{high} << 32U) | low;
    return read && in.consume(BEXT_UMID_TO_RESERVED);
}

[[nodiscard]] auto parse_bext(std::span<uint8_t const> const body) noexcept -> StatusValue<Bw64Bext>
{
    BufferDeserializer in{body};
    Bw64Bext bext{};
    if (body.size() < BEXT_FIXED_SIZE || !read_bext_text(in, bext) || !read_bext_numbers(in, bext)) {
        return malformed();
    }
    bext.coding_history = trimmed(as_string_view(body.subspan(BEXT_FIXED_SIZE)));
    return bext;
}

struct ChunkHeader
{
    FourCc id{};
    uint32_t size{0};
};

[[nodiscard]] auto read_chunk_header(BufferDeserializer& in, ChunkHeader& header) noexcept -> bool
{
    return read_bytes(in, header.id) && read_le(in, header.size);
}

/// The form header: its container and 32-bit size.
struct FormHeader
{
    Bw64Container container{Bw64Container::riff};
    uint32_t size{0};
};

[[nodiscard]] auto read_form(BufferDeserializer& in) noexcept -> std::optional<FormHeader>
{
    ChunkHeader header{};
    FourCc form{};
    if (!read_chunk_header(in, header) || !read_bytes(in, form) || form != WAVE_ID) {
        return std::nullopt;
    }
    auto const container = container_of(header.id);
    return container ? std::optional{FormHeader{.container = *container, .size = header.size}} : std::nullopt;
}

/// A chunk body of `size` bytes and its pad byte, consumed; nullopt when it overruns.
[[nodiscard]] auto read_body(BufferDeserializer& in, uint32_t const size) noexcept -> std::optional<std::span<uint8_t const>>
{
    auto const body = in.peek(size);
    if (body.size() != size || !in.consume(size)) {
        return std::nullopt;
    }
    skip_pad(in, size);
    return body;
}

/// One pass over a WAVE file's chunks, filling a Bw64Info.
class Bw64Parser
{
  public:
    explicit Bw64Parser(std::span<uint8_t const> const bytes) noexcept
        : bytes_{bytes}
    {}

    [[nodiscard]] auto run() noexcept -> StatusValue<Bw64Info>
    {
        BufferDeserializer in{bytes_};
        if (auto const header = parse_header(in); !header) {
            return forward_failure(header);
        }
        if (auto const walked = walk(in.position()); !walked) {
            return forward_failure(walked);
        }
        return finish();
    }

  private:
    /// The form header (and ds64 for RF64/BW64): sets the container and where the form ends.
    [[nodiscard]] auto parse_header(BufferDeserializer& in) noexcept -> Status
    {
        auto const form = read_form(in);
        if (!form) {
            return malformed();
        }
        info_.container = form->container;
        return form->container == Bw64Container::riff ? set_riff_end(form->size) : parse_ds64(in, form->size);
    }

    [[nodiscard]] auto set_riff_end(uint32_t const size) noexcept -> Status
    {
        if (size == SIZE_IN_DS64) {
            return malformed();  // only RF64 and BW64 carry a 64-bit size
        }
        return set_form_end(size);
    }

    /// The form ends `size` bytes after its first 8, or at the span's end when that is sooner.
    [[nodiscard]] auto set_form_end(uint64_t const size) noexcept -> Status
    {
        uint64_t const room = bytes_.size() - CHUNK_HEADER_SIZE;
        form_end_ = CHUNK_HEADER_SIZE + std::min(size, room);
        return success();
    }

    /// The ds64 chunk, first in an RF64 or BW64 form (EBU Tech 3306, ITU-R BS.2088): the 64-bit sizes.
    [[nodiscard]] auto parse_ds64(BufferDeserializer& in, uint32_t const form_size) noexcept -> Status
    {
        ChunkHeader header{};
        uint64_t riff_size{0};
        bool const read = read_chunk_header(in, header) && header.id == DS64_ID && header.size >= DS64_MIN_SIZE &&
            header.size <= in.available() && read_le(in, riff_size) && read_le(in, ds64_data_size_) && in.consume(header.size - 16);
        if (!read) {
            return malformed();
        }
        skip_pad(in, header.size);
        return set_form_end(form_size == SIZE_IN_DS64 ? riff_size : form_size);
    }

    /// Every chunk from `start` to the form's end, stopping at a data chunk cut short.
    [[nodiscard]] auto walk(size_t const start) noexcept -> Status
    {
        BufferDeserializer in{bytes_.first(form_end_)};
        bool more = in.reset(start);
        while (more && in.available() >= CHUNK_HEADER_SIZE) {
            auto const next = parse_chunk(in);
            if (!next) {
                return forward_failure(next);
            }
            more = *next;
        }
        return success();
    }

    /// The next chunk: whether the walk goes on past it.
    [[nodiscard]] auto parse_chunk(BufferDeserializer& in) noexcept -> StatusValue<bool>
    {
        ChunkHeader header{};
        if (!read_chunk_header(in, header)) {
            return malformed();
        }
        if (header.id == DATA_ID) {
            return place_data(in, header.size);
        }
        auto const body = read_body(in, header.size);
        return body ? accept_chunk(header.id, *body) : malformed();
    }

    /// The data chunk's declared size: ds64's when its own says so in RF64 or BW64.
    [[nodiscard]] auto data_size(uint32_t const size32) const noexcept -> uint64_t
    {
        return (size32 == SIZE_IN_DS64 && info_.container != Bw64Container::riff) ? ds64_data_size_ : size32;
    }

    /// The data chunk's place; the walk goes on only when it is whole within the form.
    [[nodiscard]] auto place_data(BufferDeserializer& in, uint32_t const size32) noexcept -> StatusValue<bool>
    {
        if (has_data_) {
            return malformed();
        }
        record_data(in.position(), data_size(size32), in.available());
        if (!info_.complete() || !in.consume(static_cast<size_t>(info_.data_size))) {
            return false;
        }
        skip_pad(in, info_.data_size);
        return true;
    }

    void record_data(uint64_t const offset, uint64_t const size, uint64_t const available) noexcept
    {
        has_data_ = true;
        info_.data_offset = offset;
        info_.data_size = size;
        info_.data_available = std::min(size, available);
    }

    /// A chunk other than data: the ones the reader knows are kept, the rest skipped.
    [[nodiscard]] auto accept_chunk(FourCc const& id, std::span<uint8_t const> const body) noexcept -> StatusValue<bool>
    {
        if (id == FMT_ID) {
            return accept_fmt(body);
        }
        if (id == BEXT_ID) {
            return accept_bext(body);
        }
        if (auto* const raw = raw_chunk(id); raw != nullptr) {
            *raw = body;
        }
        return true;
    }

    /// Where a chunk kept as raw bytes goes (chna, axml, iXML); nullptr for any other.
    [[nodiscard]] auto raw_chunk(FourCc const& id) noexcept -> std::span<uint8_t const>*
    {
        if (id == CHNA_ID) {
            return &info_.chna;
        }
        if (id == AXML_ID) {
            return &info_.axml;
        }
        return id == IXML_ID ? &info_.ixml : nullptr;
    }

    [[nodiscard]] auto accept_fmt(std::span<uint8_t const> const body) noexcept -> StatusValue<bool>
    {
        auto const format = has_format_ ? StatusValue<Bw64Format>{malformed()} : parse_fmt(body);  // a second fmt is malformed
        if (!format) {
            return forward_failure(format);
        }
        info_.format = *format;
        has_format_ = true;
        return true;
    }

    [[nodiscard]] auto accept_bext(std::span<uint8_t const> const body) noexcept -> StatusValue<bool>
    {
        auto const bext = parse_bext(body);
        if (!bext) {
            return forward_failure(bext);
        }
        info_.bext = *bext;
        return true;
    }

    [[nodiscard]] auto finish() noexcept -> StatusValue<Bw64Info>
    {
        if (!has_format_ || !has_data_) {
            return malformed();
        }
        info_.frames = info_.data_available / info_.format.block_align;
        return info_;
    }

    std::span<uint8_t const> bytes_;
    Bw64Info info_{};
    uint64_t form_end_{0};        ///< where the form's chunks end within bytes_
    uint64_t ds64_data_size_{0};  ///< ds64's dataSize (RF64, BW64)
    bool has_format_{false};
    bool has_data_{false};
};

}  // namespace

auto parse_bw64(std::span<uint8_t const> const bytes) noexcept -> StatusValue<Bw64Info>
{
    if (bytes.size() < FORM_HEADER_SIZE) {
        return malformed();
    }
    return Bw64Parser{bytes}.run();
}

auto bw64_channel_label(uint32_t const mask, uint16_t const channel) noexcept -> std::string_view
{
    uint16_t seen{0};
    for (size_t bit = 0; bit < SPEAKER_LABELS.size(); ++bit) {
        if ((mask & (1U << bit)) == 0) {
            continue;
        }
        if (seen++ == channel) {
            return SPEAKER_LABELS.at(bit);
        }
    }
    return {};
}

}  // namespace statusbar::audio
