#pragma once

// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// A WAVE file reader over a byte span (a mapped file): RIFF, RF64 (EBU Tech 3306) and BW64 (ITU-R BS.2088)
/// with ds64; `fmt` PCM, IEEE float and WAVE_FORMAT_EXTENSIBLE; `bext` (EBU Tech 3285); `chna`, `axml` and
/// `iXML` as raw chunk bodies. It locates the samples and never copies or allocates.

#include "statusbar/status/status.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace statusbar::audio {

/// The file's form: RIFF/WAVE, or RF64/WAVE and BW64/WAVE with a ds64 chunk carrying 64-bit sizes.
enum class Bw64Container : uint8_t
{
    riff,
    rf64,
    bw64,
};

/// The sample coding of the data chunk, WAVE_FORMAT_EXTENSIBLE's SubFormat resolved.
enum class Bw64Coding : uint8_t
{
    pcm,         ///< signed little-endian integers (16, 24 or 32 bits a sample)
    ieee_float,  ///< little-endian IEEE 754 binary32
};

/// The `fmt` chunk: WAVEFORMATEX, and WAVEFORMATEXTENSIBLE's fields when present (RFC 2361).
struct Bw64Format
{
    uint16_t format_tag{0};  ///< as in the fmt chunk: 1 PCM, 3 IEEE float, 0xFFFE EXTENSIBLE
    Bw64Coding coding{Bw64Coding::pcm};
    uint16_t channels{0};
    uint32_t sample_rate{0};
    uint16_t block_align{0};      ///< bytes per frame
    uint16_t bits_per_sample{0};  ///< the container's bits per sample
    uint16_t valid_bits{0};       ///< EXTENSIBLE's wValidBitsPerSample, else bits_per_sample
    uint32_t channel_mask{0};     ///< EXTENSIBLE's dwChannelMask; 0 when absent

    /// Bytes a sample occupies in the data chunk.
    [[nodiscard]] auto bytes_per_sample() const noexcept -> uint16_t { return bits_per_sample / 8; }
};

/// The Broadcast Wave `bext` chunk (EBU Tech 3285): text fields are views into the parsed bytes, cut at the
/// first NUL and trimmed of trailing spaces.
struct Bw64Bext
{
    std::string_view description;
    std::string_view originator;
    std::string_view originator_reference;
    std::string_view origination_date;  ///< "yyyy-mm-dd" as written
    std::string_view origination_time;  ///< "hh:mm:ss" as written
    uint64_t time_reference{0};         ///< samples since midnight of the first sample
    uint16_t version{0};
    std::string_view coding_history;
};

/// Where a WAVE file's samples are and what they are; spans and views point into the parsed bytes.
struct Bw64Info
{
    Bw64Container container{Bw64Container::riff};
    Bw64Format format;
    uint64_t data_offset{0};     ///< byte offset of the first sample within the parsed span
    uint64_t data_size{0};       ///< as declared (ds64's for RF64/BW64 when the 32-bit size is 0xFFFFFFFF)
    uint64_t data_available{0};  ///< bytes of it present: min(declared, the form's or the span's end - data_offset)
    uint64_t frames{0};          ///< whole frames available: data_available / block_align
    std::optional<Bw64Bext> bext;
    std::span<uint8_t const> chna;  ///< BS.2088 `chna` body (empty when absent)
    std::span<uint8_t const> axml;  ///< BS.2088 `axml` body (empty when absent)
    std::span<uint8_t const> ixml;  ///< `iXML` body (empty when absent)

    /// Whether every declared data byte is present (false for a file cut short or still being written).
    [[nodiscard]] auto complete() const noexcept -> bool { return data_available == data_size; }
};

/// Parse a whole WAVE file held in `bytes` (a mapped file); views in the result live as long as `bytes`.
/// AudioError::MalformedFile for a broken structure, AudioError::UnsupportedFormat for a coding other than
/// 16, 24 or 32-bit PCM or 32-bit IEEE float. A data chunk cut short by the span's end is not an error.
[[nodiscard]] auto parse_bw64(std::span<uint8_t const> bytes) noexcept -> StatusValue<Bw64Info>;

/// The speaker name of channel `channel` (0-based) under WAVEFORMATEXTENSIBLE `mask` ("L", "R", "C", "LFE",
/// ...): the k-th set bit names channel k; empty when the mask names fewer channels.
[[nodiscard]] auto bw64_channel_label(uint32_t mask, uint16_t channel) noexcept -> std::string_view;

}  // namespace statusbar::audio
