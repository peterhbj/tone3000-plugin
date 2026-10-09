// The Fuzz Factory group's advanced panel (right-click the group; touch-and-
// hold the knob): the pedal's five knobs, left to right as on the Helix
// synth's fuzz panel. All 0-10 on the pedal's own travel (FuzzFactory.h):
//  - Vol: output after Q3.
//  - Gate: starves Q2; up chokes the tail into a sputter.
//  - Comp: the treble / pitch of the oscillation. 0 is velcro; up climbs
//    from a bass rip to a squeal.
//  - Drive: Q2's gain (the knob on the plate).
//  - Stab: how stable the pair is. CW sits still (a Fuzz Face); CCW it
//    hears itself and howls with no input at all.
// Plain controls, no power switches, same chrome as the gate and pitch decks.
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "services/Services.h"
#include "widgets/ParamControls.h"
#include "widgets/Popover.h"

namespace t3k::ui {

class FuzzDeckPanel : public Popover {
public:
  static constexpr int kWidth = 424;
  static constexpr int kHeight = 85;
  // Gap between the panel's bottom edge and its anchor's top.
  static constexpr int kGap = 6;

  explicit FuzzDeckPanel(Services& services);

  // The engine's defaults (FuzzFactory::Knobs in plugin/include; the UI
  // builds without the DSP headers, keep the two in step). The parameters
  // are the knobs' own 0..1 travel, so these are also the normalised values.
  struct Knobs {
    float vol = 0.70f, gate = 0.04f, comp = 0.28f, drive = 0.62f, stab = 0.94f;
  };
  static Knobs defaults() { return {}; }
  // Restores all five knobs (Alt/Option-click on the Fuzz knob resets the
  // pedal, not just the drive).
  static void resetDeck(Backend& backend);

  void paint(juce::Graphics& g) override;
  void resized() override;

private:
  ParamKnob vol_, gate_, comp_, drive_, stab_;
};

}  // namespace t3k::ui
