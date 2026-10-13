#include "mini_daw_host.h"
#include "mini_daw_verify.h"
#include <juce_gui_extra/juce_gui_extra.h>
#include <windows.h>
#include <commctrl.h>
#pragma comment(lib, "comctl32.lib")
#include <map>
#include <thread>
#include <optional>
#include <MiniDAWAssets.h>

namespace mini {
const juce::Colour bg{0xff101319}, panel{0xff1a202a}, ink{0xffe2e7ef}, muted{0xff8993a4};
const juce::Colour blue{0xff489ada}, red{0xffd97780}, green{0xff64c4a4};
constexpr float corner = 6;
constexpr int canvasWidth = 1059, canvasHeight = 751;
juce::Image brandIcon() { return juce::ImageCache::getFromMemory(MiniDAWAssets::minidaw_png, MiniDAWAssets::minidaw_pngSize); }
class Mixer;

juce::PopupMenu::Options pointerMenuOptions(juce::Component& owner, const juce::MouseEvent& event) {
  auto options = juce::PopupMenu::Options().withTargetComponent(owner).withDeletionCheck(owner);
  if (event.source.isMouse()) return options.withTargetScreenArea(juce::Rectangle<int>{}.withPosition(event.getScreenPosition()));
  return options.withStandardItemHeight(32);
}

// Windows supplies touch/pen contacts separately from mouse right-clicks.
// Keep this gesture local to the control so its release cannot also click it.
class TouchContextGesture final : private juce::Timer {
public:
  using Callback = std::function<void(const juce::PopupMenu::Options&)>;
  TouchContextGesture(juce::Component& component, Callback callback) : owner(component), open(std::move(callback)) {}
  static constexpr int delayMs = 650;
  static constexpr float slop = 8;
  void press(const juce::MouseEvent& event) {
    if (source && source->isDragging() && *source != event.source) { cancel(); return; }
    stopTimer(); source.reset(); opened = cancelled = waiting = false;
    if (event.source.isMouse() || event.mods.isPopupMenu()) return;
    source = event.source; origin = event.getScreenPosition().toFloat();
    pressedAt = juce::Time::getMillisecondCounterHiRes(); waiting = true; startTimer(25);
  }
  bool drag(const juce::MouseEvent& event) {
    if (!source || *source != event.source) return false;
    if (event.getScreenPosition().toFloat().getDistanceFrom(origin) > slop) cancel();
    return opened;
  }
  bool release(const juce::MouseEvent& event) {
    if (!source || *source != event.source) return false;
    stopTimer();
    // Also recognise a hold if a busy message thread delayed the timer.
    const bool fire = waiting && !cancelled && owner.isShowing() && owner.isEnabled() && event.getLengthOfMousePress() >= delayMs
      && event.getScreenPosition().toFloat().getDistanceFrom(origin) <= slop && !otherContactDown();
    const bool consume = opened || fire;
    waiting = false; source.reset();
    if (fire) { opened = true; show(); }
    return consume;
  }
  bool isWaiting() const { return waiting; }
  bool wasCancelled() const { return cancelled; }
private:
  juce::Component& owner;
  Callback open;
  std::optional<juce::MouseInputSource> source;
  juce::Point<float> origin;
  double pressedAt = 0;
  bool waiting = false, opened = false, cancelled = false;
  void cancel() { waiting = false; cancelled = true; stopTimer(); }
  bool otherContactDown() const {
    for (const auto& contact : juce::Desktop::getInstance().getMouseSources())
      if (contact.isDragging() && !contact.isMouse() && (!source || contact != *source)) return true;
    return false;
  }
  void show() {
    auto callback = open;
    auto options = juce::PopupMenu::Options().withTargetComponent(owner).withDeletionCheck(owner)
      .withTargetScreenArea(juce::Rectangle<int>{}.withPosition(origin.roundToInt()).expanded(12)).withStandardItemHeight(32);
    if (callback) callback(options);
  }
  void timerCallback() override {
    if (!source || !source->isDragging() || !owner.isShowing() || !owner.isEnabled() || otherContactDown()
        || source->getScreenPosition().getDistanceFrom(origin) > slop) { cancel(); return; }
    if (juce::Time::getMillisecondCounterHiRes()-pressedAt < delayMs) return;
    stopTimer(); waiting = false; opened = true; show();
  }
};

class ContextSurface : public juce::Component {
public:
  TouchContextGesture::Callback context;
  void mouseDown(const juce::MouseEvent& event) override {
    touchContext.press(event);
    if (event.mods.isPopupMenu() && context) context(pointerMenuOptions(*this, event));
  }
  void mouseDrag(const juce::MouseEvent& event) override { touchContext.drag(event); }
  void mouseUp(const juce::MouseEvent& event) override { touchContext.release(event); }
private:
  TouchContextGesture touchContext{*this, [this](const auto& options) { if (context) context(options); }};
};

class ModernLook final : public juce::LookAndFeel_V4 {
public:
  void drawScrollbar(juce::Graphics& g, juce::ScrollBar& bar, int x, int y, int w, int h,
                     bool vertical, int thumbStart, int thumbSize, bool over, bool down) override {
    if (!bar.getProperties().getWithDefault("dragNavigation", false)) {
      LookAndFeel_V4::drawScrollbar(g, bar, x, y, w, h, vertical, thumbStart, thumbSize, over, down); return;
    }
    if (!bar.getProperties().getWithDefault("navigationShown", false)) return;
    // Centre a slim thumb in a wider, permanent hit area, with equal clear space on either side.
    g.setColour(muted.withAlpha(0.12f));
    if (vertical) g.fillRoundedRectangle(x+w*0.5f-1, static_cast<float>(y+6), 2, h-12.0f, 1);
    else g.fillRoundedRectangle(static_cast<float>(x+6), y+h*0.5f-1, w-12.0f, 2, 1);
    g.setColour(muted.withAlpha(down ? 0.85f : 0.55f));
    if (vertical) g.fillRoundedRectangle(x+w*0.5f-1.5f, static_cast<float>(thumbStart+2), 3, std::max(3.0f, thumbSize-4.0f), 1.5f);
    else g.fillRoundedRectangle(static_cast<float>(thumbStart+2), y+h*0.5f-1.5f, std::max(3.0f, thumbSize-4.0f), 3, 1.5f);
  }
  void drawButtonBackground(juce::Graphics& g, juce::Button& b, const juce::Colour& c, bool over, bool down) override {
    const auto bounds = b.getLocalBounds().toFloat().reduced(0.5f);
    g.setColour(c.brighter(down ? 0.12f : over ? 0.06f : 0)); g.fillRoundedRectangle(bounds, corner);
    g.setColour(ink.withAlpha(over ? 0.18f : 0.07f)); g.drawRoundedRectangle(bounds, corner, 1);
  }
  void drawButtonText(juce::Graphics& g, juce::TextButton& b, bool over, bool down) override {
    if (!b.getProperties().getWithDefault("noTextCompression", false)) { LookAndFeel_V4::drawButtonText(g, b, over, down); return; }
    g.setFont(getTextButtonFont(b, b.getHeight())); g.setColour(b.findColour(juce::TextButton::textColourOffId).withMultipliedAlpha(b.isEnabled() ? 1.0f : 0.5f));
    g.drawText(b.getButtonText(), b.getLocalBounds().reduced(6, 0), juce::Justification::centred, true);
  }
  juce::Font getTextButtonFont(juce::TextButton&, int height) override { return juce::Font(juce::FontOptions(juce::jmin(14.0f, height*0.52f))); }
  juce::Font getComboBoxFont(juce::ComboBox&) override { return juce::Font(juce::FontOptions(13)); }
  void positionComboBoxText(juce::ComboBox& box, juce::Label& label) override {
    LookAndFeel_V4::positionComboBoxText(box, label); label.setMinimumHorizontalScale(1.0f);
  }
  void drawComboBox(juce::Graphics& g, int w, int h, bool, int, int, int, int, juce::ComboBox& box) override {
    const auto bounds = juce::Rectangle<float>(0.5f, 0.5f, w-1.0f, h-1.0f);
    g.setColour(box.findColour(juce::ComboBox::backgroundColourId)); g.fillRoundedRectangle(bounds, corner);
    g.setColour(box.findColour(juce::ComboBox::outlineColourId).withAlpha(0.35f)); g.drawRoundedRectangle(bounds, corner, 1);
    juce::Path chevron; chevron.startNewSubPath(w-17.0f, h*0.45f); chevron.lineTo(w-13.0f, h*0.58f); chevron.lineTo(w-9.0f, h*0.45f);
    g.setColour(muted); g.strokePath(chevron, juce::PathStrokeType(1.4f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
  }
};

class ToolbarIcon final : public juce::TextButton {
public:
  enum Kind { power, gear, back, forward };
  explicit ToolbarIcon(Kind k) : kind(k) {
    const void* data = nullptr; int size = 0;
    if (kind == power) { data = MiniDAWAssets::toolbarpower_png; size = MiniDAWAssets::toolbarpower_pngSize; }
    if (kind == gear) { data = MiniDAWAssets::toolbarsettings_png; size = MiniDAWAssets::toolbarsettings_pngSize; }
    if (kind == back) { data = MiniDAWAssets::toolbarundo_png; size = MiniDAWAssets::toolbarundo_pngSize; }
    if (kind == forward) { data = MiniDAWAssets::toolbarredo_png; size = MiniDAWAssets::toolbarredo_pngSize; }
    icon = juce::ImageCache::getFromMemory(data, size);
  }
  void paintButton(juce::Graphics& g, bool over, bool down) override {
    getLookAndFeel().drawButtonBackground(g, *this, findColour(juce::TextButton::buttonColourId), over, down);
    g.setColour(findColour(juce::TextButton::textColourOffId).withMultipliedAlpha(isEnabled() ? 1.0f : 0.28f));
    const int inset = kind == back || kind == forward ? 9 : 5;
    g.drawImageWithin(icon, inset, inset, getWidth()-inset*2, getHeight()-inset*2, juce::RectanglePlacement::centred, true);
  }
private:
  Kind kind; juce::Image icon;
};

class PresetLoadButton final : public juce::TextButton, private juce::Timer {
public:
  PresetLoadButton() : TextButton(utf8("加载预设")) {}
  std::function<void()> showRecent;
  void mouseEnter(const juce::MouseEvent& e) override { TextButton::mouseEnter(e); if (e.source.isMouse()) startTimer(450); }
  void mouseExit(const juce::MouseEvent& e) override { stopTimer(); TextButton::mouseExit(e); }
  void mouseDown(const juce::MouseEvent& e) override {
    stopTimer(); touchContext.press(e); popupPress = e.mods.isPopupMenu();
    if (popupPress) { if (showRecent) showRecent(); }
    else TextButton::mouseDown(e);
  }
  void mouseDrag(const juce::MouseEvent& e) override {
    if (!touchContext.drag(e)) TextButton::mouseDrag(e);
  }
  void mouseUp(const juce::MouseEvent& e) override {
    if (touchContext.release(e) || popupPress) { popupPress = false; setState(juce::Button::buttonNormal); return; }
    TextButton::mouseUp(e);
  }
private:
  bool popupPress = false;
  TouchContextGesture touchContext{*this, [this](const auto&) {
    popupPress = true; setState(juce::Button::buttonNormal); if (showRecent) showRecent();
  }};
  void timerCallback() override {
    stopTimer(); if (isEnabled() && isShowing() && isMouseOver() && !isDown() && showRecent) showRecent();
  }
};

class ColourPalette final : public juce::ColourSelector, private juce::ChangeListener {
public:
  ColourPalette(juce::Colour colour, std::function<void(juce::Colour)> fn, std::function<void()> done = {})
      : ColourSelector(showColourAtTop | editableColour | showSliders | showColourspace), apply(std::move(fn)), finished(std::move(done)) {
    for (int i = 0; i < 24; ++i) swatches[i] = juce::Colour::fromHSV((i%12)/12.0f, i < 12 ? 0.50f : 0.80f, i < 12 ? 0.93f : 0.70f, 1.0f);
    setCurrentColour(colour, juce::dontSendNotification); setColour(backgroundColourId, panel); setColour(labelTextColourId, ink);
    addChangeListener(this); setSize(360, 440);
  }
  ~ColourPalette() override { removeChangeListener(this); if (finished) finished(); }
  int getNumSwatches() const override { return static_cast<int>(swatches.size()); }
  juce::Colour getSwatchColour(int i) const override { return swatches[i]; }
  void setSwatchColour(int i, const juce::Colour& c) override { swatches[i] = c; }
private:
  std::array<juce::Colour, 24> swatches;
  std::function<void(juce::Colour)> apply;
  std::function<void()> finished;
  void changeListenerCallback(juce::ChangeBroadcaster*) override { if (apply) apply(getCurrentColour()); }
};

class HoverMotion final : private juce::Timer {
public:
  explicit HoverMotion(std::function<void()> changed) : repaint(std::move(changed)) {}
  float amount() const { return value; }
  void reset() { stopTimer(); value = target = 0; if (repaint) repaint(); }
  void set(bool active) { const float next = active ? 1.0f : 0.0f; if (target == next) return; target = next; lastFrame = juce::Time::getMillisecondCounterHiRes(); startTimerHz(60); }
private:
  float value = 0, target = 0;
  double lastFrame = 0;
  std::function<void()> repaint;
  void timerCallback() override {
    const double now = juce::Time::getMillisecondCounterHiRes();
    value += (target-value)*static_cast<float>(1-std::exp(-(now-lastFrame)/55.0)); lastFrame = now;
    if (std::abs(target-value) < 0.005f) { value = target; stopTimer(); }
    if (repaint) repaint();
  }
};

class DropGap final : public juce::Component, public juce::DragAndDropTarget {
public:
  DropGap(bool horizontalLine, juce::String prefix) : horizontal(horizontalLine), kind(std::move(prefix)) {}
  std::function<void(const juce::String&)> dropped;
  bool isInterestedInDragSource(const SourceDetails& d) override { return d.description.toString().startsWith(kind); }
  void itemDragEnter(const SourceDetails&) override { motion.set(true); }
  void itemDragExit(const SourceDetails&) override { motion.set(false); }
  void itemDropped(const SourceDetails& d) override { motion.set(false); if (dropped) dropped(d.description.toString()); }
  void paint(juce::Graphics& g) override {
    const float amount = motion.amount(); if (amount == 0) return;
    g.setColour(green.withAlpha(amount));
    if (horizontal) { const float w = getWidth()*(0.5f+amount*0.5f); g.fillRoundedRectangle((getWidth()-w)*0.5f, getHeight()*0.5f-1, w, 2, 1); }
    else { const float h = (getHeight()-16.0f)*(0.5f+amount*0.5f); g.fillRoundedRectangle(getWidth()*0.5f-1.5f, (getHeight()-h)*0.5f, 3, h, 1.5f); }
  }
private:
  bool horizontal; juce::String kind;
  HoverMotion motion{[this] { repaint(); }};
};

class MeterFader final : public juce::Component, private juce::Timer {
public:
  std::function<void(float)> changed;
  std::function<void()> begin, end;
  void setUnityVisualPosition(float value) { unityVisual = juce::jlimit(0.05f, 0.95f, value); repaint(); }
  float visualPosition(float value) const {
    const float db = positionToDb(value), step = visualDbStep(), tail = unityVisual-36*step;
    return value <= 0 ? 0 : db >= -36 ? unityVisual+db*step : tail*std::exp2((db+36)/24);
  }
  explicit MeterFader(Channel& c) : channel(c) {
    setMouseCursor(juce::MouseCursor::UpDownResizeCursor); setWantsKeyboardFocus(true); startTimerHz(60);
  }
  void paint(juce::Graphics& g) override {
    auto rail = getLocalBounds().toFloat().reduced(7, 10);
    g.setColour(juce::Colour(0xff090e16)); g.fillRoundedRectangle(rail, corner);
    const float handleY = rail.getBottom() - rail.getHeight()*visualPosition(channel.fader.load());
    g.setColour(channel.colour.withAlpha(0.10f)); g.fillRoundedRectangle(rail.withTop(handleY), 4);
    for (int side = 0; side < 2; ++side) {
      const float peak = channel.peak[side].load();
      float position = visualPosition(displayed[side]);
      g.setColour(peak >= 1 ? red : green);
      auto bar = rail.withWidth(rail.getWidth()/2-1).translated(side*(rail.getWidth()/2+1), 0);
      g.fillRoundedRectangle(bar.withTop(bar.getBottom()-bar.getHeight()*position), 3);
    }
    g.setColour(juce::Colours::white.withAlpha(0.15f));
    g.drawHorizontalLine(static_cast<int>(rail.getBottom()-rail.getHeight()*unityVisual), rail.getX(), rail.getRight());
    float y = rail.getBottom() - rail.getHeight()*visualPosition(channel.fader.load());
    g.setColour(juce::Colour(0xffb5c8dd)); g.fillRoundedRectangle(1, y-8, static_cast<float>(getWidth()-2), 16, corner);
    g.setColour(panel); g.drawHorizontalLine(static_cast<int>(y), 5, static_cast<float>(getWidth()-5));
  }
  void mouseDown(const juce::MouseEvent& e) override { grabKeyboardFocus(); startPosition = channel.fader.load(); dragging = false; }
  void mouseDrag(const juce::MouseEvent& e) override {
    if (e.mods.isPopupMenu()) return;
    if (!dragging && e.getDistanceFromDragStart() < 4) return;
    if (!dragging) { dragging = true; if (begin) begin(); }
    const float height = static_cast<float>(getHeight()-20);
    const float value = juce::jlimit(0.0f, 1.0f, visualPosition(startPosition)-e.getDistanceFromDragStartY()/height);
    set(std::abs(value-unityVisual)*height <= 5 ? dbToPosition(0) : modelPosition(value));
  }
  void mouseUp(const juce::MouseEvent&) override { if (dragging && end) end(); dragging = false; }
  void mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override { Component::mouseWheelMove(e, w); }
  bool keyPressed(const juce::KeyPress& key) override {
    if (key == juce::KeyPress::upKey || key == juce::KeyPress::downKey) {
      if (begin) begin(); set(modelPosition(visualPosition(channel.fader.load())+(key == juce::KeyPress::upKey ? 1 : -1)/84.0f)); if (end) end(); return true;
    }
    if (key == juce::KeyPress::homeKey) { if (begin) begin(); set(dbToPosition(0)); if (end) end(); return true; }
    return false;
  }
private:
  std::array<float, 2> displayed{0, 0};
  double lastFrame = juce::Time::getMillisecondCounterHiRes();
  void timerCallback() override {
    const double now = juce::Time::getMillisecondCounterHiRes();
    const float dt = static_cast<float>(juce::jlimit(0.001, 0.1, (now-lastFrame)/1000.0)); lastFrame = now;
    for (int side = 0; side < 2; ++side) {
      const float peak = channel.peak[side].load();
      const float target = dbToPosition(peak > 0 ? 20*std::log10(peak) : -INFINITY);
      const float tau = target > displayed[side] ? 0.045f : 0.32f;
      displayed[side] += (target-displayed[side])*(1-std::exp(-dt/tau));
      if (displayed[side] < 0.0001f) displayed[side] = 0;
    }
    repaint();
  }
  Channel& channel;
  float unityVisual = dbToPosition(0);
  float visualDbStep() const { return std::min((1-unityVisual)/12, (unityVisual-0.04f)/36); }
  float modelPosition(float value) const {
    if (value <= 0) return 0;
    const float step = visualDbStep(), tail = unityVisual-36*step;
    return dbToPosition(value >= tail ? (value-unityVisual)/step : -36+24*std::log2(value/tail));
  }
  float startPosition = 0; bool dragging = false;
  void set(float value) { value = juce::jlimit(0.0f, 1.0f, value); channel.fader = value; if (changed) changed(value); repaint(); }
};

class PluginSlot final : public juce::Component, public juce::DragAndDropTarget, public juce::SettableTooltipClient, private juce::Timer {
public:
  std::function<void()> open, drag, toggle;
  TouchContextGesture::Callback context;
  std::function<void(juce::Point<int>)> dragFollow, dragReleased;
  std::function<juce::Colour()> tint;
  std::function<void(const juce::String&)> drop;
  PluginSlot(Slot& s, int i) : slot(s), number(i) { setMouseCursor(juce::MouseCursor::PointingHandCursor); }
  void showAsEmpty(bool empty) { stopTimer(); dragEmpty = empty; reveal = 1; blurred = {}; repaint(); }
  bool isDragEmpty() const { return dragEmpty; }
  bool isRevealing() const { return isTimerRunning(); }
  void revealAfterCopy() {
    showAsEmpty(false);
    const auto sharp = createComponentSnapshot(getLocalBounds(), true, 2);
    blurred = sharp.createCopy(); juce::ImageConvolutionKernel kernel(9); kernel.createGaussianBlur(1.8f);
    kernel.applyToImage(blurred, sharp, blurred.getBounds());
    reveal = 0; revealStarted = juce::Time::getMillisecondCounterHiRes(); startTimerHz(60); repaint();
  }
  void paint(juce::Graphics& g) override {
    if (dragEmpty || reveal < 1) drawCard(g, false);
    if (!dragEmpty && reveal > 0) {
      juce::Graphics::ScopedSaveState save(g);
      if (blurred.isValid() && reveal < 1) {
        g.setOpacity(reveal*(1-reveal)); g.drawImage(blurred, getLocalBounds().toFloat());
        g.setOpacity(1); g.beginTransparencyLayer(reveal*reveal);
      }
      drawCard(g, slot.plugin != nullptr);
      if (blurred.isValid() && reveal < 1) g.endTransparencyLayer();
    }
  }
  void clearDropHighlight() { motion.reset(); }
  juce::Image landingImageFrom(const PluginSlot& origin, std::optional<juce::Colour> colour = {}) {
    juce::Image result(juce::Image::ARGB, (getWidth()+28)*2, (getHeight()+28)*2, true);
    juce::Graphics g(result); g.addTransform(juce::AffineTransform::scale(2)); g.setOrigin(14, 14);
    drawCard(g, true, &origin.slot, 0, colour); return result;
  }
  void drawCard(juce::Graphics& g, bool occupied, const Slot* content = nullptr, float hoverOverride = -1, std::optional<juce::Colour> colourOverride = {}) {
    const auto& shown = content ? *content : slot;
    const auto colour = colourOverride.value_or(tint ? tint() : blue);
    const auto bounds = getLocalBounds().toFloat().reduced(0.5f);
    g.setColour(occupied ? (shown.enabled ? colour.withAlpha(0.14f) : juce::Colour(0xff232932)) : juce::Colour(0xff151a22));
    g.fillRoundedRectangle(bounds, corner);
    const float hover = hoverOverride >= 0 ? hoverOverride : motion.amount();
    g.setColour((occupied ? colour.withAlpha(0.32f) : ink.withAlpha(0.06f)).interpolatedWith(green, hover)); g.drawRoundedRectangle(bounds, corner, 1+hover);
    if (occupied) {
      g.setColour(shown.enabled ? colour : muted); g.fillEllipse(7, getHeight()*0.5f-3, 6, 6);
      g.setFont(12); g.setColour(muted); g.drawText(juce::String(number+1).paddedLeft('0', 2), 19, 0, 19, getHeight(), juce::Justification::centredLeft);
      g.setFont(13); g.setColour(ink); g.drawText(shown.description.name, 41, 0, getWidth()-46, getHeight(), juce::Justification::centredLeft, true);
    }
    if (hover > 0) { g.setColour(green.withAlpha(0.14f*hover)); g.fillRoundedRectangle(bounds, corner); }
  }
  void mouseDown(const juce::MouseEvent& e) override {
    touchContext.press(e); dragged = false; dot = slot.plugin && !dragEmpty && e.position.x < 18;
    if (e.mods.isPopupMenu()) { if (context) context(pointerMenuOptions(*this, e)); }
    else if (dot && e.source.isMouse() && e.getNumberOfClicks() == 1) { if (toggle) toggle(); repaint(); }
  }
  void mouseDrag(const juce::MouseEvent& e) override {
    if (touchContext.drag(e)) return;
    const float threshold = e.source.isMouse() ? 6.0f : TouchContextGesture::slop;
    if (!dot && !dragged && !dragEmpty && !e.mods.isPopupMenu() && slot.plugin && e.getDistanceFromDragStart() > threshold) { dragged = true; if (drag) drag(); }
    if (dragged && dragFollow) dragFollow(e.getScreenPosition());
  }
  void mouseUp(const juce::MouseEvent& e) override {
    const bool cancelled = touchContext.wasCancelled();
    if (touchContext.release(e)) return;
    if (dragged && dragReleased) dragReleased(e.getScreenPosition());
    if (dot && !e.source.isMouse() && !cancelled && !e.mods.isPopupMenu() && e.getNumberOfClicks() == 1) {
      if (toggle) toggle(); repaint();
    }
    if (!dot && !dragged && (e.source.isMouse() || !cancelled) && !e.mods.isPopupMenu() && e.getNumberOfClicks() == 1 && open) open();
  }
  bool isInterestedInDragSource(const SourceDetails& d) override { return d.description.toString().startsWith("plugin:"); }
  void itemDragEnter(const SourceDetails& d) override { itemDragMove(d); }
  void itemDragMove(const SourceDetails&) override { motion.set(true); }
  void itemDragExit(const SourceDetails&) override { motion.set(false); }
  void itemDropped(const SourceDetails& d) override {
    motion.set(false); if (drop) drop(d.description.toString());
  }
private:
  Slot& slot; int number;
  bool dragged = false, dot = false, dragEmpty = false;
  TouchContextGesture touchContext{*this, [this](const auto& options) { if (context) context(options); }};
  float reveal = 1; double revealStarted = 0; juce::Image blurred;
  void timerCallback() override {
    const float progress = juce::jlimit(0.0f, 1.0f, static_cast<float>((juce::Time::getMillisecondCounterHiRes()-revealStarted)/180));
    reveal = 1-std::pow(1-progress, 3); if (progress >= 1) { stopTimer(); blurred = {}; } repaint();
  }
  HoverMotion motion{[this] { repaint(); }};
};

class ChannelTitle final : public juce::Label {
public:
  std::function<void()> drag;
  TouchContextGesture::Callback context;
  void mouseDown(const juce::MouseEvent& e) override {
    touchContext.press(e); dragging = false;
    if (e.mods.isPopupMenu()) { if (context) context(pointerMenuOptions(*this, e)); } else Label::mouseDown(e);
  }
  void mouseDrag(const juce::MouseEvent& e) override {
    if (touchContext.drag(e)) return;
    const float threshold = e.source.isMouse() ? 6.0f : TouchContextGesture::slop;
    if (!dragging && !e.mods.isPopupMenu() && e.getDistanceFromDragStart() > threshold) { dragging = true; if (drag) drag(); }
  }
  void mouseUp(const juce::MouseEvent& e) override {
    if (touchContext.release(e) || dragging) return;
    if (!e.source.isMouse() && !touchContext.wasCancelled() && e.getNumberOfClicks() == 2) Label::mouseDoubleClick(e);
    else Label::mouseUp(e);
  }
  void mouseDoubleClick(const juce::MouseEvent& e) override { if (e.source.isMouse()) Label::mouseDoubleClick(e); }
private:
  bool dragging = false;
  TouchContextGesture touchContext{*this, [this](const auto& options) { if (context) context(options); }};
};
class SendArea final : public ContextSurface {
public:
  std::function<void(juce::Graphics&)> draw;
  void paint(juce::Graphics& g) override { if (draw) draw(g); }
};

class StripResizeEdge final : public juce::Component {
public:
  explicit StripResizeEdge(bool widthEdge) : horizontal(widthEdge) {
    setMouseCursor(horizontal ? juce::MouseCursor::LeftRightResizeCursor : juce::MouseCursor::UpDownResizeCursor);
  }
  std::function<void(const juce::ModifierKeys&)> begin, reset;
  std::function<void()> end;
  std::function<void(int)> drag;
  void mouseDown(const juce::MouseEvent& e) override { active = false; if (e.mods.isLeftButtonDown() && begin) begin(e.mods); }
  void mouseDrag(const juce::MouseEvent& e) override {
    if (!e.mods.isLeftButtonDown()) return;
    active = true; if (drag) drag(horizontal ? e.getDistanceFromDragStartX() : e.getDistanceFromDragStartY()); repaint();
  }
  void mouseUp(const juce::MouseEvent&) override { if (end) end(); active = false; repaint(); }
  void mouseDoubleClick(const juce::MouseEvent& e) override { if (reset) reset(e.mods); }
  void mouseEnter(const juce::MouseEvent&) override { repaint(); }
  void mouseExit(const juce::MouseEvent&) override { repaint(); }
  void paint(juce::Graphics& g) override {
    if (!active && !isMouseOver()) return;
    g.setColour(muted.withAlpha(active ? 0.65f : 0.3f));
    if (horizontal) g.fillRoundedRectangle(getWidth()*0.5f-1, 12, 2, getHeight()-24.0f, 1);
    else g.fillRoundedRectangle(12, getHeight()*0.5f-1, getWidth()-24.0f, 2, 1);
  }
private:
  bool horizontal, active = false;
};

class SendKnob final : public juce::Slider, private juce::Timer, public juce::DragAndDropTarget {
public:
  static juce::Rectangle<int> valueLabelBounds(float scale, int width) { return {0, juce::roundToInt(32*scale), width, 12}; }
  TouchContextGesture::Callback choose;
  std::function<void()> floatDrag;
  std::function<void(const juce::String&)> drop;
  SendKnob() : Slider(RotaryVerticalDrag, TextBoxBelow) {
    setRange(-72, 12, 0.1); setValue(0, juce::dontSendNotification); setTextBoxStyle(TextBoxBelow, false, 58, 12);
    setDoubleClickReturnValue(true, 0);
    setColour(rotarySliderFillColourId, green); setColour(textBoxOutlineColourId, juce::Colours::transparentBlack);
    setScrollWheelEnabled(false); setViewportIgnoreDragFlag(true);
  }
  void resized() override {
    Slider::resized();
    const float scale = static_cast<float>(getProperties().getWithDefault("knobScale", 1.0f));
    // Sliders are constructed before the strip joins the mixer; style the existing label as well.
    for (auto* child : getChildren()) if (auto* label = dynamic_cast<juce::Label*>(child)) {
      label->setFont(juce::Font(juce::FontOptions(10))); label->setMinimumHorizontalScale(1);
      label->setBorderSize(juce::BorderSize<int>(0)); label->setColour(juce::Label::textColourId, muted.withAlpha(0.8f));
      label->setBounds(valueLabelBounds(scale, getWidth()));
    }
  }
  void paint(juce::Graphics& g) override {
    const float start = juce::MathConstants<float>::pi*1.25f;
    const float sweep = juce::MathConstants<float>::pi*1.5f;
    const float angle = start + static_cast<float>((getValue()+72)/84)*sweep;
    const float scale = static_cast<float>(getProperties().getWithDefault("knobScale", 1.0f));
    const float x = getWidth()*0.5f, y = 17*scale;
    g.addTransform(juce::AffineTransform::scale(scale).translated(x*(1-scale), 0));
    const float centreY = y/scale;
    const auto stroke = juce::PathStrokeType(2, juce::PathStrokeType::curved, juce::PathStrokeType::rounded);
    juce::Path track; track.addCentredArc(x, centreY, 14, 14, 0, start, start+sweep, true);
    g.setColour(ink.withAlpha(0.10f)); g.strokePath(track, stroke);
    juce::Path level; level.addCentredArc(x, centreY, 14, 14, 0, start, angle, true);
    g.setColour(findColour(rotarySliderFillColourId)); g.strokePath(level, stroke);
    g.setColour(juce::Colour(0xff0e131b)); g.fillEllipse(x-10, centreY-10, 20, 20);
    if (hoverDrop || floating) { g.setColour(green.withAlpha(0.22f)); g.fillEllipse(x-16, centreY-16, 32, 32); }
    g.setColour(findColour(rotarySliderFillColourId)); g.drawLine(x+std::sin(angle)*3, centreY-std::cos(angle)*3,
                                 x+std::sin(angle)*8, centreY-std::cos(angle)*8, 2);
  }
  juce::String getTextFromValue(double v) override { return v <= -72 ? juce::String::fromUTF8("−∞") : juce::String(v, 1)+" dB"; }
  double getValueFromText(const juce::String& s) override { return s.contains(juce::String::fromUTF8("∞")) ? -72 : s.getDoubleValue(); }
  void mouseDown(const juce::MouseEvent& e) override {
    stopTimer(); hold.release(); initialEvent.reset(); adjusting = floating = false;
    touchMenuOpened = touchCancelled = false;
    if (e.mods.isPopupMenu()) { if (choose) choose(pointerMenuOptions(*this, e)); return; }
    initialEvent.emplace(e);
    pressedAt = juce::Time::getMillisecondCounterHiRes();
    if (e.source.isMouse()) hold.press(pressedAt);
    else if (otherTouchDown()) { touchCancelled = true; return; }
    startTimer(16);
  }
  void mouseDrag(const juce::MouseEvent& e) override {
    if (floating || !initialEvent) return;
    if (!e.source.isMouse()) {
      if (otherTouchDown()) { stopTimer(); touchCancelled = true; return; }
      const bool moved = e.getScreenPosition().toFloat().getDistanceFrom(initialEvent->getScreenPosition().toFloat()) > TouchContextGesture::slop;
      if (touchMenuOpened) {
        // Moving into the open menu cancels the second hold stage.
        if (moved) { stopTimer(); touchCancelled = true; }
        return;
      }
      if (!adjusting && !touchCancelled && !moved) return;
      touchCancelled = true;
    }
    hold.move(static_cast<float>(e.getDistanceFromDragStart()));
    if (hold.waiting) return;
    stopTimer();
    if (!adjusting) { adjusting = true; Slider::mouseDown(*initialEvent); }
    Slider::mouseDrag(e);
  }
  void mouseUp(const juce::MouseEvent& e) override {
    const bool showMenu = !e.source.isMouse() && initialEvent && !adjusting && !floating && !touchMenuOpened && !touchCancelled
      && e.getLengthOfMousePress() >= touchMenuDelayMs && !otherTouchDown()
      && e.getScreenPosition().toFloat().getDistanceFrom(initialEvent->getScreenPosition().toFloat()) <= TouchContextGesture::slop;
    const bool consumed = touchMenuOpened || floating || showMenu;
    stopTimer(); if (!consumed && adjusting) Slider::mouseUp(e);
    if (!consumed && !adjusting && !e.source.isMouse() && !touchCancelled && e.getNumberOfClicks() == 2)
      Slider::mouseDoubleClick(e);
    if (showMenu) openTouchMenu();
    hold.release(); initialEvent.reset(); adjusting = floating = false; repaint();
  }
  void mouseDoubleClick(const juce::MouseEvent& e) override { if (e.source.isMouse()) Slider::mouseDoubleClick(e); }
  bool isInterestedInDragSource(const SourceDetails& d) override { return d.description.toString().startsWith("send:"); }
  void itemDragEnter(const SourceDetails&) override { hoverDrop = true; repaint(); }
  void itemDragExit(const SourceDetails&) override { hoverDrop = false; repaint(); }
  void itemDropped(const SourceDetails& d) override { hoverDrop = false; repaint(); if (drop) drop(d.description.toString()); }
private:
  static constexpr int touchMenuDelayMs = 350, touchDragDelayMs = 850;
  InitialHoldGesture hold;
  std::optional<juce::MouseEvent> initialEvent;
  double pressedAt = 0;
  bool adjusting = false, floating = false, hoverDrop = false, touchMenuOpened = false, touchCancelled = false;
  bool otherTouchDown() const {
    for (const auto& source : juce::Desktop::getInstance().getMouseSources())
      if (!source.isMouse() && source.isDragging() && (!initialEvent || source != initialEvent->source)) return true;
    return false;
  }
  void openTouchMenu() {
    if (!initialEvent) return;
    touchMenuOpened = true;
    auto options = juce::PopupMenu::Options().withTargetComponent(*this).withDeletionCheck(*this)
      .withTargetScreenArea(juce::Rectangle<int>{}.withPosition(initialEvent->getScreenPosition()).expanded(12)).withStandardItemHeight(32);
    auto callback = choose; if (callback) callback(options);
  }
  void timerCallback() override {
    if (!initialEvent || !initialEvent->source.isDragging()) { stopTimer(); hold.release(); initialEvent.reset(); return; }
    if (!initialEvent->source.isMouse()) {
      if (!isShowing() || !isEnabled() || otherTouchDown() || touchCancelled
          || initialEvent->source.getScreenPosition().getDistanceFrom(initialEvent->getScreenPosition().toFloat()) > TouchContextGesture::slop) {
        stopTimer(); touchCancelled = true; return;
      }
      const auto elapsed = juce::Time::getMillisecondCounterHiRes()-pressedAt;
      if (elapsed >= touchDragDelayMs) {
        stopTimer(); floating = true; repaint();
        if (touchMenuOpened) juce::PopupMenu::dismissAllActiveMenus();
        // Leave the menu's modal state before handing this same finger to JUCE drag-and-drop.
        juce::MessageManager::callAsync([safe = juce::Component::SafePointer<SendKnob>(this)] {
          if (!safe || !safe->floating || !safe->initialEvent || !safe->initialEvent->source.isDragging() || safe->otherTouchDown()) return;
          auto callback = safe->floatDrag; if (callback) callback();
        });
      } else if (elapsed >= touchMenuDelayMs && !touchMenuOpened) openTouchMenu();
      return;
    }
    // Account for small pointer jitter even when the component received no drag event.
    hold.move(initialEvent->source.getScreenPosition().getDistanceFrom(initialEvent->getScreenPosition().toFloat()));
    if (!hold.waiting) { stopTimer(); return; }
    if (hold.trigger(juce::Time::getMillisecondCounterHiRes())) { stopTimer(); floating = true; repaint(); if (floatDrag) floatDrag(); }
  }
};

class EditorWindow final : public juce::DocumentWindow {
public:
  EditorWindow(Host& h, juce::AudioPluginInstance& p, bool visible = true) : DocumentWindow(p.getName(), panel, closeButton), host(h), plugin(p) {
    setUsingNativeTitleBar(true); auto* editor = plugin.createEditorIfNeeded();
    if (!editor) editor = new juce::GenericAudioProcessorEditor(plugin);
    if (auto* slot = findSlot()) {
      editor->setScaleFactor(slot->editorScale);
      if (editor->isResizable() && slot->editorWidth > 0 && slot->editorHeight > 0) editor->setBoundsConstrained({0, 0, slot->editorWidth, slot->editorHeight});
    }
    setContentOwned(editor, true); setResizable(editor->isResizable(), false);
    centreWithSize(editor->getWidth(), editor->getHeight()); setVisible(visible);
    hwnd = static_cast<HWND>(getPeer()->getNativeHandle());
    SetWindowLongPtrW(hwnd, GWL_EXSTYLE, GetWindowLongPtrW(hwnd, GWL_EXSTYLE) | WS_EX_CONTEXTHELP);
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
    auto menu = GetSystemMenu(hwnd, FALSE);
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr); AppendMenuW(menu, MF_STRING, SC_CONTEXTHELP, L"Settings...");
    SetWindowSubclass(hwnd, captionProc, 1, reinterpret_cast<DWORD_PTR>(this));
  }
  ~EditorWindow() override { rememberSize(); if (hwnd && IsWindow(hwnd)) RemoveWindowSubclass(hwnd, captionProc, 1); }
  void resized() override { DocumentWindow::resized(); rememberSize(); }
  void closeButtonPressed() override { setVisible(false); }
  void showSettings() {
    int channel = -1, slotIndex = -1; auto* slot = findSlot(&channel, &slotIndex); if (!slot) return;
    auto* editor = static_cast<juce::AudioProcessorEditor*>(getContentComponent());
    juce::PopupMenu menu;
    menu.addItem(1, utf8("窗口尺寸…")+" ("+juce::String(editor->getWidth())+utf8(" × ")+juce::String(editor->getHeight())+")", editor->isResizable());
    juce::PopupMenu dpi;
    const std::array<float, 7> scales{0.75f, 1, 1.25f, 1.5f, 1.75f, 2, 2.5f};
    for (int i = 0; i < static_cast<int>(scales.size()); ++i) dpi.addItem(10+i, (scales[i] == 1 ? utf8("跟随系统 · ") : juce::String())+juce::String(juce::roundToInt(scales[i]*100))+"%", true, std::abs(slot->editorScale-scales[i]) < 0.01f);
    menu.addSubMenu(utf8("高 DPI / 缩放（插件支持时）"), dpi);
    struct Choice { int bus, source; }; std::vector<Choice> choices;
    for (int bus = 1; bus < plugin.getBusCount(true); ++bus) {
      auto* b = plugin.getBus(true, bus); if (!b->isEnabled()) continue;
      juce::PopupMenu sources; bool connected = false;
      for (const auto& c : host.channels) for (const auto& r : c->sends) if (r.target == channel && r.slot == slotIndex && r.bus == bus) connected = true;
      choices.push_back({bus, -1}); sources.addItem(1000+static_cast<int>(choices.size()), utf8("无"), true, !connected);
      for (int source = 0; source < static_cast<int>(host.channels.size()); ++source) {
        if (source == channel) continue;
        bool selected = false;
        for (const auto& r : host.channels[source]->sends) if (r.target == channel && r.slot == slotIndex && r.bus == bus) selected = true;
        choices.push_back({bus, source}); sources.addItem(1000+static_cast<int>(choices.size()), host.channelLabel(source), true, selected);
      }
      menu.addSubMenu(utf8("侧链 SC ")+juce::String(bus)+utf8(" · ")+b->getName(), sources);
    }
    if (choices.empty()) menu.addItem(2, utf8("该插件没有可用侧链输入"), false);
    menu.showMenuAsync(juce::PopupMenu::Options(), [safe = juce::Component::SafePointer<EditorWindow>(this), choices, scales](int result) {
      if (!safe) return;
      if (result == 1) safe->chooseSize();
      else if (result >= 10 && result < 10+static_cast<int>(scales.size())) {
        safe->host.beginEdit();
        if (auto* slot = safe->findSlot()) slot->editorScale = scales[result-10];
        static_cast<juce::AudioProcessorEditor*>(safe->getContentComponent())->setScaleFactor(scales[result-10]); safe->rememberSize();
        safe->host.endEdit(utf8("插件缩放"));
      } else if (result > 1000 && result <= 1000+static_cast<int>(choices.size())) safe->setSidechain(choices[result-1001].bus, choices[result-1001].source);
    });
  }
private:
  Host& host; juce::AudioPluginInstance& plugin; HWND hwnd = nullptr;
  Slot* findSlot(int* channel = nullptr, int* index = nullptr) {
    for (int c = 0; c < static_cast<int>(host.channels.size()); ++c) for (int s = 0; s < slotCount; ++s)
      if (host.channels[c]->slots[s].plugin.get() == &plugin) { if (channel) *channel = c; if (index) *index = s; return &host.channels[c]->slots[s]; }
    return nullptr;
  }
  void rememberSize() { if (auto* slot = findSlot()) if (auto* editor = getContentComponent()) { slot->editorWidth = editor->getWidth(); slot->editorHeight = editor->getHeight(); } }
  void chooseSize() {
    auto* editor = static_cast<juce::AudioProcessorEditor*>(getContentComponent());
    if (!editor->isResizable()) return;
    auto dialog = std::make_shared<juce::AlertWindow>(utf8("插件窗口尺寸"), utf8("尺寸单位为逻辑像素；实际范围由插件决定。"), juce::MessageBoxIconType::NoIcon, this);
    dialog->addTextEditor("width", juce::String(editor->getWidth()), utf8("宽度"));
    dialog->addTextEditor("height", juce::String(editor->getHeight()), utf8("高度"));
    dialog->addButton(utf8("应用"), 1); dialog->addButton(utf8("取消"), 0);
    dialog->enterModalState(true, juce::ModalCallbackFunction::create([safe = juce::Component::SafePointer<EditorWindow>(this), dialog](int result) {
      if (!safe || result != 1) return;
      auto* editor = static_cast<juce::AudioProcessorEditor*>(safe->getContentComponent());
      const int width = juce::jlimit(100, 4096, dialog->getTextEditorContents("width").getIntValue());
      const int height = juce::jlimit(100, 2160, dialog->getTextEditorContents("height").getIntValue());
      safe->host.beginEdit(); editor->setBoundsConstrained({0, 0, width, height}); safe->rememberSize(); safe->host.endEdit(utf8("插件窗口尺寸"));
    }), false);
  }
  void setSidechain(int bus, int source) {
    int channel = -1, index = -1; if (!findSlot(&channel, &index)) return;
    host.beginEdit();
    if (source >= 0) {
      auto& sends = host.channels[source]->sends;
      int send = static_cast<int>(sends.size()); float gain = 1;
      for (int i = 0; i < static_cast<int>(sends.size()); ++i) if (sends[i].target == channel && sends[i].slot == index && sends[i].bus == bus) { send = i; gain = sends[i].gain; break; }
      if (!host.setRoute(source, send, {channel, index, bus, gain})) { host.endEdit(utf8("插件侧链")); juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "Mini DAW", host.error); return; }
    }
    for (int c = 0; c < static_cast<int>(host.channels.size()); ++c) if (c != source) {
      auto& sends = host.channels[c]->sends;
      for (int i = static_cast<int>(sends.size()); --i >= 0;) if (sends[i].target == channel && sends[i].slot == index && sends[i].bus == bus) host.removeRoute(c, i);
    }
    host.endEdit(utf8("插件侧链")); if (host.changed) host.changed();
  }
  static LRESULT CALLBACK captionProc(HWND window, UINT message, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR data) {
    auto* self = reinterpret_cast<EditorWindow*>(data);
    if (message == WM_SYSCOMMAND && (w & 0xfff0) == SC_CONTEXTHELP) {
      juce::MessageManager::callAsync([safe = juce::Component::SafePointer<EditorWindow>(self)] { if (safe) safe->showSettings(); }); return 0;
    }
    if (message == WM_NCDESTROY) { self->hwnd = nullptr; RemoveWindowSubclass(window, captionProc, id); }
    return DefSubclassProc(window, message, w, l);
  }
};

class PluginPicker final : public juce::Component, public juce::ListBoxModel, private juce::Timer {
public:
  PluginPicker(Host& h, std::function<void(const juce::PluginDescription&)> selected) : host(h), picked(std::move(selected)) {
    addAndMakeVisible(list); list.setModel(this); list.setRowHeight(36); list.setColour(juce::ListBox::backgroundColourId, bg);
    for (auto* b : {&scan, &folder, &load}) addAndMakeVisible(*b);
    addAndMakeVisible(search); addAndMakeVisible(info); addAndMakeVisible(progress);
    search.setTextToShowWhenEmpty(utf8("搜索效果插件 / 厂商"), muted); search.onTextChange = [this] { refresh(); };
    scan.onClick = [this] { beginScan(host.scanPaths()); };
    folder.onClick = [this] {
      chooser = std::make_unique<juce::FileChooser>(utf8("选择 VST2 / VST3 插件目录"));
      chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
        [safe = juce::Component::SafePointer<PluginPicker>(this)](const juce::FileChooser& fc) {
          if (safe && fc.getResult().isDirectory()) safe->beginScan(juce::FileSearchPath(fc.getResult().getFullPathName()));
        });
    };
    load.onClick = [this] { select(list.getSelectedRow()); };
    info.setText(utf8("缓存立即可用 · 效果插件加载时验证音频输入/输出"), juce::dontSendNotification);
    refresh(); setSize(660, 540);
    beginScan(host.scanPaths()); startTimer(100);
  }
  ~PluginPicker() override { stopTimer(); }
  void resized() override {
    search.setBounds(16, 16, getWidth()-32, 30); list.setBounds(16, 58, getWidth()-32, getHeight()-178);
    progress.setBounds(16, getHeight()-114, getWidth()-32, 12);
    info.setBounds(16, getHeight()-96, getWidth()-32, 40);
    scan.setBounds(16, getHeight()-48, 148, 32); folder.setBounds(178, getHeight()-48, 148, 32); load.setBounds(getWidth()-164, getHeight()-48, 148, 32);
  }
  void paint(juce::Graphics& g) override { g.fillAll(bg); }
  int getNumRows() override { return descriptions.size(); }
  void paintListBoxItem(int row, juce::Graphics& g, int width, int height, bool selected) override {
    if (!juce::isPositiveAndBelow(row, descriptions.size())) return;
    if (selected) g.fillAll(blue.withAlpha(0.22f));
    g.setColour(ink); g.setFont(15); g.drawText(descriptions[row].name, 10, 0, width/2, height, juce::Justification::centredLeft, true);
    g.setColour(muted); g.setFont(12); g.drawText(descriptions[row].manufacturerName+utf8(" · ")+(descriptions[row].pluginFormatName == "VST" ? "VST2" : descriptions[row].pluginFormatName), width/2, 0, width/2-10, height, juce::Justification::centredRight, true);
  }
  void listBoxItemDoubleClicked(int row, const juce::MouseEvent&) override { select(row); }
private:
  Host& host; std::function<void(const juce::PluginDescription&)> picked;
  juce::ListBox list; juce::TextEditor search; juce::Label info;
  juce::TextButton scan{utf8("扫描默认目录")}, folder{utf8("添加插件目录")}, load{utf8("加载选中插件")};
  juce::Array<juce::PluginDescription> descriptions;
  double scanProgress = 0;
  juce::ProgressBar progress{scanProgress};
  int lastCatalogCount = -1;
  uint64_t lastRevision = UINT64_MAX;
  int effectCount = 0;
  std::unique_ptr<juce::FileChooser> chooser;
  void select(int row) { if (juce::isPositiveAndBelow(row, descriptions.size())) picked(descriptions[row]); }
  void refresh() {
    descriptions.clear(); effectCount = 0;
    for (const auto& d : preferredEffectTypes(host.catalog.getTypes())) {
        ++effectCount;
        if (search.getText().isEmpty() || (d.name+" "+d.manufacturerName).containsIgnoreCase(search.getText())) descriptions.add(d);
    }
    list.updateContent(); list.repaint();
  }
  void beginScan(const juce::FileSearchPath& paths) {
    host.beginScan(paths); timerCallback();
  }
  void timerCallback() override {
    auto job = host.scanJob; if (!job) return;
    const int count = host.catalog.getNumTypes();
    if (lastCatalogCount != count || lastRevision != host.catalogRevision) { lastCatalogCount = count; lastRevision = host.catalogRevision; refresh(); }
    scan.setEnabled(job->done); folder.setEnabled(job->done);
    scanProgress = job->total > 0 ? double(job->completed)/job->total : (job->done ? 1 : -1);
    juce::String current, failures;
    { const juce::ScopedLock guard(job->lock); current = job->current; failures = job->failures; }
    info.setText((job->done ? utf8("扫描完成") : (job->total == 0 ? utf8("正在查找 VST2 / VST3 文件…") : utf8("扫描中：")+current))
        +utf8(" · ")+juce::String(job->completed)+"/"+juce::String(job->total)+utf8(" · 效果 ")+juce::String(effectCount)
        +utf8(" · 缓存 ")+juce::String(job->cached)+utf8(" · 超时/失败 ")+juce::String(job->failed), juce::dontSendNotification);
    info.setTooltip(failures.isEmpty() ? utf8("扫描结果逐批加入列表；搜索可随时使用") : utf8("未完成的插件（可重新扫描）：\n")+failures);
  }
};
class PickerWindow final : public juce::DocumentWindow {
public:
  PickerWindow(Host& h, std::function<void(const juce::PluginDescription&)> picked) : DocumentWindow(utf8("效果插件列表"), panel, closeButton) {
    setUsingNativeTitleBar(true); setContentOwned(new PluginPicker(h, std::move(picked)), true); setResizable(true, false); centreWithSize(660, 540); setVisible(true);
  }
  void closeButtonPressed() override { setVisible(false); }
};

