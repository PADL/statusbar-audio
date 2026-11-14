// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

/// Tests for the WAVE reader: RIFF, RF64 and BW64 files crafted in memory, and one written by Bw64Writer.

#include "statusbar/audio/audio_bw64_reader.hpp"

#include "statusbar/audio/audio_bw64_writer.hpp"
#include "statusbar/audio/audio_error.hpp"
#include "statusbar/test/test.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

using namespace statusbar;
using namespace statusbar::audio;

namespace {

using Bytes = std::vector<uint8_t>;

constexpr std::array<uint8_t, 14> SUBTYPE_TAIL{0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71};

void put_le(Bytes& out, uint64_t const value, size_t const size)
{
    for (size_t i = 0; i < size; ++i) {
        out.push_back(static_cast<uint8_t>((value >> (8 * i)) & 0xFFU));
    }
}

void put_text(Bytes& out, std::string_view const text, size_t const size)
{
    for (size_t i = 0; i < size; ++i) {
        out.push_back(i < text.size() ? static_cast<uint8_t>(text[i]) : 0);
    }
}

/// A WAVE file built chunk by chunk; sizes are computed unless given.
class Wave
{
  public:
    explicit Wave(std::string_view const form = "RIFF")
        : form_{form}
    {}

    auto chunk(std::string_view const id, Bytes const& body, std::optional<uint32_t> const size = std::nullopt) -> Wave&
    {
        put_text(chunks_, id, 4);
        put_le(chunks_, size.value_or(static_cast<uint32_t>(body.size())), 4);
        chunks_.insert(chunks_.end(), body.begin(), body.end());
        if ((body.size() & 1U) != 0) {
            chunks_.push_back(0);
        }
        return *this;
    }

    [[nodiscard]] auto bytes(std::optional<uint32_t> const riff_size = std::nullopt) const -> Bytes
    {
        Bytes out;
        put_text(out, form_, 4);
        put_le(out, riff_size.value_or(static_cast<uint32_t>(chunks_.size() + 4)), 4);
        put_text(out, "WAVE", 4);
        out.insert(out.end(), chunks_.begin(), chunks_.end());
        return out;
    }

  private:
    std::string form_;
    Bytes chunks_;
};

[[nodiscard]] auto fmt_body(uint16_t const tag, uint16_t const channels, uint32_t const rate, uint16_t const bits) -> Bytes
{
    Bytes out;
    uint16_t const align = static_cast<uint16_t>(channels * (bits / 8));
    put_le(out, tag, 2);
    put_le(out, channels, 2);
    put_le(out, rate, 4);
    put_le(out, uint64_t{rate} * align, 4);
    put_le(out, align, 2);
    put_le(out, bits, 2);
    return out;
}

[[nodiscard]] auto extensible_body(
    uint16_t const channels,
    uint16_t const bits,
    uint16_t const valid,
    uint32_t const mask,
    uint16_t const code,
    std::array<uint8_t, 14> const& tail = SUBTYPE_TAIL) -> Bytes
{
    Bytes out = fmt_body(0xFFFE, channels, 48000, bits);
    put_le(out, 22, 2);
    put_le(out, valid, 2);
    put_le(out, mask, 4);
    put_le(out, code, 2);
    out.insert(out.end(), tail.begin(), tail.end());
    return out;
}

[[nodiscard]] auto ramp(size_t const size) -> Bytes
{
    Bytes out(size);
    for (size_t i = 0; i < size; ++i) {
        out[i] = static_cast<uint8_t>(i);
    }
    return out;
}

[[nodiscard]] auto bext_body(std::string_view const description, uint64_t const time_reference, std::string_view const history)
    -> Bytes
{
    Bytes out;
    put_text(out, description, 256);
    put_text(out, "statusbar", 32);
    put_text(out, "ref-1", 32);
    put_text(out, "2026-11-14", 10);
    put_text(out, "12:34:56", 8);
    put_le(out, time_reference, 8);
    put_le(out, 2, 2);
    put_text(out, "", 254);
    put_text(out, history, history.size());
    return out;
}

[[nodiscard]] auto ds64_body(uint64_t const riff_size, uint64_t const data_size, uint64_t const frames) -> Bytes
{
    Bytes out;
    put_le(out, riff_size, 8);
    put_le(out, data_size, 8);
    put_le(out, frames, 8);
    put_le(out, 0, 4);
    return out;
}

[[nodiscard]] auto parse(Bytes const& bytes) -> StatusValue<Bw64Info>
{
    return parse_bw64(std::span<uint8_t const>{bytes});
}

[[nodiscard]] auto refused_as(Bytes const& bytes, AudioError const error) -> bool
{
    auto const info = parse(bytes);
    return !info && info.error() == make_error_code(error);
}

[[nodiscard]] auto pcm(uint16_t const channels, uint16_t const bits, size_t const frames) -> Bytes
{
    return Wave{}.chunk("fmt ", fmt_body(1, channels, 48000, bits)).chunk("data", ramp(frames * channels * (bits / 8))).bytes();
}

[[nodiscard]] auto read_file(std::string const& path) -> Bytes
{
    Bytes bytes(std::filesystem::file_size(path));
    auto* const file = std::fopen(path.c_str(), "rb");
    if (file == nullptr) {
        return {};
    }
    auto const read = std::fread(bytes.data(), 1, bytes.size(), file);
    (void)std::fclose(file);
    bytes.resize(read);
    return bytes;
}

}  // namespace

