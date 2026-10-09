#pragma once
#include <juce_audio_basics/juce_audio_basics.h>
#include <array>
#include <cstdint>

/**
 * Fuzz Factory input stage: one circuit, an LPB-1 (Q1) into a germanium
 * Fuzz Face (Q2/Q3). Ported sample-for-sample from the Helix desktop synth
 * (synth-emulator-desktop/src/audio/fuzz_factory.rs); keep the two in step,
 * the Rust file is the spec.
 *
 * Stab = how stable the wave is. CW the pair sits still (Face). CCW it
 * hears itself; raising Stab then pushes that howl up in pitch until it
 * can't hold and goes quiet.
 * Comp = the treble / pitch of that wave. At 0 the texture is velcro (and
 * with Stab down, silence: Q3 is off). Turning Comp up starts a bass rip
 * and climbs to a high squeal.
 * Gate starves Q2. Drive is Q2's emitter gain. Vol is after Q3.
 *
 * Mono, per-sample, 4x oversampled internally (each input sample is held
 * for four ticks and the ticks averaged). The knobs glide per sample
 * (Comp and Stab slower, they move the howl's pitch). process() never
 * allocates.
 */
class FuzzFactory {
public:
  static constexpr int kOversample = 4;
  static constexpr int kDelaySize = 4096;

  /** All five knobs, 0..1. */
  struct Knobs {
    float vol = 0.70f;
    float gate = 0.04f;
    float comp = 0.28f;
    float drive = 0.62f;
    float stab = 0.94f;

    Knobs clamped() const;

    // The Helix presets.
    static Knobs smooth() { return {0.72f, 0.0f, 0.55f, 0.58f, 1.0f}; }
    static Knobs velcro() { return {0.74f, 0.08f, 0.04f, 0.80f, 0.72f}; }
    static Knobs squeal() { return {0.70f, 0.10f, 0.78f, 0.82f, 0.14f}; }
    static Knobs gatedHi() { return {0.74f, 0.55f, 0.35f, 0.85f, 0.95f}; }
    static Knobs plug() { return {0.76f, 0.12f, 0.62f, 0.72f, 0.96f}; }
  };

  explicit FuzzFactory(float sampleRate = 48000.0f);

  /** Back to the constructed state at a new rate (knob targets kept). */
  void reset(float sampleRate);

  void setKnobs(const Knobs& k) { knobs_ = k.clamped(); }
  const Knobs& knobs() const { return knobs_; }

  /** Supply sag readout for the smoothed Stab (the Helix meter). */
  float va() const;

  float process(float x);
  void process(float* data, int numSamples) {
    for (int i = 0; i < numSamples; ++i)
      data[i] = process(data[i]);
  }

private:
  // Everything tick() derives from the smoothed knobs alone. The knobs only
  // glide once per input sample, so this is built once and shared by the
  // kOversample ticks (the transcendentals are most of the stage's cost).
  struct Coeffs {
    float unstable, va, aEnv, live, dead, aIn, aFf, fOsc, aF, aRail, nfb, pfb, th, g2, gateHead,
        g3, head3Base, aQ3, aFb, aHpf, outGain;
  };
  Coeffs coeffs() const;
  float tick(float x, const Coeffs& c);
  float delayAt(float n) const;

  float sr_ = 48000.0f;
  Knobs knobs_;
  Knobs knobsZ_;
  float dcIn_ = 0.0f, dcFf_ = 0.0f, dcOut_ = 0.0f;
  float q3Lp_ = 0.0f, fbLp_ = 0.0f, railLp_ = 0.0f;
  float env_ = 0.0f, q3_ = 0.0f, fZ_ = 200.0f;
  uint32_t rng_ = 0xC0FFEE;
  std::array<float, kDelaySize> delay_{};
  int delayI_ = 0;
};

/**
 * The faceplate Fuzz group: one FuzzFactory per input channel, after the
 * pitch shifter and before the chain, so the amp sees a fuzzed guitar (and
 * a whammy into the fuzz, the classic order).
 *
 * Power blends like the pitch shifter's: on resets the engines (no stale
 * howl from the last time it ran) and fades the fuzz in over kBlendSeconds;
 * off fades it out and isRunning() stays true until that lands, after which
 * the processor stops calling it, so a powered-off stage is a bit-exact
 * passthrough. Zero latency either way.
 *
 * Threading: prepare() from prepareToPlay; the rest from the audio thread.
 */
class FuzzStage {
public:
  static constexpr int kMaxChannels = 2;
  static constexpr double kBlendSeconds = 0.025;

  void prepare(double sampleRate);
  void setEnabled(bool on);
  bool isRunning() const { return enabled_ || wet_.isSmoothing() || wet_.getCurrentValue() > 0.0f; }
  void setKnobs(const FuzzFactory::Knobs& k);
  /** Fuzzes up to kMaxChannels in place; a no-op unless running. */
  void process(juce::AudioBuffer<float>& buffer);

private:
  std::array<FuzzFactory, kMaxChannels> engines_;
  juce::LinearSmoothedValue<float> wet_{0.0f};
  double sampleRate_ = 48000.0;
  bool enabled_ = false;
};