class SettingsPanel final : public juce::Component, private juce::Timer {
public:
  SettingsPanel(Host& h, std::function<void()> update) : host(h), changed(std::move(update)), github("github.com/WanAn522z", juce::URL("https://github.com/WanAn522z")) {
    const juce::StringArray names{utf8("ASIO 设置"), utf8("插件扫描设置"), utf8("内录设置"), utf8("作者信息"), utf8("GitHub 链接")};
    for (int i = 0; i < static_cast<int>(nav.size()); ++i) { nav[i].setButtonText(names[i]); addAndMakeVisible(nav[i]); nav[i].onClick = [this, i] { select(i); }; }
    for (auto* c : std::initializer_list<juce::Component*>{&title,&driverLabel,&rateLabel,&bufferLabel,&driver,&rate,&buffer,&engine,&control,&audioInfo,&paths,&pathLabel,&scan,&folder,&defaults,&cancelScan,&scanInfo,&author,&github,&githubInfo,&loopOutputLabel,&loopSourceLabel,&loopOutput,&loopSource,&loopHost,&loopRefresh,&loopInfo,&loopHelp}) addAndMakeVisible(*c);
    driverLabel.setText(utf8("ASIO 驱动"), juce::dontSendNotification); rateLabel.setText(utf8("采样率"), juce::dontSendNotification); bufferLabel.setText(utf8("缓冲区"), juce::dontSendNotification);
    pathLabel.setText(utf8("VST2 / VST3 扫描目录 · 每行一个"), juce::dontSendNotification);
    title.setFont(juce::Font(juce::FontOptions(20).withStyle("Bold")));
    for (auto* label : {&title,&driverLabel,&rateLabel,&bufferLabel,&pathLabel,&author,&githubInfo,&audioInfo,&scanInfo,&loopOutputLabel,&loopSourceLabel,&loopInfo,&loopHelp}) { label->setColour(juce::Label::textColourId, ink); label->setMinimumHorizontalScale(1); }
    loopOutputLabel.setText(utf8("内录选择的 ASIO 输出端口"), juce::dontSendNotification);
    loopSourceLabel.setText(utf8("对应的 Windows 输出 / 声道"), juce::dontSendNotification);
    loopHost.setButtonText(utf8("合并宿主输出")); loopHost.setColour(juce::ToggleButton::textColourId, ink);
    loopInfo.setJustificationType(juce::Justification::topLeft); loopHelp.setJustificationType(juce::Justification::topLeft);
    loopHelp.setText(utf8("左右声道分别设置，系统声音与宿主一起进入内录插件链。\n\nAggregate Audio 建议保留合并。其他驱动若回送已包含宿主声音，可取消合并以避免重复。"), juce::dontSendNotification);
    loopOutput.setScrollWheelEnabled(false); loopSource.setScrollWheelEnabled(false);
    loopOutput.onChange = [this] {
      if (updating) return;
      loopOutputBinding = AsioBinding::fromIndex(loopOutput.getSelectedId()-1, host.outputNames, host.driverName);
      refreshLoopSource();
    };
    loopSource.onChange = [this] { if (!updating) applyLoopSource(); };
    loopHost.onClick = [this] { applyLoopSource(); };
    loopRefresh.onClick = [this] { host.refreshSystemOutputs(); refreshLoopSource(); };
    author.setText(utf8("Mini DAW\n\npowered by WanAn522z\n\nWindows ASIO / VST2 / VST3 调音台\n版本 0.2.0"), juce::dontSendNotification);
    author.setJustificationType(juce::Justification::topLeft); author.setFont(juce::Font(juce::FontOptions(16)));
    githubInfo.setText(utf8("作者的 GitHub 主页\n\nWanAn522z"), juce::dontSendNotification); githubInfo.setJustificationType(juce::Justification::topLeft);
    github.setColour(juce::HyperlinkButton::textColourId, blue);
    paths.setMultiLine(true); paths.setReturnKeyStartsNewLine(true); paths.setColour(juce::TextEditor::backgroundColourId, bg); paths.setColour(juce::TextEditor::textColourId, ink);
    paths.setText(host.scanPaths().toStringWithSeparator("\n"));
    driver.setScrollWheelEnabled(false); rate.setScrollWheelEnabled(false); buffer.setScrollWheelEnabled(false);
    driver.onChange = [this] { if (updating) return; host.driverName = driver.getText(); host.start(); notify(); };
    rate.onChange = [this] { if (!updating && rate.getSelectedId() > 0) { host.setSampleRate(rates[rate.getSelectedId()-1]); notify(); } };
    buffer.onChange = [this] { if (!updating && buffer.getSelectedId() > 0) { host.setBufferSize(buffer.getSelectedId()); notify(); } };
    engine.onClick = [this] { if (host.isRunning()) host.stop(); else host.start(); notify(); };
    control.onClick = [this] { host.controlPanel(); notify(); };
    scan.onClick = [this] { auto searchPaths = parsePaths(); host.setScanPaths(searchPaths); host.beginScan(searchPaths); refresh(); };
    cancelScan.onClick = [this] { if (host.scanJob) host.scanJob->cancelled = true; refresh(); };
    defaults.onClick = [this] { paths.setText(defaultPluginSearchPaths().toStringWithSeparator("\n")); };
    folder.onClick = [this] {
      chooser = std::make_unique<juce::FileChooser>(utf8("添加 VST2 / VST3 扫描目录"));
      chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
        [safe = juce::Component::SafePointer<SettingsPanel>(this)](const juce::FileChooser& fc) {
          if (!safe || !fc.getResult().isDirectory()) return;
          auto searchPaths = safe->parsePaths(); searchPaths.add(fc.getResult()); searchPaths.removeRedundantPaths(); safe->paths.setText(searchPaths.toStringWithSeparator("\n"));
        });
    };
    refresh(); select(0); setSize(590, 450); startTimer(250);
  }
  ~SettingsPanel() override { stopTimer(); }
  void select(int page) {
    selected = page;
    for (auto* c : std::initializer_list<juce::Component*>{&driverLabel,&rateLabel,&bufferLabel,&driver,&rate,&buffer,&engine,&control,&audioInfo}) c->setVisible(page == 0);
    for (auto* c : std::initializer_list<juce::Component*>{&paths,&pathLabel,&scan,&folder,&defaults,&cancelScan,&scanInfo}) c->setVisible(page == 1);
    for (auto* c : std::initializer_list<juce::Component*>{&loopOutputLabel,&loopSourceLabel,&loopOutput,&loopSource,&loopHost,&loopRefresh,&loopInfo,&loopHelp}) c->setVisible(page == 2);
    author.setVisible(page == 3); github.setVisible(page == 4); githubInfo.setVisible(page == 4);
    title.setText(nav[page].getButtonText(), juce::dontSendNotification);
    for (int i = 0; i < static_cast<int>(nav.size()); ++i) nav[i].setColour(juce::TextButton::buttonColourId, i == page ? blue.withAlpha(0.20f) : panel);
  }
  void paint(juce::Graphics& g) override { g.fillAll(bg); g.setColour(panel); g.fillRect(0, 0, 154, getHeight()); }
  void resized() override {
    for (int i = 0; i < static_cast<int>(nav.size()); ++i) nav[i].setBounds(12, 20+i*46, 130, 34);
    const int x = 174, w = getWidth()-194; title.setBounds(x, 16, w, 32);
    driverLabel.setBounds(x, 65, w, 24); driver.setBounds(x, 92, w, 30);
    rateLabel.setBounds(x, 140, w, 24); rate.setBounds(x, 166, w, 30);
    bufferLabel.setBounds(x, 212, w, 24); buffer.setBounds(x, 238, w, 30);
    engine.setBounds(x, 290, 130, 32); control.setBounds(x+140, 290, w-140, 32);
    audioInfo.setBounds(x, 334, w, 60);
    pathLabel.setBounds(x, 62, w, 24); paths.setBounds(x, 94, w, 196);
    folder.setBounds(x, 302, 130, 30); defaults.setBounds(x+140, 302, 130, 30);
    scan.setBounds(x, 346, 130, 32); cancelScan.setBounds(x+140, 346, 130, 32); scanInfo.setBounds(x, 392, w, 44);
    author.setBounds(x, 70, w, 250); githubInfo.setBounds(x, 70, w, 120); github.setBounds(x, 196, w, 32);
    loopOutputLabel.setBounds(x, 65, w, 24); loopOutput.setBounds(x, 92, w, 30);
    loopSourceLabel.setBounds(x, 140, w, 24); loopSource.setBounds(x, 166, w, 30);
    loopHost.setBounds(x, 212, w-110, 30); loopRefresh.setBounds(x+w-100, 212, 100, 30);
    loopInfo.setBounds(x, 258, w, 54); loopHelp.setBounds(x, 316, w, 124);
  }