TEST(bw64_reader, riff_pcm16_locates_its_samples)
{
    auto const bytes = pcm(2, 16, 10);
    auto const info = parse(bytes);
    EXPECT_TRUE(info.has_value());
    EXPECT_TRUE(info->container == Bw64Container::riff);
    EXPECT_TRUE(info->format.coding == Bw64Coding::pcm);
    EXPECT_EQ(info->format.channels, 2U);
    EXPECT_EQ(info->format.sample_rate, 48000U);
    EXPECT_EQ(info->format.bytes_per_sample(), 2U);
    EXPECT_EQ(info->format.valid_bits, 16U);
    EXPECT_EQ(info->data_offset, 12U + 8U + 16U + 8U);
    EXPECT_EQ(info->data_size, 40U);
    EXPECT_EQ(info->frames, 10U);
    EXPECT_TRUE(info->complete());
    EXPECT_EQ(bytes.at(info->data_offset + 5), 5U);
    EXPECT_FALSE(info->bext.has_value());
}

TEST(bw64_reader, pcm24_and_pcm32_frames_follow_block_align)
{
    auto const pcm24 = parse(pcm(8, 24, 7));
    auto const pcm32 = parse(pcm(3, 32, 5));
    EXPECT_TRUE(pcm24.has_value() && pcm32.has_value());
    EXPECT_EQ(pcm24->format.block_align, 24U);
    EXPECT_EQ(pcm24->frames, 7U);
    EXPECT_EQ(pcm32->format.block_align, 12U);
    EXPECT_EQ(pcm32->frames, 5U);
}

TEST(bw64_reader, ieee_float_is_read_as_float)
{
    auto const info = parse(Wave{}.chunk("fmt ", fmt_body(3, 2, 96000, 32)).chunk("data", ramp(64)).bytes());
    EXPECT_TRUE(info.has_value());
    EXPECT_TRUE(info->format.coding == Bw64Coding::ieee_float);
    EXPECT_EQ(info->format.sample_rate, 96000U);
    EXPECT_EQ(info->frames, 8U);
}

TEST(bw64_reader, extensible_resolves_its_subformat_mask_and_valid_bits)
{
    auto const bytes = Wave{}.chunk("fmt ", extensible_body(6, 32, 24, 0x3F, 1)).chunk("data", ramp(size_t{6} * 4 * 3)).bytes();
    auto const info = parse(bytes);
    EXPECT_TRUE(info.has_value());
    EXPECT_EQ(info->format.format_tag, 0xFFFEU);
    EXPECT_TRUE(info->format.coding == Bw64Coding::pcm);
    EXPECT_EQ(info->format.bits_per_sample, 32U);
    EXPECT_EQ(info->format.valid_bits, 24U);
    EXPECT_EQ(info->format.channel_mask, 0x3FU);
    EXPECT_EQ(info->frames, 3U);
    auto const float_info = parse(Wave{}.chunk("fmt ", extensible_body(2, 32, 0, 0x3, 3)).chunk("data", ramp(16)).bytes());
    EXPECT_TRUE(float_info.has_value());
    EXPECT_TRUE(float_info->format.coding == Bw64Coding::ieee_float);
    EXPECT_EQ(float_info->format.valid_bits, 32U);
}

