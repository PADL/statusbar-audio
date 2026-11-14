// Copyright 2026 Jeff Koftinoff <jeff.koftinoff@statusbar.com>
// SPDX-License-Identifier: MIT

// Tests for parse_bw64: a Bw64Writer file read back, hand-built RIFF, extensible,
// RF64 and BW64 files, a data chunk cut short, the metadata chunks, the error
// paths, and the channel labels.

#include "statusbar/audio/audio_bw64_reader.hpp"

#include "statusbar/audio/audio_bw64_writer.hpp"
#include "statusbar/test/test.hpp"

#include <unistd.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

using statusbar::audio::AudioError;
using statusbar::audio::bw64_channel_label;
using statusbar::audio::Bw64Container;
using statusbar::audio::Bw64Sample;
using statusbar::audio::Bw64Writer;
using statusbar::audio::parse_bw64;

namespace {

/// Builds a RIFF-family file octet by octet.
class FileBuilder
{
  public:
    auto u16(uint16_t v) -> FileBuilder&
    {
        bytes_.push_back(static_cast<uint8_t>(v & 0xffU));
        bytes_.push_back(static_cast<uint8_t>(v >> 8));
        return *this;
    }
    auto u32(uint32_t v) -> FileBuilder&
    {
        u16(static_cast<uint16_t>(v & 0xffffU));
        return u16(static_cast<uint16_t>(v >> 16));
    }
    auto u64(uint64_t v) -> FileBuilder&
    {
        u32(static_cast<uint32_t>(v & 0xffffffffU));
        return u32(static_cast<uint32_t>(v >> 32));
    }
    auto fourcc(std::string_view id) -> FileBuilder&
    {
        for (char const c : id) {
            bytes_.push_back(static_cast<uint8_t>(c));
        }
        return *this;
    }
    auto raw(std::vector<uint8_t> const& v) -> FileBuilder&
    {
        bytes_.insert(bytes_.end(), v.begin(), v.end());
        return *this;
    }
    auto zeros(size_t n) -> FileBuilder&
    {
        bytes_.insert(bytes_.end(), n, 0);
        return *this;
    }
    /// A chunk header: id and 32-bit size.
    auto chunk(std::string_view id, uint32_t size) -> FileBuilder& { return fourcc(id).u32(size); }
    /// A 16-octet fmt chunk.
    auto fmt16(uint16_t tag, uint16_t channels, uint32_t rate, uint16_t bits) -> FileBuilder&
    {
        uint16_t const block = static_cast<uint16_t>(channels * (bits / 8));
        return chunk("fmt ", 16).u16(tag).u16(channels).u32(rate).u32(rate * block).u16(block).u16(bits);
    }
    /// A RIFF/WAVE header whose size is patched by bytes().
    auto riff(std::string_view container = "RIFF") -> FileBuilder& { return fourcc(container).u32(0).fourcc("WAVE"); }

    [[nodiscard]] auto bytes() -> std::vector<uint8_t>
    {
        if (bytes_.size() >= 8) {
            uint32_t const size = static_cast<uint32_t>(bytes_.size() - 8);
            bytes_[4] = static_cast<uint8_t>(size & 0xffU);
            bytes_[5] = static_cast<uint8_t>((size >> 8) & 0xffU);
            bytes_[6] = static_cast<uint8_t>((size >> 16) & 0xffU);
            bytes_[7] = static_cast<uint8_t>(size >> 24);
        }
        return bytes_;
    }