private:
  Host& host; std::function<void()> changed;
  std::array<juce::TextButton, 5> nav;
  juce::Label title, driverLabel, rateLabel, bufferLabel, pathLabel, author, githubInfo, audioInfo, scanInfo;
  juce::ComboBox driver, rate, buffer; juce::Array<double> rates;
  juce::Label loopOutputLabel, loopSourceLabel, loopInfo, loopHelp;
  juce::ComboBox loopOutput, loopSource; juce::ToggleButton loopHost;
  juce::TextButton loopRefresh{utf8("刷新设备")};
  std::vector<LoopbackSource> loopChoices;
  LoopbackSource displayedLoopSource;
  juce::StringArray loopOutputNames;
  AsioBinding loopOutputBinding;
  unsigned loopSystemGeneration = UINT_MAX;
  juce::TextEditor paths;
  juce::TextButton engine, control{utf8("驱动控制面板…")}, scan{utf8("保存目录并扫描")}, folder{utf8("添加目录…")}, defaults{utf8("恢复默认目录")}, cancelScan{utf8("停止扫描")};
  juce::HyperlinkButton github; std::unique_ptr<juce::FileChooser> chooser;
  bool updating = false; int selected = 0; unsigned generation = UINT_MAX;
  juce::FileSearchPath parsePaths() const {
    juce::FileSearchPath result; for (auto line : juce::StringArray::fromLines(paths.getText())) if (line.trim().isNotEmpty()) result.add(juce::File(line.trim()));
    result.removeRedundantPaths(); return result;
  }
  void notify() { generation = UINT_MAX; refresh(); if (changed) changed(); }
  void refreshLoopSource() {
    const juce::ScopedValueSetter<bool> guard(updating, true);
    const int output = loopOutput.getSelectedId()-1;
    const bool available = juce::isPositiveAndBelow(output, host.outputNames.size());
    const auto current = available ? host.systemSource(output) : displayedLoopSource;
    displayedLoopSource = current;
    loopSource.clear(juce::dontSendNotification); loopChoices = {{}, {"off", 0, true}};
    loopSource.addItem(utf8("自动匹配"), 1); loopSource.addItem(utf8("仅宿主输出"), 2);
    for (const auto& endpoint : host.systemOutputs) for (int channel = 0; channel < endpoint.channels; ++channel) {
      loopChoices.push_back({endpoint.id, channel, true, endpoint.name});
      loopSource.addItem(endpoint.name+utf8(" · 声道 ")+juce::String(channel+1), static_cast<int>(loopChoices.size()));
    }
    int selectedChoice = 0;
    for (int i = 0; i < static_cast<int>(loopChoices.size()); ++i)
      if (loopChoices[i].endpoint == current.endpoint && (current.endpoint.isEmpty() || current.endpoint == "off" || loopChoices[i].channel == current.channel)) selectedChoice = i+1;
    if (!selectedChoice) {
      loopChoices.push_back(current); selectedChoice = static_cast<int>(loopChoices.size());
      loopSource.addItem((current.name.isEmpty() ? utf8("已保存的 Windows 设备") : current.name)
        +utf8(" · 声道 ")+juce::String(current.channel+1)+utf8("（未连接）"), selectedChoice);
    }
    loopSource.setSelectedId(selectedChoice, juce::dontSendNotification); loopHost.setToggleState(current.includeHost, juce::dontSendNotification);
    loopSource.setEnabled(available); loopHost.setEnabled(available && current.endpoint != "off");
    loopSystemGeneration = host.systemDeviceGeneration;
  }
  void applyLoopSource() {
    const int choice = loopSource.getSelectedId()-1;
    if (!juce::isPositiveAndBelow(choice, static_cast<int>(loopChoices.size()))) return;
    auto source = loopChoices[choice]; source.includeHost = source.endpoint == "off" || loopHost.getToggleState();
    host.beginEdit(); host.setSystemSource(loopOutput.getSelectedId()-1, source); host.endEdit(utf8("内录来源")); refreshLoopSource(); if (changed) changed();
  }
  void refresh() {
    bool loopLayoutChanged = false;
    if (generation != host.deviceGeneration || driver.getText() != host.driverName) {
      updating = true; generation = host.deviceGeneration;
      driver.clear(juce::dontSendNotification); driver.addItemList(host.drivers(), 1); driver.setText(host.driverName, juce::dontSendNotification);
      rate.clear(juce::dontSendNotification); rates = host.sampleRates(); for (int i = 0; i < rates.size(); ++i) { rate.addItem(juce::String(rates[i]/1000, 1)+" kHz", i+1); if (rates[i] == host.sampleRate()) rate.setSelectedId(i+1, juce::dontSendNotification); }
      buffer.clear(juce::dontSendNotification); for (int size : host.bufferSizes()) buffer.addItem(juce::String(size)+" samples / "+juce::String(size*1000.0/host.sampleRate(), 2)+" ms", size);
      if (rates.isEmpty()) rate.setText(juce::String(host.sampleRate()/1000, 1)+" kHz", juce::dontSendNotification);
      buffer.setSelectedId(host.blockSize(), juce::dontSendNotification); if (host.bufferSizes().isEmpty()) buffer.setText(juce::String(host.blockSize())+" samples", juce::dontSendNotification); updating = false;
      loopLayoutChanged = true;
    }
    if (loopLayoutChanged || loopOutputNames != host.outputNames) {
      const juce::ScopedValueSetter<bool> guard(updating, true);
      loopOutputNames = host.outputNames; loopOutput.clear(juce::dontSendNotification);
      for (int i = 0; i < loopOutputNames.size(); ++i) loopOutput.addItem(juce::String(i+1)+utf8(" · ")+loopOutputNames[i], i+1);
      if (loopOutputBinding.name.isEmpty() && !loopOutputNames.isEmpty()) loopOutputBinding = AsioBinding::fromIndex(0, loopOutputNames, host.driverName);
      const int port = loopOutputBinding.resolve(loopOutputNames, host.driverName);
      if (port < 0 && loopOutputBinding.name.isNotEmpty()) {
        loopOutput.addItem(loopOutputBinding.name+utf8("（未连接）"), 90000); loopOutput.setItemEnabled(90000, false);
        loopOutput.setSelectedId(90000, juce::dontSendNotification);
      } else loopOutput.setSelectedId(port+1, juce::dontSendNotification);
      refreshLoopSource();
    }
    if (selected == 2) {
      const bool available = juce::isPositiveAndBelow(loopOutput.getSelectedId()-1, host.outputNames.size());
      if ((available && host.systemSource(loopOutput.getSelectedId()-1) != displayedLoopSource)
          || loopSystemGeneration != host.systemDeviceGeneration) refreshLoopSource();
      const int output = loopOutput.getSelectedId()-1; const auto source = host.systemSource(output, true);
      juce::String deviceName;
      for (const auto& endpoint : host.systemOutputs) if (endpoint.id == source.endpoint) deviceName = endpoint.name+utf8(" · 声道 ")+juce::String(source.channel+1);
      loopInfo.setText(available ? (deviceName.isEmpty() ? juce::String() : deviceName+"\n")+host.systemSourceStatus(output)
        : utf8("ASIO 端口暂未连接，原映射已保留，重新连接后自动恢复。"), juce::dontSendNotification);
    }
    rate.setEnabled(host.isRunning()); buffer.setEnabled(host.isRunning()); engine.setButtonText(host.isRunning() ? utf8("停止音频") : utf8("启动音频"));
    audioInfo.setText(host.error.isNotEmpty() ? host.error : utf8("采样率 ")+juce::String(host.sampleRate()/1000, 1)+" kHz\n"+utf8("I/O 延迟 ")+juce::String(host.deviceLatencyMs(), 2)+" ms", juce::dontSendNotification);
    auto job = host.scanJob; const bool busy = job && !job->done; scan.setEnabled(!busy); cancelScan.setEnabled(busy);
    scanInfo.setText(job ? (job->cancelled ? utf8("扫描已停止") : job->done ? utf8("扫描完成") : utf8("正在扫描…"))+"  "+juce::String(job->completed)+" / "+juce::String(job->total)+utf8(" · 失败 ")+juce::String(job->failed) : utf8("扫描设置将在启动扫描时保存"), juce::dontSendNotification);
  }
  void timerCallback() override { refresh(); }
};
class SettingsWindow final : public juce::DocumentWindow {
public:
  SettingsWindow(Host& host, juce::LookAndFeel& look, std::function<void()> changed) : DocumentWindow(utf8("Mini DAW 设置"), bg, closeButton) {
    setLookAndFeel(&look); setUsingNativeTitleBar(true); setContentOwned(new SettingsPanel(host, std::move(changed)), true); setResizable(false, false); centreWithSize(590, 450); setVisible(true);
  }
  ~SettingsWindow() override { setLookAndFeel(nullptr); }
  void closeButtonPressed() override { setVisible(false); }
};

class NavigationViewport : public juce::Viewport, private juce::Timer {
public:
  NavigationViewport() {
    for (auto* bar : {&getVerticalScrollBar(), &getHorizontalScrollBar()}) {
      bar->getProperties().set("dragNavigation", true); bar->getProperties().set("navigationShown", false);
    }
    startTimerHz(60);
  }
  void setVerticalWheelStep(double pixelsPerNotch) { verticalWheelScale = pixelsPerNotch*512.0/120.0; }
  void reveal(bool vertical) { expires[vertical ? 0 : 1] = juce::Time::getMillisecondCounterHiRes()+3000; updateNavigation(); }
  void visibleAreaChanged(const juce::Rectangle<int>& area) override {
    if (area.getX() != previous.x) reveal(false);
    if (area.getY() != previous.y) reveal(true);
    previous = area.getPosition();
    // Direct scrollbar grabs, layout changes and explicit navigation cancel inertia.
    if (!movingFrame) { scrolling = false; velocity = {}; current = target = area.getPosition().toDouble(); }
  }
  void mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override {
    if (getVerticalScrollBar().isVisible() && w.deltaY != 0 && !e.mods.isShiftDown()) scrollBy(0, -w.deltaY*verticalWheelScale);
    else if (getHorizontalScrollBar().isVisible() && (w.deltaX != 0 || w.deltaY != 0)) scrollBy(-(w.deltaX != 0 ? w.deltaX : w.deltaY)*560, 0);
    else Component::mouseWheelMove(e, w);
  }
  void scrollBy(double dx, double dy) {
    if (dx != 0) reveal(false); if (dy != 0) reveal(true);
    if (!scrolling) { velocity = {}; current = target = getViewPosition().toDouble(); }
    target = constrain(target+juce::Point<double>(dx, dy)); scrolling = current.getDistanceFrom(target) > 0.1;
  }
  void setEdgeScroll(double dxPerSecond, double dyPerSecond) {
    if (edgeVelocity.isOrigin() && (dxPerSecond != 0 || dyPerSecond != 0)) current = target = getViewPosition().toDouble();
    edgeVelocity = {dxPerSecond, dyPerSecond};
  }
  void updateNavigation() {
    const double now = juce::Time::getMillisecondCounterHiRes();
    int i = 0;
    for (auto* bar : {&getVerticalScrollBar(), &getHorizontalScrollBar()}) {
      const bool shown = now < expires[i++] || (bar->isVisible() && (bar->isMouseOver(true) || bar->isMouseButtonDown()));
      if (static_cast<bool>(bar->getProperties().getWithDefault("navigationShown", false)) != shown) {
        bar->getProperties().set("navigationShown", shown); bar->repaint();
      }
    }
  }
private:
  std::array<double, 2> expires{}; juce::Point<int> previous;
  juce::Point<double> current, target, velocity, edgeVelocity;
  bool scrolling = false, movingFrame = false;
  double lastFrame = juce::Time::getMillisecondCounterHiRes();
  double verticalWheelScale = 300;
  juce::Point<double> constrain(juce::Point<double> value) const {
    const auto* content = getViewedComponent();
    return {juce::jlimit(0.0, static_cast<double>(content ? std::max(0, content->getWidth()-getViewWidth()) : 0), value.x),
            juce::jlimit(0.0, static_cast<double>(content ? std::max(0, content->getHeight()-getViewHeight()) : 0), value.y)};
  }
  void timerCallback() override {
    const double now = juce::Time::getMillisecondCounterHiRes();
    const double dt = juce::jlimit(0.001, 0.04, (now-lastFrame)/1000.0); lastFrame = now;
    if (scrolling && (getVerticalScrollBar().isMouseButtonDown() || getHorizontalScrollBar().isMouseButtonDown())) {
      scrolling = false; velocity = {}; current = target = getViewPosition().toDouble();
    }
    if (!edgeVelocity.isOrigin()) {
      current = constrain(current+edgeVelocity*dt); target = current; velocity = {}; scrolling = false;
      const juce::ScopedValueSetter<bool> frame(movingFrame, true); setViewPosition(current.roundToInt());
    } else if (scrolling) {
      // Critically damped motion keeps velocity continuous across wheel ticks.
      constexpr double omega = 2.0/0.14;
      target = constrain(target); const auto offset = current-target;
      const auto step = (velocity+offset*omega)*dt; const double decay = std::exp(-omega*dt);
      velocity = (velocity-step*omega)*decay;
      current = constrain(target+(offset+step)*decay);
      if (current.getDistanceFrom(target) < 0.35 && velocity.getDistanceFromOrigin() < 5) { current = target; velocity = {}; scrolling = false; }
      const juce::ScopedValueSetter<bool> frame(movingFrame, true); setViewPosition(current.roundToInt());
    }
    updateNavigation();
  }
};