TEST(bw64_reader, rf64_data_size_comes_from_ds64)
{
    auto const data = ramp(48);
    auto const bytes = Wave{"RF64"}
                           .chunk("ds64", ds64_body(4 + 36 + 24 + 56, 48, 8))
                           .chunk("fmt ", fmt_body(1, 2, 48000, 24))
                           .chunk("data", data, 0xFFFFFFFFU)
                           .bytes(0xFFFFFFFFU);
    auto const info = parse(bytes);
    EXPECT_TRUE(info.has_value());
    EXPECT_TRUE(info->container == Bw64Container::rf64);
    EXPECT_EQ(info->data_size, 48U);
    EXPECT_EQ(info->frames, 8U);
    EXPECT_TRUE(info->complete());
}

TEST(bw64_reader, bw64_form_is_recognised)
{
    auto const bytes =
        Wave{"BW64"}.chunk("ds64", ds64_body(0, 16, 4)).chunk("fmt ", fmt_body(1, 2, 48000, 16)).chunk("data", ramp(16)).bytes();
    auto const info = parse(bytes);
    EXPECT_TRUE(info.has_value());
    EXPECT_TRUE(info->container == Bw64Container::bw64);
    EXPECT_EQ(info->data_size, 16U);
    EXPECT_EQ(info->frames, 4U);
}

TEST(bw64_reader, bext_fields_and_time_reference_are_read)
{
    uint64_t const time_reference = 0x0000000123456789ULL;
    auto const bytes = Wave{}
                           .chunk("bext", bext_body("a jingle  ", time_reference, "A=PCM,F=48000\r\n"))
                           .chunk("fmt ", fmt_body(1, 1, 48000, 16))
                           .chunk("data", ramp(2))
                           .bytes();
    auto const info = parse(bytes);
    EXPECT_TRUE(info.has_value() && info->bext.has_value());
    auto const bext = info->bext.value_or(Bw64Bext{});
    EXPECT_TRUE(bext.description == "a jingle");
    EXPECT_TRUE(bext.originator == "statusbar");
    EXPECT_TRUE(bext.originator_reference == "ref-1");
    EXPECT_TRUE(bext.origination_date == "2026-11-14");
    EXPECT_TRUE(bext.origination_time == "12:34:56");
    EXPECT_EQ(bext.time_reference, time_reference);
    EXPECT_EQ(bext.version, 2U);
    EXPECT_TRUE(bext.coding_history == "A=PCM,F=48000\r\n");
}

TEST(bw64_reader, chna_axml_and_ixml_bodies_are_exposed)
{
    auto const bytes = Wave{"BW64"}
                           .chunk("ds64", ds64_body(0, 4, 1))
                           .chunk("fmt ", fmt_body(1, 2, 48000, 16))
                           .chunk("data", ramp(4))
                           .chunk("chna", Bytes{1, 2, 3, 4})
                           .chunk("axml", Bytes{'<', 'a', '/', '>'})
                           .chunk("iXML", Bytes{'<', 'B', 'W', 'F', '/', '>'})
                           .bytes();
    auto const info = parse(bytes);
    EXPECT_TRUE(info.has_value());
    EXPECT_EQ(info->chna.size(), 4U);
    EXPECT_EQ(info->chna[3], 4U);
    EXPECT_EQ(info->axml.size(), 4U);
    EXPECT_EQ(info->ixml.size(), 6U);
    EXPECT_EQ(info->ixml[1], static_cast<uint8_t>('B'));
}

