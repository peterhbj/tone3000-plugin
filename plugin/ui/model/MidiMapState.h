// MIDI mapping engine snapshot (port of midiMap.ts).
#pragma once

#include <juce_core/juce_core.h>

#include <optional>
#include <vector>

namespace t3k::ui {

enum class MidiSource { cc, note };

// The CC sweep of a continuous mapping, in the parameter's real units:
// CC 0 lands on low, CC 127 on high (low > high inverts); min/max are the
// parameter's full span.
struct MidiRange {
  double low = 0, high = 1;
  double min = 0, max = 1;
};

struct MidiMapping {
  juce::String targetId;
  MidiSource source = MidiSource::cc;
  int number = 0;
  // Only on mappings that take one (CC on a continuous parameter).
  std::optional<MidiRange> range;
};

struct MidiMapState {
  // 0 = omni, 1-16 = that channel only.
  int channel = 0;
  // Target armed for learn ("" when idle).
  juce::String learnTargetId;
  std::vector<MidiMapping> mappings;

  static MidiMapState parse(const juce::var& v);
  const MidiMapping* mappingFor(const juce::String& targetId) const;
};

}  // namespace t3k::ui
