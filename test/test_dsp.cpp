// Ember — DSP tests
// SPDX-License-Identifier: GPL-3.0-or-later
#include <cmath>
#include <cstdio>
#include <vector>

#include "ember_strip.hpp"
#include "macro.hpp"
#include "params.hpp"

using namespace ember;

static int g_failures = 0;

static void check(bool ok, const char* what) {
  if (!ok) {
    ++g_failures;
    printf("FAIL: %s\n", what);
  }
}

static std::vector<float> renderTone(float amount01, float freq, float amp,
                                     uint32_t frames, bool enabled = true,
                                     float sr = 48000.0f) {
  EmberStrip strip;
  strip.prepare(sr);
  Params p;
  p.amount = amount01;
  p.enabled = enabled;
  strip.setTargets(p);
  strip.reset();

  std::vector<float> left(frames);
  std::vector<float> right(frames);
  for (uint32_t i = 0; i < frames; ++i) {
    const float x =
        amp * std::sin(2.0f * kPi * freq * (static_cast<float>(i) / sr));
    left[i] = x;
    right[i] = x;
  }
  float gr = 0.0f;
  float out = -60.0f;
  strip.process(left.data(), right.data(), left.data(), right.data(), frames,
                gr, out);
  return left;
}

static float peakFrom(const std::vector<float>& buf, size_t start) {
  float peak = 0.0f;
  for (size_t i = start; i < buf.size(); ++i) {
    peak = std::max(peak, std::fabs(buf[i]));
  }
  return peak;
}

static float dftMag(const std::vector<float>& buf, size_t start, float freq,
                    float sr) {
  double re = 0.0;
  double im = 0.0;
  const size_t n = buf.size() - start;
  for (size_t i = 0; i < n; ++i) {
    const double phase =
        2.0 * 3.14159265358979323846 * freq * (static_cast<double>(i) / sr);
    re += buf[start + i] * std::cos(phase);
    im -= buf[start + i] * std::sin(phase);
  }
  return static_cast<float>(std::sqrt(re * re + im * im) / n);
}

