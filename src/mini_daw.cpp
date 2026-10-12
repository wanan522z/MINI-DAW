#include "mini_daw_host.h"
#include "mini_daw_verify.h"
#include <juce_gui_extra/juce_gui_extra.h>
#include <windows.h>
#include <map>

namespace mini {
const juce::Colour bg{0xff10141c}, panel{0xff1b2230}, ink{0xffd8e1ef}, muted{0xff8291aa};
const juce::Colour blue{0xff489ada}, red{0xffd97780}, green{0xff64c4a4};
class Mixer;

class MeterFader final : public juce::Component {
public:
  std::function<void(float)> changed;
  explicit MeterFader(Channel& c) : channel(c) {
    setMouseCursor(juce::MouseCursor::UpDownResizeCursor); setWantsKeyboardFocus(true);
  }
  void paint(juce::Graphics& g) override {
    auto rail = getLocalBounds().toFloat().reduced(7, 10);
    g.setColour(juce::Colour(0xff090e16)); g.fillRoundedRectangle(rail, 5);
    for (int side = 0; side < 2; ++side) {
      const float peak = channel.peak[side].load();
      float position = dbToPosition(peak > 0 ? 20 * std::log10(peak) : -INFINITY);
      g.setColour(peak >= 1 ? red : green);
      auto bar = rail.withWidth(rail.getWidth()/2-1).translated(side*(rail.getWidth()/2+1), 0);
      g.fillRoundedRectangle(bar.withTop(bar.getBottom()-bar.getHeight()*position), 3);
    }
    g.setColour(juce::Colours::white.withAlpha(0.15f));
    g.drawHorizontalLine(static_cast<int>(rail.getBottom()-rail.getHeight()*dbToPosition(0)), rail.getX(), rail.getRight());
    float y = rail.getBottom() - rail.getHeight()*channel.fader.load();
    g.setColour(juce::Colour(0xffb5c8dd)); g.fillRoundedRectangle(2, y-7, static_cast<float>(getWidth()-4), 14, 3);
    g.setColour(panel); g.drawHorizontalLine(static_cast<int>(y), 5, static_cast<float>(getWidth()-5));
  }
  void mouseDown(const juce::MouseEvent& e) override { grabKeyboardFocus(); update(e.position.y); }
  void mouseDrag(const juce::MouseEvent& e) override { update(e.position.y); }
  void mouseDoubleClick(const juce::MouseEvent&) override { set(dbToPosition(0)); }
  void mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails& w) override { set(channel.fader.load()+w.deltaY*0.08f); }
  bool keyPressed(const juce::KeyPress& key) override {
    if (key == juce::KeyPress::upKey || key == juce::KeyPress::downKey) {
      set(channel.fader.load()+(key == juce::KeyPress::upKey ? 1 : -1)/84.0f); return true;
    }
    if (key == juce::KeyPress::homeKey) { set(dbToPosition(0)); return true; }
    return false;
  }
private:
  Channel& channel;
  void set(float value) { value = juce::jlimit(0.0f, 1.0f, value); channel.fader = value; if (changed) changed(value); repaint(); }
  void update(float y) { set(1-(y-10)/(getHeight()-20)); }
};

class PluginSlot final : public juce::Component, private juce::Timer {
public:
  std::function<void()> open, context;
  PluginSlot(Slot& s, int i) : slot(s), number(i) { setMouseCursor(juce::MouseCursor::PointingHandCursor); }
  void paint(juce::Graphics& g) override {
    g.setColour(slot.plugin ? (slot.enabled ? juce::Colour(0xff203d58) : juce::Colour(0xff252c38)) : juce::Colour(0xff182b3e));
    g.fillRoundedRectangle(getLocalBounds().toFloat().reduced(0.5f), 4);
    g.setColour(blue.withAlpha(slot.enabled ? 0.9f : 0.35f)); g.drawRoundedRectangle(getLocalBounds().toFloat().reduced(0.5f), 4, 1);
    g.setFont(12); g.setColour(slot.enabled && slot.plugin ? green : muted); g.fillEllipse(7, 9, 5, 5);
    g.setColour(slot.plugin ? ink : muted);
    g.drawText(juce::String(number+1).paddedLeft('0', 2)+"  "+(slot.plugin ? slot.description.name : utf8("空槽 · 双击添加")), 18, 0, getWidth()-24, getHeight(), juce::Justification::centredLeft, true);
  }
  void mouseDown(const juce::MouseEvent& e) override {
    if (e.mods.isPopupMenu()) { stopTimer(); if (context) context(); }
    else if (e.getNumberOfClicks() >= 2) { stopTimer(); if (open) open(); }
    else startTimer(static_cast<int>(GetDoubleClickTime())+30);
  }
private:
  Slot& slot; int number;
  void timerCallback() override { stopTimer(); if (slot.plugin) slot.enabled = !slot.enabled.load(); repaint(); }
};

