#include "tts_stub.h"
#include <cstdint>
#include <cmath>
#include <fstream>
#include <sstream>
#include <vector>
#include <sys/stat.h>
#include <sys/types.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// ---------------------------------------------------------------------------
// Minimal WAV writer: 8 kHz mono PCM16
// ---------------------------------------------------------------------------

static void writeWav(const std::string& path, const std::vector<int16_t>& samples) {
    std::ofstream f(path, std::ios::binary);
    uint32_t data_bytes = static_cast<uint32_t>(samples.size()) * 2;
    uint32_t sample_rate = 8000;

    auto write32 = [&](uint32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); };
    auto write16 = [&](uint16_t v) { f.write(reinterpret_cast<const char*>(&v), 2); };

    f.write("RIFF", 4);
    write32(36 + data_bytes);
    f.write("WAVE", 4);
    f.write("fmt ", 4);
    write32(16);                                // chunk size
    write16(1);                                 // PCM
    write16(1);                                 // mono
    write32(sample_rate);                       // sample rate
    write32(sample_rate * 2);                   // byte rate
    write16(2);                                 // block align
    write16(16);                                // bits per sample
    f.write("data", 4);
    write32(data_bytes);
    f.write(reinterpret_cast<const char*>(samples.data()), data_bytes);
}

// ---------------------------------------------------------------------------
// synth: text -> WAV file path
// ---------------------------------------------------------------------------

std::string synth(const std::string& text, const std::string& outdir) {
    // Ensure outdir exists
    mkdir(outdir.c_str(), 0755);

    // Count words
    std::istringstream iss(text);
    std::vector<std::string> words;
    std::string w;
    while (iss >> w) words.push_back(w);

    if (words.empty()) return "";

    // Generate: 0.5s of 440Hz sine per word, 0.2s silence between words
    constexpr int sample_rate = 8000;
    constexpr int samples_per_word = sample_rate / 2;   // 0.5s
    constexpr int samples_pause   = sample_rate / 5;     // 0.2s
    constexpr double two_pi = 2.0 * M_PI;

    std::vector<int16_t> samples;
    for (size_t i = 0; i < words.size(); ++i) {
        // (void)words[i]; // don't vary output by content — deterministic per word count
        for (int j = 0; j < samples_per_word; ++j) {
            double t = static_cast<double>(j) / sample_rate;
            double val = sin(two_pi * 440.0 * t);
            samples.push_back(static_cast<int16_t>(val * 16000.0));
        }
        if (i + 1 < words.size()) {
            samples.insert(samples.end(), samples_pause, 0);
        }
    }

    std::string outpath = outdir + "/tts_stub.wav";
    writeWav(outpath, samples);
    return outpath;
}