int main() {
  printf("Ember DSP tests\n");

  check(std::fabs(dbToGain(0.0f) - 1.0f) < 1.0e-6f, "dbToGain(0) == 1");
  check(std::fabs(gainToDb(1.0f)) < 1.0e-3f, "gainToDb(1) == 0");

  // The macro must be completely neutral at Amount 0.
  {
    const MacroSettings m = macroFor(0.0f);
    check(m.lowGainDb == 0.0f, "macro low gain is neutral at 0");
    check(m.presenceGainDb == 0.0f, "macro presence is neutral at 0");
    check(m.airGainDb == 0.0f, "macro air is neutral at 0");
    check(!m.compOn, "macro compressor is off at 0");
    check(m.satMix == 0.0f, "macro harmonics are off at 0");
    check(m.outputGainDb == 0.0f, "macro output gain is neutral at 0");
  }

  // Amount 0 is a mathematically transparent path.
  {
    constexpr uint32_t n = 48000;
    std::vector<float> input(n);
    for (uint32_t i = 0; i < n; ++i) {
      input[i] = 0.31f * std::sin(2.0f * kPi * 997.0f * (i / 48000.0f));
    }
    EmberStrip strip;
    strip.prepare(48000.0f);
    Params p;
    p.amount = 0.0f;
    p.enabled = true;
    strip.setTargets(p);
    strip.reset();
    std::vector<float> left = input;
    std::vector<float> right = input;
    float gr = 0.0f;
    float out = -60.0f;
    strip.process(left.data(), right.data(), left.data(), right.data(), n, gr,
                  out);
    float maxError = 0.0f;
    for (uint32_t i = 0; i < n; ++i) {
      maxError = std::max(maxError, std::fabs(left[i] - input[i]));
    }
    check(maxError < 1.0e-7f, "amount 0 is transparent");
    check(gr == 0.0f, "amount 0 reports no gain reduction");
  }

  // Low-end weight.
  {
    constexpr uint32_t n = 48000;
    const auto dry = renderTone(0.0f, 40.0f, 0.05f, n);
    const auto wet = renderTone(0.82f, 40.0f, 0.05f, n);
    const float dryPeak = peakFrom(dry, 24000);
    const float wetPeak = peakFrom(wet, 24000);
    check(wetPeak > dryPeak * 1.25f, "amount adds low-end weight");
  }

  // Upper-mid presence.
  {
    constexpr uint32_t n = 48000;
    const auto dry = renderTone(0.0f, 3400.0f, 0.05f, n);
    const auto wet = renderTone(0.82f, 3400.0f, 0.05f, n);
    const float dryPeak = peakFrom(dry, 24000);
    const float wetPeak = peakFrom(wet, 24000);
    check(wetPeak > dryPeak * 1.10f, "amount adds upper-mid presence");
  }

  // Air.
  {
    constexpr uint32_t n = 48000;
    const auto dry = renderTone(0.0f, 13000.0f, 0.05f, n);
    const auto wet = renderTone(0.95f, 13000.0f, 0.05f, n);
    const float dryPeak = peakFrom(dry, 24000);
    const float wetPeak = peakFrom(wet, 24000);
    check(wetPeak > dryPeak * 1.10f, "amount adds air");
  }

  // Glue compression on a loud tone.
  {
    constexpr uint32_t n = 48000;
    EmberStrip strip;
    strip.prepare(48000.0f);
    Params p;
    p.amount = 0.82f;
    p.enabled = true;
    strip.setTargets(p);
    strip.reset();

    std::vector<float> left(n);
    std::vector<float> right(n);
    for (uint32_t i = 0; i < n; ++i) {
      const float x = 0.5f * std::sin(2.0f * kPi * 1000.0f * (i / 48000.0f));
      left[i] = x;
      right[i] = x;
    }
    float gr = 0.0f;
    float out = -60.0f;
    strip.process(left.data(), right.data(), left.data(), right.data(), n, gr,
                  out);
    check(gr < -1.0f, "amount engages glue compression");
    const float wetPeak = peakFrom(left, 24000);
    check(wetPeak < 0.45f, "glue reduces a loud peak");
    check(std::isfinite(out), "output meter is finite");
  }

  // Harmonic generation.
  {
    constexpr uint32_t n = 48000;
    const auto clean = renderTone(0.0f, 1000.0f, 0.20f, n);
    const auto warm = renderTone(0.95f, 1000.0f, 0.20f, n);
    const float cleanH2 = dftMag(clean, 4800, 2000.0f, 48000.0f) /
                          std::max(dftMag(clean, 4800, 1000.0f, 48000.0f),
                                   1.0e-9f);
    const float warmH2 = dftMag(warm, 4800, 2000.0f, 48000.0f) /
                         std::max(dftMag(warm, 4800, 1000.0f, 48000.0f),
                                  1.0e-9f);
    check(cleanH2 < 1.0e-3f, "amount 0 is harmonically clean");
    check(warmH2 > 2.0e-3f, "amount adds even harmonics");
    check(warmH2 > cleanH2 * 5.0f, "harmonics rise with amount");
  }

  // Bypass is a clean dry path even at a high amount.
  {
    constexpr uint32_t n = 4800;
    const auto dry = renderTone(0.9f, 1000.0f, 0.5f, n, false);
    const auto ref = renderTone(0.0f, 1000.0f, 0.5f, n, true);
    float maxError = 0.0f;
    for (uint32_t i = 0; i < n; ++i) {
      maxError = std::max(maxError, std::fabs(dry[i] - ref[i]));
    }
    check(maxError < 1.0e-7f, "bypass is a clean dry path");
  }

  // Hidden output clipper.
  {
    constexpr uint32_t n = 4800;
    std::vector<float> left(n);
    std::vector<float> right(n);
    for (uint32_t i = 0; i < n; ++i) {
      left[i] = 2.0f;
      right[i] = -2.0f;
    }
    EmberStrip strip;
    strip.prepare(48000.0f);
    Params p;
    p.amount = 0.0f;
    p.enabled = true;
    strip.setTargets(p);
    strip.reset();
    float gr = 0.0f;
    float out = -60.0f;
    strip.process(left.data(), right.data(), left.data(), right.data(), n, gr,
                  out);
    check(peakFrom(left, 0) <= 1.024f, "hidden clipper ceiling");
    check(peakFrom(left, 0) > 0.95f, "hidden clipper does not mute");
    check(out <= 0.3f, "output meter respects clipper ceiling");
  }

  // Bypass skips the clipper.
  {
    constexpr uint32_t n = 4800;
    std::vector<float> left(n, 2.0f);
    std::vector<float> right(n, 2.0f);
    EmberStrip strip;
    strip.prepare(48000.0f);
    Params p;
    p.amount = 0.0f;
    p.enabled = false;
    strip.setTargets(p);
    strip.reset();
    float gr = 0.0f;
    float out = -60.0f;
    strip.process(left.data(), right.data(), left.data(), right.data(), n, gr,
                  out);
    check(std::fabs(left[n - 1] - 2.0f) < 1.0e-6f,
          "bypass skips the output clipper");
  }

  // Extreme settings stay finite at several sample rates.
  for (const float sr : {44100.0f, 48000.0f, 96000.0f, 192000.0f}) {
    EmberStrip strip;
    strip.prepare(sr);
    Params p;
    p.amount = 1.0f;
    p.enabled = true;
    strip.setTargets(p);
    strip.reset();

    const uint32_t frames = 4096;
    std::vector<float> left(frames);
    std::vector<float> right(frames);
    uint32_t seed = 12345;
    for (uint32_t i = 0; i < frames; ++i) {
      seed = seed * 1103515245u + 12345u;
      const float noise =
          static_cast<float>(static_cast<int>((seed >> 16) & 0x7fffu) - 4096) /
          4096.0f;
      left[i] = noise * 1.4f;
      right[i] = -noise * 1.4f;
    }
    float gr = 0.0f;
    float out = -60.0f;
    strip.process(left.data(), right.data(), left.data(), right.data(), frames,
                  gr, out);
    bool finite = std::isfinite(gr) && std::isfinite(out);
    for (uint32_t i = 0; i < frames; ++i) {
      finite = finite && std::isfinite(left[i]) && std::isfinite(right[i]);
    }
    check(finite, "extreme chain stays finite at all sample rates");
  }

  if (g_failures == 0) {
    printf("ALL DSP TESTS PASSED\n");
  }
  return g_failures == 0 ? 0 : 1;
}