  private:
    std::vector<uint8_t> bytes_{};
};

/// Stereo 16-bit PCM frames: frame i holds (i, -i).
auto pcm16_stereo(size_t frames) -> std::vector<uint8_t>
{
    FileBuilder b;
    for (size_t i = 0; i < frames; ++i) {
        b.u16(static_cast<uint16_t>(i)).u16(static_cast<uint16_t>(-static_cast<int>(i)));
    }
    return b.bytes();
}

auto plain_pcm16(size_t frames) -> std::vector<uint8_t>
{
    auto const data = pcm16_stereo(frames);
    FileBuilder b;
    b.riff().fmt16(1, 2, 48000, 16).chunk("data", static_cast<uint32_t>(data.size())).raw(data);
    return b.bytes();
}

auto read_file(std::string const& path) -> std::vector<uint8_t>
{
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

/// A Bw64Writer file of 480 stereo frames at 48 kHz with every bext field set.
auto write_with_the_writer(char const* path) -> bool
{
    Bw64Writer w;
    Bw64Writer::Params const params{
        .sample_rate = 48000,
        .channel_count = 2,
        .description = "reader test",
        .originator = "statusbar",
        .originator_ref = "ref-1",
        .coding_history = "A=PCM,F=48000,W=32,M=stereo\r\n",
        .time_reference = 0x1'0000'0001ULL,
    };
    if (!w.open(path, params)) {
        return false;
    }
    std::vector<float> frames(480 * 2);
    for (size_t i = 0; i < frames.size(); ++i) {
        frames[i] = static_cast<float>(i) / 1000.0F;
    }
    return w.write_samples(frames).has_value() && w.close().has_value();
}

}  // namespace

TEST(bw64_reader, reads_back_what_the_writer_wrote)
{
    char path[] = "/tmp/statusbar_bw64_reader_test_XXXXXX";
    int const fd = mkstemp(path);
    EXPECT_TRUE(fd >= 0);
    close(fd);
    EXPECT_TRUE(write_with_the_writer(path));
    auto const file = read_file(path);
    (void)std::remove(path);

    auto const info = parse_bw64(file);
    EXPECT_TRUE(info.has_value());
    EXPECT_TRUE(info->container == Bw64Container::riff);
    EXPECT_TRUE(info->format.sample == Bw64Sample::float32);
    EXPECT_EQ(info->format.channels, static_cast<uint16_t>(2));
    EXPECT_EQ(info->format.sample_rate, 48000U);
    EXPECT_EQ(info->format.bits_per_sample, static_cast<uint16_t>(32));
    EXPECT_FALSE(info->format.extensible);
    EXPECT_EQ(info->frames, 480U);
    EXPECT_TRUE(info->complete());
    EXPECT_EQ(info->data.size(), size_t{480 * 2 * 4});
    EXPECT_TRUE(info->bext.has_value());
    EXPECT_EQ(info->bext->description, std::string_view{"reader test"});
    EXPECT_EQ(info->bext->originator, std::string_view{"statusbar"});
    EXPECT_EQ(info->bext->originator_reference, std::string_view{"ref-1"});
    EXPECT_EQ(info->bext->time_reference, 0x1'0000'0001ULL);
    EXPECT_EQ(info->bext->origination_date.size(), size_t{10});
    EXPECT_EQ(info->bext->coding_history, std::string_view{"A=PCM,F=48000,W=32,M=stereo\r\n"});
    // The second float of the data is the second sample written, 0.001.
    float second = 0.0F;
    std::memcpy(&second, info->data.data() + 4, sizeof(second));
    EXPECT_TRUE(second > 0.0009F && second < 0.0011F);
}

TEST(bw64_reader, plain_riff_pcm16)
{
    auto const file = plain_pcm16(100);
    auto const info = parse_bw64(file);
    EXPECT_TRUE(info.has_value());
    EXPECT_TRUE(info->format.sample == Bw64Sample::pcm16);
    EXPECT_EQ(info->format.format_tag, static_cast<uint16_t>(1));
    EXPECT_EQ(info->format.block_align, static_cast<uint16_t>(4));
    EXPECT_EQ(info->format.valid_bits, static_cast<uint16_t>(16));
    EXPECT_EQ(info->frames, 100U);
    EXPECT_EQ(info->data_offset, size_t{12 + 8 + 16 + 8});
    EXPECT_TRUE(info->complete());
    EXPECT_FALSE(info->bext.has_value());
    EXPECT_TRUE(info->chna.empty());
    // Frame 3, left channel = 3 as a little-endian u16.
    EXPECT_EQ(info->data[12], static_cast<uint8_t>(3));
    EXPECT_EQ(info->data[13], static_cast<uint8_t>(0));
}

TEST(bw64_reader, extensible_pcm24_resolves_subformat_valid_bits_and_mask)
{
    FileBuilder b;
    b.riff().chunk("fmt ", 40).u16(0xFFFE).u16(6).u32(96000).u32(96000 * 18).u16(18).u16(24);
    b.u16(22).u16(20).u32(0x3F);  // cbSize, valid bits, mask FL FR FC LFE BL BR
    b.u16(1).u16(0).u32(0x00100000).raw({0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71});  // SubFormat: the PCM GUID
    b.chunk("data", 18 * 4).zeros(18 * 4);
    auto const file = b.bytes();
    auto const info = parse_bw64(file);
    EXPECT_TRUE(info.has_value());
    EXPECT_TRUE(info->format.extensible);
    EXPECT_TRUE(info->format.sample == Bw64Sample::pcm24);
    EXPECT_EQ(info->format.format_tag, static_cast<uint16_t>(1));
    EXPECT_EQ(info->format.valid_bits, static_cast<uint16_t>(20));
    EXPECT_EQ(info->format.channel_mask, 0x3FU);
    EXPECT_EQ(info->frames, 4U);
    EXPECT_EQ(bw64_channel_label(info->format.channel_mask, 0), std::string_view{"FL"});
    EXPECT_EQ(bw64_channel_label(info->format.channel_mask, 3), std::string_view{"LFE"});
    EXPECT_EQ(bw64_channel_label(info->format.channel_mask, 5), std::string_view{"BR"});
    EXPECT_TRUE(bw64_channel_label(info->format.channel_mask, 6).empty());
}

TEST(bw64_reader, rf64_and_bw64_take_the_data_size_from_ds64)
{
    for (std::string_view const container : {"RF64", "BW64"}) {
        auto const data = pcm16_stereo(50);
        FileBuilder b;
        b.riff(container);
        b.chunk("ds64", 28).u64(0).u64(data.size()).u64(50).u32(0);
        b.fmt16(1, 2, 44100, 16).chunk("data", 0xFFFFFFFFU).raw(data);
        auto const file = b.bytes();
        auto const info = parse_bw64(file);
        EXPECT_TRUE(info.has_value());
        EXPECT_TRUE(info->container == (container == "RF64" ? Bw64Container::rf64 : Bw64Container::bw64));
        EXPECT_EQ(info->data_size, data.size());
        EXPECT_EQ(info->frames, 50U);
        EXPECT_TRUE(info->complete());
    }
}

TEST(bw64_reader, ds64_table_sizes_another_chunk)
{
    auto const data = pcm16_stereo(10);
    FileBuilder b;
    b.riff("RF64");
    b.chunk("ds64", 28 + 12).u64(0).u64(data.size()).u64(10).u32(1).fourcc("axml").u64(6);
    b.fmt16(1, 2, 48000, 16);
    b.chunk("axml", 0xFFFFFFFFU).raw({'<', 'a', 'd', 'm', '/', '>'});
    b.chunk("data", 0xFFFFFFFFU).raw(data);
    auto const file = b.bytes();
    auto const info = parse_bw64(file);
    EXPECT_TRUE(info.has_value());
    EXPECT_EQ(info->axml.size(), size_t{6});
    EXPECT_EQ(info->frames, 10U);
}

TEST(bw64_reader, a_data_chunk_cut_short_is_read_as_far_as_it_goes)
{
    auto file = plain_pcm16(100);    // 400 data octets declared
    file.resize(file.size() - 150);  // 250 left: 62 whole frames and a half
    auto const info = parse_bw64(file);
    EXPECT_TRUE(info.has_value());
    EXPECT_FALSE(info->complete());
    EXPECT_EQ(info->data_size, 400U);
    EXPECT_EQ(info->data.size(), size_t{250});
    EXPECT_EQ(info->frames, 62U);
}

TEST(bw64_reader, a_streamed_riff_data_size_runs_to_the_end)
{
    auto const data = pcm16_stereo(20);
    FileBuilder b;
    b.riff().fmt16(1, 2, 48000, 16).chunk("data", 0xFFFFFFFFU).raw(data);
    auto const file = b.bytes();
    auto const info = parse_bw64(file);
    EXPECT_TRUE(info.has_value());
    EXPECT_EQ(info->frames, 20U);
    EXPECT_TRUE(info->complete());
}

TEST(bw64_reader, metadata_chunks_are_returned_raw_and_odd_sizes_are_padded)
{
    auto const data = pcm16_stereo(5);
    FileBuilder b;
    b.riff().fmt16(1, 2, 48000, 16);
    b.chunk("chna", 3).raw({1, 2, 3}).zeros(1);  // odd size, one pad octet
    b.chunk("iXML", 5).raw({'<', 'x', '/', '>', '!'}).zeros(1);
    b.chunk("JUNK", 4).zeros(4);
    b.chunk("data", static_cast<uint32_t>(data.size())).raw(data);
    auto const file = b.bytes();
    auto const info = parse_bw64(file);
    EXPECT_TRUE(info.has_value());
    EXPECT_EQ(info->chna.size(), size_t{3});
    EXPECT_EQ(info->chna[2], static_cast<uint8_t>(3));
    EXPECT_EQ(info->ixml.size(), size_t{5});
    EXPECT_EQ(info->frames, 5U);
}

TEST(bw64_reader, chunks_after_the_data_are_still_read)
{
    auto const data = pcm16_stereo(5);
    FileBuilder b;
    b.riff().fmt16(1, 2, 48000, 16).chunk("data", static_cast<uint32_t>(data.size())).raw(data);
    b.chunk("bext", 602).zeros(602);
    auto const file = b.bytes();
    auto const info = parse_bw64(file);
    EXPECT_TRUE(info.has_value());
    EXPECT_TRUE(info->bext.has_value());
    EXPECT_TRUE(info->bext->description.empty());
    EXPECT_EQ(info->bext->umid.size(), size_t{64});
}

TEST(bw64_reader, malformed_structures)
{
    auto const malformed = statusbar::audio::make_error_code(AudioError::MalformedFile);
    std::vector<uint8_t> const not_riff{'R', 'I', 'F', 'X', 0, 0, 0, 0, 'W', 'A', 'V', 'E'};
    EXPECT_EQ(parse_bw64(not_riff).error(), malformed);
    std::vector<uint8_t> const short_file{'R', 'I', 'F'};
    EXPECT_EQ(parse_bw64(short_file).error(), malformed);

    std::vector<FileBuilder> cases(7);
    cases[0].riff().chunk("data", 4).zeros(4);                                              // no fmt
    cases[1].riff().fmt16(1, 2, 48000, 16);                                                 // no data
    cases[2].riff().chunk("fmt ", 10).zeros(10).chunk("data", 0);                           // fmt too short
    cases[3].riff().fmt16(1, 2, 48000, 16).chunk("bext", 10000).zeros(10);                  // a chunk overrunning the span
    cases[4].riff().fmt16(1, 2, 48000, 16).chunk("bext", 100).zeros(100).chunk("data", 0);  // bext too short
    cases[5].riff().chunk("fmt ", 16).u16(1).u16(2).u32(48000).u32(48000 * 4).u16(3).u16(16).chunk("data", 0);  // block_align wrong
    cases[6].riff().fmt16(1, 0, 48000, 16).chunk("data", 0);                                                    // zero channels
    for (auto& c : cases) {
        auto const file = c.bytes();
        EXPECT_EQ(parse_bw64(file).error(), malformed);
    }
}

TEST(bw64_reader, unsupported_sample_formats)
{
    auto const unsupported = statusbar::audio::make_error_code(AudioError::UnsupportedFormat);
    for (auto const [tag, bits] : std::array<std::array<uint16_t, 2>, 5>{{{1, 8}, {3, 64}, {6, 8}, {7, 8}, {0x55, 16}}}) {
        FileBuilder b;
        b.riff().fmt16(tag, 1, 8000, bits).chunk("data", 0);
        auto const file = b.bytes();
        EXPECT_EQ(parse_bw64(file).error(), unsupported);
    }
    FileBuilder b;  // an extensible SubFormat outside the KSDATAFORMAT family
    b.riff().chunk("fmt ", 40).u16(0xFFFE).u16(2).u32(48000).u32(48000 * 4).u16(4).u16(16).u16(22).u16(16).u32(3);
    b.u16(1).u16(0).u32(0).raw({0, 0, 0, 0, 0, 0, 0, 0}).chunk("data", 0);
    auto const file = b.bytes();
    EXPECT_EQ(parse_bw64(file).error(), unsupported);
}

TEST(bw64_reader, channel_labels_follow_the_set_bits)
{
    EXPECT_EQ(bw64_channel_label(0x3U, 0), std::string_view{"FL"});
    EXPECT_EQ(bw64_channel_label(0x3U, 1), std::string_view{"FR"});
    EXPECT_TRUE(bw64_channel_label(0x3U, 2).empty());
    EXPECT_EQ(bw64_channel_label(0x4U | 0x400U, 1), std::string_view{"SR"});  // FC then SR
    EXPECT_EQ(bw64_channel_label(1U << 17U, 0), std::string_view{"TBC"});
    EXPECT_TRUE(bw64_channel_label(1U << 18U, 0).empty());  // a bit with no name
    EXPECT_TRUE(bw64_channel_label(0, 0).empty());
}

//
// Test Runner
//

TEST_MAIN(statusbar_audio, audio_bw64_reader_test)