class SendKnob final : public juce::Slider {
public:
  std::function<void()> choose;
  SendKnob() : Slider(RotaryVerticalDrag, TextBoxBelow) {
    setRange(-72, 12, 0.1); setValue(-72, juce::dontSendNotification); setTextBoxStyle(TextBoxBelow, false, 78, 18);
    setColour(rotarySliderFillColourId, green); setColour(textBoxOutlineColourId, juce::Colours::transparentBlack);
  }
  juce::String getTextFromValue(double v) override { return v <= -72 ? juce::String::fromUTF8("−∞") : juce::String(v, 1)+" dB"; }
  double getValueFromText(const juce::String& s) override { return s.contains(juce::String::fromUTF8("∞")) ? -72 : s.getDoubleValue(); }
  void mouseDown(const juce::MouseEvent& e) override { if (e.mods.isPopupMenu()) { if (choose) choose(); } else Slider::mouseDown(e); }
};

class EditorWindow final : public juce::DocumentWindow {
public:
  EditorWindow(juce::AudioPluginInstance& plugin) : DocumentWindow(plugin.getName(), panel, closeButton) {
    setUsingNativeTitleBar(true); auto* editor = plugin.createEditorIfNeeded();
    if (!editor) editor = new juce::GenericAudioProcessorEditor(plugin);
    setContentOwned(editor, true); setResizable(editor->isResizable(), false);
    centreWithSize(editor->getWidth(), editor->getHeight()); setVisible(true);
  }
  void closeButtonPressed() override { setVisible(false); }
};

class PluginPicker final : public juce::Component, public juce::ListBoxModel, private juce::Timer {
public:
  PluginPicker(Host& h, std::function<void(const juce::PluginDescription&)> selected) : host(h), picked(std::move(selected)) {
    addAndMakeVisible(list); list.setModel(this); list.setRowHeight(36); list.setColour(juce::ListBox::backgroundColourId, bg);
    for (auto* b : {&scan, &folder, &load}) addAndMakeVisible(*b);
    addAndMakeVisible(search); addAndMakeVisible(info);
    search.setTextToShowWhenEmpty(utf8("搜索效果插件 / 厂商"), muted); search.onTextChange = [this] { refresh(); };
    scan.onClick = [this] { beginScan(host.formats.getFormat(0)->getDefaultLocationsToSearch()); };
    folder.onClick = [this] {
      chooser = std::make_unique<juce::FileChooser>(utf8("选择 VST3 插件目录"));
      chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
        [safe = juce::Component::SafePointer<PluginPicker>(this)](const juce::FileChooser& fc) {
          if (safe && fc.getResult().isDirectory()) safe->beginScan(juce::FileSearchPath(fc.getResult().getFullPathName()));
        });
    };
    load.onClick = [this] { select(list.getSelectedRow()); };
    info.setText(utf8("仅显示有音频输入、输出的 VST3 效果插件"), juce::dontSendNotification);
    refresh(); setSize(660, 540);
    if (host.catalog.getNumTypes() == 0) beginScan(host.formats.getFormat(0)->getDefaultLocationsToSearch());
  }
  ~PluginPicker() override { stopTimer(); }
  void resized() override {
    search.setBounds(16, 16, getWidth()-32, 30); list.setBounds(16, 58, getWidth()-32, getHeight()-156);
    info.setBounds(16, getHeight()-88, getWidth()-32, 30);
    scan.setBounds(16, getHeight()-48, 148, 32); folder.setBounds(178, getHeight()-48, 148, 32); load.setBounds(getWidth()-164, getHeight()-48, 148, 32);
  }
  void paint(juce::Graphics& g) override { g.fillAll(bg); }
  int getNumRows() override { return descriptions.size(); }
  void paintListBoxItem(int row, juce::Graphics& g, int width, int height, bool selected) override {
    if (!juce::isPositiveAndBelow(row, descriptions.size())) return;
    if (selected) g.fillAll(blue.withAlpha(0.22f));
    g.setColour(ink); g.setFont(15); g.drawText(descriptions[row].name, 10, 0, width/2, height, juce::Justification::centredLeft, true);
    g.setColour(muted); g.setFont(12); g.drawText(descriptions[row].manufacturerName+utf8(" · ")+descriptions[row].category, width/2, 0, width/2-10, height, juce::Justification::centredRight, true);
  }
  void listBoxItemDoubleClicked(int row, const juce::MouseEvent&) override { select(row); }
  void cancelScan() { stopTimer(); scanner.reset(); }
