#include "NativeEditor.h"

#include "core/Fonts.h"

namespace t3k::ui {

namespace {

// Bring-up trace lines share one prefix so a user's log can be grepped for
// the editor's stages next to the "[Processor]" / "[Restore]" ones.
void trace(const juce::String& message) { juce::Logger::writeToLog("[Editor] " + message); }

juce::String formatScale(double scale) { return juce::String(scale, 2) + "x"; }

// "Direct2D" / "Software Renderer" on Windows; the index is opaque without
// the peer's own names, so fall back to it only when the list is empty.
juce::String rendererName(const juce::ComponentPeer& peer) {
  const auto engines = const_cast<juce::ComponentPeer&>(peer).getAvailableRenderingEngines();
  const int index = peer.getCurrentRenderingEngine();
  if (juce::isPositiveAndBelow(index, engines.size())) return engines[index];
  return "engine " + juce::String(index) + " of " + juce::String(engines.size());
}

}  // namespace

NativeEditor::Trace::Trace(const TONE3000Processor& processor) {
  // Everything a Windows-10-vs-11 / host-specific report needs in one line:
  // OS, JUCE, plugin format + host, and the display scale the peer will be
  // created at (DPI is the usual suspect when only one machine crashes).
  juce::String line = "Constructing: " + juce::SystemStats::getOperatingSystemName() +
                      (juce::SystemStats::isOperatingSystem64Bit() ? " (64-bit)" : " (32-bit)") +
                      " | JUCE " + juce::SystemStats::getJUCEVersion().fromFirstOccurrenceOf("v", false, false) +
                      " | " + juce::AudioProcessor::getWrapperTypeDescription(processor.wrapperType) +
                      " in " + juce::String(juce::PluginHostType().getHostDescription());
  const auto& displays = juce::Desktop::getInstance().getDisplays();
  if (const auto* display = displays.getPrimaryDisplay()) {
    line << " | display " << juce::roundToInt(display->logicalBounds.getWidth()) << "x"
         << juce::roundToInt(display->logicalBounds.getHeight()) << " @ " << formatScale(display->scale)
         << " (dpi " << juce::roundToInt(display->dpi) << ")";
  } else {
    line << " | display: none";
  }
  line << " | displays " << displays.displays.size()
       << " | globalScale " << formatScale(juce::Desktop::getInstance().getGlobalScaleFactor());
  trace(line);
}

NativeEditor::FontsReady::FontsReady() {
  // Resolving these touches the OS font system (DirectWrite on Windows) and
  // decodes the embedded faces; if that is where a machine dies, the log
  // stops between "Constructing" and this line.
  const auto sans = Fonts::sans(14.0f);
  const auto mono = Fonts::mono(14.0f);
  trace("Fonts ready: sans=" + sans.getTypefaceName() +
        (sans.getTypefacePtr() != nullptr ? "" : " (NULL TYPEFACE)") + " | mono=" +
        mono.getTypefaceName() + (mono.getTypefacePtr() != nullptr ? "" : " (NULL TYPEFACE)"));
}

void NativeEditor::logPeerAttached() {
  // Once per native window: hosts can re-parent the editor (and the
  // standalone flips its title bar, see parentHierarchyChanged), each of
  // which may hand us a new peer with a different renderer or scale.
  auto* peer = getPeer();
  if (peer == nullptr || peer == loggedPeer_) return;
  const bool reattached = loggedPeer_ != nullptr;
  loggedPeer_ = peer;
  trace(juce::String(reattached ? "Peer re-attached" : "Peer attached") +
        ": renderer=" + rendererName(*peer) +
        " | platformScale " + formatScale(peer->getPlatformScaleFactor()) +
        " | editor " + juce::String(getWidth()) + "x" + juce::String(getHeight()) +
        " | peerBounds " + peer->getBounds().toString());
}

NativeEditor::NativeEditor(TONE3000Processor& owner)
    : AudioProcessorEditor(&owner),
      trace_(owner),
      processor_(owner),
      backend_(owner, *this),
      prefs_(&prefsFile_->file, &prefsFile_->lock),
      session_(backend_, prefs_, http_, Tone3000Session::Config::fromBuild()),
      services_(backend_, session_, *this, prefs_),
      root_(services_) {
  // Dark-theme every JUCE-drawn surface outside the root (standalone audio
  // settings dialog etc.). SharedResourcePointer keeps one instance across
  // plugin instances in the same process.
  juce::LookAndFeel::setDefaultLookAndFeel(&darkLookAndFeel_.get());
  setOpaque(true);
  addAndMakeVisible(root_);
  // Nothing polls or repaints while the window is minimised or the editor
  // hidden (isShowing covers both); visibilityChanged /
  // parentHierarchyChanged wake the clock when it is back.
  services_.clock.visible = [this] { return isShowing(); };
  // extraContentHeight_ is already set: the root reported its chrome from
  // its constructor (see setExtraContentHeight).

#if JUCE_IOS
  // iOS gets one fixed, full-screen window: no corner drags, no host resize
  // request, no persisted scale. The kiosk window hands us its bounds and the
  // root letterboxes into them (see fitRoot); an aspect constrainer would
  // resolve the 4:3 screen by height and clip a third of the UI.
  setSize(design::kWidth, designHeight());
  setResizable(false, false);
#else
  setResizable(true, true);
  // Read the persisted scale before touching the constraints: installing the
  // resize limits already snaps the editor to the 1x minimum, and resized()
  // writes that back through processor.editorScale.
  const double savedScale = juce::jlimit(1.0, maxStartScale(), processor_.editorScale.load());
  updateResizeConstraints();
  applyScaledSize(savedScale);
#endif
  // Separates a crash inside PluginRoot's construction (log stops at "Fonts
  // ready") from one in the host's window attach (stops here).
  trace("Constructed: " + juce::String(getWidth()) + "x" + juce::String(getHeight()));
}

NativeEditor::~NativeEditor() { stopTimer(); }

double NativeEditor::maxStartScale() const {
  // In a DAW the host owns the plugin window, so the persisted scale is
  // restored as-is there.
  if (!StandaloneAudioSettings::isAvailable()) return design::kMaxScale;
  const auto* display = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay();
  if (display == nullptr) return design::kMaxScale;
  // Fit the standalone window into the primary display's usable area, with
  // headroom for the native title bar (GitHub issue #43; Mutter's 2x title
  // bar is 37 logical px). Floor at 1.0: the design box is the hard minimum.
  constexpr int titleBarAllowance = 40;
  const double fitW = display->userBounds.getWidth() / static_cast<double>(design::kWidth);
  const double fitH =
      (display->userBounds.getHeight() - titleBarAllowance) / static_cast<double>(designHeight());
  return juce::jlimit(1.0, design::kMaxScale, juce::jmin(fitW, fitH));
}

bool NativeEditor::windowManagerOwnsSize() const {
  return processor_.wrapperType == juce::AudioProcessor::wrapperType_Standalone;
}

double NativeEditor::currentScale() const {
  const double byWidth = getWidth() / static_cast<double>(design::kWidth);
  if (!windowManagerOwnsSize()) return byWidth;
  return juce::jmin(byWidth, getHeight() / static_cast<double>(designHeight()));
}

void NativeEditor::applyScaledSize(double scale) {
  setSize(juce::roundToInt(design::kWidth * scale), juce::roundToInt(designHeight() * scale));
}

void NativeEditor::applyConstraints(juce::ComponentBoundsConstrainer& c) const {
  if (windowManagerOwnsSize()) {
    // The standalone window belongs to the window manager: a tiling
    // compositor (Hyprland, Sway, i3), a maximise or a fullscreen hands us
    // whatever box the screen has, at any aspect ratio. JUCE's standalone
    // wrapper pushes that box through this constrainer, so an aspect lock or
    // a 2x cap here left the editor at its own size inside a bigger window:
    // a grey strip beside it and the faceplate clipped below (ultrawide
    // 2560x1080). Take any size and let fitRoot letterbox.
    c.setSizeLimits(kMinFreeWidth, kMinFreeHeight, kMaxFreeSide, kMaxFreeSide);
    c.setFixedAspectRatio(0.0);
    return;
  }
  c.setSizeLimits(design::kWidth, designHeight(), juce::roundToInt(design::kWidth * design::kMaxScale),
                  juce::roundToInt(designHeight() * design::kMaxScale));
  // The ratio tracks the chrome strips, so corner drags at any extra-height
  // state preserve the current layout exactly.
  c.setFixedAspectRatio(static_cast<double>(design::kWidth) / designHeight());
}

void NativeEditor::updateResizeConstraints() {
  // setResizeLimits() installs the default constrainer and re-applies it to
  // the current bounds; the limits themselves come from applyConstraints.
  setResizeLimits(design::kWidth, designHeight(), juce::roundToInt(design::kWidth * design::kMaxScale),
                  juce::roundToInt(designHeight() * design::kMaxScale));
  applyConstraints(*getConstrainer());
}

void NativeEditor::setExtraContentHeight(int total, int persistent) {
  const int clamped = juce::jlimit(0, kMaxExtraHeight, total);
  // Remember the session-persistent portion (the hint bar; the banner is
  // dynamic) even when the window size itself doesn't change, so the next
  // editor opens pre-sized for the chrome the UI will render on first paint.
  processor_.editorExtraHeight.store(juce::jlimit(0, kMaxExtraHeight, persistent));
  if (clamped == extraContentHeight_) return;
  const bool grew = clamped > extraContentHeight_;
  // The scale on screen before the box changes: what a taller box holds
  // while its resize is pending (see fitRoot).
  const double scale = getWidth() > 0 ? currentScale() : 1.0;
  [[maybe_unused]] const int delta = clamped - extraContentHeight_;
  extraContentHeight_ = clamped;
  // Before the constructor has sized us there is nothing to resize yet.
  if (getWidth() == 0) return;

  // A taller box keeps the current scale until the (possibly async or
  // refused) host resize lands, or the grace period ends.
  shrinkAllowedAtMs_ = grew ? juce::Time::currentTimeMillis() + kShrinkGraceMs : 0;
  heldScale_ = scale;
#if JUCE_IOS
  // The window is the screen; it cannot grow. Shrink the box to fit.
  fitRoot();
#else
  // setSize() reaches the host as a resize request through the plugin
  // wrapper (resizeView in VST3); a host that refuses keeps the old size and
  // fitRoot letterboxes instead. Constrainer values are set directly rather
  // than via setResizeLimits(), which re-applies to the *current* bounds and
  // can snap the width for a frame mid-change.
  if (auto* c = getConstrainer()) applyConstraints(*c);
  if (windowManagerOwnsSize()) {
    // Any aspect goes here, so grow (or shrink) by the strip alone and keep
    // the width the window manager gave us; a refusal letterboxes.
    setSize(getWidth(), juce::jmax(kMinFreeHeight, getHeight() + juce::roundToInt(delta * scale)));
  } else {
    applyScaledSize(scale);
  }
  fitRoot();
#endif
}

void NativeEditor::fitRoot() {
  const double byWidth = getWidth() / static_cast<double>(design::kWidth);
  const double byHeight = getHeight() / static_cast<double>(designHeight());
  // Fit both ways: a window wider than the design box (ultrawide, a tiled
  // or maximised standalone) letterboxes left and right.
  double scale = juce::jmin(byWidth, byHeight);
  const auto now = juce::Time::currentTimeMillis();
  if (now < shrinkAllowedAtMs_ && byHeight < juce::jmin(byWidth, heldScale_) - 1e-6) {
    // The box just grew taller than the window allows. Hold the scale it
    // had while a resize may still land (bottom strip clips, black on
    // black), then fit.
    startTimer(static_cast<int>(shrinkAllowedAtMs_ - now) + 1);
    scale = juce::jmin(byWidth, heldScale_);
  } else {
    stopTimer();
  }
  scale = juce::jmax(0.05, scale);
  services_.zoom.set(scale);
  // Top-anchored, horizontally centred. iOS centres vertically too: its
  // window never resizes, so nothing can jump.
  const int x = juce::roundToInt((getWidth() - design::kWidth * scale) / 2);
#if JUCE_IOS
  const int y = juce::jmax(0, juce::roundToInt((getHeight() - designHeight() * scale) / 2));
#else
  const int y = 0;
#endif
  // The offset rides in the transform: a component's position is applied
  // before its transform, so setTopLeftPosition(x, y) would land at
  // (x, y) * scale and push a letterboxed root off the right edge.
  root_.setTopLeftPosition(0, 0);
  root_.setTransform(juce::AffineTransform::scale(static_cast<float>(scale))
                         .translated(static_cast<float>(x), static_cast<float>(y)));
}

void NativeEditor::paint(juce::Graphics& g) { g.fillAll(juce::Colours::black); }

void NativeEditor::paintOverChildren(juce::Graphics& g) {
  // Not paint(): the opaque root covers us, so JUCE clips our own paint to
  // nothing and never calls it. This runs after the whole tree has painted
  // once, which is the marker we want.
  if (loggedFirstPaint_) return;
  loggedFirstPaint_ = true;
  // The peer exists by now even if parentHierarchyChanged never saw it.
  logPeerAttached();
  trace("First paint: " + juce::String(getWidth()) + "x" + juce::String(getHeight()) + " | clip " +
        g.getClipBounds().toString());
}

void NativeEditor::resized() {
  if (!loggedFirstResize_) {
    loggedFirstResize_ = true;
    trace("First resized: " + juce::String(getWidth()) + "x" + juce::String(getHeight()) +
          " (scale " + formatScale(currentScale()) + ", extraHeight " +
          juce::String(extraContentHeight_) + ")");
  }
  fitRoot();
  // Persist the user's (or host's) chosen scale; skip while correcting our
  // own size. No chosen scale exists on iOS (the window is the screen).
#if !JUCE_IOS
  if (!restoringSize_) processor_.editorScale.store(currentScale());
#endif
}

// Key presses reach the editor when no control took them (the peer falls
// back to its component; children pass unused keys up). Space and Enter
// are the host's transport keys, so they go back to it (keyPassthrough.ts).
// Since clicks never focus buttons (Clickable), that is the state after any
// mouse work; only a text field, or a control the user Tabbed to, takes
// them for itself (PluginRoot's focus policy).
bool NativeEditor::keyPressed(const juce::KeyPress& key) {
  if (key == juce::KeyPress::spaceKey) return backend_.forwardKeyToHost(Backend::HostKey::space);
  if (key == juce::KeyPress::returnKey) return backend_.forwardKeyToHost(Backend::HostKey::enter);
  return false;
}

void NativeEditor::visibilityChanged() { services_.clock.wake(); }

void NativeEditor::parentHierarchyChanged() {
  services_.clock.wake();
  logPeerAttached();
#if !JUCE_IOS
  if (auto* window = dynamic_cast<juce::DocumentWindow*>(getTopLevelComponent())) {
    // Flipping the native title bar on relayouts the window's content split
    // and can briefly mis-size us; re-assert our exact size so JUCE's own
    // resize listener grows the window to contain us again, and don't let
    // that correction clobber the persisted scale.
    const int w = getWidth();
    const int h = getHeight();
    restoringSize_ = true;
    window->setUsingNativeTitleBar(true);
    // Plugin wrappers read usesWindowsMultiTouch(); the standalone's window
    // owns the peer instead, so opt it in here (see NativeEditor.h).
    window->setUsingWindowsMultiTouch(true);
    setSize(w, h);
    juce::Component::SafePointer<NativeEditor> self(this);
    juce::MessageManager::callAsync([self] {
      if (self != nullptr) self->restoringSize_ = false;
    });
  }
#endif
}

}  // namespace t3k::ui
