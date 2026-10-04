// Fuzz Factory tests
//
// The engine (FuzzFactory.h) is a port of the Helix desktop synth's
// fuzz_factory.rs. FuzzFactoryTest is that file's test module, one test per
// Rust test with the same inputs and thresholds, so the port is held to the
// spec's own behaviour: the Face fuzzes and sustains, Stab down self-starts
// a howl from silence, Comp climbs it from motorboat to squeal, Stab CW
// kills it, Gate chokes the tail, and every knob is audible.
//
// FuzzStageTest and ProcessorTest cover the faceplate stage around it: off
// by default and a bit-exact passthrough even with knobs dialled, power
// fuzzes the output, the power-off blend lands back on a passthrough, the
// parameters round-trip through state and presets, a state saved before the
// stage existed lands it off, and the Helix CC layout maps onto it.
#include "FuzzFactory.h"
#include "Processor.h"
#include "test_helpers.h"

#include <gtest/gtest.h>
#include <juce_events/juce_events.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace {

constexpr float kSr = 48000.0f;
constexpr int kBlock = 512;
constexpr float kPiF = 3.14159265358979323846f;
using Knobs = FuzzFactory::Knobs;

std::vector<float> render(FuzzFactory& ff, size_t n, const std::function<float(size_t)>& input) {
  std::vector<float> y(n);
  for (size_t i = 0; i < n; ++i)
    y[i] = ff.process(input(i));
  return y;
}

std::vector<float> render(const Knobs& k, size_t n, const std::function<float(size_t)>& input) {
  FuzzFactory ff(kSr);
  ff.setKnobs(k);
  return render(ff, n, input);
}

std::function<float(size_t)> sineAt(float hz, float amp) {
  return [hz, amp](size_t i) { return std::sin(2.0f * kPiF * hz * static_cast<float>(i) / kSr) * amp; };
}

std::function<float(size_t)> pulse(size_t from, size_t to, float level) {
  return [=](size_t i) { return i >= from && i < to ? level : 0.0f; };
}

float silence(size_t) {
  return 0.0f;
}

float rms(const std::vector<float>& x, size_t from, size_t to) {
  if (to <= from) return 0.0f;
  float s = 0.0f;
  for (size_t i = from; i < to; ++i)
    s += x[i] * x[i];
  return std::sqrt(s / static_cast<float>(to - from));
}
float rmsTail(const std::vector<float>& x, size_t n) {
  return rms(x, x.size() - n, x.size());
}

float crest(const std::vector<float>& x, size_t from, size_t to) {
  const float r = rms(x, from, to);
  if (r < 1.0e-6f) return 0.0f;
  float peak = 0.0f;
  for (size_t i = from; i < to; ++i)
    peak = std::max(peak, std::abs(x[i]));
  return peak / r;
}

float goertzel(const std::vector<float>& x, size_t from, size_t to, float hz) {
  const float w = 2.0f * kPiF * hz / kSr;
  const float c = 2.0f * std::cos(w);
  float s1 = 0.0f, s2 = 0.0f;
  for (size_t i = from; i < to; ++i) {
    const float s0 = x[i] + c * s1 - s2;
    s2 = s1;
    s1 = s0;
  }
  const float re = s1 - s2 * std::cos(w);
  const float im = s2 * std::sin(w);
  return std::sqrt(re * re + im * im) / static_cast<float>(to - from);
}

float zcr(const std::vector<float>& x, size_t from, size_t to) {
  float n = 0.0f;
  for (size_t i = from + 1; i < to; ++i) {
    if (x[i] == 0.0f || x[i - 1] == 0.0f) continue;
    if (std::signbit(x[i]) != std::signbit(x[i - 1])) n += 1.0f;
  }
  return n * 0.5f * kSr / static_cast<float>(to - from);
}

std::vector<float> howl(float comp, float stab) {
  return render(Knobs{0.7f, 0.05f, comp, 0.8f, stab}, 14000, pulse(80, 200, 0.25f));
}

}  // namespace