class ChannelStrip final : public juce::Component, private juce::Timer, public juce::DragAndDropTarget {
public:
  ChannelStrip(Host&, Mixer&, int);
  void paint(juce::Graphics&) override;
  void resized() override;
  void sync(bool animate = false, const juce::AudioPluginInstance* landingPlugin = nullptr);
  PluginSlot* pluginCard(int slot) const { return slots[slot].get(); }
  const juce::ComboBox& inputSelector() const { return input; }
  const juce::ComboBox& outputSelector() const { return output; }
  void setIndex(int value) { index = value; }
  bool matchesChannel(const Channel* value) const { return boundChannel == value; }
  void scrollDuringDrag(bool active, juce::Point<int> screenPosition);
  void mouseDown(const juce::MouseEvent&) override;
  void mouseDrag(const juce::MouseEvent& e) override { touchContext.drag(e); }
  void mouseUp(const juce::MouseEvent& e) override { touchContext.release(e); }
  void mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
  bool isInterestedInDragSource(const SourceDetails& d) override { return d.description.toString().startsWith("channel:") || d.description.toString().startsWith("send:"); }
  void itemDragEnter(const SourceDetails&) override { dropMotion.set(true); }
  void itemDragExit(const SourceDetails&) override { dropMotion.set(false); }
  void itemDropped(const SourceDetails&) override;
private:
  Host& host; Mixer& mixer; int index; Channel* boundChannel;
  ChannelTitle title; juce::ComboBox input, output;
  std::vector<WindowsLoopbackInput> windowsInputChoices;
  juce::TextButton mainReceive, mute{"M"};
  MeterFader fader;
  std::array<std::unique_ptr<PluginSlot>, slotCount> slots;
  std::array<std::unique_ptr<DropGap>, slotCount+1> pluginGaps;
  std::vector<std::unique_ptr<SendKnob>> knobs;
  std::vector<std::unique_ptr<DropGap>> sendGaps;
  SendArea sendArea;
  NavigationViewport sendViewport, chainViewport;
  ContextSurface chain;
  juce::ComponentAnimator pluginAnimator;
  std::array<juce::AudioPluginInstance*, slotCount> displayedPlugins{};
  juce::Rectangle<int> loopbackTagBounds;
  HoverMotion dropMotion{[this] { repaint(); }};
  StripResizeEdge rightEdge{true}, bottomEdge{false};
  bool updating = false, sendEditing = false;
  int slotPitch = 26, outputTop = 430, receiveTop = 472, contentX = 64, contentWidth = 120, knobWidth = 44;
  int sendRowPitch = 52, resizeStartHeight = 0, resizeStartContentHeight = 0;
  std::vector<int> resizeStartWidths; std::vector<float> resizeStartScales;
  bool resizeSingle = false;
  void paintSends(juce::Graphics&);
  void contextMenu(const juce::PopupMenu::Options& options = {});
  void chooseSend(int, const juce::PopupMenu::Options& options = {}); void chooseReceive(bool);
  TouchContextGesture touchContext{*this, [this](const auto& options) { contextMenu(options); }};
  void timerCallback() override {
    fader.repaint(); repaint(0, getHeight()-34, 60, 30);
    const auto& c = *host.channels[index]; if (c.loopback) {
      input.setTooltip(input.getText()+utf8("\n")+host.loopbackInputStatus(index)
        +(c.windowsInput.endpoint.isNotEmpty() ? utf8("\n直接读取 Windows 输出；宿主声音可通过发送混入。")
          : utf8("\n选择 ASIO 输出端口，合并系统声音与宿主输出。\n可在设置 → 内录设置中指定 Windows 输出及声道。")));
      output.setTooltip(output.getText()+utf8("\n回送到 Mini DAW 内部选用此输入的普通通道，位于插件之前。"));
    }
  }
  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ChannelStrip)
};

class HorizontalViewport final : public NavigationViewport {
public:
  void mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails& w) override {
    scrollWheel(w);
  }
  void scrollWheel(const juce::MouseWheelDetails& w) { scrollBy(-(w.deltaY != 0 ? w.deltaY : w.deltaX)*560, 0); }
};

struct PluginDropVisual {
  juce::Image image;
  juce::Rectangle<int> screenBounds;
};
class FloatingCard final : public juce::Component {
public:
  juce::Image image;
  juce::Component::SafePointer<PluginSlot> source, destination;
  std::function<bool(FloatingCard&)> arrived;
  bool approved = true, returning = false;
  FloatingCard() { setInterceptsMouseClicks(false, false); }
  void paint(juce::Graphics& g) override { g.drawImage(image, getLocalBounds().toFloat()); }
  void setLandingImage(const juce::Image& final) {
    colourBlend = 0;
    endImage = final; startImage = juce::Image(juce::Image::ARGB, final.getWidth(), final.getHeight(), true);
    juce::Graphics g(startImage); g.drawImage(image, startImage.getBounds().toFloat());
    image = startImage.createCopy();
  }
  void landAt(juce::Rectangle<int> bounds) {
    startBounds = getBounds().toFloat(); endBounds = bounds.toFloat();
    started = juce::Time::getMillisecondCounterHiRes();
    duration = juce::jlimit(160.0, 220.0, 150.0+startBounds.getCentre().getDistanceFrom(endBounds.getCentre())*0.12);
  }
  bool advance(double now) {
    const float t = juce::jlimit(0.0f, 1.0f, static_cast<float>((now-started)/duration));
    const float progress = 1-std::pow(1-t, 3);
    const auto position = startBounds.getPosition()+(endBounds.getPosition()-startBounds.getPosition())*progress;
    setBounds(juce::Rectangle<float>(position.x, position.y,
        startBounds.getWidth()+(endBounds.getWidth()-startBounds.getWidth())*progress,
        startBounds.getHeight()+(endBounds.getHeight()-startBounds.getHeight())*progress).toNearestInt());
    if (endImage.isValid()) {
      const float colourTime = juce::jlimit(0.0f, 1.0f, (t-0.18f)/0.68f);
      colourBlend = colourTime*colourTime*(3-2*colourTime);
      const auto mix = static_cast<juce::uint32>(juce::roundToInt(256*colourBlend));
      const juce::Image::BitmapData from(startImage, juce::Image::BitmapData::readOnly), to(endImage, juce::Image::BitmapData::readOnly);
      juce::Image::BitmapData frame(image, juce::Image::BitmapData::writeOnly);
      for (int y = 0; y < frame.height; ++y) for (int x = 0; x < frame.width; ++x) {
        auto pixel = *reinterpret_cast<const juce::PixelARGB*>(from.getPixelPointer(x, y));
        pixel.tween(*reinterpret_cast<const juce::PixelARGB*>(to.getPixelPointer(x, y)), mix);
        *reinterpret_cast<juce::PixelARGB*>(frame.getPixelPointer(x, y)) = pixel;
      }
      repaint();
    }
    return t >= 1;
  }
  float colourTransition() const { return colourBlend; }
private:
  juce::Rectangle<float> startBounds, endBounds;
  juce::Image startImage, endImage;
  float colourBlend = 0;
  double started = 0, duration = 180;
};

class Mixer final : public juce::Component, private juce::Timer, public juce::DragAndDropContainer {
public:
  Host host;
  std::function<void()> hide;
  std::function<void(int)> resizeContentHeight;
  explicit Mixer(bool offline = false) : host(!offline) {
    look.setColour(juce::ResizableWindow::backgroundColourId, bg);
    look.setColour(juce::TextButton::buttonColourId, panel); look.setColour(juce::TextButton::textColourOffId, ink);
    look.setColour(juce::ComboBox::backgroundColourId, juce::Colour(0xff080d14)); look.setColour(juce::ComboBox::textColourId, ink);
    look.setColour(juce::PopupMenu::backgroundColourId, panel); look.setColour(juce::PopupMenu::textColourId, ink);
    look.setColour(juce::Slider::textBoxTextColourId, ink); look.setColour(juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
    setLookAndFeel(&look);
    addAndMakeVisible(viewport); viewport.setViewedComponent(&array, false);
    viewport.setScrollBarsShown(false, true); viewport.setScrollBarThickness(14); viewport.setScrollOnDragEnabled(false);
    viewport.getHorizontalScrollBar().getProperties().set("dragNavigation", true);
    for (auto* c : std::initializer_list<juce::Component*>{&audio, &settings, &save, &background, &savePreset, &loadPreset, &undoButton, &redoButton}) addAndMakeVisible(*c);
    settings.setTooltip(utf8("设置")); undoButton.setTooltip(utf8("撤销 · Ctrl+Z")); redoButton.setTooltip(utf8("重做 · Ctrl+Y / Ctrl+Shift+Z"));
    undoButton.onClick = [this] { applyHistory(false); }; redoButton.onClick = [this] { applyHistory(true); };
    array.addAndMakeVisible(add); add.setButtonText("+"); add.setTooltip(utf8("添加通道"));
    addAndMakeVisible(status); status.setFont(juce::Font(juce::FontOptions(10)));
    driver.addItemList(host.drivers(), 1); driver.setText(host.driverName, juce::dontSendNotification);
    driver.onChange = [this] { host.driverName = driver.getText(); host.start(); refresh(); };
    add.onClick = [this] { host.beginEdit(); const bool added = host.addChannel(); host.endEdit(utf8("添加通道")); if (added) { refresh(); scrollToEnd(); } };
    audio.onClick = [this] { if (host.isRunning()) host.stop(); else host.start(); refresh(); };
    settings.onClick = [this] { showSettings(); };
    save.onClick = [this] { host.save(); };
    background.onClick = [this] { host.save(); if (hide) hide(); };
    savePreset.onClick = [this] { chooseProject(true); }; loadPreset.onClick = [this] { chooseProject(false); };
    loadPreset.showRecent = [this] { showRecentPresets(); };
    buffer.onClick = [this] {
      juce::PopupMenu menu; const auto sizes = host.bufferSizes();
      for (int size : sizes) menu.addItem(size, juce::String(size)+utf8(" samples · ")+juce::String(size*1000.0/host.sampleRate(), 2)+" ms", true, size == host.blockSize());
      menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&buffer), [safe = juce::Component::SafePointer<Mixer>(this)](int size) {
        if (safe && size > 0 && !safe->host.setBufferSize(size)) safe->showError();
      });
    };
    host.changed = [safe = juce::Component::SafePointer<Mixer>(this)] { if (safe) safe->refresh(); };
    refresh(); setSize(canvasWidth, host.windowHeight); timerCallback(); startTimerHz(60);
    if (!offline) { host.start(); refresh(); host.beginScan(host.scanPaths()); }
  }
  ~Mixer() override { settingsWindow.reset(); stopTimer(); landingCards.clear(); dragOverlay.reset(); stripAnimator.cancelAllAnimations(false); host.changed = {}; host.stop(); editors.clear(); picker.reset(); if (host.saveOnExit) host.save(); setLookAndFeel(nullptr); }
  void liftPluginCard(PluginSlot* source, const PluginDropVisual& visual) {
    dragSnapshot = visual.image;
    dragOverlay = std::make_unique<FloatingCard>(); dragOverlay->image = visual.image; dragOverlay->source = source;
    dragOverlay->setBounds(getLocalArea(nullptr, visual.screenBounds)); addAndMakeVisible(*dragOverlay);
    if (source) source->showAsEmpty(true);
  }
  void startCardDrag(const juce::String& description, juce::Component* source, juce::Component* card = nullptr, juce::Rectangle<int> area = {}) {
    stripAnimator.cancelAllAnimations(true);
    cardDragSource.reset();
    float closest = std::numeric_limits<float>::max();
    auto& desktop = juce::Desktop::getInstance();
    for (const auto& pointer : desktop.getMouseSources()) if (pointer.isDragging()) {
      const float distance = pointer.getScreenPosition().getDistanceSquaredFrom(source->getScreenBounds().getCentre().toFloat());
      if (distance < closest) { closest = distance; cardDragSource = pointer; }
    }
    if (!cardDragSource) return;
    if (!card) card = source; if (area.isEmpty()) area = card->getLocalBounds();
    constexpr int padding = 14; constexpr float resolution = 2;
    const auto snapshot = card->createComponentSnapshot(area, true, resolution);
    dragSnapshot = juce::Image(juce::Image::ARGB, snapshot.getWidth()+padding*4, snapshot.getHeight()+padding*4, true);
    {
      juce::Graphics g(dragSnapshot);
      juce::DropShadow(juce::Colours::black.withAlpha(0.55f), 18, {0, 5}).drawForRectangle(g, {padding*2, padding*2, snapshot.getWidth(), snapshot.getHeight()});
      g.setOpacity(0.94f); g.drawImageAt(snapshot, padding*2, padding*2);
    }
    const auto grab = card->getLocalPoint(nullptr, cardDragSource->getLastMouseDownPosition())-area.getPosition().toFloat();
    dragAnchor = grab+juce::Point<float>(padding, padding); dragLiftStarted = juce::Time::getMillisecondCounterHiRes();
    const auto offset = -dragAnchor.roundToInt();
    const bool pluginDrag = description.startsWith("plugin:");
    nativePluginDrag = pluginDrag;
    if (pluginDrag) {
      liftPluginCard(dynamic_cast<PluginSlot*>(source), {dragSnapshot, card->localAreaToGlobal(area.expanded(padding))});
      followFloatingCard(cardDragSource->getScreenPosition().roundToInt());
    }
    // Own the plugin visual so JUCE's default fade can hand off to a landing animation.
    const auto image = pluginDrag ? juce::Image(juce::Image::ARGB, 1, 1, true) : dragSnapshot;
    startDragging(description, source, juce::ScaledImage(image, resolution), false, &offset, &*cardDragSource);
    if (isDragAndDropActive()) {
      if (pluginDrag) { if (dragOverlay->source) dragOverlay->source->showAsEmpty(true); }
      else { liftedSource = card; card->setAlpha(0.45f); }
    } else { if (dragOverlay && dragOverlay->source) dragOverlay->source->showAsEmpty(false); dragOverlay.reset(); nativePluginDrag = false; cardDragSource.reset(); }
  }
  void followFloatingCard(juce::Point<int> screenPosition) {
    if (!dragOverlay) return;
    const auto top = getLocalPoint(nullptr, screenPosition)-dragAnchor.roundToInt();
    dragOverlay->setBounds(top.x, top.y, dragSnapshot.getWidth()/2, dragSnapshot.getHeight()/2); dragOverlay->toFront(false);
  }
  void dragOperationEnded(const juce::DragAndDropTarget::SourceDetails&) override {
    if (dragOverlay) returnFloatingPlugin();
    if (liftedSource) liftedSource->setAlpha(1); liftedSource = nullptr; dragSnapshot = {}; nativePluginDrag = false;
    cardDragSource.reset();
    viewport.setEdgeScroll(0, 0); for (auto& strip : strips) strip->scrollDuringDrag(false, {});
  }
  void pluginDragReleased(juce::Point<int> position) {
    followFloatingCard(position);
    // JUCE dispatches itemDropped after the source mouseUp. Finalise unused drops in that same event turn.
    juce::MessageManager::callAsync([safe = juce::Component::SafePointer<Mixer>(this), card = juce::Component::SafePointer<FloatingCard>(dragOverlay.get())] {
      if (safe && card && safe->dragOverlay.get() == card.getComponent()) safe->returnFloatingPlugin();
    });
  }
  void openPlugin(int channel, int slot) {
    auto& s = host.channels[channel]->slots[slot]; int key = channel*slotCount+slot;
    if (!s.plugin) {
      choosePlugin(channel, slot); return;
    }
    if (!editors.contains(key)) editors[key] = std::make_unique<EditorWindow>(host, *s.plugin);
    editors[key]->setVisible(true); editors[key]->toFront(true);
  }
  void choosePlugin(int channel, int slot) {
    picker = std::make_unique<PickerWindow>(host, [safe = juce::Component::SafePointer<Mixer>(this), channel, slot](const juce::PluginDescription& desc) {
      if (!safe) return;
      safe->editors.erase(channel*slotCount+slot); safe->host.beginEdit();
      const bool loaded = safe->host.loadPreferredPlugin(channel, slot, desc); safe->host.endEdit(utf8("加载 / 替换插件"));
      if (loaded) { safe->picker->setVisible(false); safe->refresh(); safe->openPlugin(channel, slot); } else safe->showError();
    });
  }
  void pluginContext(int channel, int slot, const juce::PopupMenu::Options& options = {}) {
    auto& s = host.channels[channel]->slots[slot]; juce::PopupMenu menu;
    menu.addItem(2, s.plugin ? utf8("打开界面") : utf8("选择插件")); menu.addItem(4, utf8("替换插件…"), s.plugin != nullptr);
    menu.addSeparator(); menu.addItem(3, utf8("卸载插件"), s.plugin != nullptr);
    menu.showMenuAsync(options, [safe = juce::Component::SafePointer<Mixer>(this), channel, slot](int result) {
      if (!safe) return; auto& s = safe->host.channels[channel]->slots[slot];
      if (result == 2) safe->openPlugin(channel, slot);
      if (result == 4) safe->choosePlugin(channel, slot);
      if (result == 3) { safe->editors.erase(channel*slotCount+slot); safe->host.beginEdit(); safe->host.removePlugin(channel, slot); safe->host.endEdit(utf8("卸载插件")); safe->refresh(); }
    });
  }
  void refresh(bool animate = false, const juce::AudioPluginInstance* landingPlugin = nullptr) {
    for (int i = 0; i < static_cast<int>(std::min(strips.size(), host.channels.size())); ++i) if (!strips[i]->matchesChannel(host.channels[i].get())) {
      stripAnimator.cancelAllAnimations(false); strips.clear(); break;
    }
    while (strips.size() > host.channels.size()) strips.pop_back();
    while (strips.size() < host.channels.size()) { auto s = std::make_unique<ChannelStrip>(host, *this, static_cast<int>(strips.size())); array.addAndMakeVisible(*s); strips.push_back(std::move(s)); }
    for (auto& s : strips) s->sync(animate, landingPlugin); layoutStrips(animate); repaint();
    for (auto& card : landingCards) if (card->destination && !card->returning) card->destination->setAlpha(0);
  }
  void layoutStrips(bool animate = false) {
    const int height = availableStripHeight(), scrollX = viewport.getViewPositionX();
    int totalWidth = 64;
    for (const auto& channel : host.channels) totalWidth += channel->stripWidth+12;
    array.setSize(std::max(viewport.getWidth(), totalWidth), height);
    if (channelGaps.size() != strips.size()+1) {
      channelGaps.clear();
      for (int i = 0; i <= static_cast<int>(strips.size()); ++i) {
        auto gap = std::make_unique<DropGap>(false, "channel:"); array.addAndMakeVisible(*gap);
        gap->dropped = [this, i](const juce::String& description) {
          const int from = description.fromFirstOccurrenceOf(":", false, false).getIntValue();
          reorderChannel(from, i > from ? i-1 : i, false);
        };
        channelGaps.push_back(std::move(gap));
      }
    }
    int x = 6;
    for (int i = 0; i < static_cast<int>(strips.size()); ++i) {
      const auto& channel = *host.channels[i];
      channelGaps[i]->setBounds(x-12, 0, 12, height);
      strips[i]->setVisible(true);
      const juce::Rectangle<int> destination(x, 0, channel.stripWidth, height);
      if (animate && strips[i]->getBounds() != destination && strips[i]->getWidth() == destination.getWidth() && strips[i]->getHeight() == destination.getHeight())
        stripAnimator.animateComponent(strips[i].get(), destination, 1, 210, false, 1.8, 0);
      else { stripAnimator.cancelAnimation(strips[i].get(), false); strips[i]->setBounds(destination); }
      if (!animate) strips[i]->resized(); x += channel.stripWidth+12;
    }
    channelGaps.back()->setBounds(x-12, 0, 12, height);
    add.setBounds(x, 0, 40, height);
    add.setEnabled(strips.size() < maximumChannels);
    viewport.setViewPosition(scrollX, 0);
  }
  int availableStripHeight() const { return std::max(1, viewport.getHeight()-20); }
  void scrollToEnd() { viewport.setViewPosition(std::max(0, array.getWidth()-viewport.getWidth()), 0); }
  void scrollChannels(const juce::MouseWheelDetails& w) {
    viewport.scrollWheel(w);
  }
  void reorderChannel(int from, int to, bool swap = true) {
    juce::MessageManager::callAsync([safe = juce::Component::SafePointer<Mixer>(this), from, to, swap] {
      if (!safe || from == to) return;
      safe->host.beginEdit();
      if (swap ? safe->host.swapChannels(from, to) : safe->host.moveChannel(from, to)) {
        safe->host.endEdit(swap ? utf8("交换通道") : utf8("移动通道"));
        safe->picker.reset();
        std::map<int, std::unique_ptr<EditorWindow>> remapped;
        for (auto& [key, editor] : safe->editors) remapped.emplace((swap ? swappedIndex(key/slotCount, from, to) : movedChannelIndex(key/slotCount, from, to))*slotCount+key%slotCount, std::move(editor));
        safe->editors = std::move(remapped);
        if (swap) std::swap(safe->strips[from], safe->strips[to]);
        else {
          auto strip = std::move(safe->strips[from]); safe->strips.erase(safe->strips.begin()+from);
          safe->strips.insert(safe->strips.begin()+to, std::move(strip));
        }
        for (int i = 0; i < static_cast<int>(safe->strips.size()); ++i) safe->strips[i]->setIndex(i);
        safe->refresh(true);
      } else safe->host.endEdit(utf8("移动通道"));
    });
  }
  void returnPluginCard(FloatingCard& card) {
    if (card.destination && card.destination != card.source) card.destination->setAlpha(1);
    card.destination = card.source; card.returning = true; card.approved = true; card.arrived = {};
    if (card.source) {
      card.source->setAlpha(1); card.source->showAsEmpty(true);
      card.source->clearDropHighlight(); card.setLandingImage(card.source->landingImageFrom(*card.source));
      card.landAt(getLocalArea(card.source.getComponent(), card.source->getLocalBounds()).expanded(14));
    }
  }
  void returnFloatingPlugin() {
    if (!dragOverlay) return;
    returnPluginCard(*dragOverlay); landingCards.push_back(std::move(dragOverlay));
  }
  void dropPlugin(const juce::String& description, int target, int insertion, bool swap = false, std::optional<PluginDropVisual> visual = {}) {
    const auto parts = juce::StringArray::fromTokens(description, ":", "");
    if (parts.size() != 3 || parts[0] != "plugin") return;
    const int source = parts[1].getIntValue(), slot = parts[2].getIntValue();
    if (!juce::isPositiveAndBelow(source, static_cast<int>(host.channels.size())) || !juce::isPositiveAndBelow(target, static_cast<int>(host.channels.size()))
        || !juce::isPositiveAndBelow(slot, slotCount) || insertion < 0 || insertion > slotCount || (swap && insertion == slotCount)) return;
    const auto origin = host.channels[source]->slots[slot].plugin;
    if (!origin) return;
    if (!visual && dragOverlay) {
      visual = PluginDropVisual{dragOverlay->image, dragOverlay->getScreenBounds()};
    }
    FloatingCard* moving = nullptr;
    if (visual && visual->image.isValid()) {
      auto card = dragOverlay ? std::move(dragOverlay) : std::make_unique<FloatingCard>();
      card->image = visual->image; card->source = strips[source]->pluginCard(slot);
      card->setBounds(getLocalArea(nullptr, visual->screenBounds)); addAndMakeVisible(*card);
      card->source->showAsEmpty(true); moving = card.get(); landingCards.push_back(std::move(card));
    }
    if (source == target && (swap ? slot == insertion : insertion == slot || insertion == slot+1)) {
      if (moving) returnPluginCard(*moving);
      return;
    }
    const int landingSlot = juce::jlimit(0, slotCount-1, source == target && !swap && insertion > slot ? insertion-1 : insertion);
    const auto destination = host.channels[target]->slots[landingSlot].plugin;
    const bool replacing = swap && source != target && destination != nullptr;
    const auto landingColour = source == target || host.channels[target]->colourAssigned ? host.channels[target]->colour
      : juce::Colour::fromHSV(juce::Random::getSystemRandom().nextFloat(), 0.48f, 0.88f, 1.0f);
    const auto safe = juce::Component::SafePointer<Mixer>(this);
    const auto card = juce::Component::SafePointer<FloatingCard>(moving);
    auto commit = [safe, source, slot, target, insertion, swap, replacing, origin, destination,
                   sourceChannel = host.channels[source].get(), targetChannel = host.channels[target].get(), landingSlot, landingColour, card] {
      if (!safe || !juce::isPositiveAndBelow(source, static_cast<int>(safe->host.channels.size())) || !juce::isPositiveAndBelow(target, static_cast<int>(safe->host.channels.size()))
          || safe->host.channels[source].get() != sourceChannel || safe->host.channels[target].get() != targetChannel
          || safe->host.channels[source]->slots[slot].plugin != origin || (replacing && safe->host.channels[target]->slots[landingSlot].plugin != destination)) return false;
      safe->picker.reset();
      if (source == target) safe->editors.clear(); else safe->editors.erase(target*slotCount+landingSlot);
      safe->host.beginEdit();
      bool succeeded = false;
      if (replacing) succeeded = safe->host.replaceWithCopy(source, slot, target, insertion);
      else if (source != target) succeeded = safe->host.copyPlugin(source, slot, target, insertion);
      else if (swap) succeeded = safe->host.swapPlugins(source, slot, target, insertion);
      else if (source == target) succeeded = safe->host.movePlugin(target, slot, insertion);
      if (succeeded && source != target) { safe->host.channels[target]->colour = landingColour; safe->host.channels[target]->colourAssigned = true; }
      safe->host.endEdit(source == target ? utf8("移动插件") : replacing ? utf8("复制并替换插件") : utf8("复制插件"));
      if (succeeded && card && source == target) { if (card->source) card->source->showAsEmpty(false); card->source = nullptr; }
      safe->refresh(succeeded, succeeded && card ? safe->host.channels[target]->slots[landingSlot].plugin.get() : nullptr);
      if (!succeeded && safe->host.error.isNotEmpty()) safe->showError();
      return succeeded;
    };
    if (moving) {
      moving->destination = strips[target]->pluginCard(landingSlot); moving->destination->setAlpha(0);
      moving->destination->clearDropHighlight(); moving->setLandingImage(moving->destination->landingImageFrom(*moving->source, landingColour));
      moving->landAt(getLocalArea(moving->destination.getComponent(), moving->destination->getLocalBounds()).expanded(14));
      if (source == target) {
        // Sorting is cheap; update neighbours while the one lifted visual travels to its slot.
        if (!commit()) returnPluginCard(*moving);
        return;
      }
      moving->approved = !replacing;
      moving->arrived = [safe, commit](FloatingCard& c) {
        if (!c.approved) return false;
        // Plugin construction/state transfer begins after the visible movement, so it cannot stall the landing frames.
        if (commit()) return true;
        if (safe) safe->returnPluginCard(c);
        return false;
      };
      if (!replacing && moving->source) moving->source->revealAfterCopy();
    }
    auto approve = [commit, card] {
      if (card) { card->approved = true; if (card->source) card->source->revealAfterCopy(); }
      else commit();
    };
    if (!replacing) { if (!moving) juce::MessageManager::callAsync(approve); return; }
    const auto message = utf8("将 ")+host.channelLabel(source)+utf8(" 的槽 ")+juce::String(slot+1)+utf8(" · ")+origin->getName()+
      utf8(" 复制到 ")+host.channelLabel(target)+utf8("，替换槽 ")+juce::String(insertion+1)+utf8(" · ")+destination->getName()+utf8("？\n源插件保留，可撤销。");
    juce::AlertWindow::showOkCancelBox(juce::MessageBoxIconType::QuestionIcon, utf8("复制并替换插件"), message, utf8("替换"), utf8("取消"), this,
      juce::ModalCallbackFunction::create([safe, card, approve](int result) {
        if (!safe) return;
        if (result == 1) approve(); else if (card) safe->returnPluginCard(*card);
      }));
  }
  void dropSend(const juce::String& description, int targetChannel, int targetSend = -1, bool insert = false) {
    const auto parts = juce::StringArray::fromTokens(description, ":", "");
    if (parts.size() != 3 || parts[0] != "send") return;
    const int source = parts[1].getIntValue(), send = parts[2].getIntValue();
    if (!juce::isPositiveAndBelow(source, static_cast<int>(host.channels.size())) || send < 0 || send > 128) return;
    juce::MessageManager::callAsync([safe = juce::Component::SafePointer<Mixer>(this), source, send, targetChannel, targetSend, insert]() mutable {
      if (!safe || source >= static_cast<int>(safe->host.channels.size()) || !juce::isPositiveAndBelow(targetChannel, static_cast<int>(safe->host.channels.size()))) return;
      if (source == targetChannel && (targetSend < 0 || targetSend == send || (insert && targetSend == send+1))) return;
      safe->host.beginEdit();
      {
        const juce::ScopedLock guard(safe->host.lock);
        auto& sends = safe->host.channels[source]->sends;
        if (source == targetChannel) {
          if (targetSend <= 128) {
            if (send >= static_cast<int>(sends.size())) sends.resize(send+1);
            if (insert) targetSend = juce::jmin(targetSend, static_cast<int>(sends.size()));
            else sends.resize(std::max(static_cast<int>(sends.size()), targetSend+1));
            if (insert) {
              const auto route = sends[send]; sends.erase(sends.begin()+send);
              const int destination = targetSend > send ? targetSend-1 : targetSend;
              sends.insert(sends.begin()+juce::jlimit(0, static_cast<int>(sends.size()), destination), route);
            } else std::swap(sends[send], sends[targetSend]);
            safe->host.rebuildOrder();
          }
        } else {
          auto route = send < static_cast<int>(sends.size()) ? sends[send] : Route{-1, -1, 0, 1};
          if (route.target < 0) route.gain = 1;
          route.target = targetChannel; route.slot = -1; route.bus = 0;
          if (!safe->host.setRoute(source, send, route)) safe->showError();
        }
      }
      safe->host.endEdit(source == targetChannel ? utf8("发送旋钮排序") : utf8("拖动连接发送")); safe->refresh();
    });
  }
  void pasteChannel(int index) {
    picker.reset();
    for (int s = 0; s < slotCount; ++s) editors.erase(index*slotCount+s);
    host.beginEdit();
    if (!host.pasteChannel(index, juce::SystemClipboard::getTextFromClipboard()) || host.error.isNotEmpty()) showError();
    host.endEdit(utf8("粘贴通道"));
    strips.clear(); refresh();
  }
  void hideEditors() { if (settingsWindow) settingsWindow->setVisible(false); for (auto& entry : editors) entry.second->setVisible(false); if (picker) picker->closeButtonPressed(); host.save(); }
  void showError() { juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "Mini DAW", host.error); }
  void applyHistory(bool redo) {
    if (redo ? !host.canRedo() : !host.canUndo()) return;
    picker.reset(); editors.clear(); strips.clear();
    if (redo) host.redo(); else host.undo();
    if (resizeContentHeight && host.windowHeight != getHeight()) resizeContentHeight(host.windowHeight); refresh();
  }
  bool keyPressed(const juce::KeyPress& key) override {
    if (key.getModifiers().isCtrlDown()) {
      const int code = key.getKeyCode();
      if (code == 'Z' || code == 'z') { applyHistory(key.getModifiers().isShiftDown()); return true; }
      if (code == 'Y' || code == 'y') { applyHistory(true); return true; }
    }
    return false;
  }
  void resized() override {
    host.windowHeight = getHeight();
    int x = getWidth()-500;
    audio.setBounds(x, 14, 32, 32); settings.setBounds(x+38, 14, 32, 32);
    undoButton.setBounds(x+76, 14, 32, 32); redoButton.setBounds(x+114, 14, 32, 32);
    save.setBounds(x+152, 14, 80, 32); savePreset.setBounds(x+238, 14, 80, 32);
    loadPreset.setBounds(x+324, 14, 80, 32); background.setBounds(x+410, 14, 78, 32);
    status.setBounds(14, 34, getWidth()-524, 16);
    viewport.setBounds(12, 62, getWidth()-24, getHeight()-74); layoutStrips();
  }
  void paint(juce::Graphics& g) override {
    g.fillAll(bg); g.drawImage(brandIcon(), juce::Rectangle<float>(16, 8, 24, 24));
    g.setColour(ink); g.setFont(20); g.drawText("MINI DAW", 48, 6, 116, 28, juce::Justification::centredLeft);
    g.setColour(ink.withAlpha(0.06f)); g.drawHorizontalLine(54, 14, getWidth()-14.0f);
  }