TEST(bw64_reader, odd_sized_chunks_are_padded)
{
    auto const bytes = Wave{}
                           .chunk("JUNK", Bytes{9, 9, 9})
                           .chunk("fmt ", fmt_body(1, 1, 48000, 24))
                           .chunk("data", ramp(9))
                           .chunk("iXML", Bytes{'x'})
                           .bytes();
    auto const info = parse(bytes);
    EXPECT_TRUE(info.has_value());
    EXPECT_EQ(info->frames, 3U);
    EXPECT_EQ(info->data_offset, 12U + 12U + 8U + 16U + 8U);
    EXPECT_EQ(info->ixml.size(), 1U);
}

TEST(bw64_reader, a_truncated_data_chunk_is_incomplete_with_whole_frames)
{
    auto bytes = Wave{}.chunk("fmt ", fmt_body(1, 2, 48000, 24)).chunk("data", ramp(60)).bytes();
    bytes.resize(bytes.size() - 20);
    auto const info = parse(bytes);
    EXPECT_TRUE(info.has_value());
    EXPECT_FALSE(info->complete());
    EXPECT_EQ(info->data_size, 60U);
    EXPECT_EQ(info->data_available, 40U);
    EXPECT_EQ(info->frames, 6U);
}

TEST(bw64_reader, chunks_after_a_cut_data_chunk_are_not_read)
{
    auto bytes =
        Wave{}.chunk("fmt ", fmt_body(1, 1, 48000, 16)).chunk("data", ramp(8), 4000).chunk("iXML", Bytes{'x', 'y'}).bytes();
    auto const info = parse(bytes);
    EXPECT_TRUE(info.has_value());
    EXPECT_FALSE(info->complete());
    EXPECT_TRUE(info->ixml.empty());
}

TEST(bw64_reader, unsupported_codings_are_refused)
{
    auto const refused = [](Bytes const& fmt) {
        return refused_as(Wave{}.chunk("fmt ", fmt).chunk("data", ramp(16)).bytes(), AudioError::UnsupportedFormat);
    };
    EXPECT_TRUE(refused(fmt_body(1, 2, 48000, 8)));
    EXPECT_TRUE(refused(fmt_body(3, 2, 48000, 64)));
    EXPECT_TRUE(refused(fmt_body(6, 2, 48000, 16)));  // A-law
    EXPECT_TRUE(refused(fmt_body(7, 2, 48000, 16)));  // mu-law
    EXPECT_TRUE(refused(fmt_body(0x55, 2, 48000, 16)));
    EXPECT_TRUE(refused(fmt_body(1, 2, 48000, 20)));
    auto unknown = SUBTYPE_TAIL;
    unknown.back() = 0;
    EXPECT_TRUE(refused(extensible_body(2, 24, 24, 3, 1, unknown)));
}

TEST(bw64_reader, malformed_files_are_refused)
{
    auto const fmt = fmt_body(1, 2, 48000, 16);
    auto const malformed = [](Bytes const& bytes) { return refused_as(bytes, AudioError::MalformedFile); };
    EXPECT_TRUE(malformed(Bytes{'R', 'I', 'F', 'F'}));
    EXPECT_TRUE(malformed(Wave{"RIFX"}.chunk("fmt ", fmt).chunk("data", ramp(4)).bytes()));
    EXPECT_TRUE(malformed(Wave{}.chunk("data", ramp(4)).bytes()));
    EXPECT_TRUE(malformed(Wave{}.chunk("fmt ", fmt).bytes()));
    EXPECT_TRUE(malformed(Wave{}.chunk("fmt ", Bytes{1, 0, 2, 0}).chunk("data", ramp(4)).bytes()));
    EXPECT_TRUE(malformed(Wave{}.chunk("fmt ", fmt, 400).chunk("data", ramp(4)).bytes()));
    EXPECT_TRUE(malformed(Wave{}.chunk("fmt ", fmt).chunk("data", ramp(4)).bytes(0xFFFFFFFFU)));
    EXPECT_TRUE(malformed(Wave{"RF64"}.chunk("fmt ", fmt).chunk("data", ramp(4)).bytes(0xFFFFFFFFU)));
    EXPECT_TRUE(malformed(Wave{"RF64"}.chunk("ds64", Bytes(12)).chunk("fmt ", fmt).chunk("data", ramp(4)).bytes()));
    EXPECT_TRUE(malformed(Wave{}.chunk("fmt ", fmt_body(1, 0, 48000, 16)).chunk("data", ramp(4)).bytes()));
    EXPECT_TRUE(malformed(Wave{}.chunk("fmt ", fmt_body(1, 2, 0, 16)).chunk("data", ramp(4)).bytes()));
    auto misaligned = fmt;
    misaligned.at(12) = 3;
    EXPECT_TRUE(malformed(Wave{}.chunk("fmt ", misaligned).chunk("data", ramp(4)).bytes()));
    EXPECT_TRUE(malformed(Wave{}.chunk("fmt ", fmt).chunk("data", ramp(4)).chunk("data", ramp(4)).bytes()));
    EXPECT_TRUE(malformed(Wave{}.chunk("bext", Bytes(100)).chunk("fmt ", fmt).chunk("data", ramp(4)).bytes()));
    EXPECT_TRUE(malformed(Wave{}.chunk("fmt ", fmt).chunk("fmt ", fmt).chunk("data", ramp(4)).bytes()));  // a second fmt
    EXPECT_TRUE(malformed(Wave{}.chunk("fmt ", extensible_body(2, 24, 32, 3, 1)).chunk("data", ramp(12)).bytes()));
    auto short_extension = extensible_body(2, 24, 24, 3, 1);
    short_extension.at(16) = 10;  // cbSize short of WAVEFORMATEXTENSIBLE's 22
    EXPECT_TRUE(malformed(Wave{}.chunk("fmt ", short_extension).chunk("data", ramp(12)).bytes()));
}