TEST(FuzzFactoryTest, SineGetsFuzzed) {
  const auto y = render(Knobs::smooth(), 8000, sineAt(110.0f, 0.35f));
  const size_t from = y.size() - 2048;
  EXPECT_GT(rms(y, from, y.size()), 0.08f);
  float c1 = 0.0f, c3 = 0.0f;
  for (size_t i = from; i < y.size(); ++i) {
    const float p = 2.0f * kPiF * 110.0f * static_cast<float>(i - from) / kSr;
    c1 += y[i] * std::sin(p);
    c3 += y[i] * std::sin(3.0f * p);
  }
  EXPECT_GT(std::abs(c3), std::abs(c1) * 0.08f) << "need harmonics h1=" << c1 << " h3=" << c3;
}

TEST(FuzzFactoryTest, SmoothStaysOnForTheWholeNote) {
  const auto y = render(Knobs::smooth(), 12000, sineAt(146.8f, 0.4f));
  const float mid = rms(y, 3000, 5000);
  const float late = rms(y, 10000, 12000);
  EXPECT_GT(mid, 0.08f);
  EXPECT_GT(late, mid * 0.75f) << "sustain died";
}

TEST(FuzzFactoryTest, StabUpIsQuietWithNoInput) {
  EXPECT_LT(rmsTail(render(Knobs::smooth(), 4000, silence), 1024), 0.02f);
}

TEST(FuzzFactoryTest, HowlStartsFromAbsoluteSilence) {
  EXPECT_GT(rmsTail(render(Knobs::squeal(), 16000, silence), 4096), 0.05f)
      << "must self-start without a key tap";
}

TEST(FuzzFactoryTest, StabDownSquealsWithNoInput) {
  const auto y = render(Knobs::squeal(), 12000, pulse(80, 120, 0.3f));
  const size_t from = y.size() - 4096;
  float peak = 0.0f;
  for (size_t i = from; i < y.size(); ++i)
    peak = std::max(peak, std::abs(y[i]));
  EXPECT_GT(rms(y, from, y.size()), 0.05f);
  EXPECT_GT(peak, 0.10f);
}

TEST(FuzzFactoryTest, SquealIsDirtyNotASine) {
  const auto y = render(Knobs::squeal(), 16000, pulse(80, 160, 0.25f));
  EXPECT_GT(crest(y, y.size() - 8192, y.size()), 1.25f);
}

TEST(FuzzFactoryTest, VelcroSplatsOnTheDecay) {
  const auto burst = [](size_t i) {
    return i < 4000 ? std::sin(2.0f * kPiF * 98.0f * static_cast<float>(i) / kSr) * 0.55f : 0.0f;
  };
  const auto ys = render(Knobs::smooth(), 10000, burst);
  const auto yv = render(Knobs::velcro(), 10000, burst);
  const float cs = crest(ys, 5000, ys.size());
  const float cv = crest(yv, 5000, yv.size());
  EXPECT_TRUE(cv > cs * 1.05f || cv > 1.8f) << "velcro crest=" << cv << " smooth=" << cs;
}

TEST(FuzzFactoryTest, GraveVelcroMotorboats) {
  const auto y = render(Knobs{0.72f, 0.08f, 0.22f, 0.70f, 0.16f}, 14000, pulse(60, 180, 0.28f));
  const size_t from = y.size() - 6144;
  const float low = goertzel(y, from, y.size(), 72.0f);
  const float high = goertzel(y, from, y.size(), 1400.0f);
  EXPECT_GT(rms(y, from, y.size()), 0.035f) << "grave velcro should keep oscillating";
  EXPECT_GT(low, high * 0.25f) << "want motorboat not whistle";
}