private:
  ModernLook look;
  std::unique_ptr<SettingsWindow> settingsWindow; HorizontalViewport viewport; juce::Component array;
  std::vector<std::unique_ptr<ChannelStrip>> strips;
  juce::ComponentAnimator stripAnimator;
  std::unique_ptr<FloatingCard> dragOverlay;
  std::vector<std::unique_ptr<FloatingCard>> landingCards;
  juce::Image dragSnapshot;
  juce::Point<float> dragAnchor;
  double dragLiftStarted = 0;
  bool nativePluginDrag = false;
  std::optional<juce::MouseInputSource> cardDragSource;
  juce::Component::SafePointer<juce::Component> liftedSource;
  int statusFrame = 3;
  std::vector<std::unique_ptr<DropGap>> channelGaps;
  std::map<int, std::unique_ptr<EditorWindow>> editors; std::unique_ptr<PickerWindow> picker;
  juce::ComboBox driver;
  std::unique_ptr<juce::FileChooser> projectChooser;
  juce::TooltipWindow tooltips{nullptr, 650};
  juce::TextButton savePreset{utf8("保存预设")}, buffer;
  PresetLoadButton loadPreset;
  bool recentMenuOpen = false, projectChooserOpen = false;
  juce::File currentPreset;
  ToolbarIcon undoButton{ToolbarIcon::back}, redoButton{ToolbarIcon::forward};
  ToolbarIcon audio{ToolbarIcon::power}, settings{ToolbarIcon::gear};
  juce::TextButton add{utf8("+ 添加通道")}, save{utf8("保存工程")}, background{utf8("后台驻留")}; juce::Label status;
  void timerCallback() override {
    const double now = juce::Time::getMillisecondCounterHiRes();
    for (auto i = landingCards.begin(); i != landingCards.end();) {
      if ((*i)->destination && !(*i)->advance(now)) { ++i; continue; }
      const auto arrived = (*i)->arrived;
      if ((*i)->destination && arrived && !arrived(**i)) { ++i; continue; }
      if ((*i)->source && (*i)->returning) (*i)->source->showAsEmpty(false);
      if ((*i)->destination) (*i)->destination->setAlpha(1);
      i = landingCards.erase(i);
    }
    const bool dragging = isDragAndDropActive() && cardDragSource && cardDragSource->isDragging();
    const auto mouse = cardDragSource ? cardDragSource->getScreenPosition().roundToInt() : juce::Desktop::getMousePosition();
    double edgeSpeed = 0;
    if (dragging) {
      followFloatingCard(mouse);
      const auto p = viewport.getLocalPoint(nullptr, mouse);
      if (p.y >= 0 && p.y < viewport.getHeight()) {
        if (p.x >= 0 && p.x < 28) edgeSpeed = -360.0*(28-p.x)/28;
        else if (p.x > viewport.getWidth()-28 && p.x <= viewport.getWidth()) edgeSpeed = 360.0*(p.x-(viewport.getWidth()-28))/28;
      }
      if (!nativePluginDrag && dragSnapshot.isValid() && juce::Time::getMillisecondCounterHiRes()-dragLiftStarted < 220) {
        const float lift = static_cast<float>(1-std::exp(-(juce::Time::getMillisecondCounterHiRes()-dragLiftStarted)/50));
        juce::Image frame(juce::Image::ARGB, dragSnapshot.getWidth(), dragSnapshot.getHeight(), true);
        juce::Graphics g(frame); const auto anchor = dragAnchor*2;
        g.addTransform(juce::AffineTransform::scale(1+0.012f*lift, 1+0.012f*lift, anchor.x, anchor.y)); g.drawImageAt(dragSnapshot, 0, 0);
        if (dragOverlay) { dragOverlay->image = frame; dragOverlay->repaint(); }
        else if (!nativePluginDrag) setCurrentDragImage(juce::ScaledImage(frame, 2));
      }
    }
    viewport.setEdgeScroll(edgeSpeed, 0); for (auto& strip : strips) strip->scrollDuringDrag(dragging, mouse);
    if (++statusFrame % 4 != 0) return;
    audio.setButtonText(juce::String::fromUTF8("⏻")); audio.setTooltip(host.isRunning() ? utf8("停止音频引擎") : utf8("启动音频引擎")); audio.setColour(juce::TextButton::textColourOffId, host.isRunning() ? green : muted);
    undoButton.setEnabled(host.canUndo()); redoButton.setEnabled(host.canRedo());
    undoButton.setTooltip(utf8("撤销 · Ctrl+Z")+(host.canUndo() ? "\n"+host.undoLabel() : juce::String()));
    redoButton.setTooltip(utf8("重做 · Ctrl+Y / Ctrl+Shift+Z")+(host.canRedo() ? "\n"+host.redoLabel() : juce::String()));
    buffer.setButtonText(juce::String(host.blockSize())+utf8(" samples ▾")); buffer.setEnabled(host.isRunning());
    status.setColour(juce::Label::textColourId, host.error.isNotEmpty() ? red : host.isRunning() ? green : muted);
    if (host.error.isNotEmpty()) status.setText(host.error, juce::dontSendNotification);
    else status.setText((host.isRunning() ? utf8("● ASIO 运行中") : utf8("○ ASIO 已停止"))+utf8(" · ")+host.driverName+utf8(" · ")+juce::String(host.sampleRate()/1000, 1)+" kHz"+
      (host.isRunning() ? utf8(" · I/O ")+juce::String(host.deviceLatencyMs(), 2)+" ms" : juce::String()), juce::dontSendNotification);
    status.setTooltip(utf8("驱动报告的输入 + 输出延迟：")+juce::String(host.deviceLatencyMs(), 2)+utf8(" ms\n已连接输出路径的最大插件延迟：")+
      juce::String(host.pluginLatencySamples()*1000.0/host.sampleRate(), 2)+utf8(" ms；宿主不额外缓存音频块。"));
  }
  void showSettings() {
    if (!settingsWindow) settingsWindow = std::make_unique<SettingsWindow>(host, look, [safe = juce::Component::SafePointer<Mixer>(this)] { if (safe) safe->refresh(); });
    settingsWindow->setVisible(true); settingsWindow->toFront(true);
  }
  bool tooltipsEnabled = true;
  void showRecentPresets() {
    if (recentMenuOpen || projectChooserOpen) return;
    juce::PopupMenu menu; menu.addSectionHeader(utf8("最近打开的预设"));
    std::vector<juce::File> files;
    for (int i = 0; i < host.recentPresets.getNumFiles(); ++i) files.push_back(host.recentPresets.getFile(i));
    for (int i = 0; i < static_cast<int>(files.size()); ++i) {
      const auto& file = files[i]; juce::String label = file.getFileNameWithoutExtension();
      if (std::count_if(files.begin(), files.end(), [&](const juce::File& other) { return other.getFileNameWithoutExtension() == label; }) > 1)
        label += utf8(" · ")+file.getParentDirectory().getFullPathName();
      menu.addItem(i+1, label, file.existsAsFile(), file == currentPreset);
    }
    if (files.empty()) menu.addItem(999, utf8("暂无最近打开的预设"), false);
    menu.addSeparator(); menu.addItem(1000, utf8("打开其他预设…"));
    recentMenuOpen = true;
    // An attached PopupMenu swallows clicks on its originating button. Anchor
    // by screen area so dismissing the hover menu passes the click through.
    menu.setLookAndFeel(&look);
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetScreenArea(loadPreset.getScreenBounds()).withMinimumWidth(260)
        .withDeletionCheck(*this),
      [safe = juce::Component::SafePointer<Mixer>(this), files](int result) {
        if (!safe) return; safe->recentMenuOpen = false;
        if (result == 1000) safe->chooseProject(false);
        else if (juce::isPositiveAndBelow(result-1, static_cast<int>(files.size()))) safe->openPreset(files[result-1]);
      });
  }
  void openPreset(const juce::File& file) {
    editors.clear(); picker.reset(); strips.clear();
    const bool loaded = host.loadProject(file);
    if (loaded) { currentPreset = file; if (resizeContentHeight) resizeContentHeight(host.windowHeight); }
    viewport.setViewPosition(0, 0); driver.setText(host.driverName, juce::dontSendNotification); refresh();
    if (!loaded || host.error.isNotEmpty()) showError();
  }
  void chooseProject(bool saving) {
    if (projectChooserOpen) return; projectChooserOpen = true;
    juce::PopupMenu::dismissAllActiveMenus();
    const auto initial = currentPreset.existsAsFile() ? currentPreset : juce::File();
    projectChooser = std::make_unique<juce::FileChooser>(saving ? utf8("保存项目预设") : utf8("加载项目预设"), initial, "*.minidaw", true, false, getTopLevelComponent());
    projectChooser->launchAsync(saving ? juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles | juce::FileBrowserComponent::warnAboutOverwriting
                                      : juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
      [safe = juce::Component::SafePointer<Mixer>(this), saving](const juce::FileChooser& fc) {
        if (!safe) return; safe->projectChooserOpen = false;
        if (fc.getResult() == juce::File()) return;
        if (saving) { if (!safe->host.saveProject(fc.getResult().withFileExtension("minidaw"))) safe->showError(); }
        else safe->openPreset(fc.getResult());
      });
  }
  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(Mixer)
};

