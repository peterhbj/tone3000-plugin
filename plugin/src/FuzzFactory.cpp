#include "FuzzFactory.h"

#include <algorithm>
#include <cmath>

// Every expression keeps the Rust source's float ordering so the two
// engines stay numerically interchangeable (see FuzzFactory.h).

namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr int kDelayMask = FuzzFactory::kDelaySize - 1;

inline float fastTanh(float x) {
  x = std::clamp(x, -4.5f, 4.5f);
  const float x2 = x * x;
  return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}

inline float geClip(float x, float head) {
  const float s = std::max(head, 0.07f);
  const float y = fastTanh(x / s);
  return (y + 0.32f * y * y - 0.07f * y * y * y) * s;
}

// One-pole coefficient for a corner at `hz` on the oversampled clock.
inline float onePole(float hz, float osSr) {
  return 1.0f - std::exp(-2.0f * kPi * hz / osSr);
}

inline float sag(float stab) {
  return 1.20f + 7.60f * std::pow(stab, 0.78f);
}

}  // namespace

FuzzFactory::Knobs FuzzFactory::Knobs::clamped() const {
  return {std::clamp(vol, 0.0f, 1.0f), std::clamp(gate, 0.0f, 1.0f), std::clamp(comp, 0.0f, 1.0f),
          std::clamp(drive, 0.0f, 1.0f), std::clamp(stab, 0.0f, 1.0f)};
}

FuzzFactory::FuzzFactory(float sampleRate) {
  reset(sampleRate);
}

void FuzzFactory::reset(float sampleRate) {
  sr_ = std::max(sampleRate, 8000.0f);
  knobsZ_ = Knobs{};
  dcIn_ = dcFf_ = dcOut_ = 0.0f;
  q3Lp_ = fbLp_ = railLp_ = 0.0f;
  env_ = q3_ = 0.0f;
  fZ_ = 200.0f;
  rng_ = 0xC0FFEE;
  delay_.fill(0.0f);
  delayI_ = 0;
}

float FuzzFactory::va() const {
  return sag(knobsZ_.stab);
}

float FuzzFactory::process(float x) {
  const float a = 0.04f;
  const float aPitch = 0.012f;
  knobsZ_.vol += a * (knobs_.vol - knobsZ_.vol);
  knobsZ_.gate += a * (knobs_.gate - knobsZ_.gate);
  knobsZ_.comp += aPitch * (knobs_.comp - knobsZ_.comp);
  knobsZ_.drive += a * (knobs_.drive - knobsZ_.drive);
  knobsZ_.stab += aPitch * (knobs_.stab - knobsZ_.stab);

  const Coeffs c = coeffs();
  float acc = 0.0f;
  for (int i = 0; i < kOversample; ++i)
    acc += tick(x, c);
  return std::clamp(acc / static_cast<float>(kOversample), -1.4f, 1.4f);
}

FuzzFactory::Coeffs FuzzFactory::coeffs() const {
  const Knobs& k = knobsZ_;
  const float osSr = sr_ * static_cast<float>(kOversample);
  Coeffs c;

  // Stab = rail stiffness. Low = the pair can hear itself.
  c.unstable = std::max((0.84f - k.stab) / 0.84f, 0.0f);
  c.va = 0.11f + 0.58f * std::pow(k.stab, 0.72f);
  c.aEnv = onePole(14.0f + k.stab * 50.0f, osSr);

  // Stab down + Comp at 0: Q3 is off, silence. Comp is the start switch.
  c.live = std::clamp(k.comp / 0.10f, 0.0f, 1.0f);
  c.dead = c.unstable * (1.0f - c.live);

  c.aIn = onePole(12.0f, osSr);
  c.aFf = onePole(48.0f + k.drive * 55.0f, osSr);

  // Howl pitch: Comp climbs grave to squeal; Stab also pushes pitch up until
  // the wave is too stable to hold.
  c.fOsc = 40.0f * std::pow(3600.0f / 40.0f, std::pow(k.comp, 0.88f)) * (1.0f + k.stab * 2.0f) *
           (0.92f + 0.16f * k.drive);
  c.aF = onePole(14.0f, osSr);
  // Comp is the treble of the wave: low Comp keeps rumble, Comp up strips bass.
  c.aRail = onePole(18.0f + k.comp * std::min(0.55f * c.fOsc, 900.0f), osSr);

  c.nfb = 0.03f + 0.18f * k.stab;
  c.pfb = std::pow(c.unstable, 0.55f) * std::pow(c.live, 0.40f) * (1.10f + k.drive * 0.85f) *
          (1.0f - 0.28f * k.gate);

  // Comp at 0 (with Stab still up) = velcro splat on the note.
  const float velcro = std::pow(1.0f - k.comp, 1.35f) * k.stab;
  const float starve = k.gate * (0.40f + 0.35f * (1.0f - k.comp)) + velcro * 0.28f;
  c.th = starve * starve * 0.50f;

  c.g2 = 1.4f + k.drive * 15.0f + k.comp * 2.2f;
  c.gateHead = 1.0f - 0.48f * k.gate;
  // Comp up = Q3 conducts more treble. Comp 0 + Stab down = Q3 off.
  c.g3 = (0.38f + k.comp * 3.1f) * (1.0f - 0.95f * c.dead);
  c.head3Base = k.comp * 0.06f;

  c.aQ3 = onePole(650.0f + k.comp * 8200.0f + k.stab * 900.0f, osSr);
  c.aFb = onePole(30.0f + k.stab * 80.0f, osSr);
  c.aHpf = onePole(22.0f, osSr);
  c.outGain = 0.02f + 1.48f * k.vol;
  return c;
}