TEST(FuzzFactoryTest, GateChokesTheTail) {
  const auto burst = [](size_t i) {
    return i < 3000 ? std::sin(2.0f * kPiF * 146.8f * static_cast<float>(i) / kSr) * 0.6f : 0.0f;
  };
  const auto open = render(Knobs{0.7f, 0.0f, 0.3f, 0.7f, 0.97f}, 8000, burst);
  const auto shut = render(Knobs{0.7f, 1.0f, 0.3f, 0.7f, 0.97f}, 8000, burst);
  const float to = rms(open, 5000, open.size());
  const float ts = rms(shut, 5000, shut.size());
  EXPECT_LT(ts, to * 0.55f + 0.004f) << "gate open=" << to << " shut=" << ts;
}

TEST(FuzzFactoryTest, KnobsMoveTheSupply) {
  FuzzFactory ff(kSr);
  Knobs k;
  k.stab = 1.0f;
  ff.setKnobs(k);
  for (int i = 0; i < 200; ++i)
    ff.process(0.0f);
  const float hi = ff.va();
  k.stab = 0.0f;
  ff.setKnobs(k);
  for (int i = 0; i < 200; ++i)
    ff.process(0.0f);
  EXPECT_GT(hi, ff.va() + 2.0f);
}

TEST(FuzzFactoryTest, SilentIsNotNan) {
  FuzzFactory ff(kSr);
  for (int i = 0; i < 512; ++i)
    ASSERT_TRUE(std::isfinite(ff.process(0.0f)));
}

TEST(FuzzFactoryTest, DriveAndVolAreAudible) {
  const auto input = sineAt(220.0f, 0.18f);
  const auto lo = render(Knobs{0.7f, 0.0f, 0.2f, 0.05f, 0.97f}, 6000, input);
  const auto hi = render(Knobs{0.7f, 0.0f, 0.2f, 1.0f, 0.97f}, 6000, input);
  float diff = 0.0f;
  for (size_t i = 2048; i < lo.size(); ++i)
    diff += (hi[i] - lo[i]) * (hi[i] - lo[i]);
  EXPECT_GT(std::sqrt(diff / static_cast<float>(lo.size() - 2048)), 0.04f) << "drive";

  const auto quiet = render(Knobs{0.05f, 0.0f, 0.3f, 0.7f, 0.97f}, 4000, input);
  const auto loud = render(Knobs{1.0f, 0.0f, 0.3f, 0.7f, 0.97f}, 4000, input);
  EXPECT_GT(rms(loud, 1024, loud.size()), rms(quiet, 1024, quiet.size()) * 3.0f) << "vol";
}

TEST(FuzzFactoryTest, StabLowCompZeroIsSilence) {
  EXPECT_LT(rmsTail(howl(0.0f, 0.08f), 4096), 0.03f);
}

TEST(FuzzFactoryTest, CompClimbsFromVelcroToSqueal) {
  const auto low = howl(0.22f, 0.12f);
  const auto high = howl(0.82f, 0.12f);
  const size_t from = low.size() - 6144;
  EXPECT_GT(rms(low, from, low.size()), 0.04f) << "low Comp should howl";
  EXPECT_GT(rms(high, from, high.size()), 0.04f) << "high Comp should howl";
  const float bassLo = goertzel(low, from, low.size(), 80.0f);
  const float treLo = goertzel(low, from, low.size(), 1800.0f);
  const float treHi = goertzel(high, from, high.size(), 1800.0f);
  EXPECT_GT(bassLo, treLo) << "low Comp should be grave";
  EXPECT_GT(treHi, treLo * 2.0f) << "Comp up should add squeal";
}

TEST(FuzzFactoryTest, StabRaisesPitchThenKillsTheHowl) {
  const auto a = howl(0.70f, 0.10f);
  const auto b = howl(0.70f, 0.45f);
  const auto c = howl(0.70f, 0.96f);
  const float fa = zcr(a, a.size() - 6144, a.size());
  const float fb = zcr(b, b.size() - 6144, b.size());
  EXPECT_GT(rmsTail(a, 4096), 0.04f) << "stab down should howl";
  EXPECT_GT(fb, fa * 1.15f) << "Stab up should raise pitch";
  EXPECT_LT(rmsTail(c, 4096), 0.04f) << "Stab CW should kill the squeal";
}

