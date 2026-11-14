// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT
//
// parse_bw64() — a reader for RIFF WAVE, RF64 (EBU Tech 3306) and BW64
// (ITU-R BS.2088) files held in memory, typically a MappedFile.
//
// The reader walks the chunk list once and reports where things are: the
// data chunk's offset and sizes, the fmt chunk resolved to one of the sample
// layouts the package converts, the bext chunk (EBU Tech 3285) field by
// field, and the chna, axml and iXML bodies as they are. Nothing is copied
// and nothing is allocated: every view in the result points into the span
// given, and is good for as long as that span is.
//
// A data chunk cut short by the end of the span is not an error. frames
// counts what is there and complete() says whether that is all of it, so a
// file another process is still writing can be read as far as it goes.

#pragma once

#include "statusbar/audio/audio_error.hpp"
#include "statusbar/status/status.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace statusbar::audio {

/// The container the file's first four octets name.
enum class Bw64Container : uint8_t
{
    riff,  ///< "RIFF": sizes are 32-bit
    rf64,  ///< "RF64": 64-bit sizes in the ds64 chunk (EBU Tech 3306)
    bw64,  ///< "BW64": the same layout under ITU-R BS.2088's name
};

/// The sample layouts the reader accepts, as the data chunk holds them
/// (little-endian, channel-interleaved).
enum class Bw64Sample : uint8_t
{
    pcm16,    ///< WAVE_FORMAT_PCM, 16 bits
    pcm24,    ///< WAVE_FORMAT_PCM, 24 bits in 3 octets
    pcm32,    ///< WAVE_FORMAT_PCM, 32 bits
    float32,  ///< WAVE_FORMAT_IEEE_FLOAT, 32 bits
};

/// The fmt chunk, with a WAVE_FORMAT_EXTENSIBLE SubFormat resolved.
struct Bw64Format
{
    uint16_t format_tag{0};  ///< as written: 1 PCM, 3 IEEE float, 0xFFFE extensible
    uint16_t channels{0};
    uint32_t sample_rate{0};
    uint32_t byte_rate{0};
    uint16_t block_align{0};      ///< octets per frame
    uint16_t bits_per_sample{0};  ///< container bits per sample
    uint16_t valid_bits{0};       ///< extensible: wValidBitsPerSample; else bits_per_sample
    uint32_t channel_mask{0};     ///< extensible: dwChannelMask; else 0
    bool extensible{false};
    Bw64Sample sample{Bw64Sample::pcm16};
};

/// The bext chunk of EBU Tech 3285. Strings are the field with its trailing
/// NULs removed; UMID is the 64 octets as written.
struct Bw64Bext
{
    std::string_view description;
    std::string_view originator;
    std::string_view originator_reference;
    std::string_view origination_date;
    std::string_view origination_time;
    uint64_t time_reference{0};
    uint16_t version{0};
    std::span<uint8_t const> umid;
    int16_t loudness_value{0};
    int16_t loudness_range{0};
    int16_t max_true_peak_level{0};
    int16_t max_momentary_loudness{0};
    int16_t max_short_term_loudness{0};
    std::string_view coding_history;
};

/// What parse_bw64() found.
struct Bw64Info
{
    Bw64Container container{Bw64Container::riff};
    Bw64Format format{};
    size_t data_offset{0};            ///< offset in the span of the first sample octet
    uint64_t data_size{0};            ///< the data chunk's declared size (from ds64 when the chunk says 0xFFFFFFFF)
    std::span<uint8_t const> data{};  ///< the sample octets present in the span: min(data_size, what is there)
    uint64_t frames{0};               ///< whole frames in data
    std::optional<Bw64Bext> bext{};
    std::span<uint8_t const> chna{};  ///< ITU-R BS.2076 channel allocation, raw
    std::span<uint8_t const> axml{};  ///< ADM XML, raw
    std::span<uint8_t const> ixml{};  ///< iXML, raw

    /// True when the whole declared data chunk is in the span.
    [[nodiscard]] auto complete() const noexcept -> bool { return data.size() == data_size; }
};

/// Parse a WAVE, RF64 or BW64 file held in @p file. AudioError::MalformedFile
/// for a structure that is not one; AudioError::UnsupportedFormat for a fmt
/// chunk the reader does not decode (8-bit PCM, 64-bit float, A-law, mu-law,
/// compressed tags).
[[nodiscard]] auto parse_bw64(std::span<uint8_t const> file) noexcept -> StatusValue<Bw64Info>;

/// The speaker position of @p channel (0-based, in file order) under a
/// WAVE_FORMAT_EXTENSIBLE @p channel_mask: the label of its set bit, "FL",
/// "FR", "FC", "LFE", "BL", "BR", "FLC", "FRC", "BC", "SL", "SR", "TC",
/// "TFL", "TFC", "TFR", "TBL", "TBR", "TBC"; empty when the mask has no bit
/// for it.
[[nodiscard]] auto bw64_channel_label(uint32_t channel_mask, uint16_t channel) noexcept -> std::string_view;

}  // namespace statusbar::audio