ChannelStrip::ChannelStrip(Host& h, Mixer& m, int i) : host(h), mixer(m), index(i), boundChannel(h.channels[i].get()), fader(*h.channels[i]) {
  for (juce::Component* c : std::initializer_list<juce::Component*>{&title, &input, &output, &mainReceive, &mute, &fader}) addAndMakeVisible(*c);
  title.setText(host.channels[i]->name, juce::dontSendNotification); title.setEditable(false, true, false); title.setColour(juce::Label::textColourId, ink);
  title.setFont(juce::Font(juce::FontOptions(14).withStyle("Bold")));
  title.drag = [this] { mixer.startCardDrag("channel:"+juce::String(index), &title, this); };
  title.context = [this](const auto& options) { contextMenu(options); };
  chain.context = sendArea.context = [this](const auto& options) { contextMenu(options); };
  addAndMakeVisible(chainViewport); chainViewport.setViewedComponent(&chain, false);
  chainViewport.setScrollBarsShown(true, false); chainViewport.setScrollBarThickness(12); chainViewport.setScrollOnDragEnabled(false);
  chainViewport.getVerticalScrollBar().getProperties().set("dragNavigation", true);
  chain.addAndMakeVisible(input); chain.addAndMakeVisible(output); chain.addAndMakeVisible(mainReceive);
  chain.addAndMakeVisible(sendViewport); sendViewport.setViewedComponent(&sendArea, false);
  sendViewport.setScrollBarsShown(false, false); sendViewport.setScrollBarThickness(12); sendViewport.setScrollOnDragEnabled(false);
  sendArea.draw = [this](juce::Graphics& g) { paintSends(g); };
  addAndMakeVisible(rightEdge); addAndMakeVisible(bottomEdge);
  rightEdge.begin = [this](const juce::ModifierKeys& mods) {
    resizeSingle = mods.isCtrlDown(); resizeStartWidths.clear(); for (const auto& channel : host.channels) resizeStartWidths.push_back(channel->stripWidth); host.beginEdit();
  };
  rightEdge.drag = [this](int delta) {
    if (!resizeSingle) delta /= index+1;
    for (int i = 0; i < static_cast<int>(host.channels.size()); ++i) if (!resizeSingle || i == index)
      host.channels[i]->stripWidth = juce::jlimit(176, 460, resizeStartWidths[i]+delta);
    mixer.layoutStrips();
  };
  rightEdge.end = [this] { host.endEdit(utf8("通道宽度")); };
  rightEdge.reset = [this](const juce::ModifierKeys& mods) {
    host.beginEdit(); for (int i = 0; i < static_cast<int>(host.channels.size()); ++i) if (!mods.isCtrlDown() || i == index) host.channels[i]->stripWidth = 194;
    mixer.layoutStrips(); host.endEdit(utf8("通道宽度"));
  };
  bottomEdge.begin = [this](const juce::ModifierKeys&) {
    resizeStartHeight = getHeight(); resizeStartContentHeight = mixer.getHeight(); resizeStartScales.clear();
    for (const auto& channel : host.channels) resizeStartScales.push_back(channel->stripScale); host.beginEdit();
  };
  bottomEdge.drag = [this](int delta) {
    if (mixer.resizeContentHeight) mixer.resizeContentHeight(juce::jlimit(550, 2100, resizeStartContentHeight+delta));
    const float ratio = static_cast<float>(mixer.availableStripHeight())/std::max(1, resizeStartHeight);
    for (int i = 0; i < static_cast<int>(host.channels.size()); ++i) {
      host.channels[i]->stripHeight = 0; host.channels[i]->stripScale = juce::jlimit(0.6f, 2.0f, resizeStartScales[i]*ratio);
    }
    mixer.layoutStrips();
  };
  bottomEdge.end = [this] { host.endEdit(utf8("通道纵向比例")); };
  bottomEdge.reset = [this](const juce::ModifierKeys&) { host.beginEdit(); for (auto& channel : host.channels) { channel->stripHeight = 0; channel->stripScale = 1; } mixer.layoutStrips(); host.endEdit(utf8("通道纵向比例")); };
  title.onTextChange = [this] { host.beginEdit(); { const juce::ScopedLock guard(host.lock); host.channels[index]->name = title.getText().trim(); } host.endEdit(utf8("通道改名")); mixer.refresh(); };
  input.setColour(juce::ComboBox::outlineColourId, muted); output.setColour(juce::ComboBox::outlineColourId, red);
  input.setScrollWheelEnabled(false); output.setScrollWheelEnabled(false);
  input.onChange = [this] {
    if (updating || input.getSelectedId() == 90000) return;
    const int id = input.getSelectedId();
    if (id == 300000) { host.refreshSystemOutputs(); mixer.refresh(); return; }
    host.beginEdit();
    if (id >= 100000) {
      const int choice = id-100000;
      if (juce::isPositiveAndBelow(choice, static_cast<int>(windowsInputChoices.size())))
        if (!host.setWindowsInput(index, windowsInputChoices[choice])) mixer.showError();
      host.endEdit(utf8("Windows 内录输入")); mixer.refresh(); return;
    }
    int left = -1, right = -1;
    if (id >= 1001) { left = id-1001; right = left+1; } else if (id >= 2) left = right = id-2;
    if (!host.setAsioInput(index, left, right)) mixer.showError();
    host.endEdit(utf8("输入绑定")); input.setTooltip(input.getText()); mixer.refresh();
  };
  output.onChange = [this] {
    if (updating || output.getSelectedId() == 90000) return;
    host.beginEdit(); const int id = output.getSelectedId();
    int left = -1, right = -1;
    if (id >= 1001) { left = id-1001; right = left+1; } else if (id >= 2) left = id-2;
    if (!host.setAsioOutput(index, left, right)) mixer.showError();
    host.endEdit(utf8("输出绑定")); output.setTooltip(output.getText()); mixer.refresh();
  };
  mute.setClickingTogglesState(true); mute.setColour(juce::TextButton::buttonOnColourId, red.withAlpha(0.45f)); mute.onClick = [this] { host.beginEdit(); host.channels[index]->mute = mute.getToggleState(); host.endEdit(utf8("静音")); };
  fader.begin = [this] { host.beginEdit(); }; fader.end = [this] { host.endEdit(utf8("通道推子")); };
  mute.setTooltip(utf8("静音"));
  mainReceive.getProperties().set("noTextCompression", true);
  mainReceive.onClick = [this] { chooseReceive(true); };
  for (int s = 0; s < slotCount; ++s) {
    slots[s] = std::make_unique<PluginSlot>(host.channels[index]->slots[s], s); chain.addAndMakeVisible(*slots[s]);
    slots[s]->open = [this, s] { mixer.openPlugin(index, s); }; slots[s]->context = [this, s](const auto& options) { mixer.pluginContext(index, s, options); };
    slots[s]->tint = [this] { return host.channels[index]->colour; };
    slots[s]->toggle = [this, s] { host.beginEdit(); auto& slot = host.channels[index]->slots[s]; slot.enabled = !slot.enabled.load(); host.endEdit(utf8("插件开关")); };
    slots[s]->drag = [this, s] { mixer.startCardDrag("plugin:"+juce::String(index)+":"+juce::String(s), slots[s].get()); };
    slots[s]->dragFollow = [this](juce::Point<int> p) { mixer.followFloatingCard(p); };
    slots[s]->dragReleased = [this](juce::Point<int> p) { mixer.pluginDragReleased(p); };
    slots[s]->drop = [this, s](const juce::String& d) { mixer.dropPlugin(d, index, s, true); };
  }
  for (int s = 0; s <= slotCount; ++s) {
    pluginGaps[s] = std::make_unique<DropGap>(true, "plugin:"); chain.addAndMakeVisible(*pluginGaps[s]);
    pluginGaps[s]->dropped = [this, s](const juce::String& d) { mixer.dropPlugin(d, index, s); };
  }
  startTimerHz(15); sync();
}
void ChannelStrip::sync(bool animate, const juce::AudioPluginInstance* landingPlugin) {
  std::array<juce::Rectangle<int>, slotCount> oldPluginBounds;
  for (int s = 0; s < slotCount; ++s) oldPluginBounds[s] = slots[s]->getBounds();
  pluginAnimator.cancelAllAnimations(true);
  updating = true;
  auto fill = [](juce::ComboBox& box, const juce::StringArray& names, int a, int b, const AsioBinding& left, const AsioBinding& right, bool out, bool asioHeading = false) {
    box.clear(juce::dontSendNotification); box.addItem(out ? utf8("直接输出：无") : utf8("输入：无"), 1);
    if (asioHeading) box.addSectionHeading(utf8("ASIO 设备"));
    for (int i = 0; i < names.size(); ++i) box.addItem(juce::String(i+1)+utf8(" · ")+names[i], i+2);
    if (names.size() >= 2) { box.addSeparator(); for (int i = 0; i+1 < names.size(); i += 2) box.addItem(juce::String(i+1)+"/"+juce::String(i+2)+utf8(" · ")+names[i]+" / "+names[i+1], 1001+i); }
    const bool aValid = juce::isPositiveAndBelow(a, names.size()), bValid = juce::isPositiveAndBelow(b, names.size());
    const bool missing = (!aValid && left.name.isNotEmpty()) || (!bValid && right.name.isNotEmpty());
    const bool partial = (!aValid && bValid) || (aValid && bValid && b != a && (b != a+1 || a%2 != 0));
    auto label = [&](int index, const AsioBinding& binding) {
      if (juce::isPositiveAndBelow(index, names.size())) return names[index];
      return binding.name.isEmpty() ? utf8("空") : binding.name+utf8("（未连接）");
    };
    if (missing || partial) {
      const bool single = left == right || (aValid && a == b) || (b < 0 && right.name.isEmpty());
      const auto text = label(a, left)+(single ? juce::String() : " / "+label(b, right));
      box.addItem(text, 90000); box.setItemEnabled(90000, false); box.setSelectedId(90000, juce::dontSendNotification);
    }
    else box.setSelectedId(a < 0 ? 1 : (b == a+1 ? 1001+a : a+2), juce::dontSendNotification);
  };
  auto& c = *host.channels[index];
  fill(input, c.loopback ? host.outputNames : host.inputNames, c.input, c.inputRight, c.asioBindings[0], c.asioBindings[1], false, c.loopback);
  windowsInputChoices.clear();
  if (c.loopback) {
    input.changeItemText(1, utf8("内录输入：无"));
    input.addSeparator(); input.addSectionHeading(utf8("Windows 输出"));
    const auto windowsOutputs = host.windowsLoopbackOutputs(); int selectedWindows = 0;
    auto addWindows = [&](const WindowsOutput& endpoint, int left, int right) {
      const int id = 100000+static_cast<int>(windowsInputChoices.size());
      windowsInputChoices.push_back({endpoint.id, endpoint.name, left, right});
      const auto channels = juce::String(left+1)+(left == right ? juce::String() : "/"+juce::String(right+1));
      input.addItem(utf8("Windows · ")+endpoint.name+utf8(" · ")+channels, id);
      if (c.windowsInput.endpoint == endpoint.id && c.windowsInput.left == left && c.windowsInput.right == right) selectedWindows = id;
    };
    for (const auto& endpoint : windowsOutputs) {
      for (int left = 0; left+1 < endpoint.channels; left += 2) addWindows(endpoint, left, left+1);
      for (int channel = 0; channel < endpoint.channels; ++channel) addWindows(endpoint, channel, channel);
    }
    if (c.windowsInput.endpoint.isNotEmpty() && !selectedWindows) {
      // Keep the saved selection visible in the collapsed control without
      // offering an occupied/missing device in the Windows menu.
      const auto name = c.windowsInput.name.isEmpty() ? utf8("已保存的 Windows 设备") : c.windowsInput.name;
      input.setText(utf8("Windows · ")+name+utf8(" · ")+juce::String(c.windowsInput.left+1)
        +(c.windowsInput.left == c.windowsInput.right ? juce::String() : "/"+juce::String(c.windowsInput.right+1))+utf8("（未连接）"), juce::dontSendNotification);
    } else if (selectedWindows) input.setSelectedId(selectedWindows, juce::dontSendNotification);
    input.addSeparator(); input.addItem(utf8("刷新 Windows 输出设备…"), 300000);
    fill(output, host.inputNames, c.output, c.outputRight, c.asioBindings[2], c.asioBindings[3], true);
    if (output.getSelectedId() == 1) output.changeItemText(1, utf8("输入跳线：无"));
  } else fill(output, host.outputNames, c.output, c.outputRight, c.asioBindings[2], c.asioBindings[3], true);
  input.setTooltip(input.getText()); output.setTooltip(output.getText());
  title.setText(c.name, juce::dontSendNotification);
  mute.setToggleState(c.mute.load(), juce::dontSendNotification);
  const int count = sendControlCount(static_cast<int>(c.sends.size()));
  if (static_cast<int>(knobs.size()) != count) {
    knobs.clear(); sendGaps.clear();
    for (int i = 0; i <= count; ++i) {
      auto gap = std::make_unique<DropGap>(true, "send:"); sendArea.addAndMakeVisible(*gap);
      gap->dropped = [this, i](const juce::String& d) { mixer.dropSend(d, index, i, true); }; sendGaps.push_back(std::move(gap));
    }
    for (int i = 0; i < count; ++i) {
      auto knob = std::make_unique<SendKnob>(); sendArea.addAndMakeVisible(*knob); knob->choose = [this, i](const auto& options) { chooseSend(i, options); };
      knob->floatDrag = [this, i] {
        const auto row = juce::Rectangle<int>(0, i*sendRowPitch, sendArea.getWidth(), sendRowPitch);
        mixer.startCardDrag("send:"+juce::String(index)+":"+juce::String(i), knobs[i].get(), &sendArea, row);
      };
      knob->drop = [this, i](const juce::String& d) { mixer.dropSend(d, index, i); };
      knob->onDragStart = [this] { sendEditing = true; host.beginEdit(); };
      knob->onDragEnd = [this] { host.endEdit(utf8("发送音量")); sendEditing = false; };
      knob->onValueChange = [this, i] { if (updating) return; if (!sendEditing) host.beginEdit(); const juce::ScopedLock guard(host.lock); auto& sends = host.channels[index]->sends;
        if (i < static_cast<int>(sends.size())) sends[i].gain = knobs[i]->getValue() <= -72 ? 0 : juce::Decibels::decibelsToGain(static_cast<float>(knobs[i]->getValue()));
        if (!sendEditing) host.endEdit(utf8("发送音量"));
      }; knobs.push_back(std::move(knob));
    }
  }
  for (int i = 0; i < count; ++i) {
    const bool connected = i < static_cast<int>(c.sends.size()) && c.sends[i].target >= 0;
    knobs[i]->setValue(connected ? juce::Decibels::gainToDecibels(c.sends[i].gain, -72.0f) : 0, juce::dontSendNotification);
    knobs[i]->setColour(juce::Slider::rotarySliderFillColourId, connected ? host.channels[c.sends[i].target]->colour : muted);
    knobs[i]->setTooltip(utf8("触屏短长按打开菜单，继续按住浮动拖动 · 拖到旋钮交换 / 间隙插入 / 其他通道发送 · 直接拖动调音量 · 右键选择总线 · 双击 0 dB"));
  }
  juce::StringArray mains, sides;
  for (int source = 0; source < static_cast<int>(host.channels.size()); ++source) for (const auto& r : host.channels[source]->sends) if (r.target == index) {
    if (r.slot < 0) mains.add(host.channelLabel(source)); else sides.add("SC"+juce::String(r.bus)+" "+host.channelLabel(source));
  }
  auto all = mains; all.addArray(sides);
  mainReceive.setButtonText(all.isEmpty() ? utf8("接收 ▾") : (sides.isEmpty() ? mains : sides).joinIntoString(", ")+utf8(" ▾"));
  mainReceive.setTooltip(utf8("主输入：")+mains.joinIntoString(", ")+utf8("\n侧链：")+sides.joinIntoString(", "));
  updating = false; resized(); repaint(); sendArea.repaint();
  for (int s = 0; s < slotCount; ++s) {
    auto* plugin = c.slots[s].plugin.get();
    if (plugin == landingPlugin && plugin) { slots[s]->setAlpha(0); continue; }
    if (animate && plugin && plugin != displayedPlugins[s]) {
      const auto destination = slots[s]->getBounds();
      const auto previous = std::find(displayedPlugins.begin(), displayedPlugins.end(), plugin);
      if (previous != displayedPlugins.end()) slots[s]->setBounds(oldPluginBounds[std::distance(displayedPlugins.begin(), previous)]);
      else slots[s]->setAlpha(0.25f);
      pluginAnimator.animateComponent(slots[s].get(), destination, 1, 180, false, 1.8, 0);
    }
  }
  for (int s = 0; s < slotCount; ++s) displayedPlugins[s] = c.slots[s].plugin.get();
}
void ChannelStrip::resized() {
  const juce::Rectangle<int> header(6, 6, getWidth()-12, 34);
  constexpr int headerInset = 5, titleGap = 6;
  mute.setBounds(header.getRight()-headerInset-32, header.getY()+headerInset, 32, header.getHeight()-2*headerInset);
  loopbackTagBounds = {mute.getX()-titleGap-26, mute.getY(), 26, mute.getHeight()};
  const int titleRight = host.channels[index]->loopback ? loopbackTagBounds.getX()-titleGap : mute.getX()-titleGap;
  title.setBounds(34, 8, std::max(1, titleRight-34), 28);
  contentX = juce::jlimit(53, 78, juce::roundToInt(getWidth()*0.28f)); contentWidth = getWidth()-contentX-10;
  const float scale = host.channels[index]->stripScale;
  const auto sy = [scale](int y) { return juce::roundToInt(y*scale); };
  slotPitch = sy(32);
  // Leave the same clear space from the slim navigation bar to the boxes and the strip edge.
  const int chainWidth = contentWidth-20;
  const float knobScale = juce::jlimit(0.6f, 2.0f, std::min(scale, (chainWidth-14)/72.0f));
  knobWidth = std::max(44, juce::roundToInt(42*knobScale));
  sendRowPitch = juce::jmax(36, sy(48), juce::roundToInt(32*knobScale)+16);
  const int sendHeight = static_cast<int>(knobs.size())*sendRowPitch+2;
  // Routing frames have equal breathing room, larger than the gaps within the plugin chain.
  const int slotGap = std::max(3, sy(5)), routingGap = std::max(6, sy(10)), routingHeight = sy(25);
  // The broad title outline needs a little extra optical space above the first routing frame.
  const int chainTop = header.getBottom()+routingGap+sy(2), pluginTop = routingHeight+routingGap;
  outputTop = pluginTop+slotCount*slotPitch-slotGap+routingGap;
  receiveTop = outputTop+routingHeight+routingGap;
  const int sendTop = receiveTop+routingHeight+routingGap;
  const int viewportHeight = std::max(1, getHeight()-chainTop-13), availableSendHeight = viewportHeight-sendTop;
  const int secondValueBottom = 2+sendRowPitch+SendKnob::valueLabelBounds(knobScale, knobWidth).getBottom();
  const bool scrollSendsOnly = availableSendHeight >= secondValueBottom;
  chainViewport.setVerticalWheelStep(sendRowPitch*0.5); sendViewport.setVerticalWheelStep(sendRowPitch*0.5);
  if (scrollSendsOnly) {
    if (sendViewport.getParentComponent() != this) addAndMakeVisible(sendViewport);
    chainViewport.setScrollBarsShown(false, false); chainViewport.setViewPosition(0, 0);
    chainViewport.setBounds(contentX, chainTop, contentWidth, sendTop); chain.setSize(chainWidth, sendTop);
    sendViewport.setScrollBarsShown(true, false);
    sendViewport.setBounds(contentX, chainTop+sendTop, contentWidth, availableSendHeight);
  } else {
    if (sendViewport.getParentComponent() != &chain) chain.addAndMakeVisible(sendViewport);
    sendViewport.setScrollBarsShown(false, false); sendViewport.setViewPosition(0, 0);
    chainViewport.setScrollBarsShown(true, false);
    chainViewport.setBounds(contentX, chainTop, contentWidth, viewportHeight); chain.setSize(chainWidth, sendTop+sendHeight+10);
    sendViewport.setBounds(0, sendTop, chainWidth, sendHeight);
  }
  input.setBounds(0, 0, chainWidth, routingHeight);
  mainReceive.setBounds(0, receiveTop, chainWidth, routingHeight);
  fader.setBounds(contentX-34, 46, 25, getHeight()-78);
  const float unityY = static_cast<float>(chainTop+pluginTop+3*slotPitch);
  fader.setUnityVisualPosition((fader.getBottom()-10-unityY)/(fader.getHeight()-20));
  for (int s = 0; s < slotCount; ++s) slots[s]->setBounds(0, pluginTop+s*slotPitch, chainWidth, slotPitch-slotGap);
  for (int s = 0; s <= slotCount; ++s) pluginGaps[s]->setBounds(0, pluginTop-slotGap+s*slotPitch, chainWidth, slotGap);
  output.setBounds(0, outputTop, chainWidth, routingHeight);
  sendArea.setSize(chainWidth, sendHeight);
  for (int i = 0; i < static_cast<int>(knobs.size()); ++i) {
    knobs[i]->getProperties().set("knobScale", knobScale);
    // Align each knob to the unchanged text column of the neighbouring rows.
    const float centre = i%2 ? (chainWidth+knobWidth+8)*0.5f : (chainWidth-knobWidth-8)*0.5f;
    knobs[i]->setBounds(juce::roundToInt(centre-knobWidth*0.5f), 2+i*sendRowPitch, knobWidth, sendRowPitch-4);
    knobs[i]->resized();
  }
  for (int i = 0; i < static_cast<int>(sendGaps.size()); ++i) sendGaps[i]->setBounds(0, i*sendRowPitch, chainWidth, 5);
  rightEdge.setBounds(getWidth()-6, 40, 6, getHeight()-46); bottomEdge.setBounds(6, getHeight()-6, getWidth()-12, 6);
}
void ChannelStrip::paint(juce::Graphics& g) {
  const auto colour = host.channels[index]->colour;
  g.setColour(panel); g.fillRoundedRectangle(getLocalBounds().toFloat(), corner);
  g.setColour(colour.withAlpha(0.12f)); g.fillRoundedRectangle(6, 6, static_cast<float>(getWidth()-12), 34, corner);
  g.setColour(colour.withAlpha(0.7f)); g.drawRoundedRectangle(6.5f, 6.5f, getWidth()-13.0f, 33, corner, 1);
  if (dropMotion.amount() > 0) { g.setColour(green.withAlpha(dropMotion.amount())); g.drawRoundedRectangle(getLocalBounds().toFloat().reduced(1), corner, 2); }
  g.setFont(12); g.setColour(colour); g.drawText(juce::String(index+1).paddedLeft('0', 2), 11, 11, 24, 24, juce::Justification::centredLeft);
  if (host.channels[index]->loopback) { g.setFont(9); g.drawText(utf8("内录"), loopbackTagBounds, juce::Justification::centred); }
  g.setFont(10); g.setColour(muted);
  int previousTickY = -100;
  for (int db : {12, 6, 0, -6, -12, -18, -24, -30, -36, -48, -60, -72, -96}) {
    int y = fader.getY()+10+juce::roundToInt((1-fader.visualPosition(dbToPosition(static_cast<float>(db))))*(fader.getHeight()-20));
    if (db < -36 && (y-previousTickY < 14 || y > fader.getBottom()-30)) continue;
    previousTickY = y; g.setColour(db == 0 ? ink : muted); g.drawText(juce::String(db), 0, y-7, fader.getX()-3, 14, juce::Justification::centredRight);
  }
  g.drawText(juce::String::fromUTF8("−∞"), 0, fader.getBottom()-16, fader.getX()-3, 16, juce::Justification::centredRight);
  float value = positionToDb(host.channels[index]->fader.load()); g.setColour(ink); g.drawText(std::isfinite(value) ? juce::String(value, 1)+" dB" : juce::String::fromUTF8("−∞"), 0, getHeight()-28, contentX-4, 20, juce::Justification::centred);
}
void ChannelStrip::paintSends(juce::Graphics& g) {
  g.setFont(10);
  const auto& sends = host.channels[index]->sends;
  for (int i = 0; i < static_cast<int>(knobs.size()); ++i) {
    juce::String number = juce::String::charToString(static_cast<juce::juce_wchar>('A'+i)), name = "+";
    if (i < static_cast<int>(sends.size()) && sends[i].target >= 0) {
      const auto& r = sends[i]; number += utf8(" · ")+juce::String(r.target+1).paddedLeft('0', 2); name = host.channels[r.target]->name;
      if (r.slot >= 0) number += " SC"+juce::String(r.bus);
    }
    const int gap = 8, x = i%2 ? 0 : knobWidth+gap, width = sendArea.getWidth()-knobWidth-gap;
    const auto align = juce::Justification::centred;
    const int top = knobs[i]->getY()+juce::roundToInt(16*static_cast<float>(knobs[i]->getProperties().getWithDefault("knobScale", 1.0f)))-12;
    g.setColour(knobs[i]->findColour(juce::Slider::rotarySliderFillColourId));
    g.setFont(9); g.drawText(number, x, top, width, 12, align, true);
    g.setFont(10); g.drawText(name, x, top+12, width, 13, align, true);
  }
}
void ChannelStrip::mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails& w) { mixer.scrollChannels(w); }
void ChannelStrip::scrollDuringDrag(bool active, juce::Point<int> screenPosition) {
  for (auto* viewport : {&chainViewport, &sendViewport}) {
    const auto p = viewport->getLocalPoint(nullptr, screenPosition); double speed = 0;
    if (active && viewport->getVerticalScrollBar().isVisible() && viewport->getLocalBounds().contains(p)) {
      if (p.y < 24) speed = -260.0*(24-p.y)/24;
      else if (p.y > viewport->getHeight()-24) speed = 260.0*(p.y-(viewport->getHeight()-24))/24;
    }
    viewport->setEdgeScroll(0, speed);
  }
}
void ChannelStrip::mouseDown(const juce::MouseEvent& e) {
  touchContext.press(e); if (e.mods.isPopupMenu()) contextMenu(pointerMenuOptions(*this, e));
}
void ChannelStrip::itemDropped(const SourceDetails& d) {
  dropMotion.set(false);
  if (d.description.toString().startsWith("send:")) mixer.dropSend(d.description.toString(), index);
  else mixer.reorderChannel(d.description.toString().fromFirstOccurrenceOf(":", false, false).getIntValue(), index);
}
void ChannelStrip::contextMenu(const juce::PopupMenu::Options& options) {
  juce::PopupMenu menu;
  const auto& channel = *host.channels[index];
  menu.addItem(20, channel.loopback ? utf8("转为普通通道") : utf8("转为内录通道"));
  menu.addSeparator(); menu.addItem(1, utf8("复制通道"));
  const auto clipboard = juce::XmlDocument::parse(juce::SystemClipboard::getTextFromClipboard());
  menu.addItem(2, utf8("粘贴通道"), clipboard && clipboard->hasTagName("MINI_DAW_CHANNEL"));
  menu.addSeparator(); menu.addItem(10, utf8("通道颜色…")); menu.addItem(18, utf8("随机颜色"));
  menu.showMenuAsync(options, [safe = juce::Component::SafePointer<ChannelStrip>(this)](int result) {
    if (!safe) return;
    if (result == 20) {
      safe->host.beginEdit(); if (!safe->host.setLoopbackMode(safe->index, !safe->host.channels[safe->index]->loopback)) safe->mixer.showError();
      safe->host.endEdit(utf8("转换内录模式")); safe->mixer.refresh();
    } else if (result == 1) juce::SystemClipboard::copyTextToClipboard(safe->host.copyChannel(safe->index));
    else if (result == 2) {
      const auto mixer = juce::Component::SafePointer<Mixer>(&safe->mixer); const int index = safe->index;
      juce::MessageManager::callAsync([mixer, index] { if (mixer) mixer->pasteChannel(index); });
    } else if (result == 10) {
      safe->host.beginEdit();
      auto palette = std::make_unique<ColourPalette>(safe->host.channels[safe->index]->colour, [safe](juce::Colour colour) {
        if (!safe) return;
        auto& c = *safe->host.channels[safe->index]; c.colour = colour; c.colourAssigned = true; safe->mixer.refresh();
      }, [mixer = juce::Component::SafePointer<Mixer>(&safe->mixer)] { if (mixer) mixer->host.endEdit(utf8("通道颜色")); });
      juce::CallOutBox::launchAsynchronously(std::move(palette), safe->title.getScreenBounds(), nullptr);
    } else if (result == 18) {
      safe->host.beginEdit();
      auto& c = *safe->host.channels[safe->index];
      c.colour = juce::Colour::fromHSV(juce::Random::getSystemRandom().nextFloat(), 0.48f, 0.88f, 1.0f);
      c.colourAssigned = true; safe->host.endEdit(utf8("通道颜色")); safe->mixer.refresh();
    }
  });
}
void ChannelStrip::chooseSend(int sendIndex, const juce::PopupMenu::Options& menuOptions) {
  juce::PopupMenu menu; std::vector<Route> options;
  for (int target = 0; target < static_cast<int>(host.channels.size()); ++target) {
    if (target == index) continue; auto& c = *host.channels[target]; juce::PopupMenu buses;
    options.push_back({target, -1, 0, 0}); buses.addItem(static_cast<int>(options.size()), utf8("主输入"));
    for (int s = 0; s < slotCount; ++s) if (auto* plugin = c.slots[s].plugin.get()) for (int bus = 1; bus < plugin->getBusCount(true); ++bus) {
      auto* b = plugin->getBus(true, bus); if (!b->isEnabled()) continue; options.push_back({target, s, bus, 0});
      buses.addItem(static_cast<int>(options.size()), juce::String(s+1).paddedLeft('0', 2)+utf8(" · ")+c.slots[s].description.name+utf8(" · SC ")+juce::String(bus)+utf8(" · ")+b->getName());
    }
    menu.addSubMenu(host.channelLabel(target), buses);
  }
  const auto& sends = host.channels[index]->sends;
  const bool preFx = sendIndex < static_cast<int>(sends.size()) && sends[sendIndex].preFx;
  menu.addSeparator(); menu.addSectionHeader("Signal Tap");
  menu.addItem(10001, "Pre-FX", true, preFx); menu.addItem(10002, "Post-FX", true, !preFx);
  menu.addSeparator(); menu.addItem(10000, utf8("移除发送"), sendIndex < static_cast<int>(sends.size()));
  menu.addSeparator(); menu.addItem(10003, utf8("向上移动发送"), sendIndex > 0);
  menu.addItem(10004, utf8("向下移动发送"), sendIndex+1 < static_cast<int>(knobs.size()));
  menu.showMenuAsync(menuOptions, [safe = juce::Component::SafePointer<ChannelStrip>(this), options, sendIndex](int result) {
    if (!safe || result == 0) return;
    if (result == 10003 || result == 10004) {
      safe->mixer.dropSend("send:"+juce::String(safe->index)+":"+juce::String(sendIndex), safe->index, sendIndex+(result == 10003 ? -1 : 1));
      return;
    }
    safe->host.beginEdit();
    if (result == 10001 || result == 10002) safe->host.setSendTap(safe->index, sendIndex, result == 10001);
    else if (result == 10000) safe->host.removeRoute(safe->index, sendIndex);
    else if (juce::isPositiveAndBelow(result-1, static_cast<int>(options.size()))) {
      auto r = options[result-1]; auto& sends = safe->host.channels[safe->index]->sends;
      r.gain = sendIndex < static_cast<int>(sends.size()) && sends[sendIndex].target >= 0 ? sends[sendIndex].gain : juce::Decibels::decibelsToGain(static_cast<float>(safe->knobs[sendIndex]->getValue()));
      r.preFx = sendIndex < static_cast<int>(sends.size()) && sends[sendIndex].preFx;
      if (!safe->host.setRoute(safe->index, sendIndex, r)) safe->mixer.showError();
    }
    safe->host.endEdit(result == 10001 || result == 10002 ? utf8("发送信号位置") : utf8("发送路由")); safe->mixer.refresh();
  });
}
void ChannelStrip::chooseReceive(bool sidechain) {
  juce::PopupMenu menu;
  struct Option { int source, send; Route route; bool remove; }; std::vector<Option> options;
  auto sources = [&](int slot, int bus) {
    juce::PopupMenu list;
    for (int source = 0; source < static_cast<int>(host.channels.size()); ++source) {
      if (source == index) continue; auto& c = *host.channels[source]; int found = -1;
      for (int i = 0; i < static_cast<int>(c.sends.size()); ++i) { const auto& r = c.sends[i]; if (r.target == index && r.slot == slot && r.bus == bus) { found = i; break; } }
      options.push_back({source, found < 0 ? static_cast<int>(c.sends.size()) : found, {index, slot, bus, 1}, found >= 0});
      list.addItem(static_cast<int>(options.size()), (slot < 0 ? juce::String("IN") : "SC "+juce::String(bus))+utf8(" · ")+host.channelLabel(source), true, found >= 0);
    }
    return list;
  };
  menu.addSubMenu(utf8("主输入接收"), sources(-1, 0));
  if (sidechain) for (int s = 0; s < slotCount; ++s) if (auto* plugin = host.channels[index]->slots[s].plugin.get()) for (int bus = 1; bus < plugin->getBusCount(true); ++bus)
    if (plugin->getBus(true, bus)->isEnabled()) menu.addSubMenu(juce::String(s+1).paddedLeft('0', 2)+utf8(" · ")+host.channels[index]->slots[s].description.name+utf8(" · SC ")+juce::String(bus)+utf8(" · ")+plugin->getBus(true, bus)->getName(), sources(s, bus));
  if (!menu.getNumItems()) menu.addItem(10000, utf8("已加载插件未提供可用侧链输入总线"), false);
  menu.showMenuAsync(juce::PopupMenu::Options(), [safe = juce::Component::SafePointer<ChannelStrip>(this), options](int result) {
    if (!safe || !juce::isPositiveAndBelow(result-1, static_cast<int>(options.size()))) return; const auto& o = options[result-1];
    safe->host.beginEdit();
    if (o.remove) safe->host.removeRoute(o.source, o.send); else if (!safe->host.setRoute(o.source, o.send, o.route)) safe->mixer.showError();
    safe->host.endEdit(utf8("接收路由")); safe->mixer.refresh();
  });
}