TEST(bw64_reader, an_odd_sized_ds64_is_padded)
{
    auto ds64 = ds64_body(0, 16, 4);
    ds64.push_back(0);  // an odd size: the pad byte follows it
    auto const info =
        parse_bw64(Wave{"RF64"}.chunk("ds64", ds64).chunk("fmt ", fmt_body(1, 2, 48000, 16)).chunk("data", ramp(16)).bytes());
    EXPECT_TRUE(info.has_value());
    EXPECT_EQ(info ? info->frames : 0, uint64_t{4});
}

TEST(bw64_reader, a_bw64_writer_file_round_trips)
{
    auto const path = (std::filesystem::temp_directory_path() / "statusbar_bw64_reader_test.wav").string();
    Bw64Writer writer;
    std::array<float, 6> const samples{0.5F, -0.5F, 0.25F, -0.25F, 1.0F, -1.0F};
    EXPECT_TRUE(writer.open(path, {.sample_rate = 48000, .channel_count = 2, .description = "round trip", .time_reference = 4800})
                    .has_value());
    EXPECT_TRUE(writer.write_samples(samples).has_value());
    EXPECT_TRUE(writer.close().has_value());
    auto const bytes = read_file(path);
    std::filesystem::remove(path);
    auto const info = parse(bytes);
    EXPECT_TRUE(info.has_value() && info->bext.has_value());
    auto const bext = info->bext.value_or(Bw64Bext{});
    EXPECT_TRUE(info->format.coding == Bw64Coding::ieee_float);
    EXPECT_EQ(info->frames, 3U);
    EXPECT_EQ(bext.time_reference, 4800U);
    EXPECT_TRUE(bext.description == "round trip");
    EXPECT_EQ(bytes.at(info->data_offset + 3), 0x3FU);  // 0.5F little-endian: 00 00 00 3F
}

TEST(bw64_reader, channel_labels_follow_the_mask)
{
    EXPECT_TRUE(bw64_channel_label(0x3F, 0) == "L");
    EXPECT_TRUE(bw64_channel_label(0x3F, 2) == "C");
    EXPECT_TRUE(bw64_channel_label(0x3F, 3) == "LFE");
    EXPECT_TRUE(bw64_channel_label(0x3F, 5) == "Rs");
    EXPECT_TRUE(bw64_channel_label(0x3F, 6).empty());
    EXPECT_TRUE(bw64_channel_label(0x104, 1) == "Cs");
    EXPECT_TRUE(bw64_channel_label(0, 0).empty());
}

TEST_MAIN(statusbar_audio, audio_bw64_reader_test)