private:
  Host& host; std::function<void(const juce::PluginDescription&)> picked;
  juce::ListBox list; juce::TextEditor search; juce::Label info;
  juce::TextButton scan{utf8("扫描默认目录")}, folder{utf8("添加插件目录")}, load{utf8("加载选中插件")};
  juce::Array<juce::PluginDescription> descriptions;
  std::unique_ptr<juce::PluginDirectoryScanner> scanner; std::unique_ptr<juce::FileChooser> chooser;
  void select(int row) { if (juce::isPositiveAndBelow(row, descriptions.size())) picked(descriptions[row]); }
  void refresh() {
    descriptions.clear();
    for (const auto& d : host.catalog.getTypes())
      if (!d.isInstrument && d.numInputChannels > 0 && d.numOutputChannels > 0 &&
          (search.getText().isEmpty() || (d.name+" "+d.manufacturerName).containsIgnoreCase(search.getText()))) descriptions.add(d);
    list.updateContent(); list.repaint();
  }
  void beginScan(const juce::FileSearchPath& paths) {
    host.catalogFile().getParentDirectory().createDirectory();
    scanner = std::make_unique<juce::PluginDirectoryScanner>(host.catalog, *host.formats.getFormat(0), paths, true, host.catalogFile().getSiblingFile("scan-crashes.txt"));
    info.setText(utf8("正在扫描 VST3 效果…"), juce::dontSendNotification); startTimer(100);
  }
  void timerCallback() override {
    juce::String name;
    if (!scanner || !scanner->scanNextFile(true, name)) {
      const int failed = scanner ? scanner->getFailedFiles().size() : 0;
      cancelScan(); refresh(); host.save(); info.setText(utf8("扫描完成 · 效果 ")+juce::String(descriptions.size())+utf8(" · 失败 ")+juce::String(failed), juce::dontSendNotification);
    } else { info.setText(utf8("扫描：")+name, juce::dontSendNotification); refresh(); }
  }
};
class PickerWindow final : public juce::DocumentWindow {
public:
  PickerWindow(Host& h, std::function<void(const juce::PluginDescription&)> picked) : DocumentWindow(utf8("VST3 效果插件列表"), panel, closeButton) {
    setUsingNativeTitleBar(true); setContentOwned(new PluginPicker(h, std::move(picked)), true); setResizable(true, false); centreWithSize(660, 540); setVisible(true);
  }
  void closeButtonPressed() override { static_cast<PluginPicker*>(getContentComponent())->cancelScan(); setVisible(false); }
};

class ChannelStrip final : public juce::Component, private juce::Timer {
public:
  ChannelStrip(Host&, Mixer&, int);
  void paint(juce::Graphics&) override;
  void resized() override;
  void sync();
  int requiredHeight() const { return 566+((static_cast<int>(knobs.size())+1)/2)*100; }
private:
  Host& host; Mixer& mixer; int index;
  juce::Label title; juce::ComboBox input, output;
  juce::TextButton mainReceive, mute{"MUTE"};
  MeterFader fader;
  std::array<std::unique_ptr<PluginSlot>, slotCount> slots;
  std::vector<std::unique_ptr<SendKnob>> knobs;
  bool updating = false;
  void chooseSend(int); void chooseReceive(bool);
  void timerCallback() override { fader.repaint(); repaint(); for (auto& s : slots) s->repaint(); }
  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ChannelStrip)
};