class MixerView final : public juce::Component {
public:
  explicit MixerView(bool offline = false) : mixer(offline) {
    addAndMakeVisible(mixer); mixer.resizeContentHeight = [this](int height) { setSize(getWidth(), height); }; setSize(canvasWidth, mixer.host.windowHeight);
  }
  Mixer mixer;
  void paint(juce::Graphics& g) override { g.fillAll(bg); }
  void resized() override {
    mixer.setBounds(getLocalBounds());
  }
};

inline int verifyUI(const juce::File& file) {
  juce::XmlElement report("UI_VERIFICATION"); bool passed = true;
  auto check = [&](const char* label, bool value) { auto* c = report.createNewChildElement("CHECK"); c->setAttribute("name", label); c->setAttribute("passed", value); passed &= value; };
  Channel channel; MeterFader fader(channel); fader.setSize(28, 620);
  auto event = [&](float y, bool dragged) {
    return juce::MouseEvent(juce::Desktop::getInstance().getMainMouseSource(), {14, y}, juce::ModifierKeys::leftButtonModifier,
      1, 0, 0, 0, 0, &fader, &fader, juce::Time::getCurrentTime(), {14, 300}, juce::Time::getCurrentTime(), 1, dragged);
  };
  const auto initial = channel.fader.load();
  fader.mouseDown(event(300, false)); fader.mouseUp(event(300, false));
  check("click anywhere leaves fader unchanged", channel.fader == initial);
  fader.mouseDoubleClick(event(300, false)); check("double click leaves fader unchanged", channel.fader == initial);
  fader.mouseDown(event(300, false)); fader.mouseDrag(event(302, true));
  check("small accidental movement ignored", channel.fader == initial);
  fader.mouseDrag(event(420, true)); check("drag changes value relatively without jumping", std::abs(fader.visualPosition(channel.fader.load())-(fader.visualPosition(initial)-0.2f)) < 0.00001f);
  channel.fader = dbToPosition(-6); fader.mouseDown(event(300, false));
  const float unityDrag = (fader.visualPosition(dbToPosition(0))-fader.visualPosition(dbToPosition(-6)))*600;
  fader.mouseDrag(event(300-unityDrag+2, true));
  check("drag near unity snaps to exactly zero dB", channel.fader == dbToPosition(0));
  fader.mouseDrag(event(300-unityDrag-10, true)); check("drag can leave unity detent", channel.fader > dbToPosition(0));
  const float sixDbStep = fader.visualPosition(dbToPosition(12))-fader.visualPosition(dbToPosition(6));
  bool equalSteps = true;
  for (int db = 6; db > -36; db -= 6) equalSteps &= std::abs(fader.visualPosition(dbToPosition(static_cast<float>(db)))-fader.visualPosition(dbToPosition(static_cast<float>(db-6)))-sixDbStep) < 0.00001f;
  check("every six dB from plus twelve through minus thirty-six has equal spacing", equalSteps);
  check("below minus thirty-six the dB spacing gradually compresses to digital mute", fader.visualPosition(dbToPosition(-60))-fader.visualPosition(dbToPosition(-72)) < fader.visualPosition(dbToPosition(-36))-fader.visualPosition(dbToPosition(-48)) && fader.visualPosition(0) == 0);
  channel.fader = dbToPosition(-12); const auto savedFader = channel.fader.load(); fader.setUnityVisualPosition(0.7f);
  check("fader layout remapping leaves the stored audio gain unchanged", channel.fader == savedFader && gainForPosition(channel.fader.load()) == gainForPosition(savedFader));
  MixerView view(true); view.setSize(canvasWidth, canvasHeight);
  check("small window fills real bounds without scaling text", view.mixer.getBounds() == view.getLocalBounds() && view.mixer.getTransform().isIdentity());
  auto stripSize = [&] {
    for (auto* child : view.mixer.getChildren()) if (auto* viewport = dynamic_cast<HorizontalViewport*>(child))
      for (auto* strip : viewport->getViewedComponent()->getChildren()) if (dynamic_cast<ChannelStrip*>(strip)) return strip->getBounds();
    return juce::Rectangle<int>();
  };
  const auto smallStrip = stripSize();
  view.setSize(1500, canvasHeight); const auto wideStrip = stripSize();
  check("horizontal resize reveals channels while retaining width", wideStrip.getWidth() == smallStrip.getWidth() && wideStrip.getHeight() == smallStrip.getHeight());
  view.setSize(1500, 960); const auto tallStrip = stripSize();
  check("vertical resize stretches channels while retaining width", tallStrip.getHeight() > wideStrip.getHeight() && tallStrip.getWidth() == wideStrip.getWidth());
  view.setSize(900, 900);
  check("nonmatching aspect ratio fills window without margins or clipping", view.mixer.getBounds() == view.getLocalBounds() && view.mixer.getTransform().isIdentity());
  {
    MixerView sizing(true); HorizontalViewport* scroller = nullptr;
    for (auto* child : sizing.mixer.getChildren()) if (auto* v = dynamic_cast<HorizontalViewport*>(child)) scroller = v;
    if (scroller) {
      std::vector<ChannelStrip*> channels;
      for (auto* child : scroller->getViewedComponent()->getChildren()) if (auto* strip = dynamic_cast<ChannelStrip*>(child)) channels.push_back(strip);
      auto* strip = channels.front(); StripResizeEdge *right = nullptr, *bottom = nullptr; juce::Viewport* plugins = nullptr;
      for (auto* child : strip->getChildren()) {
        if (auto* edge = dynamic_cast<StripResizeEdge*>(child)) { if (edge->getWidth() == 6) right = edge; else bottom = edge; }
        if (auto* v = dynamic_cast<juce::Viewport*>(child))
          for (auto* part : v->getViewedComponent()->getChildren()) if (dynamic_cast<PluginSlot*>(part)) { plugins = v; break; }
      }
      auto gesture = [](juce::Component* target, juce::Point<float> position, juce::Point<float> start, bool ctrl = false) {
        return juce::MouseEvent(juce::Desktop::getInstance().getMainMouseSource(), position, juce::ModifierKeys::leftButtonModifier | (ctrl ? juce::ModifierKeys::ctrlModifier : 0),
          1, 0, 0, 0, 0, target, target, juce::Time::getCurrentTime(), start, juce::Time::getCurrentTime(), 1, position != start);
      };
      check("both channel resize edges are accessible", right && bottom && plugins);
      if (right && bottom && plugins) {
        const int initialWidth = strip->getWidth(), nextX = channels[1]->getX();
        right->mouseDown(gesture(right, {3, 100}, {3, 100})); right->mouseDrag(gesture(right, {53, 100}, {3, 100})); right->mouseUp(gesture(right, {53, 100}, {3, 100}));
        check("right edge resizes all channels together by default", strip->getWidth() == initialWidth+50 && channels[1]->getWidth() == initialWidth+50 && channels[1]->getX() == nextX+50);
        right->mouseDown(gesture(right, {3, 100}, {3, 100}, true)); right->mouseDrag(gesture(right, {33, 100}, {3, 100}, true)); right->mouseUp(gesture(right, {33, 100}, {3, 100}, true));
        check("Ctrl right edge resizes only the selected channel", strip->getWidth() == initialWidth+80 && channels[1]->getWidth() == initialWidth+50);
        PluginSlot* slot = nullptr;
        for (auto* child : plugins->getViewedComponent()->getChildren()) if (auto* p = dynamic_cast<PluginSlot*>(child)) { slot = p; break; }
        const int originalSlotHeight = slot ? slot->getHeight() : 0;
        sizing.setSize(canvasWidth, canvasHeight+100);
        check("window resize retains plugin dimensions", slot && slot->getHeight() == originalSlotHeight);
        const int initialHeight = strip->getHeight(), initialWindowHeight = sizing.getHeight();
        bottom->mouseDown(gesture(bottom, {50, 3}, {50, 3})); bottom->mouseDrag(gesture(bottom, {50, -117}, {50, 3})); bottom->mouseUp(gesture(bottom, {50, -117}, {50, 3}));
        check("bottom edge resizes window and all channels with their vertical proportions", sizing.getHeight() == initialWindowHeight-120
          && strip->getHeight() == initialHeight-120 && channels[1]->getHeight() == strip->getHeight() && slot && slot->getHeight() < originalSlotHeight
          && sizing.mixer.host.channels[0]->stripScale < 1 && sizing.mixer.host.channels[1]->stripScale == sizing.mixer.host.channels[0]->stripScale);
        for (int i = 0; i < 6; ++i) { sizing.mixer.host.setRoute(0, i, {i+2, -1, 0, 1}); sizing.mixer.host.setRoute(1, i, {i+2, -1, 0, 1}); }
        sizing.mixer.refresh();
        NavigationViewport *firstNavigation = nullptr, *secondNavigation = nullptr;
        for (auto* child : strip->getChildren()) if (auto* v = dynamic_cast<NavigationViewport*>(child); v && v->getVerticalScrollBar().isVisible()) firstNavigation = v;
        for (auto* child : channels[1]->getChildren()) if (auto* v = dynamic_cast<NavigationViewport*>(child); v && v->getVerticalScrollBar().isVisible()) secondNavigation = v;
        if (firstNavigation && secondNavigation) {
          firstNavigation->setViewPosition(0, 30);
          auto shown = [](NavigationViewport& v) { return static_cast<bool>(v.getVerticalScrollBar().getProperties().getWithDefault("navigationShown", false)); };
          check("vertical navigation is independent for each channel", shown(*firstNavigation) && !shown(*secondNavigation)
            && !static_cast<bool>(scroller->getHorizontalScrollBar().getProperties().getWithDefault("navigationShown", false)));
          juce::MessageManager::getInstance()->runDispatchLoopUntil(700);
          check("navigation remains visible after scrolling stops", shown(*firstNavigation));
          juce::MessageManager::getInstance()->runDispatchLoopUntil(2450);
          check("navigation hides three seconds after the last scroll", !shown(*firstNavigation));
        } else check("independent navigation viewports exist", false);
        sizing.mixer.applyHistory(false);
        check("undo restores window height and proportions without losing channel widths", sizing.getHeight() == initialWindowHeight
          && sizing.mixer.host.channels[0]->stripScale == 1 && sizing.mixer.host.channels[0]->stripWidth == initialWidth+80 && sizing.mixer.host.channels[1]->stripWidth == initialWidth+50);
      }
    } else check("resize viewport exists", false);
  }
  HorizontalViewport* viewport = nullptr;
  for (auto* child : view.mixer.getChildren()) if (auto* v = dynamic_cast<HorizontalViewport*>(child)) viewport = v;
  if (viewport) {
    juce::MouseWheelDetails wheel{}; wheel.deltaY = -0.25f;
    viewport->mouseWheelMove(event(300, false), wheel);
    check("wheel starts smoothly without jumping a whole step", viewport->getViewPositionX() == 0);
    juce::MessageManager::getInstance()->runDispatchLoopUntil(50);
    check("wheel advances through intermediate positions", viewport->getViewPositionX() > 0 && viewport->getViewPositionX() < 140);
    viewport->mouseWheelMove(event(300, false), wheel);
    juce::MessageManager::getInstance()->runDispatchLoopUntil(700);
    check("repeated wheel input accumulates without losing distance", viewport->getViewPositionX() == 280);
    viewport->mouseWheelMove(event(300, false), wheel);
    juce::MessageManager::getInstance()->runDispatchLoopUntil(30); viewport->setViewPosition(0, 0);
    juce::MessageManager::getInstance()->runDispatchLoopUntil(300);
    check("direct navigation cancels pending wheel motion", viewport->getViewPositionX() == 0);
    wheel.deltaY = 100; viewport->mouseWheelMove(event(300, false), wheel);
    juce::MessageManager::getInstance()->runDispatchLoopUntil(40);
    check("wheel at boundary remains clamped", viewport->getViewPositionX() == 0);
    auto* array = viewport->getViewedComponent();
    const int stripPitch = 206;
    auto findTarget = [&](juce::Point<int> point, const juce::String& kind) -> juce::DragAndDropTarget* {
      juce::DragAndDropTarget::SourceDetails details(kind, {}, {});
      for (auto* item = array->getComponentAt(point); item; item = item->getParentComponent())
        if (auto* target = dynamic_cast<juce::DragAndDropTarget*>(item); target && target->isInterestedInDragSource(details)) return target;
      return nullptr;
    };
    auto* body = findTarget({stripPitch+30, 43}, "channel:0");
    check("channel body hit tests as swap target", dynamic_cast<ChannelStrip*>(body) != nullptr);
    auto* gap = findTarget({stripPitch, 43}, "channel:0");
    check("channel gap hit tests as insertion target", dynamic_cast<DropGap*>(gap) != nullptr);
    ChannelStrip* firstStrip = nullptr;
    for (auto* child : array->getChildren())
      if (auto* strip = dynamic_cast<ChannelStrip*>(child); strip && strip->matchesChannel(view.mixer.host.channels[0].get())) firstStrip = strip;
    if (firstStrip) {
      auto* card = firstStrip->pluginCard(0); const auto bounds = array->getLocalArea(card, card->getLocalBounds());
      check("plugin body and gap have separate hit targets", dynamic_cast<PluginSlot*>(findTarget(bounds.getCentre(), "plugin:0:0")) != nullptr
        && dynamic_cast<DropGap*>(findTarget({bounds.getCentreX(), bounds.getBottom()+2}, "plugin:0:0")) != nullptr);
    } else check("first channel exists for plugin hit testing", false);
    auto* first = view.mixer.host.channels[0].get(); auto* second = view.mixer.host.channels[1].get(); auto* third = view.mixer.host.channels[2].get();
    if (body) { body->itemDropped({"channel:0", {}, {100, 48}}); juce::MessageManager::getInstance()->runDispatchLoopUntil(30); }
    check("body drop exchanges channels through UI handler", view.mixer.host.channels[0].get() == second && view.mixer.host.channels[1].get() == first);
    ChannelStrip* moving = nullptr;
    for (auto* child : array->getChildren()) if (auto* strip = dynamic_cast<ChannelStrip*>(child); strip && strip->matchesChannel(first)) moving = strip;
    check("channel sorting animates the retained card through intermediate positions", moving && moving->getX() > 6 && moving->getX() < 212);
    juce::MessageManager::getInstance()->runDispatchLoopUntil(230);
    check("channel card settles exactly in its new position", moving && moving->getX() == 212);
    gap = findTarget({stripPitch*3, 43}, "channel:1");
    if (gap) { gap->itemDropped({"channel:1", {}, {6, 48}}); juce::MessageManager::getInstance()->runDispatchLoopUntil(30); }
    check("gap drop inserts channel through UI handler", view.mixer.host.channels[0].get() == second && view.mixer.host.channels[1].get() == third && view.mixer.host.channels[2].get() == first);
    const auto insertedName = view.mixer.host.channels[2]->name;
    view.mixer.applyHistory(false);
    check("toolbar undo restores channel settings", view.mixer.host.channels[1]->name == insertedName && view.mixer.host.canRedo());
    view.mixer.applyHistory(true);
    check("toolbar redo reapplies insertion", view.mixer.host.channels[2]->name == insertedName);
  } else check("mixer viewport exists", false);
  {
    MixerView scrolling(true); scrolling.setSize(canvasWidth, canvasHeight);
    for (int i = 0; i < 10; ++i) scrolling.mixer.host.setRoute(0, i, {i+1, -1, 0, 1});
    scrolling.mixer.refresh();
    ChannelStrip* strip = nullptr;
    for (auto* child : scrolling.mixer.getChildren()) if (auto* v = dynamic_cast<HorizontalViewport*>(child))
      for (auto* part : v->getViewedComponent()->getChildren()) if (auto* candidate = dynamic_cast<ChannelStrip*>(part)) { strip = candidate; break; }
    auto activeNavigation = [&]() -> NavigationViewport* {
      if (strip) for (auto* child : strip->getChildren())
        if (auto* v = dynamic_cast<NavigationViewport*>(child); v && v->getVerticalScrollBar().isVisible()) return v;
      return nullptr;
    };
    auto* sends = activeNavigation();
    if (strip) {
      MeterFader* meter = nullptr; for (auto* child : strip->getChildren()) if (auto* candidate = dynamic_cast<MeterFader*>(child)) meter = candidate;
      check("zero dB aligns with the fourth plugin top", meter && std::abs(meter->getScreenPosition().y+meter->getHeight()-10
        -(meter->getHeight()-20)*meter->visualPosition(dbToPosition(0))-strip->pluginCard(3)->getScreenPosition().y) < 0.5f);
    }
    check("two complete send rows keep the plugin chain fixed and scroll only sends", sends && dynamic_cast<SendArea*>(sends->getViewedComponent()));
    if (sends && strip) {
      const auto pluginPosition = strip->pluginCard(0)->getScreenPosition();
      sends->scrollBy(0, 80); juce::MessageManager::getInstance()->runDispatchLoopUntil(650);
      check("send wheel smoothly scrolls without moving plugin boxes", sends->getViewPositionY() == 80 && strip->pluginCard(0)->getScreenPosition() == pluginPosition);
      sends->setViewPosition(0, 0);
      juce::MouseWheelDetails wheel{}; wheel.deltaY = -120.0f/512.0f;
      sends->mouseWheelMove(event(300, false), wheel); juce::MessageManager::getInstance()->runDispatchLoopUntil(650);
      check("one Windows wheel notch scrolls half a send row", sends->getViewPositionY() == 24 && strip->pluginCard(0)->getScreenPosition() == pluginPosition);
    }
    scrolling.setSize(canvasWidth, canvasHeight-4); sends = activeNavigation();
    check("the second dB label may fit without the unused row padding", sends && dynamic_cast<SendArea*>(sends->getViewedComponent()));
    scrolling.setSize(canvasWidth, canvasHeight-5);
    auto* chain = activeNavigation();
    check("clipping the second dB label switches to whole-chain scrolling", chain && !dynamic_cast<SendArea*>(chain->getViewedComponent()));
    if (chain && strip) {
      const auto pluginPosition = strip->pluginCard(0)->getScreenPosition();
      chain->setViewPosition(0, 40);
      check("whole-chain scroll moves plugin boxes together with sends", strip->pluginCard(0)->getScreenPosition().y == pluginPosition.y-40);
    }
    scrolling.setSize(canvasWidth, canvasHeight); sends = activeNavigation();
    check("growing the window returns to independent send scrolling", sends && dynamic_cast<SendArea*>(sends->getViewedComponent()));
    scrolling.mixer.host.inputNames = {"Mic", "Return"}; scrolling.mixer.host.outputNames = {"Output A", "Output B", "Unmapped output C"};
    scrolling.mixer.host.setLoopbackMode(0, true); scrolling.mixer.host.setBindings(0, 2, 2, 0, -1); scrolling.mixer.refresh();
    check("loopback input lists every ASIO output", strip && strip->inputSelector().getItemText(1).contains("Output A")
      && strip->inputSelector().getItemText(2).contains("Output B") && strip->inputSelector().getItemText(3).contains("Unmapped output C")
      && strip->inputSelector().isItemEnabled(4) && strip->inputSelector().getSelectedId() == 4);
    check("loopback output still lists ASIO input destinations", strip && strip->outputSelector().getItemText(1).contains("Mic") && strip->outputSelector().getItemText(2).contains("Return"));
    check("stereo input menu identifies both output ports", strip && strip->inputSelector().getItemText(4).contains("Output A / Output B"));
    check("stereo output menu identifies both input ports", strip && strip->outputSelector().getItemText(3).contains("Mic / Return"));
    scrolling.mixer.host.setBindings(0, 0, 1, 0, 1); scrolling.mixer.refresh();
    check("selected stereo pair identifies both endpoints", strip && strip->inputSelector().getText().contains("Output A / Output B")
      && strip->outputSelector().getText().contains("Mic / Return"));
  }
  // Exercise the actual send-drop handler, routing plan and history together.
  view.mixer.dropSend("send:0:0", 1); juce::MessageManager::getInstance()->runDispatchLoopUntil(30);
  check("dropping unused send on another channel connects at unity", view.mixer.host.channels[0]->sends.size() == 1 && view.mixer.host.channels[0]->sends[0].target == 1 && view.mixer.host.channels[0]->sends[0].gain == 1);
  view.mixer.dropSend("send:0:1", 2); juce::MessageManager::getInstance()->runDispatchLoopUntil(30);
  view.mixer.dropSend("send:0:0", 0, 1); juce::MessageManager::getInstance()->runDispatchLoopUntil(30);
  check("send knob drop reorders sends without changing destinations", view.mixer.host.channels[0]->sends[0].target == 2 && view.mixer.host.channels[0]->sends[1].target == 1);
  view.mixer.applyHistory(false);
  check("send reorder can be undone", view.mixer.host.channels[0]->sends[0].target == 1 && view.mixer.host.channels[0]->sends[1].target == 2);
  // A gap inserts rather than exchanging, even across three existing sends.
  view.mixer.dropSend("send:0:2", 3); juce::MessageManager::getInstance()->runDispatchLoopUntil(30);
  view.mixer.dropSend("send:0:0", 0, 2); juce::MessageManager::getInstance()->runDispatchLoopUntil(30);
  check("knob body drop swaps without moving intervening sends", view.mixer.host.channels[0]->sends[0].target == 3 && view.mixer.host.channels[0]->sends[1].target == 2 && view.mixer.host.channels[0]->sends[2].target == 1);
  view.mixer.dropSend("send:0:2", 0, 0, true); juce::MessageManager::getInstance()->runDispatchLoopUntil(30);
  check("knob gap drop inserts and shifts intervening sends", view.mixer.host.channels[0]->sends[0].target == 1 && view.mixer.host.channels[0]->sends[1].target == 3 && view.mixer.host.channels[0]->sends[2].target == 2);
  SettingsPanel settings(view.mixer.host, {});
  settings.select(0); settings.select(1); settings.select(2); settings.select(3);
  check("settings categories switch without opening devices or scans", !view.mixer.host.isRunning() && !view.mixer.host.scanJob);
  ColourPalette palette(blue, {}); check("full palette with editable colour and 24 swatches", palette.getNumSwatches() == 24 && palette.getCurrentColour() == blue);
  const auto icon = brandIcon(); check("square application icon embedded", icon.isValid() && icon.getWidth() == 256 && icon.getHeight() == 256);
  report.setAttribute("passed", passed); file.getParentDirectory().createDirectory(); report.writeTo(file); return passed ? 0 : 1;
}

