#include "FuzzDeckPanel.h"

#include "core/Paint.h"
#include "core/Theme.h"

namespace t3k::ui {

namespace {

// The gate / pitch decks' geometry (see GateDeckPanel.cpp).
constexpr int kPadTop = 14, kPadSide = 16, kPadBottom = 8;
constexpr int kSectionWidth = 64;
constexpr int kSectionGap = 18;

Knob::Options deckKnob(const char* label, float def, help::Key help) {
  Knob::Options o;
  o.label = label;
  o.size = theme::kKnobSizeSecondary;
  o.thumb = Knob::Thumb::secondary;
  o.scale = &scales::tone();  // the pedal's 0-10
  o.defaultValue = def;
  o.help = help;
  return o;
}

}  // namespace

FuzzDeckPanel::FuzzDeckPanel(Services& services)
    : vol_(services.backend, "fuzzVol", deckKnob("Vol", defaults().vol, help::Key::fuzzVol)),
      gate_(services.backend, "fuzzGate", deckKnob("Gate", defaults().gate, help::Key::fuzzGate)),
      comp_(services.backend, "fuzzComp", deckKnob("Comp", defaults().comp, help::Key::fuzzComp)),
      drive_(services.backend, "fuzzDrive", deckKnob("Drive", defaults().drive, help::Key::fuzz)),
      stab_(services.backend, "fuzzStab", deckKnob("Stab", defaults().stab, help::Key::fuzzStab)) {
  for (auto* k : {&vol_, &gate_, &comp_, &drive_, &stab_}) addAndMakeVisible(*k);
  primaryOnly = true;          // right-click toggles the panel; don't dismiss on it
  dismissOnAnchorPress = true; // the anchor is the Fuzz knob, a control
  setSize(kWidth, kHeight);
}

void FuzzDeckPanel::resetDeck(Backend& backend) {
  const auto d = defaults();
  ParamBinding(backend, "fuzzVol").set(d.vol);
  ParamBinding(backend, "fuzzGate").set(d.gate);
  ParamBinding(backend, "fuzzComp").set(d.comp);
  ParamBinding(backend, "fuzzDrive").set(d.drive);
  ParamBinding(backend, "fuzzStab").set(d.stab);
}

void FuzzDeckPanel::paint(juce::Graphics& g) {
  const auto box = getLocalBounds().toFloat();
  paint::fill(g, box, theme::kPanelCorner, theme::kPanelBg);
  paint::border(g, box, theme::kPanelCorner, theme::kBorder);
}

void FuzzDeckPanel::resized() {
  auto content = contentBounds();
  content.removeFromTop(kPadTop);
  content.removeFromBottom(kPadBottom);
  content.reduce(kPadSide, 0);
  const int height = Knob::heightFor(theme::kKnobSizeSecondary);
  int x = content.getX();
  for (auto* k : {&vol_, &gate_, &comp_, &drive_, &stab_}) {
    k->setBounds(x, content.getBottom() - height + Knob::kEditorOverflow, kSectionWidth, height);
    x += kSectionWidth + kSectionGap;
  }
}

}  // namespace t3k::ui
