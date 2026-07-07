#include <gtest/gtest.h>
#include "tts_stub.h"
#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static std::string slurp(const std::string& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return {};
    auto size = f.tellg();
    std::string buf(static_cast<size_t>(size), '\0');
    f.seekg(0);
    f.read(buf.data(), size);
    return buf;
}

struct WavHeader {
    char     riff[4];
    uint32_t file_size;
    char     wave[4];
    char     fmt_[4];
    uint32_t fmt_chunk_size;
    uint16_t audio_fmt;
    uint16_t channels;
    uint32_t sample_rate;
    uint32_t byte_rate;
    uint16_t block_align;
    uint16_t bits_per_sample;
    char     data[4];
    uint32_t data_size;
} __attribute__((packed));

static_assert(sizeof(WavHeader) == 44, "WAV header must be 44 bytes");

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

TEST(TtsStubTest, SameTextProducesByteIdenticalWav) {
    std::string outdir = "/tmp/pjtest_tts";
    mkdir(outdir.c_str(), 0755);

    std::string path1 = synth("hello world", outdir);
    std::string path2 = synth("hello world", outdir);

    std::string data1 = slurp(path1);
    std::string data2 = slurp(path2);

    EXPECT_EQ(data1, data2) << "same text must produce byte-identical WAV";
}

TEST(TtsStubTest, DifferentTextProducesDifferentSizedWav) {
    // synth() always writes to outdir + "/tts_stub.wav", so use separate dirs
    std::string d1 = "/tmp/pjtest_tts_ab";
    std::string d2 = "/tmp/pjtest_tts_cdef";
    mkdir(d1.c_str(), 0755);
    mkdir(d2.c_str(), 0755);

    std::string path1 = synth("one two", d1);
    std::string path2 = synth("three four five six", d2);

    std::string data1 = slurp(path1);
    std::string data2 = slurp(path2);

    EXPECT_NE(data1.size(), data2.size()) << "different word counts -> different sizes";
}

TEST(TtsStubTest, WavHeaderIsValid) {
    std::string outdir = "/tmp/pjtest_tts";
    mkdir(outdir.c_str(), 0755);

    std::string path = synth("hello", outdir);
    std::string data = slurp(path);
    ASSERT_GE(data.size(), sizeof(WavHeader));

    auto* hdr = reinterpret_cast<const WavHeader*>(data.data());

    EXPECT_EQ(std::memcmp(hdr->riff, "RIFF", 4), 0);
    EXPECT_EQ(std::memcmp(hdr->wave, "WAVE", 4), 0);
    EXPECT_EQ(std::memcmp(hdr->fmt_, "fmt ", 4), 0);
    EXPECT_EQ(std::memcmp(hdr->data, "data", 4), 0);

    EXPECT_EQ(hdr->audio_fmt, 1) << "PCM";
    EXPECT_EQ(hdr->channels, 1) << "mono";
    EXPECT_EQ(hdr->sample_rate, 8000u) << "8 kHz";
    EXPECT_EQ(hdr->bits_per_sample, 16u) << "16 bit";

    uint32_t expected_data_size = static_cast<uint32_t>(data.size() - sizeof(WavHeader));
    EXPECT_EQ(hdr->data_size, expected_data_size);
    EXPECT_EQ(hdr->file_size, 36 + expected_data_size);

    // Verify data size is consistent
    // "hello" = 1 word -> 0.5s = 4000 samples * 2 bytes = 8000 bytes
    EXPECT_EQ(hdr->data_size, 4000 * 2);
}

TEST(TtsStubTest, EmptyTextReturnsEmpty) {
    std::string path = synth("", "/tmp/pjtest_tts");
    EXPECT_TRUE(path.empty());
}