inline int verifyDropConfirmation(const juce::File& file, const juce::String& path) {
  MixerView view(true); auto& mixer = view.mixer;
  mixer.host.channels[0]->colour = blue; mixer.host.channels[0]->colourAssigned = true;
  mixer.host.channels[1]->colour = red; mixer.host.channels[1]->colourAssigned = true;
  juce::XmlElement report("DROP_VERIFICATION"); bool passed = true;
  auto check = [&](const char* label, bool value) { auto* c = report.createNewChildElement("CHECK"); c->setAttribute("name", label); c->setAttribute("passed", value); passed &= value; };
  juce::OwnedArray<juce::PluginDescription> descriptions; scanPluginFactory(juce::File(path), descriptions);
  const juce::PluginDescription* effect = nullptr; for (auto* d : descriptions) if (isEffectCandidate(*d)) { effect = d; break; }
  const bool loaded = effect && mixer.host.loadPlugin(0, 0, *effect) && mixer.host.loadPlugin(1, 0, *effect);
  check("source and occupied destination loaded", loaded);
  if (loaded) {
    const auto origin = mixer.host.channels[0]->slots[0].plugin, destination = mixer.host.channels[1]->slots[0].plugin;
    auto prompt = [&] {
      mixer.dropPlugin("plugin:0:0", 1, 0, true);
      juce::MessageManager::getInstance()->runDispatchLoopUntil(150);
      return dynamic_cast<juce::AlertWindow*>(juce::Component::getCurrentlyModalComponent());
    };
    auto* alert = prompt(); check("occupied cross channel drop asks for confirmation before modifying", alert
      && mixer.host.channels[0]->slots[0].plugin == origin && mixer.host.channels[1]->slots[0].plugin == destination);
    if (alert) alert->exitModalState(0);
    juce::MessageManager::getInstance()->runDispatchLoopUntil(100);
    check("cancel keeps both plugins and creates no history", mixer.host.channels[0]->slots[0].plugin == origin
      && mixer.host.channels[1]->slots[0].plugin == destination && !mixer.host.canUndo());
    alert = prompt(); if (alert) alert->exitModalState(1);
    juce::MessageManager::getInstance()->runDispatchLoopUntil(200);
    const auto replacement = mixer.host.channels[1]->slots[0].plugin;
    check("confirm copies and replaces while retaining source", alert && replacement != destination && replacement != origin && mixer.host.channels[0]->slots[0].plugin == origin);
    mixer.applyHistory(false);
    check("undo confirmed replacement restores original destination", mixer.host.channels[1]->slots[0].plugin == destination && mixer.host.channels[0]->slots[0].plugin == origin);
    mixer.applyHistory(true); check("redo reuses copied replacement", mixer.host.channels[1]->slots[0].plugin == replacement);
    mixer.refresh();
    auto findCard = [&](int channel, int slot) -> PluginSlot* {
      for (auto* child : mixer.getChildren()) if (auto* v = dynamic_cast<HorizontalViewport*>(child))
        for (auto* part : v->getViewedComponent()->getChildren())
          if (auto* strip = dynamic_cast<ChannelStrip*>(part); strip && strip->matchesChannel(mixer.host.channels[channel].get())) return strip->pluginCard(slot);
      return nullptr;
    };
    auto* sourceCard = findCard(0, 0);
    if (sourceCard) {
      const auto image = sourceCard->createComponentSnapshot(sourceCard->getLocalBounds().expanded(14), false, 2);
      auto equalImages = [](const juce::Image& a, const juce::Image& b) {
        if (a.getBounds() != b.getBounds()) return false;
        for (int y = 0; y < a.getHeight(); ++y) for (int x = 0; x < a.getWidth(); ++x) if (a.getPixelAt(x, y) != b.getPixelAt(x, y)) return false;
        return true;
      };
      const auto finalColour = findCard(1, 1)->landingImageFrom(*sourceCard);
      FloatingCard colourPreview; colourPreview.image = image; colourPreview.setBounds(0, 0, image.getWidth()/2, image.getHeight()/2);
      colourPreview.setLandingImage(finalColour); colourPreview.landAt(colourPreview.getBounds());
      colourPreview.advance(juce::Time::getMillisecondCounterHiRes()+80);
      check("landing colour and opacity blend gradually rather than switching on the last frame", colourPreview.colourTransition() > 0 && colourPreview.colourTransition() < 1
        && !equalImages(colourPreview.image, image) && !equalImages(colourPreview.image, finalColour));
      const auto middleColour = colourPreview.image.createCopy();
      colourPreview.advance(juce::Time::getMillisecondCounterHiRes()+150);
      check("colour reaches the exact destination appearance before position handoff", colourPreview.colourTransition() == 1 && equalImages(colourPreview.image, finalColour));
      juce::Image colourFrames(juce::Image::ARGB, image.getWidth()*3, image.getHeight(), true);
      { juce::Graphics g(colourFrames); g.fillAll(panel); g.drawImageAt(image, 0, 0); g.drawImageAt(middleColour, image.getWidth(), 0); g.drawImageAt(finalColour, image.getWidth()*2, 0); }
      if (auto stream = file.getSiblingFile("landing-colour-frames.png").createOutputStream()) { stream->setPosition(0); stream->truncate(); juce::PNGImageFormat().writeImageToStream(colourFrames, *stream); }
      const auto start = mixer.localAreaToGlobal(juce::Rectangle<int>(180, 210, image.getWidth()/2, image.getHeight()/2));
      auto landing = [&]() -> FloatingCard* { for (auto* child : mixer.getChildren()) if (auto* c = dynamic_cast<FloatingCard*>(child)) return c; return nullptr; };
      auto visualCount = [&] { int count = 0; for (auto* child : mixer.getChildren()) if (dynamic_cast<FloatingCard*>(child)) ++count; return count; };
      mixer.liftPluginCard(sourceCard, {image, start});
      const auto lifted = sourceCard->createComponentSnapshot(sourceCard->getLocalBounds());
      const auto empty = findCard(0, 2)->createComponentSnapshot(findCard(0, 2)->getLocalBounds());
      bool emptyStyle = lifted.getBounds() == empty.getBounds();
      for (int y = 0; emptyStyle && y < lifted.getHeight(); ++y) for (int x = 0; emptyStyle && x < lifted.getWidth(); ++x) emptyStyle &= lifted.getPixelAt(x, y) == empty.getPixelAt(x, y);
      check("lifting paints an empty slot instead of a faded plugin", sourceCard->isDragEmpty() && sourceCard->getAlpha() == 1 && emptyStyle);
      auto* liftedVisual = landing();
      mixer.dropPlugin("plugin:0:0", 0, 2, true);
      auto* moving = landing(); auto* destinationCard = findCard(0, 2);
      const auto destinationBounds = destinationCard->getScreenBounds().expanded(14);
      check("mouse release hands the same single preview directly to landing", moving == liftedVisual && visualCount() == 1 && destinationCard->getAlpha() == 0);
      check("plugin drop starts at the mouse visual rather than the original slot", moving && destinationCard->getAlpha() == 0
        && moving->getScreenBounds().getPosition().getDistanceFrom(start.getPosition()) < start.getPosition().getDistanceFrom(destinationBounds.getPosition())*0.5f);
      FloatingCard geometry; geometry.setBounds(start); geometry.landAt(destinationBounds);
      geometry.advance(juce::Time::getMillisecondCounterHiRes()+1000);
      check("landing preserves the card size and finishes at the complete target rectangle", geometry.getBounds() == destinationBounds);
      mixer.dragOperationEnded(juce::DragAndDropTarget::SourceDetails("plugin:0:0", sourceCard, {}));
      juce::MessageManager::getInstance()->runDispatchLoopUntil(40);
      check("landing moves on its first frames with no native fade duplicate", landing() == moving && visualCount() == 1 && moving->getScreenBounds() != start
        && moving->getWidth() == destinationBounds.getWidth() && moving->getHeight() == destinationBounds.getHeight() && destinationCard->getAlpha() == 0);
      juce::MessageManager::getInstance()->runDispatchLoopUntil(80);
      check("same-channel preview changes tone during landing while the target remains hidden", landing() == moving && moving->colourTransition() > 0 && destinationCard->getAlpha() == 0);
      juce::MessageManager::getInstance()->runDispatchLoopUntil(300);
      check("same-channel drop lands in its frame and restores the real card", !landing() && destinationCard->getAlpha() == 1 && mixer.host.channels[0]->slots[2].plugin == origin);
      mixer.liftPluginCard(destinationCard, {image, start}); auto* copyPreview = landing();
      mixer.dropPlugin("plugin:0:2", 1, 1, true);
      check("copy starts its preview immediately and gently reveals the source", landing() == copyPreview && visualCount() == 1 && !destinationCard->isDragEmpty() && destinationCard->isRevealing());
      juce::MessageManager::getInstance()->runDispatchLoopUntil(30);
      auto* copyCard = findCard(1, 1);
      check("cross-channel copy moves before plugin loading and hides the destination", landing() && visualCount() == 1 && copyCard->getAlpha() == 0
        && !mixer.host.channels[1]->slots[1].plugin && mixer.host.channels[0]->slots[2].plugin == origin);
      juce::MessageManager::getInstance()->runDispatchLoopUntil(300);
      check("copied plugin hands off without an overlapping preview", !landing() && copyCard->getAlpha() == 1 && !destinationCard->isRevealing()
        && mixer.host.channels[1]->slots[1].plugin && mixer.host.channels[1]->slots[1].plugin != origin);
      const auto outside = mixer.localAreaToGlobal(juce::Rectangle<int>(-80, mixer.getHeight()+70, image.getWidth()/2, image.getHeight()/2));
      mixer.liftPluginCard(destinationCard, {image, outside}); auto* returning = landing();
      mixer.pluginDragReleased(outside.getPosition());
      juce::MessageManager::getInstance()->runDispatchLoopUntil(10);
      check("release outside the mixer starts return with one preview and an empty origin", landing() == returning && returning->returning && visualCount() == 1 && destinationCard->isDragEmpty());
      juce::MessageManager::getInstance()->runDispatchLoopUntil(40);
      check("unused drag returns from its actual released position", landing() == returning && returning->getScreenBounds() != outside && destinationCard->isDragEmpty());
      juce::MessageManager::getInstance()->runDispatchLoopUntil(300);
      check("unused drag restores the origin without moving or copying the plugin", !landing() && !destinationCard->isDragEmpty() && destinationCard->getAlpha() == 1 && mixer.host.channels[0]->slots[2].plugin == origin);
      mixer.liftPluginCard(destinationCard, {image, start});
      mixer.dropPlugin("plugin:0:2", 1, 0, true);
      juce::MessageManager::getInstance()->runDispatchLoopUntil(250);
      alert = dynamic_cast<juce::AlertWindow*>(juce::Component::getCurrentlyModalComponent());
      check("occupied copy holds one preview without loading before confirmation", alert && visualCount() == 1 && destinationCard->isDragEmpty() && findCard(1, 0)->getAlpha() == 0 && mixer.host.channels[1]->slots[0].plugin == replacement);
      if (alert) alert->exitModalState(0);
      juce::MessageManager::getInstance()->runDispatchLoopUntil(30);
      check("cancelled copy smoothly returns the preview and restores the occupied target", landing() && landing()->returning && visualCount() == 1 && findCard(1, 0)->getAlpha() == 1);
      juce::MessageManager::getInstance()->runDispatchLoopUntil(300);
      check("cancelled copy returns home with both original instances unchanged", !landing() && !destinationCard->isDragEmpty() && mixer.host.channels[0]->slots[2].plugin == origin && mixer.host.channels[1]->slots[0].plugin == replacement);
    } else check("source plugin card exists for landing verification", false);
  }
  report.setAttribute("passed", passed); file.getParentDirectory().createDirectory(); report.writeTo(file); return passed ? 0 : 1;
}

class MainWindow;
class Tray final : public juce::SystemTrayIconComponent {
public:
  explicit Tray(MainWindow&);
  void mouseDoubleClick(const juce::MouseEvent&) override;
  void mouseDown(const juce::MouseEvent&) override;
private: MainWindow& window;
};
class MainWindow final : public juce::DocumentWindow {
public:
  MainWindow() : DocumentWindow(utf8("Mini DAW · ASIO 调音台"), bg, allButtons) {
    setUsingNativeTitleBar(true); setIcon(brandIcon()); auto* view = new MixerView; view->mixer.hide = [this] { hideToTray(); }; setContentOwned(view, true);
    view->mixer.resizeContentHeight = [this](int height) { setContentComponentSize(getContentComponent()->getWidth(), height); };
    setResizable(true, false); setResizeLimits(850, 550, 4096, 2160); centreWithSize(canvasWidth, view->mixer.host.windowHeight); setVisible(true); tray = std::make_unique<Tray>(*this);
  }
  void show() { setVisible(true); setMinimised(false); toFront(true); }
  void hideToTray() { if (tray) { static_cast<MixerView*>(getContentComponent())->mixer.hideEditors(); setVisible(false); } }
  void closeButtonPressed() override {
    if (closingPrompt) return; closingPrompt = true;
    juce::AlertWindow::showYesNoCancelBox(juce::MessageBoxIconType::QuestionIcon, utf8("关闭 Mini DAW"), utf8("关闭前是否保存工程？"),
      utf8("保存并关闭"), utf8("不保存关闭"), utf8("取消"), this,
      juce::ModalCallbackFunction::create([safe = juce::Component::SafePointer<MainWindow>(this)](int result) {
        if (!safe) return; safe->closingPrompt = false; if (result == 0) return;
        auto& host = static_cast<MixerView*>(safe->getContentComponent())->mixer.host;
        if (result == 1 && !host.saveProject(host.stateFile())) { static_cast<MixerView*>(safe->getContentComponent())->mixer.showError(); return; }
        host.saveOnExit = false; juce::JUCEApplication::getInstance()->quit();
      }));
  }
private: bool closingPrompt = false; std::unique_ptr<Tray> tray;
};
Tray::Tray(MainWindow& w) : window(w) {
  const auto icon = brandIcon();
  setIconImage(icon, icon); setIconTooltip(utf8("Mini DAW · 双击打开 / 右键退出"));
}
void Tray::mouseDoubleClick(const juce::MouseEvent&) { window.show(); }
void Tray::mouseDown(const juce::MouseEvent& e) {
  if (!e.mods.isPopupMenu()) return; juce::PopupMenu menu; menu.addItem(1, utf8("打开调音台")); menu.addItem(2, utf8("保存并退出"));
  menu.showMenuAsync(juce::PopupMenu::Options(), [safe = juce::Component::SafePointer<Tray>(this)](int result) { if (!safe) return; if (result == 1) safe->window.show(); if (result == 2) juce::JUCEApplication::getInstance()->systemRequestedQuit(); });
}
class Application final : public juce::JUCEApplication, private juce::Timer {
public:
  const juce::String getApplicationName() override { return "MiniDAW"; }
  const juce::String getApplicationVersion() override { return "0.2.0"; }
  bool moreThanOneInstanceAllowed() override {
    // Offline checks must execute even while the normal mixer is running.
    const auto command = getCommandLineParameters().trimStart();
    return command.startsWith("--verify-host ") || command.startsWith("--verify-asio ")
        || command.startsWith("--snapshot ") || command.startsWith("--scan-plugin ") || command.startsWith("--verify-scan ") || command.startsWith("--verify-editor ") || command.startsWith("--verify-ui ") || command.startsWith("--verify-drop ");
  }
  void initialise(const juce::String& command) override {
    auto args = juce::StringArray::fromTokens(command, true); args.removeEmptyStrings();
    if (args.size() >= 2 && args[0] == "--verify-ui") { setApplicationReturnValue(verifyUI(juce::File(args[1].unquoted()))); quit(); return; }
    if (args.size() >= 3 && args[0] == "--verify-drop") { setApplicationReturnValue(verifyDropConfirmation(juce::File(args[1].unquoted()), args[2].unquoted())); quit(); return; }
    if (args.size() >= 3 && args[0] == "--verify-editor") {
      Host host(false); juce::OwnedArray<juce::PluginDescription> types;
      scanPluginFactory(juce::File(args[2].unquoted()), types);
      juce::XmlElement report("EDITOR_VERIFICATION"); bool passed = false;
      for (auto* d : types) if (isEffectCandidate(*d) && host.loadPlugin(1, 0, *d)) {
        EditorWindow editor(host, *host.channels[1]->slots[0].plugin, false);
        const auto hwnd = static_cast<HWND>(editor.getPeer()->getNativeHandle());
        const auto style = GetWindowLongPtrW(hwnd, GWL_STYLE), exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
        passed = (exStyle & WS_EX_CONTEXTHELP) != 0 && (style & (WS_MAXIMIZEBOX | WS_MINIMIZEBOX)) == 0;
        report.setAttribute("nativeSettingsButton", passed); report.setAttribute("plugin", d->name);
        report.setAttribute("resizable", static_cast<juce::AudioProcessorEditor*>(editor.getContentComponent())->isResizable());
        report.setAttribute("sidechainBuses", host.channels[1]->slots[0].plugin->getBusCount(true)-1);
        break;
      }
      report.setAttribute("passed", passed); report.setAttribute("error", host.error);
      const juce::File file(args[1].unquoted()); file.getParentDirectory().createDirectory(); report.writeTo(file);
      setApplicationReturnValue(passed ? 0 : 1); quit(); return;
    }
    if (args.size() >= 3 && args[0] == "--scan-plugin") {
      SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
      juce::OwnedArray<juce::PluginDescription> found;
      const bool valid = scanPluginFactory(juce::File(args[1].unquoted()), found);
      juce::XmlElement report("SCAN_RESULT");
      report.setAttribute("factoryValid", valid);
      for (auto* d : found) report.addChildElement(d->createXml().release());
      report.writeTo(juce::File(args[2].unquoted()));
      setApplicationReturnValue(valid ? 0 : 1); quit(); return;
    }
    if (args.size() >= 2 && args[0] == "--verify-scan") {
      scanReport = juce::File(args[1].unquoted()); scanVerifier = std::make_unique<Host>(false);
      if (args.size() >= 4) if (auto cached = juce::XmlDocument::parse(juce::File(args[3].unquoted())))
        for (auto* p : cached->getChildWithTagNameIterator("PLUGIN")) { juce::PluginDescription d; if (d.loadFromXml(*p)) scanVerifier->catalog.addType(d); }
      scanStart = juce::Time::getMillisecondCounterHiRes();
      scanVerifier->beginScan(args.size() >= 3 ? juce::FileSearchPath(args[2].unquoted()) : defaultPluginSearchPaths());
      startTimer(100); return;
    }
    if (args.size() >= 2 && args[0] == "--verify-asio") {
      Host host(false); juce::XmlElement report("ASIO_VERIFICATION");
      const bool started = host.start(); juce::MessageManager::getInstance()->runDispatchLoopUntil(400);
      const auto initialCallbacks = host.callbackCount.load();
      const auto previousInputs = host.inputNames, previousOutputs = host.outputNames;
      if (started) { host.audioDeviceStopped(); juce::MessageManager::getInstance()->runDispatchLoopUntil(2400); }
      const bool rebuilt = host.deviceGeneration >= 2 && host.isRunning() && host.callbackCount.load() > initialCallbacks;
      bool bufferChanged = true; int testBuffer = host.blockSize(); const int originalBuffer = host.blockSize();
      for (int size : host.bufferSizes()) if (size < originalBuffer) testBuffer = size;
      if (started && testBuffer != originalBuffer) {
        const auto before = host.callbackCount.load();
        const bool opened = host.setBufferSize(testBuffer); juce::MessageManager::getInstance()->runDispatchLoopUntil(400);
        bufferChanged = opened && host.blockSize() == testBuffer && host.callbackCount.load() > before;
        bufferChanged = host.setBufferSize(originalBuffer) && bufferChanged; juce::MessageManager::getInstance()->runDispatchLoopUntil(150);
      }
      host.beginEdit(); host.channels[0]->fader = dbToPosition(-9); host.endEdit("fader");
      const auto beforeUndo = host.callbackCount.load(); const bool historyApplied = host.undo();
      juce::MessageManager::getInstance()->runDispatchLoopUntil(150);
      const bool liveUndo = historyApplied && host.channels[0]->fader == dbToPosition(0) && host.isRunning() && host.callbackCount.load() > beforeUndo;
      const bool passed = started && initialCallbacks > 0 && rebuilt && bufferChanged && liveUndo && host.inputNames == previousInputs && host.outputNames == previousOutputs;
      report.setAttribute("driver", host.driverName); report.setAttribute("started", started); report.setAttribute("passed", passed);
      report.setAttribute("callbacks", juce::String(host.callbackCount.load())); report.setAttribute("sampleRate", host.sampleRate()); report.setAttribute("blockSize", host.blockSize());
      report.setAttribute("ioLatencyMs", host.deviceLatencyMs()); report.setAttribute("pluginLatencySamples", host.pluginLatencySamples());
      report.setAttribute("testedSmallerBuffer", testBuffer); report.setAttribute("bufferChangePassed", bufferChanged);
      report.setAttribute("liveUndoPassed", liveUndo);
      report.setAttribute("automaticRebuild", rebuilt); report.setAttribute("deviceGeneration", static_cast<int>(host.deviceGeneration));
      report.setAttribute("inputs", host.inputNames.joinIntoString(" | ")); report.setAttribute("outputs", host.outputNames.joinIntoString(" | ")); report.setAttribute("error", host.error);
      host.stop(); juce::File(args[1].unquoted()).getParentDirectory().createDirectory(); report.writeTo(juce::File(args[1].unquoted()));
      setApplicationReturnValue(passed ? 0 : 1); quit(); return;
    }
    if (args.size() >= 2 && args[0] == "--verify-host") {
      setApplicationReturnValue(verifyHost(args.size() > 2 ? args[2].unquoted() : juce::String(), juce::File(args[1].unquoted()))); quit(); return;
    }
    if (args.size() >= 2 && args[0] == "--snapshot") {
      if (args.size() > 2 && args[2].startsWith("settings")) {
        Host offline(false); ModernLook look; SettingsPanel settings(offline, {}); settings.setLookAndFeel(&look);
        if (args[2] == "settings-loopback") {
          offline.refreshSystemOutputs(); offline.outputNames = {"Headphones - Output 1", "Headphones - Output 2"};
        }
        settings.select(args[2] == "settings-scan" ? 1 : args[2] == "settings-loopback" ? 2 : args[2] == "settings-author" ? 3 : args[2] == "settings-github" ? 4 : 0);
        juce::File file(args[1].unquoted()); file.getParentDirectory().createDirectory(); auto stream = file.createOutputStream();
        if (stream) { stream->setPosition(0); stream->truncate(); juce::PNGImageFormat().writeImageToStream(settings.createComponentSnapshot(settings.getLocalBounds()), *stream); }
        settings.setLookAndFeel(nullptr); setApplicationReturnValue(stream ? 0 : 1); quit(); return;
      }
      auto view = std::make_unique<MixerView>(true); auto* mixer = &view->mixer;
      if (args.size() > 2 && (args[2] == "dense" || args[2] == "small" || args[2] == "navigation")) {
        for (int i = 0; i < 12; ++i) { mixer->host.channels[i]->colour = juce::Colour::fromHSV(i/12.0f, 0.48f, 0.88f, 1.0f); mixer->host.channels[i]->colourAssigned = true; }
        for (int i = 0; i < 10; ++i) mixer->host.setRoute(0, i, {i+1, -1, 0, 1});
        mixer->refresh();
      }
      for (int i = 3; i < args.size() && i-3 < slotCount; ++i) {
        juce::OwnedArray<juce::PluginDescription> plugins; scanPluginFactory(juce::File(args[i].unquoted()), plugins);
        for (auto* p : plugins) if (isEffectCandidate(*p)) { mixer->host.loadPlugin(0, i-3, *p); break; }
      }
      if (args.size() > 2 && args[2] == "loopback") {
        mixer->host.setLoopbackMode(1, true); mixer->host.inputNames = {"Mic 1", "Mic 2", "Return 1", "Return 2"};
        mixer->host.outputNames = {"Output 1", "Output 2", "Output 3", "Output 4"};
        mixer->host.setBindings(1, 2, 3, 0, 1);
      }
      mixer->refresh();
      if (args.size() > 2 && args[2] == "resized") {
        for (auto& channel : mixer->host.channels) { channel->stripWidth = 260; channel->stripScale = 0.8f; }
        view->setSize(canvasWidth, 640); mixer->refresh();
      }
      if (args.size() > 2 && args[2] == "navigation") {
        for (auto* child : mixer->getChildren()) if (auto* v = dynamic_cast<HorizontalViewport*>(child))
          for (auto* strip : v->getViewedComponent()->getChildren()) if (auto* channel = dynamic_cast<ChannelStrip*>(strip)) {
            for (auto* part : channel->getChildren()) if (auto* navigation = dynamic_cast<NavigationViewport*>(part)) navigation->reveal(true);
            break;
          }
      }
      const auto mode = args.size() > 2 ? args[2] : juce::String();
      view->setSize(mode == "small" ? 886 : mode == "large" ? 1920 : canvasWidth, mode == "small" ? 560 : mode == "large" ? 1080 : mode == "resized" ? 640 : canvasHeight);
      if (args.size() > 2 && args[2] == "end") mixer->scrollToEnd();
      juce::File file(args[1].unquoted()); file.getParentDirectory().createDirectory();
      auto stream = file.createOutputStream();
      if (stream) { stream->setPosition(0); stream->truncate(); juce::PNGImageFormat().writeImageToStream(view->createComponentSnapshot(view->getLocalBounds()), *stream); }
      setApplicationReturnValue(stream ? 0 : 1); quit(); return;
    }
    window = std::make_unique<MainWindow>();
  }
  void shutdown() override { stopTimer(); window.reset(); scanVerifier.reset(); }
  void anotherInstanceStarted(const juce::String&) override { if (window) window->show(); }
private: std::unique_ptr<MainWindow> window;
  std::unique_ptr<Host> scanVerifier;
  juce::File scanReport;
  double scanStart = 0;
  void timerCallback() override {
    if (!scanVerifier || !scanVerifier->scanJob->done) return;
    auto job = scanVerifier->scanJob;
    { const juce::ScopedLock guard(job->lock); if (!job->ready.isEmpty()) return; }
    juce::XmlElement root("SCAN_VERIFICATION");
    root.setAttribute("elapsedMs", juce::Time::getMillisecondCounterHiRes()-scanStart);
    root.setAttribute("files", job->total.load()); root.setAttribute("completed", job->completed.load());
    root.setAttribute("failed", job->failed.load()); root.setAttribute("cached", job->cached.load());
    int effects = 0;
    for (const auto& d : scanVerifier->catalog.getTypes()) { root.addChildElement(d.createXml().release()); if (isEffectCandidate(d)) ++effects; }
    root.setAttribute("effects", effects);
    const int uniqueEffects = preferredEffectTypes(scanVerifier->catalog.getTypes()).size();
    root.setAttribute("uniqueEffects", uniqueEffects); root.setAttribute("duplicateVersions", effects-uniqueEffects);
    { const juce::ScopedLock guard(job->lock); root.setAttribute("failures", job->failures); }
    const bool passed = effects > 0 && job->completed == job->total;
    root.setAttribute("passed", passed); scanReport.getParentDirectory().createDirectory(); root.writeTo(scanReport);
    stopTimer(); setApplicationReturnValue(passed ? 0 : 1); quit();
  }
};
}
START_JUCE_APPLICATION(mini::Application)