TEST(FuzzFactoryTest, CompAddsTreble) {
  const auto input = sineAt(220.0f, 0.35f);
  const auto dark = render(Knobs{0.7f, 0.0f, 0.08f, 0.65f, 0.97f}, 8000, input);
  const auto bright = render(Knobs{0.7f, 0.0f, 0.92f, 0.65f, 0.97f}, 8000, input);
  EXPECT_GT(goertzel(bright, 2048, bright.size(), 1760.0f),
            goertzel(dark, 2048, dark.size(), 1760.0f) * 1.15f);
}

TEST(FuzzFactoryTest, PresetsDoNotSoundTheSame) {
  const auto input = sineAt(146.8f, 0.4f);
  float lo = 1.0e9f, hi = 0.0f;
  for (const auto& k :
       {Knobs::smooth(), Knobs::velcro(), Knobs::squeal(), Knobs::gatedHi(), Knobs::plug()}) {
    const auto y = render(k, 8000, input);
    const float r = rms(y, 4096, y.size());
    lo = std::min(lo, r);
    hi = std::max(hi, r);
  }
  EXPECT_GT(hi - lo, 0.03f);
}

// Not in the Rust module: the stage must also hold up at the other rates a
// host runs, and fit a real-time budget with room to spare.

TEST(FuzzFactoryTest, OtherRatesStayFiniteAndBounded) {
  for (const float sr : {44100.0f, 88200.0f, 96000.0f, 192000.0f}) {
    for (const auto& k : {Knobs::smooth(), Knobs::squeal(), Knobs::velcro()}) {
      FuzzFactory ff(sr);
      ff.setKnobs(k);
      for (int i = 0; i < static_cast<int>(sr) / 2; ++i) {
        const float y = ff.process(i < 2000 ? 0.5f * std::sin(0.02f * static_cast<float>(i)) : 0.0f);
        ASSERT_TRUE(std::isfinite(y)) << "sr " << sr;
        ASSERT_LE(std::abs(y), 1.4f);
      }
    }
  }
}

TEST(FuzzFactoryTest, CpuBudget) {
  // Ten seconds of 48 kHz audio through the busiest setting (a howl, so the
  // noise kick and the delay tap both run). Report the real-time factor and
  // fail only far beyond a sane budget: 2% of one core.
  FuzzFactory ff(kSr);
  ff.setKnobs(Knobs::squeal());
  std::vector<float> buf(512);
  const int blocks = static_cast<int>(kSr * 10.0f) / 512;
  const auto t0 = std::chrono::steady_clock::now();
  float sink = 0.0f;
  for (int b = 0; b < blocks; ++b) {
    for (size_t i = 0; i < buf.size(); ++i)
      buf[i] = 0.2f * std::sin(0.03f * static_cast<float>(b * 512 + static_cast<int>(i)));
    ff.process(buf.data(), static_cast<int>(buf.size()));
    sink += buf[0];
  }
  const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  const double load = secs / 10.0;
  RecordProperty("core_load_percent", std::to_string(load * 100.0));
  std::printf("[ fuzz cpu ] %.3f%% of one core at 48 kHz (sink %g)\n", load * 100.0, sink);
  EXPECT_LT(load, 0.02);
}

