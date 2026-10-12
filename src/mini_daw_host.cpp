#include "mini_daw_host.h"

namespace mini {
Host::Host(bool saveState) : persistent(saveState) {
  formats.addFormat(new juce::VST3PluginFormat());
  type.reset(juce::AudioIODeviceType::createAudioIODeviceType_ASIO());
  if (type) type->scanForDevices();
  if (persistent) {
    if (auto xml = juce::XmlDocument::parse(catalogFile())) catalog.recreateFromXml(*xml);
    restore();
  }
  if (channels.empty()) { for (int i = 0; i < 12; ++i) addChannel(); }
}
Host::~Host() { *alive = false; stop(); save(); }
juce::File Host::stateFile() const { return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory).getChildFile("MiniDAW/session-v2.xml"); }
juce::File Host::catalogFile() const { return stateFile().getSiblingFile("effects-vst3.xml"); }
juce::StringArray Host::drivers() { if (!type) return {}; type->scanForDevices(); return type->getDeviceNames(); }
bool Host::start() {
  stop(); error.clear();
  if (!type || !drivers().contains(driverName)) {
    error = utf8("未找到 ASIO 驱动：") + driverName + utf8("。请安装 Aggregate Audio 并在配置程序选择输入、输出声道。"); return false;
  }
  device.reset(type->createDevice(driverName, driverName));
  if (!device) { error = utf8("无法创建 ASIO 设备"); return false; }
  inputNames = device->getInputChannelNames(); outputNames = device->getOutputChannelNames();
  juce::BigInteger ins, outs; ins.setRange(0, inputNames.size(), true); outs.setRange(0, outputNames.size(), true);
  error = device->open(ins, outs, 0, 0);
  if (error.isNotEmpty()) { device.reset(); return false; }
  // Query names again: some ASIO drivers only publish bindings after open/init.
  inputNames = device->getInputChannelNames(); outputNames = device->getOutputChannelNames();
  if (needsDefaultInput && !inputNames.isEmpty()) { channels[0]->input = 0; channels[0]->inputRight = 0; needsDefaultInput = false; }
  device->start(this);
  if (!device->isPlaying()) { error = utf8("ASIO 无法启动"); return false; }
  if (changed) changed(); return true;
}
void Host::stop() {
  if (device) { device->stop(); device->close(); device.reset(); }
  for (auto& channel : channels) for (auto& peak : channel->peak) peak = 0;
}
bool Host::addChannel() {
  const juce::ScopedLock guard(lock);
  if (channels.size() >= maximumChannels) return false;
  auto channel = std::make_unique<Channel>(); channel->name = utf8("通道 ") + juce::String(static_cast<int>(channels.size()) + 1);
  channels.push_back(std::move(channel)); rebuildOrder(); return true;
}
void Host::rebuildOrder() {
  std::vector<std::vector<Route>> routes; for (const auto& channel : channels) routes.push_back(channel->sends);
  order = routingOrder(routes);
}
bool Host::setRoute(int source, int index, Route route) {
  const juce::ScopedLock guard(lock);
  if (source < 0 || source >= static_cast<int>(channels.size()) || route.target < 0 || route.target >= static_cast<int>(channels.size()) || index < 0) return false;
  auto& sends = channels[source]->sends;
  auto previous = sends;
  if (index >= static_cast<int>(sends.size())) sends.resize(index+1);
  sends[index] = route;
  rebuildOrder();
  if (order.empty()) { sends = std::move(previous); rebuildOrder(); error = utf8("该发送会形成音频反馈环路，未连接。"); return false; }
  return true;
}
void Host::removeRoute(int channel, int index) {
  const juce::ScopedLock guard(lock); auto& sends = channels[channel]->sends;
  if (juce::isPositiveAndBelow(index, static_cast<int>(sends.size()))) sends.erase(sends.begin() + index);
  rebuildOrder();
}
void Host::prepare(Slot& slot) {
  if (!slot.plugin) return;
  slot.plugin->releaseResources();
  slot.plugin->setRateAndBufferSizeDetails(rate, frames);
  slot.plugin->prepareToPlay(rate, frames);
  slot.work.setSize(std::max({2, slot.plugin->getTotalNumInputChannels(), slot.plugin->getTotalNumOutputChannels()}), 8192);
}
bool Host::loadPlugin(int channel, int index, const juce::PluginDescription& description, const juce::MemoryBlock* state) {
  if (description.isInstrument || description.numInputChannels == 0 || description.numOutputChannels == 0) { error = utf8("仅允许效果插件"); return false; }
  juce::String reason;
  auto plugin = formats.createPluginInstance(description, rate, frames, reason);
  if (!plugin) { error = utf8("加载插件失败：") + reason; return false; }
  if (plugin->getBusCount(true) == 0 || plugin->getBusCount(false) == 0) { error = utf8("插件没有音频效果输入/输出"); return false; }
  auto layout = plugin->getBusesLayout();
  // Prefer stereo mains. Extra input buses use the plug-in's own accepted layouts.
  layout.inputBuses.set(0, juce::AudioChannelSet::stereo()); layout.outputBuses.set(0, juce::AudioChannelSet::stereo());
  if (plugin->checkBusesLayoutSupported(layout)) plugin->setBusesLayout(layout);
  for (int bus = 1; bus < plugin->getBusCount(true); ++bus) {
    auto* b = plugin->getBus(true, bus);
    if (!b->isEnabled()) b->enable(true);
  }
  if (state && state->getSize()) plugin->setStateInformation(state->getData(), static_cast<int>(state->getSize()));
  const juce::ScopedLock guard(lock);
  auto& slot = channels[channel]->slots[index];
  if (slot.plugin) slot.plugin->releaseResources();
  slot.plugin = std::move(plugin); slot.description = description; slot.enabled = true;
  prepare(slot); return true;
}
void Host::removePlugin(int channel, int index) {
  const juce::ScopedLock guard(lock);
  auto& slot = channels[channel]->slots[index]; if (slot.plugin) slot.plugin->releaseResources(); slot.plugin.reset();
  for (auto& ch : channels) std::erase_if(ch->sends, [&](const Route& route) { return route.target == channel && route.slot == index; });
  rebuildOrder();
}
void Host::controlPanel() { if (device) device->showControlPanel(); }
void Host::audioDeviceAboutToStart(juce::AudioIODevice* d) {
  const juce::ScopedLock guard(lock); rate = d->getCurrentSampleRate(); frames = d->getCurrentBufferSizeSamples();
  for (auto& channel : channels) for (auto& slot : channel->slots) prepare(slot);
}
void Host::audioDeviceStopped() {}
void Host::audioDeviceError(const juce::String& message) {
  juce::MessageManager::callAsync([this, token = alive, message] { if (!token->load()) return; error = message; if (changed) changed(); });
}
void Host::audioDeviceIOCallbackWithContext(const float* const* input, int inputs, float* const* output, int outputs, int count, const juce::AudioIODeviceCallbackContext&) {
  ++callbackCount;
  for (int i = 0; i < outputs; ++i) if (output[i]) juce::FloatVectorOperations::clear(output[i], count);
  const juce::ScopedTryLock guard(lock);
  if (!guard.isLocked() || count > 8192) return;
  for (auto& channel : channels) channel->signal.clear();
  for (int index : order) {
    auto& channel = *channels[index];
    const int bindings[] = {channel.input, channel.inputRight};
    for (int side = 0; side < 2; ++side) {
      const int binding = bindings[side];
      if (binding >= 0 && binding < inputs && input[binding]) channel.signal.addFrom(side, 0, input[binding], count);
    }
    // All upstream channels are already processed, including sidechain sources.
    for (int source : order) for (const auto& route : channels[source]->sends)
      if (route.target == index && route.slot < 0) for (int side = 0; side < 2; ++side)
        channel.signal.addFrom(side, 0, channels[source]->signal, side, 0, count, route.gain);
    for (int s = 0; s < slotCount; ++s) {
      auto& slot = channel.slots[s]; if (!slot.plugin || !slot.enabled.load()) continue;
      auto& plugin = *slot.plugin; slot.work.clear();
      juce::AudioBuffer<float> work(slot.work.getArrayOfWritePointers(), slot.work.getNumChannels(), count);
      auto main = plugin.getBusBuffer(work, true, 0);
      for (int side = 0; side < main.getNumChannels(); ++side) main.copyFrom(side, 0, channel.signal, std::min(side, 1), 0, count);
      for (int source : order) for (const auto& route : channels[source]->sends) {
        if (route.target != index || route.slot != s || route.bus <= 0 || route.bus >= plugin.getBusCount(true)) continue;
        auto bus = plugin.getBusBuffer(work, true, route.bus);
        for (int side = 0; side < bus.getNumChannels(); ++side) bus.addFrom(side, 0, channels[source]->signal, std::min(side, 1), 0, count, route.gain);
      }
      midi.clear(); plugin.processBlock(work, midi);
      auto result = plugin.getBusBuffer(work, false, 0);
      channel.signal.clear();
      if (result.getNumChannels()) for (int side = 0; side < 2; ++side) channel.signal.copyFrom(side, 0, result, std::min(side, result.getNumChannels() - 1), 0, count);
    }
    for (int side = 0; side < 2; ++side) {
      channel.signal.applyGain(side, 0, count, channel.mute.load() ? 0 : gainForPosition(channel.fader.load()));
      channel.peak[side] = channel.signal.getMagnitude(side, 0, count);
    }
    const int destinations[] = {channel.output, channel.outputRight};
    for (int side = 0; side < 2; ++side) {
      const int binding = destinations[side];
      if (binding >= 0 && binding < outputs && output[binding])
        juce::FloatVectorOperations::add(output[binding], channel.signal.getReadPointer(side), count);
    }
  }
}
void Host::save() {
  if (!persistent) return;
  const juce::ScopedLock guard(lock);
  juce::XmlElement root("MINI_DAW"); root.setAttribute("version", 2); root.setAttribute("driver", driverName);
  for (const auto& channel : channels) {
    auto* c = root.createNewChildElement("CHANNEL"); c->setAttribute("name", channel->name);
    c->setAttribute("input", channel->input); c->setAttribute("inputR", channel->inputRight);
    c->setAttribute("output", channel->output); c->setAttribute("outputR", channel->outputRight);
    c->setAttribute("fader", channel->fader.load()); c->setAttribute("mute", channel->mute.load());
    for (int i = 0; i < slotCount; ++i) {
      const auto& slot = channel->slots[i]; if (!slot.plugin) continue;
      auto* p = c->createNewChildElement("SLOT"); p->setAttribute("index", i); p->setAttribute("enabled", slot.enabled.load());
      p->addChildElement(slot.description.createXml().release());
      juce::MemoryBlock state; slot.plugin->getStateInformation(state); p->setAttribute("state", state.toBase64Encoding());
    }
    for (const auto& route : channel->sends) {
      auto* r = c->createNewChildElement("SEND"); r->setAttribute("target", route.target); r->setAttribute("slot", route.slot); r->setAttribute("bus", route.bus); r->setAttribute("gain", route.gain);
    }
  }
  stateFile().getParentDirectory().createDirectory(); root.writeTo(stateFile());
  if (auto xml = catalog.createXml()) xml->writeTo(catalogFile());
}
void Host::restore() {
  auto root = juce::XmlDocument::parse(stateFile()); if (!root || !root->hasTagName("MINI_DAW")) return;
  needsDefaultInput = false;
  driverName = root->getStringAttribute("driver", "Aggregate Audio");
  for (auto* c : root->getChildWithTagNameIterator("CHANNEL")) {
    if (!addChannel()) break;
    auto& ch = *channels.back(); ch.name = c->getStringAttribute("name", ch.name);
    ch.input = c->getIntAttribute("input", -1); ch.inputRight = c->getIntAttribute("inputR", -1);
    ch.output = c->getIntAttribute("output", -1); ch.outputRight = c->getIntAttribute("outputR", -1);
    ch.fader = juce::jlimit(0.0f, 1.0f, static_cast<float>(c->getDoubleAttribute("fader", c->getDoubleAttribute("left", dbToPosition(0))))); ch.mute = c->getBoolAttribute("mute");
    for (auto* p : c->getChildWithTagNameIterator("SLOT")) {
      int index = p->getIntAttribute("index", -1); auto* desc = p->getChildByName("PLUGIN");
      if (!juce::isPositiveAndBelow(index, slotCount) || !desc) continue;
      juce::PluginDescription d; if (!d.loadFromXml(*desc)) continue;
      juce::MemoryBlock state; state.fromBase64Encoding(p->getStringAttribute("state"));
      if (loadPlugin(static_cast<int>(channels.size()) - 1, index, d, &state)) ch.slots[index].enabled = p->getBoolAttribute("enabled", true);
    }
    for (auto* r : c->getChildWithTagNameIterator("SEND")) ch.sends.push_back({r->getIntAttribute("target", -1), r->getIntAttribute("slot", -1), r->getIntAttribute("bus"), static_cast<float>(r->getDoubleAttribute("gain"))});
  }
  // Discard invalid session edges and cycles instead of enabling feedback on startup.
  for (auto& ch : channels) for (auto& r : ch->sends)
    if (r.target >= static_cast<int>(channels.size()) || r.slot >= slotCount || r.slot < -1 || r.bus < 0 || !std::isfinite(r.gain)) r = {};
  rebuildOrder(); if (order.empty()) { for (auto& ch : channels) ch->sends.clear(); rebuildOrder(); }
}
}