class Mixer final : public juce::Component, private juce::Timer {
public:
  Host host;
  std::function<void()> hide;
  explicit Mixer(bool offline = false) : host(!offline) {
    look.setColour(juce::ResizableWindow::backgroundColourId, bg);
    look.setColour(juce::TextButton::buttonColourId, panel); look.setColour(juce::TextButton::textColourOffId, ink);
    look.setColour(juce::ComboBox::backgroundColourId, juce::Colour(0xff080d14)); look.setColour(juce::ComboBox::textColourId, ink);
    look.setColour(juce::PopupMenu::backgroundColourId, panel); look.setColour(juce::PopupMenu::textColourId, ink);
    look.setColour(juce::Slider::textBoxTextColourId, ink); look.setColour(juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
    setLookAndFeel(&look);
    addAndMakeVisible(viewport); viewport.setViewedComponent(&array, false); viewport.setScrollBarsShown(true, true);
    for (auto* c : {&audio, &settings, &save, &background}) addAndMakeVisible(*c);
    array.addAndMakeVisible(add); add.setButtonText("+"); add.setTooltip(utf8("添加通道"));
    addAndMakeVisible(driver); addAndMakeVisible(status);
    driver.addItemList(host.drivers(), 1); driver.setText(host.driverName, juce::dontSendNotification);
    driver.onChange = [this] { host.driverName = driver.getText(); host.start(); refresh(); };
    add.onClick = [this] { if (host.addChannel()) { refresh(); viewport.setViewPosition(std::max(0, array.getWidth()-viewport.getWidth()), viewport.getViewPositionY()); } };
    audio.onClick = [this] { if (host.isRunning()) host.stop(); else host.start(); refresh(); };
    settings.onClick = [this] {
      auto file = juce::File::getSpecialLocation(juce::File::currentExecutableFile).getSiblingFile("AggregateAudioConfig.exe");
      if (file.existsAsFile()) file.startAsProcess(); else host.controlPanel();
    };
    save.onClick = [this] { host.save(); status.setText(utf8("工程已保存"), juce::dontSendNotification); };
    background.onClick = [this] { host.save(); if (hide) hide(); };
    host.changed = [safe = juce::Component::SafePointer<Mixer>(this)] { if (safe) safe->refresh(); };
    refresh(); setSize(1328, 840); timerCallback(); startTimerHz(15);
    if (!offline) juce::MessageManager::callAsync([safe = juce::Component::SafePointer<Mixer>(this)] { if (safe) { safe->host.start(); safe->refresh(); } });
  }
  ~Mixer() override { stopTimer(); host.changed = {}; host.stop(); editors.clear(); picker.reset(); host.save(); setLookAndFeel(nullptr); }
  void openPlugin(int channel, int slot) {
    auto& s = host.channels[channel]->slots[slot]; int key = channel*slotCount+slot;
    if (!s.plugin) {
      picker = std::make_unique<PickerWindow>(host, [safe = juce::Component::SafePointer<Mixer>(this), channel, slot](const juce::PluginDescription& desc) {
        if (!safe) return;
        if (safe->host.loadPlugin(channel, slot, desc)) { safe->picker->setVisible(false); safe->refresh(); safe->openPlugin(channel, slot); }
        else safe->showError();
      }); return;
    }
    if (!editors.contains(key)) editors[key] = std::make_unique<EditorWindow>(*s.plugin);
    editors[key]->setVisible(true); editors[key]->toFront(true);
  }
  void pluginContext(int channel, int slot) {
    auto& s = host.channels[channel]->slots[slot]; juce::PopupMenu menu;
    menu.addItem(1, s.enabled ? utf8("关闭插件") : utf8("启用插件"), s.plugin != nullptr); menu.addItem(2, utf8("打开界面 / 选择效果")); menu.addItem(3, utf8("卸载插件"), s.plugin != nullptr);
    menu.showMenuAsync(juce::PopupMenu::Options(), [safe = juce::Component::SafePointer<Mixer>(this), channel, slot](int result) {
      if (!safe) return; auto& s = safe->host.channels[channel]->slots[slot];
      if (result == 1) s.enabled = !s.enabled.load(); if (result == 2) safe->openPlugin(channel, slot);
      if (result == 3) { safe->editors.erase(channel*slotCount+slot); safe->host.removePlugin(channel, slot); safe->refresh(); }
    });
  }
  void refresh() {
    while (strips.size() < host.channels.size()) { auto s = std::make_unique<ChannelStrip>(host, *this, static_cast<int>(strips.size())); array.addAndMakeVisible(*s); strips.push_back(std::move(s)); }
    for (auto& s : strips) s->sync(); layoutStrips(); repaint();
  }
  void layoutStrips() {
    int height = 680; for (const auto& s : strips) height = std::max(height, s->requiredHeight());
    array.setSize(std::max(viewport.getWidth()-16, static_cast<int>(strips.size())*320+88), height+16);
    for (int i = 0; i < static_cast<int>(strips.size()); ++i) strips[i]->setBounds(8+i*320, 8, 308, height);
    add.setBounds(8+static_cast<int>(strips.size())*320, 8, 64, height);
    add.setEnabled(strips.size() < maximumChannels);
  }
  void scrollToEnd() { viewport.setViewPosition(std::max(0, array.getWidth()-viewport.getWidth()), 0); }
  void hideEditors() { for (auto& entry : editors) entry.second->setVisible(false); if (picker) picker->closeButtonPressed(); host.save(); }
  void showError() { juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "Mini DAW", host.error); }
  void resized() override {
    driver.setBounds(242, 25, 220, 28); audio.setBounds(478, 25, 102, 28); settings.setBounds(592, 25, 102, 28);
    save.setBounds(getWidth()-312, 25, 108, 28); background.setBounds(getWidth()-192, 25, 168, 28);
    viewport.setBounds(12, 84, getWidth()-24, getHeight()-132); status.setBounds(22, getHeight()-36, getWidth()-44, 24); layoutStrips();
  }
  void paint(juce::Graphics& g) override {
    g.fillAll(bg); g.setColour(ink); g.setFont(25); g.drawText("MINI DAW", 24, 16, 202, 30, juce::Justification::centredLeft);
    g.setFont(11); g.setColour(muted); g.drawText("ASIO  /  VST3 EFFECT MIXER", 24, 49, 200, 17, juce::Justification::centredLeft);
    g.setColour(green); g.drawText(juce::String(static_cast<int>(host.channels.size()))+" CHANNELS", 730, 25, 130, 28, juce::Justification::centredLeft);
  }
private:
  juce::LookAndFeel_V4 look; juce::Viewport viewport; juce::Component array;
  std::vector<std::unique_ptr<ChannelStrip>> strips;
  std::map<int, std::unique_ptr<EditorWindow>> editors; std::unique_ptr<PickerWindow> picker;
  juce::ComboBox driver;
  juce::TextButton add{utf8("+ 添加通道")}, audio{utf8("启动 ASIO")}, settings{utf8("驱动设置")}, save{utf8("保存工程")}, background{utf8("后台驻留")}; juce::Label status;
  void timerCallback() override {
    audio.setButtonText(host.isRunning() ? utf8("停止 ASIO") : utf8("启动 ASIO"));
    if (host.error.isNotEmpty()) status.setText(host.error, juce::dontSendNotification);
    else status.setText((host.isRunning() ? utf8("● ASIO 运行中") : utf8("○ ASIO 已停止"))+juce::String(utf8(" · "))+host.driverName+utf8(" · ")+juce::String(host.sampleRate()/1000, 1)+" kHz / "+juce::String(host.blockSize())+utf8(" samples · 空槽双击加载 / 单击开关 / 旋钮右键路由"), juce::dontSendNotification);
  }
  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(Mixer)
};