TEST(FuzzStageTest, PowerOffBlendLandsOnABitExactPassthrough) {
  FuzzStage stage;
  stage.prepare(kFs);
  EXPECT_FALSE(stage.isRunning());
  stage.setEnabled(true);
  stage.setKnobs(Knobs::smooth());
  juce::AudioBuffer<float> buf(2, kBlock);
  const auto sine = makeSine(kBlock, 196.0, 0.4f);
  for (int b = 0; b < 20; ++b) {
    for (int ch = 0; ch < 2; ++ch)
      buf.copyFrom(ch, 0, sine.data(), kBlock);
    stage.process(buf);
  }
  float diff = 0.0f;
  for (int i = 0; i < kBlock; ++i)
    diff = std::max(diff, std::abs(buf.getSample(0, i) - sine[static_cast<size_t>(i)]));
  EXPECT_GT(diff, 0.05f) << "powered, the stage must change the signal";

  stage.setEnabled(false);
  // kBlendSeconds is 1200 samples at 48 kHz: three blocks land it.
  for (int b = 0; b < 3; ++b)
    stage.process(buf);
  EXPECT_FALSE(stage.isRunning());
  for (int ch = 0; ch < 2; ++ch)
    buf.copyFrom(ch, 0, sine.data(), kBlock);
  stage.process(buf);
  for (int i = 0; i < kBlock; ++i)
    ASSERT_EQ(buf.getSample(0, i), sine[static_cast<size_t>(i)]);
}

TEST(FuzzStageTest, PowerOnFadesInWithoutAJump) {
  // Silence-adjacent input so any step is the fade's fault, not the fuzz's.
  FuzzStage stage;
  stage.prepare(kFs);
  stage.setKnobs(Knobs::plug());
  stage.setEnabled(true);
  juce::AudioBuffer<float> buf(1, kBlock);
  const auto sine = makeSine(kBlock, 110.0, 0.3f);
  buf.copyFrom(0, 0, sine.data(), kBlock);
  stage.process(buf);
  // The first output sample is still (almost) the dry input.
  EXPECT_NEAR(buf.getSample(0, 0), sine[0], 1.0e-3f);
}

namespace {

float denormalised(TONE3000Processor& proc, const char* id) {
  auto* p = proc.parameters.getParameter(id);
  return p->convertFrom0to1(p->getValue());
}

void setParam(TONE3000Processor& proc, const char* id, float value) {
  proc.parameters.getParameter(id)->setValueNotifyingHost(value);
}

void pumpMessages() {
  juce::MessageManager::getInstance()->runDispatchLoopUntil(50);
}

std::vector<float> processThrough(TONE3000Processor& proc, const std::vector<float>& in,
                                  const juce::MidiBuffer& firstBlockMidi = {}) {
  std::vector<float> out(in.size(), 0.0f);
  juce::AudioBuffer<float> buffer(2, kBlock);
  for (size_t off = 0; off < in.size(); off += kBlock) {
    juce::MidiBuffer midi;
    if (off == 0) midi = firstBlockMidi;
    buffer.copyFrom(0, 0, in.data() + off, kBlock);
    buffer.copyFrom(1, 0, in.data() + off, kBlock);
    proc.processBlock(buffer, midi);
    std::copy(buffer.getReadPointer(0), buffer.getReadPointer(0) + kBlock,
              out.begin() + static_cast<long>(off));
  }
  return out;
}

const char* const kFuzzIds[] = {"fuzzEnabled", "fuzzVol", "fuzzGate", "fuzzComp", "fuzzDrive",
                                "fuzzStab"};

}  // namespace

TEST(ProcessorTest, FuzzDefaultsAreOffAndMatchTheEngine) {
  TONE3000Processor proc;
  const Knobs k;
  EXPECT_FLOAT_EQ(proc.parameters.getRawParameterValue("fuzzEnabled")->load(), 0.0f);
  EXPECT_FLOAT_EQ(denormalised(proc, "fuzzVol"), k.vol);
  EXPECT_FLOAT_EQ(denormalised(proc, "fuzzGate"), k.gate);
  EXPECT_FLOAT_EQ(denormalised(proc, "fuzzComp"), k.comp);
  EXPECT_FLOAT_EQ(denormalised(proc, "fuzzDrive"), k.drive);
  EXPECT_FLOAT_EQ(denormalised(proc, "fuzzStab"), k.stab);
}