float FuzzFactory::tick(float x, const Coeffs& c) {
  const float osSr = sr_ * static_cast<float>(kOversample);

  env_ += c.aEnv * (std::abs(q3_) - env_);
  const float vaLive = std::max(c.va * (1.0f - c.unstable * 0.50f * std::min(env_, 1.1f)), 0.055f);

  dcIn_ += c.aIn * (x - dcIn_);
  const float ac = x - dcIn_;

  const float q1 = fastTanh(ac * 3.4f) * 1.08f;

  dcFf_ += c.aFf * (q1 - dcFf_);
  const float ffIn = q1 - dcFf_;

  fZ_ += c.aF * (c.fOsc - fZ_);
  const float n = std::clamp(osSr / std::max(fZ_, 20.0f), 2.0f,
                             static_cast<float>(kDelaySize - 3));
  const float tap = delayAt(n);
  railLp_ += c.aRail * (tap - railLp_);
  const float tapAc = tap - railLp_;

  float u = ffIn - c.nfb * fbLp_ + c.pfb * tapAc;
  u *= 1.0f - 0.97f * c.dead;
  // Iceo: the pair starts itself. No key-tap needed.
  if (c.pfb > 0.2f && env_ < 0.05f) {
    rng_ = rng_ * 1664525u + 1013904223u;
    const float nse = static_cast<float>(rng_ >> 16) / 32768.0f - 1.0f;
    u += c.pfb * 0.045f * nse;
  }

  const float au = std::abs(u);
  if (c.th > 1.0e-4f && au < c.th) {
    const float t = au / c.th;
    u *= t * t;
  }

  const float head2 = (0.09f + vaLive * 0.55f) * c.gateHead;
  const float q2 = geClip(u * c.g2, std::max(head2, 0.05f));

  const float head3 = 0.08f + vaLive * 0.48f + c.head3Base;
  float q3 = geClip(q2 * c.g3, std::max(head3, 0.045f));

  q3Lp_ += c.aQ3 * (q3 - q3Lp_);
  q3 = q3Lp_;

  q3_ = q3;
  delay_[static_cast<size_t>(delayI_)] = q3;
  delayI_ = (delayI_ + 1) & kDelayMask;
  fbLp_ += c.aFb * (q3 - fbLp_);

  dcOut_ += c.aHpf * (q3 - dcOut_);
  return (q3 - dcOut_) * c.outGain;
}

float FuzzFactory::delayAt(float n) const {
  n = std::clamp(n, 1.0f, static_cast<float>(kDelaySize - 2));
  const int n0 = static_cast<int>(std::floor(n));
  const float frac = n - static_cast<float>(n0);
  const float a = delay_[static_cast<size_t>((delayI_ + kDelaySize - n0) & kDelayMask)];
  const float b = delay_[static_cast<size_t>((delayI_ + kDelaySize - (n0 + 1)) & kDelayMask)];
  return a + frac * (b - a);
}

//==============================================================================
// FuzzStage

void FuzzStage::prepare(double sampleRate) {
  sampleRate_ = sampleRate;
  for (auto& e : engines_)
    e.reset(static_cast<float>(sampleRate));
  wet_.reset(sampleRate, kBlendSeconds);
  wet_.setCurrentAndTargetValue(enabled_ ? 1.0f : 0.0f);
}

void FuzzStage::setEnabled(bool on) {
  if (on == enabled_)
    return;
  if (on && !isRunning()) {
    // Fresh start: the previous run's oscillation and filter state would
    // otherwise come back for a few ms under the fade-in.
    for (auto& e : engines_)
      e.reset(static_cast<float>(sampleRate_));
  }
  enabled_ = on;
  wet_.setTargetValue(on ? 1.0f : 0.0f);
}

void FuzzStage::setKnobs(const FuzzFactory::Knobs& k) {
  for (auto& e : engines_)
    e.setKnobs(k);
}

void FuzzStage::process(juce::AudioBuffer<float>& buffer) {
  if (!isRunning())
    return;
  const int numChannels = juce::jmin(buffer.getNumChannels(), kMaxChannels);
  const int numSamples = buffer.getNumSamples();
  if (!wet_.isSmoothing() && wet_.getCurrentValue() >= 1.0f) {
    for (int ch = 0; ch < numChannels; ++ch)
      engines_[static_cast<size_t>(ch)].process(buffer.getWritePointer(ch), numSamples);
    return;
  }
  float* data[kMaxChannels] = {};
  for (int ch = 0; ch < numChannels; ++ch)
    data[ch] = buffer.getWritePointer(ch);
  for (int i = 0; i < numSamples; ++i) {
    const float g = wet_.getNextValue();
    for (int ch = 0; ch < numChannels; ++ch) {
      const float x = data[ch][i];
      const float y = engines_[static_cast<size_t>(ch)].process(x);
      data[ch][i] = x + g * (y - x);
    }
  }
}
