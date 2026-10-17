// Smoke test for yoai_aec3 (the C-ABI bridge over WebRTC AEC3).
//
// Synthesises a far-end signal, plays it through a made-up echo path (50 ms delay plus a
// few reflections) into the "microphone", runs both through the library in awkward chunk
// sizes, and checks that the echo goes away and that near-end speech survives.
//
// It is a smoke test, not a benchmark: the thresholds are loose on purpose, and the numbers
// it prints are the useful part. Build and run from an x64 Native Tools prompt:
//
//   cd /d D:\Projects\yoai\third
//   cl /nologo /O2 /EHsc /Iaec3_bridge aec3_bridge\smoke_test.cpp ^
//      /Fe:dist\yoai_aec3\win-x64\smoke_test.exe /Fo:dist\yoai_aec3\win-x64\ ^
//      /link dist\yoai_aec3\win-x64\yoai_aec3.dll.lib
//   dist\yoai_aec3\win-x64\smoke_test.exe
//
// (the DLL has to sit next to the .exe, which is why both go in the same directory)

#include <cmath>
#include <cstdio>
#include <vector>

#include "yoai_aec3.h"

namespace {

const double kPi = 3.14159265358979323846;
const int kRate = 16000;

unsigned g_rng = 12345u;
float Noise() {
  g_rng = g_rng * 1664525u + 1013904223u;
  return static_cast<float>((g_rng >> 8) & 0xFFFF) / 32768.0f - 1.0f;
}

double MeanPower(const std::vector<float>& v, size_t from, size_t to) {
  if (to > v.size()) to = v.size();
  if (from >= to) return 0.0;
  double s = 0.0;
  for (size_t i = from; i < to; ++i) s += double(v[i]) * double(v[i]);
  return s / double(to - from);
}

double Db(double ratio) { return 10.0 * std::log10(ratio); }

int g_failed = 0;
void Check(bool ok, const char* what) {
  std::printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
  if (!ok) ++g_failed;
}

}  // namespace

int main() {
  std::printf("API:\n");
  Check(yoai_aec3_create(8000) == NULL, "an unsupported rate (8000) gives NULL");
  yoai_aec3_destroy(NULL);
  Check(yoai_aec3_process(NULL, NULL, NULL, 0, NULL, 0) == 0, "process on a NULL handle returns 0");

  yoai_aec3* aec = yoai_aec3_create(kRate);
  Check(aec != NULL, "create(16000)");
  if (!aec) return 1;
  Check(yoai_aec3_frame_samples(aec) == 160, "a frame is 160 samples (10 ms)");

  // Ten seconds. Far end: low-passed noise under a 3 Hz envelope, so it is not stationary.
  // Echo: 50 ms delay, a direct path and two reflections. Near-end speech stand-in: a
  // 440 Hz tone, from 6 s to 8 s, on top of the echo.
  const size_t n = 10u * kRate;
  const size_t delay = 800;  // 50 ms
  std::vector<float> ref(n), mic(n);
  float lp = 0.0f;
  for (size_t i = 0; i < n; ++i) {
    const double t = double(i) / kRate;
    const double env = 0.5 * (1.0 + std::sin(2.0 * kPi * 3.0 * t));
    lp += 0.35f * (Noise() - lp);
    ref[i] = float(env) * lp * 1.5f;
  }
  for (size_t i = 0; i < n; ++i) {
    double echo = 0.0;
    if (i >= delay) echo += 0.6 * ref[i - delay];
    if (i >= delay + 40) echo += 0.2 * ref[i - delay - 40];
    if (i >= delay + 90) echo -= 0.1 * ref[i - delay - 90];
    const bool near_on = i >= 6u * kRate && i < 8u * kRate;
    const double near_end = near_on ? 0.1 * std::sin(2.0 * kPi * 440.0 * double(i) / kRate) : 0.0;
    mic[i] = float(echo + near_end);
  }

  // Feed it in chunk sizes that are not frame multiples, to exercise the buffering.
  std::printf("\nProcessing %d s in odd-sized chunks...\n", int(n / kRate));
  static const size_t kChunks[] = {37, 160, 1, 333, 480};
  std::vector<float> out;
  std::vector<float> buf(n + 160);
  size_t pos = 0, c = 0;
  while (pos < n) {
    size_t k = kChunks[c++ % 5];
    if (k > n - pos) k = n - pos;
    const size_t got = yoai_aec3_process(aec, &mic[pos], &ref[pos], k, &buf[0], buf.size());
    out.insert(out.end(), buf.begin(), buf.begin() + got);
    pos += k;
  }
  std::printf("  %llu samples in, %llu out\n", (unsigned long long)n, (unsigned long long)out.size());
  Check(out.size() + 160 > n && out.size() <= n, "output trails the input by less than one frame");

  // Echo only, after the filter has had time to converge.
  const double erle = Db(MeanPower(mic, 4u * kRate, 6u * kRate) / MeanPower(out, 4u * kRate, 6u * kRate));
  std::printf("\nEcho only, 4-6 s: microphone %.1f dB, output %.1f dB re full scale -> %.1f dB removed\n",
              Db(MeanPower(mic, 4u * kRate, 6u * kRate)), Db(MeanPower(out, 4u * kRate, 6u * kRate)), erle);
  Check(erle >= 10.0, "at least 10 dB of echo removed after convergence");

  // Near-end tone over the echo: it must come through, neither erased nor amplified.
  const double near_power = 0.1 * 0.1 / 2.0;
  const double kept = Db(MeanPower(out, 6500u * kRate / 1000u, 8u * kRate) / near_power);
  std::printf("Near-end tone, 6.5-8 s: output is %+.1f dB relative to the tone alone\n", kept);
  Check(kept > -6.0 && kept < 6.0, "near-end speech is preserved (within +-6 dB)");

  yoai_aec3_metrics m;
  if (yoai_aec3_get_metrics(aec, &m)) {
    std::printf("\nAEC3 metrics: ERL %.1f dB, ERLE %.1f dB, delay %d ms (the true delay is 50 ms)\n",
                m.erl_db, m.erle_db, m.delay_ms);
  }

  yoai_aec3_destroy(aec);
  std::printf("\n%s\n", g_failed ? "FAILED" : "PASSED");
  return g_failed ? 1 : 0;
}