TEST(ProcessorTest, FuzzOffIsBitExactAndZeroLatencyEvenWithKnobsDialled) {
  // Two processors, one with the knobs moved but the power off: identical
  // output to the sample, and no latency.
  TONE3000Processor plain, dialled;
  for (auto* p : {&plain, &dialled}) {
    p->setPlayConfigDetails(2, 2, kFs, kBlock);
    p->prepareToPlay(kFs, kBlock);
  }
  setParam(dialled, "fuzzDrive", 1.0f);
  setParam(dialled, "fuzzStab", 0.1f);
  pumpMessages();
  EXPECT_EQ(dialled.getLatencySamples(), 0);
  const auto in = makeNoise(32 * kBlock, 11, 0.4f);
  EXPECT_EQ(processThrough(plain, in), processThrough(dialled, in));
}

TEST(ProcessorTest, FuzzPowerFuzzesThePluginOutput) {
  // A clean sine through an empty chain stays a sine; with the fuzz on it
  // grows odd harmonics, and the latency stays 0.
  TONE3000Processor proc;
  proc.setPlayConfigDetails(2, 2, kFs, kBlock);
  proc.prepareToPlay(kFs, kBlock);
  setParam(proc, "fuzzEnabled", 1.0f);
  pumpMessages();
  EXPECT_EQ(proc.getLatencySamples(), 0);
  const auto out = processThrough(proc, makeSine(96 * kBlock, 220.0, 0.35f));
  const std::vector<float> tail(out.end() - 16384, out.end());
  const double h1 = goertzelPower(tail, 220.0);
  const double h3 = goertzelPower(tail, 660.0);
  EXPECT_GT(db(h3) - db(h1), -30.0) << "fuzzed output needs a real 3rd harmonic";
}

TEST(ProcessorTest, FuzzSurvivesStateRoundTrip) {
  juce::MemoryBlock state;
  {
    TONE3000Processor a;
    setParam(a, "fuzzEnabled", 1.0f);
    setParam(a, "fuzzComp", 0.78f);
    setParam(a, "fuzzStab", 0.14f);
    a.getStateInformation(state);
  }
  TONE3000Processor b;
  b.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
  EXPECT_FLOAT_EQ(b.parameters.getRawParameterValue("fuzzEnabled")->load(), 1.0f);
  EXPECT_NEAR(denormalised(b, "fuzzComp"), 0.78f, 1e-5f);
  EXPECT_NEAR(denormalised(b, "fuzzStab"), 0.14f, 1e-5f);
}

TEST(ProcessorTest, StateFromBeforeFuzzLandsOnItsDefaults) {
  // A session saved before the stage existed carries none of its entries;
  // restoring it must land the fuzz off, never leave one running.
  juce::MemoryBlock saved;
  {
    TONE3000Processor old;
    setParam(old, "gateThreshold", 0.6f);
    old.getStateInformation(saved);
  }
  juce::ValueTree tree = juce::ValueTree::readFromData(
      static_cast<const char*>(saved.getData()) + 4, saved.getSize() - 4);
  ASSERT_TRUE(tree.isValid());
  juce::ValueTree params = tree.getChildWithName("PARAMETERS");
  ASSERT_TRUE(params.isValid());
  for (const auto* id : kFuzzIds) {
    const auto child = params.getChildWithProperty("id", id);
    ASSERT_TRUE(child.isValid()) << id;
    params.removeChild(child, nullptr);
  }
  juce::MemoryBlock reframed;
  {
    juce::MemoryOutputStream out(reframed, false);
    out.write("T3KB", 4);
    tree.writeToStream(out);
  }

  TONE3000Processor proc;
  setParam(proc, "fuzzEnabled", 1.0f);
  setParam(proc, "fuzzDrive", 1.0f);
  proc.setStateInformation(reframed.getData(), static_cast<int>(reframed.getSize()));
  EXPECT_NEAR(proc.parameters.getRawParameterValue("gateThreshold")->load(), -40.0f, 0.01f);
  EXPECT_FLOAT_EQ(proc.parameters.getRawParameterValue("fuzzEnabled")->load(), 0.0f);
  EXPECT_FLOAT_EQ(denormalised(proc, "fuzzDrive"), Knobs{}.drive);
}