ChannelStrip::ChannelStrip(Host& h, Mixer& m, int i) : host(h), mixer(m), index(i), fader(*h.channels[i]) {
  for (juce::Component* c : std::initializer_list<juce::Component*>{&title, &input, &output, &mainReceive, &mute, &fader}) addAndMakeVisible(*c);
  title.setText(host.channels[i]->name, juce::dontSendNotification); title.setEditable(false, true, false); title.setColour(juce::Label::textColourId, ink);
  title.onTextChange = [this] { const juce::ScopedLock guard(host.lock); host.channels[index]->name = title.getText(); };
  input.setColour(juce::ComboBox::outlineColourId, juce::Colours::black); output.setColour(juce::ComboBox::outlineColourId, red);
  input.onChange = [this] { if (updating) return; const juce::ScopedLock guard(host.lock); int id = input.getSelectedId(); auto& c = *host.channels[index];
    if (id <= 1) c.input = c.inputRight = -1; else if (id >= 1001) { c.input = id-1001; c.inputRight = c.input+1; } else c.input = c.inputRight = id-2;
  };
  output.onChange = [this] { if (updating) return; const juce::ScopedLock guard(host.lock); int id = output.getSelectedId(); auto& c = *host.channels[index];
    if (id <= 1) c.output = c.outputRight = -1; else if (id >= 1001) { c.output = id-1001; c.outputRight = c.output+1; } else { c.output = id-2; c.outputRight = -1; }
  };
  mute.setClickingTogglesState(true); mute.setColour(juce::TextButton::buttonOnColourId, red.withAlpha(0.45f)); mute.onClick = [this] { host.channels[index]->mute = mute.getToggleState(); };
  mainReceive.onClick = [this] { chooseReceive(true); };
  for (int s = 0; s < slotCount; ++s) { slots[s] = std::make_unique<PluginSlot>(host.channels[index]->slots[s], s); addAndMakeVisible(*slots[s]); slots[s]->open = [this, s] { mixer.openPlugin(index, s); }; slots[s]->context = [this, s] { mixer.pluginContext(index, s); }; }
  startTimerHz(30); sync();
}
void ChannelStrip::sync() {
  updating = true;
  auto fill = [](juce::ComboBox& box, const juce::StringArray& names, int a, int b, bool out) {
    box.clear(juce::dontSendNotification); box.addItem(out ? utf8("直接输出：无") : utf8("输入：无"), 1);
    for (int i = 0; i < names.size(); ++i) box.addItem(juce::String(i+1)+utf8(" · ")+names[i], i+2);
    if (names.size() >= 2) { box.addSeparator(); for (int i = 0; i+1 < names.size(); i += 2) box.addItem(utf8("立体声 ")+juce::String(i+1)+"/"+juce::String(i+2)+utf8(" · ")+names[i], 1001+i); }
    box.setSelectedId(a < 0 ? 1 : (b == a+1 ? 1001+a : a+2), juce::dontSendNotification);
  };
  auto& c = *host.channels[index]; fill(input, host.inputNames, c.input, c.inputRight, false); fill(output, host.outputNames, c.output, c.outputRight, true);
  mute.setToggleState(c.mute.load(), juce::dontSendNotification);
  const int count = sendControlCount(static_cast<int>(c.sends.size()));
  if (static_cast<int>(knobs.size()) != count) {
    knobs.clear();
    for (int i = 0; i < count; ++i) {
      auto knob = std::make_unique<SendKnob>(); addAndMakeVisible(*knob); knob->choose = [this, i] { chooseSend(i); };
      knob->onValueChange = [this, i] { if (updating) return; const juce::ScopedLock guard(host.lock); auto& sends = host.channels[index]->sends;
        if (i < static_cast<int>(sends.size())) sends[i].gain = knobs[i]->getValue() <= -72 ? 0 : juce::Decibels::decibelsToGain(static_cast<float>(knobs[i]->getValue()));
      }; knobs.push_back(std::move(knob));
    }
  }
  for (int i = 0; i < count; ++i) { knobs[i]->setValue(i < static_cast<int>(c.sends.size()) ? juce::Decibels::gainToDecibels(c.sends[i].gain, -72.0f) : -72, juce::dontSendNotification); knobs[i]->setTooltip(utf8("右键选择发送通道和目标总线；拖动设置发送量")); }
  juce::StringArray mains, sides;
  for (int source = 0; source < static_cast<int>(host.channels.size()); ++source) for (const auto& r : host.channels[source]->sends) if (r.target == index) {
    if (r.slot < 0) mains.add(host.channels[source]->name); else sides.add("SC"+juce::String(r.bus)+" "+host.channels[source]->name);
  }
  auto all = mains; all.addArray(sides);
  mainReceive.setButtonText(all.isEmpty() ? utf8("接收 ▾") : (sides.isEmpty() ? mains : sides).joinIntoString(", ")+utf8(" ▾"));
  mainReceive.setTooltip(utf8("主输入：")+mains.joinIntoString(", ")+utf8("\n侧链：")+sides.joinIntoString(", "));
  updating = false; resized(); repaint();
}
void ChannelStrip::resized() {
  title.setBounds(12, 12, getWidth()-24, 28); input.setBounds(110, 54, 186, 28);
  mainReceive.setBounds(8, 112, 96, 26);
  fader.setBounds(42, 158, 54, 356);
  for (int s = 0; s < slotCount; ++s) slots[s]->setBounds(110, 99+s*25, 186, 23);
  output.setBounds(110, 414, 186, 28); mute.setBounds(8, 553, 94, 26);
  for (int i = 0; i < static_cast<int>(knobs.size()); ++i) knobs[i]->setBounds(110+(i%2)*96, 490+(i/2)*100, 90, 74);
}
void ChannelStrip::paint(juce::Graphics& g) {
  g.setColour(panel); g.fillRoundedRectangle(getLocalBounds().toFloat(), 8); g.setColour(blue); g.fillRoundedRectangle(12, 4, static_cast<float>(getWidth()-24), 2, 1);
  g.setFont(11); g.setColour(muted); g.drawText("LEVEL / dB", 34, 146, 70, 14, juce::Justification::centred);
  for (int db : {12, 6, 0, -6, -12, -24, -36, -48, -60}) {
    int y = 168+static_cast<int>((1-dbToPosition(static_cast<float>(db)))*336); g.setColour(db == 0 ? ink : muted); g.drawText(juce::String(db), 2, y-7, 28, 14, juce::Justification::centredRight);
  }
  g.drawText(juce::String::fromUTF8("−∞"), 0, 504, 32, 16, juce::Justification::centredRight);
  float value = positionToDb(host.channels[index]->fader.load()); g.setColour(ink); g.drawText(std::isfinite(value) ? juce::String(value, 1)+" dB" : juce::String::fromUTF8("−∞"), 36, 522, 64, 20, juce::Justification::centred);
  g.setColour(muted); g.drawText("INPUT DEVICE", 110, 38, 180, 14, juce::Justification::centredLeft);
  g.setColour(blue); g.drawText(utf8("12 × VST3 INSERT"), 110, 83, 186, 14, juce::Justification::centredLeft);
  g.setColour(red); g.drawText("DIRECT OUTPUT", 110, 399, 186, 14, juce::Justification::centredLeft);
  g.setColour(green); g.drawText("SEND / RECEIVE", 110, 466, 186, 16, juce::Justification::centredLeft);
  const auto& sends = host.channels[index]->sends;
  for (int i = 0; i < static_cast<int>(knobs.size()); ++i) {
    juce::String label = utf8("右键添加发送");
    if (i < static_cast<int>(sends.size()) && sends[i].target >= 0) { const auto& r = sends[i]; label = host.channels[r.target]->name; if (r.slot >= 0) label += utf8(" · ")+juce::String(r.slot+1)+"/SC"+juce::String(r.bus); }
    g.setColour(green); g.drawText(label, 110+(i%2)*96, 565+(i/2)*100, 90, 22, juce::Justification::centred, true);
  }
}
void ChannelStrip::chooseSend(int sendIndex) {
  juce::PopupMenu menu; std::vector<Route> options;
  for (int target = 0; target < static_cast<int>(host.channels.size()); ++target) {
    if (target == index) continue; auto& c = *host.channels[target]; juce::PopupMenu buses;
    options.push_back({target, -1, 0, 0}); buses.addItem(static_cast<int>(options.size()), utf8("主输入"));
    for (int s = 0; s < slotCount; ++s) if (auto* plugin = c.slots[s].plugin.get()) for (int bus = 1; bus < plugin->getBusCount(true); ++bus) {
      auto* b = plugin->getBus(true, bus); if (!b->isEnabled()) continue; options.push_back({target, s, bus, 0});
      buses.addItem(static_cast<int>(options.size()), utf8("槽 ")+juce::String(s+1)+utf8(" · SC ")+juce::String(bus)+utf8(" · ")+b->getName());
    }
    menu.addSubMenu(c.name, buses);
  }
  menu.addSeparator(); menu.addItem(10000, utf8("移除发送"), sendIndex < static_cast<int>(host.channels[index]->sends.size()));
  menu.showMenuAsync(juce::PopupMenu::Options(), [safe = juce::Component::SafePointer<ChannelStrip>(this), options, sendIndex](int result) {
    if (!safe || result == 0) return;
    if (result == 10000) safe->host.removeRoute(safe->index, sendIndex);
    else if (juce::isPositiveAndBelow(result-1, static_cast<int>(options.size()))) {
      auto r = options[result-1]; auto& sends = safe->host.channels[safe->index]->sends; r.gain = sendIndex < static_cast<int>(sends.size()) ? sends[sendIndex].gain : 0;
      if (!safe->host.setRoute(safe->index, sendIndex, r)) safe->mixer.showError();
    }
    safe->mixer.refresh();
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
      list.addItem(static_cast<int>(options.size()), (slot < 0 ? juce::String("IN") : "SC "+juce::String(bus))+utf8(" · ")+c.name, true, found >= 0);
    }
    return list;
  };
  menu.addSubMenu(utf8("主输入接收"), sources(-1, 0));
  if (sidechain) for (int s = 0; s < slotCount; ++s) if (auto* plugin = host.channels[index]->slots[s].plugin.get()) for (int bus = 1; bus < plugin->getBusCount(true); ++bus)
    if (plugin->getBus(true, bus)->isEnabled()) menu.addSubMenu(utf8("槽 ")+juce::String(s+1)+utf8(" · SC ")+juce::String(bus)+utf8(" · ")+plugin->getBus(true, bus)->getName(), sources(s, bus));
  if (!menu.getNumItems()) menu.addItem(10000, utf8("已加载插件未提供可用侧链输入总线"), false);
  menu.showMenuAsync(juce::PopupMenu::Options(), [safe = juce::Component::SafePointer<ChannelStrip>(this), options](int result) {
    if (!safe || !juce::isPositiveAndBelow(result-1, static_cast<int>(options.size()))) return; const auto& o = options[result-1];
    if (o.remove) safe->host.removeRoute(o.source, o.send); else if (!safe->host.setRoute(o.source, o.send, o.route)) safe->mixer.showError();
    safe->mixer.refresh();
  });
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
  MainWindow() : DocumentWindow(utf8("Mini DAW · ASIO / VST3 调音台"), bg, allButtons) {
    setUsingNativeTitleBar(true); auto* mixer = new Mixer; mixer->hide = [this] { hideToTray(); }; setContentOwned(mixer, true);
    setResizable(true, false); setResizeLimits(1200, 560, 4096, 2160); centreWithSize(1328, 840); setVisible(true); tray = std::make_unique<Tray>(*this);
  }
  void show() { setVisible(true); setMinimised(false); toFront(true); }
  void hideToTray() { if (tray) { static_cast<Mixer*>(getContentComponent())->hideEditors(); setVisible(false); } }
  void closeButtonPressed() override { hideToTray(); }
private: std::unique_ptr<Tray> tray;
};
Tray::Tray(MainWindow& w) : window(w) {
  juce::Image icon(juce::Image::ARGB, 32, 32, true); juce::Graphics g(icon); g.setColour(panel); g.fillRoundedRectangle(0, 0, 32, 32, 7); g.setColour(green);
  for (int i = 0; i < 3; ++i) { g.fillRoundedRectangle(static_cast<float>(7+i*8), 6, 3, 20, 1); g.fillRoundedRectangle(static_cast<float>(4+i*8), static_cast<float>(10+i*3), 9, 4, 1); }
  setIconImage(icon, icon); setIconTooltip(utf8("Mini DAW · 双击打开 / 右键退出"));
}
void Tray::mouseDoubleClick(const juce::MouseEvent&) { window.show(); }
void Tray::mouseDown(const juce::MouseEvent& e) {
  if (!e.mods.isPopupMenu()) return; juce::PopupMenu menu; menu.addItem(1, utf8("打开调音台")); menu.addItem(2, utf8("保存并退出"));
  menu.showMenuAsync(juce::PopupMenu::Options(), [safe = juce::Component::SafePointer<Tray>(this)](int result) { if (!safe) return; if (result == 1) safe->window.show(); if (result == 2) juce::JUCEApplication::getInstance()->systemRequestedQuit(); });
}
class Application final : public juce::JUCEApplication {
public:
  const juce::String getApplicationName() override { return "MiniDAW"; }
  const juce::String getApplicationVersion() override { return "0.2.0"; }
  bool moreThanOneInstanceAllowed() override {
    // Offline checks must execute even while the normal mixer is running.
    const auto command = getCommandLineParameters().trimStart();
    return command.startsWith("--verify-host ") || command.startsWith("--verify-asio ")
        || command.startsWith("--snapshot ");
  }
  void initialise(const juce::String& command) override {
    auto args = juce::StringArray::fromTokens(command, true); args.removeEmptyStrings();
    if (args.size() >= 2 && args[0] == "--verify-asio") {
      Host host(false); juce::XmlElement report("ASIO_VERIFICATION");
      const bool started = host.start(); juce::Thread::sleep(400);
      const bool passed = started && host.callbackCount.load() > 0;
      report.setAttribute("driver", host.driverName); report.setAttribute("started", started); report.setAttribute("passed", passed);
      report.setAttribute("callbacks", juce::String(host.callbackCount.load())); report.setAttribute("sampleRate", host.sampleRate()); report.setAttribute("blockSize", host.blockSize());
      report.setAttribute("inputs", host.inputNames.joinIntoString(" | ")); report.setAttribute("outputs", host.outputNames.joinIntoString(" | ")); report.setAttribute("error", host.error);
      host.stop(); juce::File(args[1].unquoted()).getParentDirectory().createDirectory(); report.writeTo(juce::File(args[1].unquoted()));
      setApplicationReturnValue(passed ? 0 : 1); quit(); return;
    }
    if (args.size() >= 2 && args[0] == "--verify-host") {
      setApplicationReturnValue(verifyHost(args.size() > 2 ? args[2].unquoted() : juce::String(), juce::File(args[1].unquoted()))); quit(); return;
    }
    if (args.size() >= 2 && args[0] == "--snapshot") {
      auto mixer = std::make_unique<Mixer>(true);
      mixer->setSize(1328, 840);
      if (args.size() > 2 && args[2] == "end") mixer->scrollToEnd();
      juce::File file(args[1].unquoted()); file.getParentDirectory().createDirectory();
      auto stream = file.createOutputStream();
      if (stream) { stream->setPosition(0); stream->truncate(); juce::PNGImageFormat().writeImageToStream(mixer->createComponentSnapshot(mixer->getLocalBounds()), *stream); }
      setApplicationReturnValue(stream ? 0 : 1); quit(); return;
    }
    window = std::make_unique<MainWindow>();
  }
  void shutdown() override { window.reset(); }
  void anotherInstanceStarted(const juce::String&) override { if (window) window->show(); }
private: std::unique_ptr<MainWindow> window;
};
}
START_JUCE_APPLICATION(mini::Application)