TEST(ProcessorTest, PresetsCarryTheFuzz) {
  const juce::File tmp = juce::File::getSpecialLocation(juce::File::tempDirectory)
                             .getChildFile("t3k-fuzz-tests-" + juce::Uuid().toString());
  tmp.createDirectory();
  {
    TONE3000Processor proc;
    proc.setPresetStoreForTesting(tmp);
    const juce::var stock = proc.savePreset("Stock");
    ASSERT_TRUE(stock.isObject());
    setParam(proc, "fuzzEnabled", 1.0f);
    setParam(proc, "fuzzComp", 0.78f);
    setParam(proc, "fuzzStab", 0.14f);
    const juce::var squeal = proc.savePreset("Squeal");
    ASSERT_TRUE(squeal.isObject());

    ASSERT_TRUE(proc.loadPreset(stock["id"].toString()));
    EXPECT_FLOAT_EQ(proc.parameters.getRawParameterValue("fuzzEnabled")->load(), 0.0f);
    EXPECT_NEAR(denormalised(proc, "fuzzComp"), Knobs{}.comp, 1e-5f);

    ASSERT_TRUE(proc.loadPreset(squeal["id"].toString()));
    EXPECT_FLOAT_EQ(proc.parameters.getRawParameterValue("fuzzEnabled")->load(), 1.0f);
    EXPECT_NEAR(denormalised(proc, "fuzzComp"), 0.78f, 1e-5f);
    EXPECT_NEAR(denormalised(proc, "fuzzStab"), 0.14f, 1e-5f);
  }
  tmp.deleteRecursively();
}

TEST(ProcessorTest, HelixCcLayoutDrivesTheFuzz) {
  // The Helix synth's map: CC 81 toggles the pedal (GT-100 CTL2), CC 16-20
  // are Gate / Comp / Stab / Drive / Vol.
  TONE3000Processor proc;
  const std::pair<const char*, int> map[] = {{"fuzzEnabled", 81}, {"fuzzGate", 16},
                                             {"fuzzComp", 17},    {"fuzzStab", 18},
                                             {"fuzzDrive", 19},   {"fuzzVol", 20}};
  for (const auto& [id, cc] : map) {
    proc.midiMapper.startLearn(id);
    pumpMessages();
    juce::MidiBuffer learnMidi;
    learnMidi.addEvent(juce::MidiMessage::controllerEvent(1, cc, 0), 0);
    proc.midiMapper.processMidi(learnMidi);
    pumpMessages();
  }
  juce::MidiBuffer midi;
  midi.addEvent(juce::MidiMessage::controllerEvent(1, 81, 127), 0);
  midi.addEvent(juce::MidiMessage::controllerEvent(1, 17, 127), 0);
  midi.addEvent(juce::MidiMessage::controllerEvent(1, 18, 0), 0);
  proc.midiMapper.processMidi(midi);
  EXPECT_FLOAT_EQ(proc.parameters.getRawParameterValue("fuzzEnabled")->load(), 1.0f);
  EXPECT_FLOAT_EQ(denormalised(proc, "fuzzComp"), 1.0f);
  EXPECT_FLOAT_EQ(denormalised(proc, "fuzzStab"), 0.0f);

  // A second stomp (press then release) turns it back off.
  juce::MidiBuffer stomp;
  stomp.addEvent(juce::MidiMessage::controllerEvent(1, 81, 0), 0);
  stomp.addEvent(juce::MidiMessage::controllerEvent(1, 81, 127), 1);
  proc.midiMapper.processMidi(stomp);
  EXPECT_FLOAT_EQ(proc.parameters.getRawParameterValue("fuzzEnabled")->load(), 0.0f);
}
