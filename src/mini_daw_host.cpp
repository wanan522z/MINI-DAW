#include "mini_daw_host.h"

namespace mini {
namespace {
juce::String audioErrorText(const juce::String& message) {
  if (message.containsIgnoreCase("Device is in use"))
    return utf8("设备被 Windows 音频或其他程序占用，请释放后重试。设备配置已保留。") + "\n" + message;
  return message;
}
void logAudioError(const juce::String& driver, const juce::String& message) {
  auto file = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory).getChildFile("MiniDAW/audio-errors.log");
  file.getParentDirectory().createDirectory();
  if (file.getSize() > 1024*1024) file.moveFileTo(file.getSiblingFile("audio-errors.previous.log"));
  file.appendText(juce::Time::getCurrentTime().toString(true, true, true, true) + " [" + driver + "] " + message + "\n");
}
}
AsioBinding AsioBinding::fromIndex(int index, const juce::StringArray& names, const juce::String& driver) {
  if (!juce::isPositiveAndBelow(index, names.size())) return {};
  AsioBinding result{driver, names[index]};
  for (int i = 0; i < index; ++i) if (names[i] == result.name) ++result.occurrence;
  return result;
}
int AsioBinding::resolve(const juce::StringArray& names, const juce::String& currentDriver) const {
  if (name.isEmpty() || (driver.isNotEmpty() && driver != currentDriver)) return -1;
  int remaining = occurrence;
  for (int i = 0; i < names.size(); ++i) if (names[i] == name && remaining-- == 0) return i;
  return -1;
}
Host::Host(bool saveState) : persistent(saveState) {
  formats.addFormat(new juce::VST3PluginFormat());
  formats.addFormat(new juce::VSTPluginFormat());
  type.reset(juce::AudioIODeviceType::createAudioIODeviceType_ASIO());
  if (type) { type->scanForDevices(); type->addListener(this); }
  recentPresets.setMaxNumberOfItems(12);
  if (persistent) {
    recentPresets.restoreFromString(stateFile().getSiblingFile("recent-presets.txt").loadFileAsString());
    if (auto xml = juce::XmlDocument::parse(catalogFile())) catalog.recreateFromXml(*xml);
    auto types = catalog.getTypes(); catalog.clear();
    for (auto d : types) { d.fileOrIdentifier = canonicalPluginFile(juce::File(d.fileOrIdentifier)).getFullPathName(); catalog.addType(d); }
    restore();
  }
  if (channels.empty()) { for (int i = 0; i < 12; ++i) addChannel(); }
  if (persistent) { systemDeviceWatcher = std::make_unique<WindowsAudioDeviceWatcher>(); refreshSystemOutputs(); }
  startTimer(250);
}
Host::~Host() { stopTimer(); *alive = false; if (scanJob) scanJob->cancelled = true; if (type) type->removeListener(this); stop(); if (saveOnExit) save(); }
juce::File Host::stateFile() const { return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory).getChildFile("MiniDAW/session-v2.xml"); }
juce::File Host::catalogFile() const { return stateFile().getSiblingFile("effects-vst3.xml"); }
juce::StringArray Host::drivers() { if (!type) return {}; type->scanForDevices(); return type->getDeviceNames(); }
bool Host::start() {
  if (opening) return false;
  wantAudio = true; opening = true;
  juce::ScopedValueSetter<bool> openingScope(opening, true, false);
  closeDevice(); error.clear(); lastStartTime = juce::Time::getMillisecondCounterHiRes();
  auto failed = [this] {
    if (persistent) logAudioError(driverName, error);
    error = audioErrorText(error);
    reconcileChannels({}, {}); ++deviceGeneration;
    if (changed) changed(); return false;
  };
  if (!type || !drivers().contains(driverName)) {
    error = utf8("未找到 ASIO 驱动：") + driverName + utf8("。设备绑定已保留，等待设备重新连接。"); return failed();
  }
  device.reset(type->createDevice(driverName, driverName));
  if (!device) { error = utf8("无法创建 ASIO 设备"); return failed(); }
  // Some ASIO backends return a device object even when driver init failed.
  // Stop here, before querying channels or trying buffers on that instance.
  error = device->getLastError();
  if (error.isNotEmpty()) { device.reset(); return failed(); }
  auto inputs = device->getInputChannelNames(), outputs = device->getOutputChannelNames();
  juce::BigInteger ins, outs; ins.setRange(0, inputs.size(), true); outs.setRange(0, outputs.size(), true);
  // Passing zero makes JUCE choose the first supported rate (often 32 kHz),
  // rather than the rate selected in the ASIO control panel.
  const auto currentRate = device->getCurrentSampleRate();
  const int requestedBuffer = device->getAvailableBufferSizes().contains(requestedFrames) ? requestedFrames : 0;
  error = device->open(ins, outs, requestedRate > 0 ? requestedRate : (currentRate > 0 ? currentRate : rate), requestedBuffer);
  if (error.isNotEmpty()) { device.reset(); return failed(); }
  // Query names again: some ASIO drivers only publish bindings after open/init.
  reconcileChannels(device->getInputChannelNames(), device->getOutputChannelNames());
  refreshSystemOutputs();
  if (needsDefaultInput && !inputNames.isEmpty()) { setAsioInput(0, 0, 0); needsDefaultInput = false; }
  device->start(this);
  syncSystemLoopback();
  lastCallbacks = callbackCount.load(); lastAudioTime = juce::Time::getMillisecondCounterHiRes();
  rebuildRequested = false;
  if (!device->isPlaying()) { error = utf8("ASIO 无法启动"); closeDevice(); return failed(); }
  ++deviceGeneration;
  if (changed) changed(); return true;
}
void Host::stop() {
  wantAudio = false; rebuildRequested = false; closeDevice();
}
void Host::closeDevice() {
  if (device) { device->stop(); device->close(); device.reset(); }
  systemLoopback.stop();
  for (auto& channel : channels) for (auto& peak : channel->peak) peak = 0;
}
void Host::reconcileChannels(const juce::StringArray& inputs, const juce::StringArray& outputs, const juce::String& driver) {
  const juce::ScopedLock guard(lock);
  const auto currentDriver = driver.isEmpty() ? driverName : driver;
  const auto& previousInputs = savedInputs.isEmpty() ? inputNames : savedInputs;
  const auto& previousOutputs = savedOutputs.isEmpty() ? outputNames : savedOutputs;
  const auto previousDriver = layoutDriver.isEmpty() ? driverName : layoutDriver;
  auto map = [&](AsioBinding& binding, int index, const juce::StringArray& previous, const juce::StringArray& now) {
    if (binding.name.isEmpty())
      binding = AsioBinding::fromIndex(index, previous.isEmpty() ? now : previous, previous.isEmpty() ? currentDriver : previousDriver);
    return binding.resolve(now, currentDriver);
  };
  for (auto& c : channels) {
    c->input = map(c->asioBindings[0], c->input, c->loopback ? previousOutputs : previousInputs, c->loopback ? outputs : inputs);
    c->inputRight = map(c->asioBindings[1], c->inputRight, c->loopback ? previousOutputs : previousInputs, c->loopback ? outputs : inputs);
    c->output = map(c->asioBindings[2], c->output, c->loopback ? previousInputs : previousOutputs, c->loopback ? inputs : outputs);
    c->outputRight = map(c->asioBindings[3], c->outputRight, c->loopback ? previousInputs : previousOutputs, c->loopback ? inputs : outputs);
  }
  inputNames = inputs; outputNames = outputs; layoutDriver = currentDriver;
  savedInputs.clear(); savedOutputs.clear(); rebuildOrder();
  if (order.empty()) {
    // A route edited while unplugged may conflict with a returning port. Restore
    // only safe pairs, retaining every rejected identity for manual selection.
    std::vector<std::array<int, 4>> candidates;
    for (auto& c : channels) {
      candidates.push_back({c->input, c->inputRight, c->output, c->outputRight});
      c->input = c->inputRight = c->output = c->outputRight = -1;
    }
    rebuildOrder();
    for (size_t i = 0; i < channels.size(); ++i) {
      auto& c = *channels[i];
      for (int side = 0; side < 2; ++side) {
        int& left = side == 0 ? c.input : c.output;
        int& right = side == 0 ? c.inputRight : c.outputRight;
        left = candidates[i][side*2]; right = candidates[i][side*2+1]; rebuildOrder();
        if (order.empty()) { left = right = -1; rebuildOrder(); }
      }
    }
  }
}
void Host::audioDeviceListChanged() { if (wantAudio && !opening) rebuildRequested = true; }
juce::FileSearchPath Host::scanPaths() const {
  const auto multiFormat = catalogFile().getSiblingFile("plugin-paths.txt");
  if (persistent && multiFormat.existsAsFile()) return juce::FileSearchPath(multiFormat.loadFileAsString());
  const auto saved = catalogFile().getSiblingFile("vst3-paths.txt");
  auto paths = defaultPluginSearchPaths();
  if (persistent && saved.existsAsFile()) paths.addPath(juce::FileSearchPath(saved.loadFileAsString()));
  paths.removeRedundantPaths(); return paths;
}
void Host::setScanPaths(const juce::FileSearchPath& paths) {
  if (!persistent) return;
  const auto file = catalogFile().getSiblingFile("plugin-paths.txt"); file.getParentDirectory().createDirectory(); file.replaceWithText(paths.toString());
}
void Host::beginScan(const juce::FileSearchPath& paths) {
  if (scanJob && !scanJob->done) return;
  auto combined = paths;
  const auto saved = catalogFile().getSiblingFile("plugin-paths.txt");
  const juce::FileSearchPath oldPaths(persistent ? saved.loadFileAsString() : juce::String());
  for (int i = 0; i < oldPaths.getNumPaths(); ++i) if (oldPaths[i].isDirectory()) combined.add(oldPaths[i]);
  combined.removeRedundantPaths();
  if (persistent) { saved.getParentDirectory().createDirectory(); saved.replaceWithText(combined.toString()); }
  lastScanSave = 0;
  scanJob = scanPlugins(combined, catalog.getTypes());
}
void Host::timerCallback() {
  if (scanJob) {
    juce::Array<juce::PluginDescription> ready;
    { const juce::ScopedLock guard(scanJob->lock); ready.swapWith(scanJob->ready); }
    for (const auto& d : ready) catalog.addType(d);
    if (!ready.isEmpty()) ++catalogRevision;
    const auto now = juce::Time::getMillisecondCounterHiRes();
    if (!ready.isEmpty() && (scanJob->done || now-lastScanSave > 1500)) {
      if (persistent) { catalogFile().getParentDirectory().createDirectory(); if (auto xml = catalog.createXml()) xml->writeTo(catalogFile()); }
      lastScanSave = now;
    }
    // Always flush the final catalog, including the last partial batch.
    if (scanJob->done && persistent && lastScanSave != -1) {
      if (auto xml = catalog.createXml()) xml->writeTo(catalogFile());
      const juce::ScopedLock guard(scanJob->lock);
      catalogFile().getSiblingFile("scan-failures.txt").replaceWithText(scanJob->failures);
      lastScanSave = -1;
    }
  }
  const auto now = juce::Time::getMillisecondCounterHiRes();
  if (systemDeviceWatcher && systemDeviceWatcher->consumeChanges()) systemRefreshAt = now+750;
  if (!opening && systemRefreshAt > 0 && now >= systemRefreshAt) {
    systemRefreshAt = 0; queueSystemOutputRefresh();
  }
  if (!opening && systemOutputRefresh.valid() && systemOutputRefresh.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
    auto endpoints = systemOutputRefresh.get();
    if (pendingSystemRefreshGeneration == systemRefreshGeneration) applySystemOutputs(std::move(endpoints));
    if (systemRefreshAgain) { systemRefreshAgain = false; queueSystemOutputRefresh(); }
  }
  if (!wantAudio || opening) return;
  const auto callbacks = callbackCount.load();
  if (callbacks != lastCallbacks) { lastCallbacks = callbacks; lastAudioTime = now; }
  if (device && (device->getInputChannelNames() != inputNames || device->getOutputChannelNames() != outputNames)) rebuildRequested = true;
  if (now-lastStartTime > 2000 && (rebuildRequested || !isRunning() || now-lastAudioTime > 2500)) start();
  else syncSystemLoopback();
}
void Host::refreshSystemOutputs() {
  ++systemRefreshGeneration;
  applySystemOutputs(windowsAudioOutputs(driverName, device ? outputNames : juce::StringArray{}));
}
void Host::queueSystemOutputRefresh() {
  if (systemOutputRefresh.valid()) { systemRefreshAgain = true; return; }
  const auto driver = driverName;
  const auto outputs = device ? outputNames : juce::StringArray{};
  pendingSystemRefreshGeneration = systemRefreshGeneration;
  systemOutputRefresh = std::async(std::launch::async, [driver, outputs] { return windowsAudioOutputs(driver, outputs); });
}
void Host::applySystemOutputs(std::vector<WindowsOutput> endpoints) {
  {
    const juce::ScopedLock guard(lock);
    if (systemOutputs == endpoints) return;
    systemOutputs = std::move(endpoints); ++systemDeviceGeneration;
    for (auto& c : channels) for (const auto& endpoint : systemOutputs)
      if (c->windowsInput.endpoint == endpoint.id) c->windowsInput.name = endpoint.name;
    for (auto& [key, source] : systemSources) for (const auto& endpoint : systemOutputs)
      if (source.endpoint == endpoint.id) source.name = endpoint.name;
    syncSystemLoopback();
  }
  if (changed) changed();
}
std::vector<WindowsOutput> Host::windowsLoopbackOutputs() const {
  const juce::ScopedLock guard(lock); std::vector<WindowsOutput> result;
  for (const auto& endpoint : systemOutputs)
    if (!isRunning() || (endpoint.sharedAvailable && !asioUsesWindowsOutput(driverName, outputNames, endpoint))) result.push_back(endpoint);
  return result;
}
juce::String Host::loopbackInputStatus(int index) const {
  const juce::ScopedLock guard(lock);
  if (!juce::isPositiveAndBelow(index, static_cast<int>(channels.size()))) return {};
  if (channels[index]->windowsInput.endpoint.isEmpty()) {
    const auto& channel = *channels[index];
    if ((channel.input < 0 && channel.asioBindings[0].name.isNotEmpty())
        || (channel.inputRight < 0 && channel.asioBindings[1].name.isNotEmpty()))
      return utf8("ASIO 设备绑定已保留，重新连接后自动恢复，也可手动切换");
    return systemSourceStatus(channel.input);
  }
  if (!isRunning()) return utf8("音频已停止");
  const auto& input = channels[index]->windowsInput;
  const auto endpoints = windowsLoopbackOutputs();
  const auto found = std::find_if(endpoints.begin(), endpoints.end(), [&](const auto& endpoint) { return endpoint.id == input.endpoint; });
  if (found == endpoints.end()) return utf8("Windows 输出当前不可用，绑定已保留；设备恢复可用后自动连接，也可手动切换");
  if (input.left >= found->channels || input.right >= found->channels) return utf8("所选 Windows 声道当前不可用");
  return systemLoopback.status(outputNames.size()+index*2);
}
LoopbackSource Host::systemSource(int output, bool resolveAutomatic) const {
  const juce::ScopedLock guard(lock);
  if (!juce::isPositiveAndBelow(output, outputNames.size())) return {};
  const auto found = systemSources.find(driverName+"\n"+outputNames[output]);
  auto source = found == systemSources.end() ? LoopbackSource{} : found->second;
  if (resolveAutomatic && source.endpoint.isEmpty()) {
    auto automatic = automaticLoopbackSource(driverName, outputNames, output, systemOutputs);
    automatic.includeHost = source.includeHost; return automatic;
  }
  if (resolveAutomatic) for (const auto& endpoint : systemOutputs)
    if (source.endpoint == endpoint.id) source.endpointChannels = endpoint.channels;
  return source;
}
bool Host::setSystemSource(int output, const LoopbackSource& source) {
  const juce::ScopedLock guard(lock);
  if (!juce::isPositiveAndBelow(output, outputNames.size()) || source.channel < 0 || source.channel >= 64) return false;
  const auto key = driverName+"\n"+outputNames[output]; const auto previous = systemSource(output);
  auto saved = source;
  for (const auto& endpoint : systemOutputs) if (endpoint.id == saved.endpoint) saved.name = endpoint.name;
  systemSources[key] = saved; rebuildOrder();
  if (order.empty()) { systemSources[key] = previous; rebuildOrder(); error = utf8("合并宿主输出会形成反馈环路，未修改。"); return false; }
  syncSystemLoopback(); error.clear(); return true;
}
void Host::syncSystemLoopback() {
  const juce::ScopedLock guard(lock);
  if (!isRunning()) return;
  std::vector<LoopbackSource> sources(static_cast<size_t>(outputNames.size())+channels.size()*2);
  const auto endpoints = windowsLoopbackOutputs();
  for (int index = 0; index < static_cast<int>(channels.size()); ++index) {
    const auto& channel = *channels[index]; if (!channel.loopback) continue;
    if (channel.windowsInput.endpoint.isNotEmpty()) {
      const auto found = std::find_if(endpoints.begin(), endpoints.end(), [&](const auto& endpoint) { return endpoint.id == channel.windowsInput.endpoint; });
      if (found == endpoints.end() || channel.windowsInput.left >= found->channels || channel.windowsInput.right >= found->channels) continue;
      const int offset = outputNames.size()+index*2;
      sources[offset] = {channel.windowsInput.endpoint, channel.windowsInput.left, false, found->name, found->channels};
      sources[offset+1] = {channel.windowsInput.endpoint, channel.windowsInput.right, false, found->name, found->channels};
    } else for (int output : {channel.input, channel.inputRight})
      if (juce::isPositiveAndBelow(output, outputNames.size())) sources[output] = systemSource(output, true);
  }
  systemLoopback.configure(sources, rate);
}
juce::String Host::systemSourceStatus(int output) const {
  const juce::ScopedLock guard(lock);
  const auto source = systemSource(output, true);
  if (source.endpoint == "off") return utf8("仅内录宿主输出");
  if (source.endpoint.isEmpty()) return utf8("未自动匹配 Windows 输出，请手动指定设备及声道");
  if (std::none_of(systemOutputs.begin(), systemOutputs.end(), [&](const auto& endpoint) { return endpoint.id == source.endpoint; }))
    return utf8("Windows 设备绑定已保留，重新连接后自动恢复，也可手动切换");
  if (!isRunning()) return utf8("音频已停止");
  return systemLoopback.status(output);
}
bool Host::addChannel() {
  const juce::ScopedLock guard(lock);
  if (channels.size() >= maximumChannels) return false;
  auto channel = std::make_unique<Channel>(); channel->name = utf8("通道 ") + juce::String(static_cast<int>(channels.size()) + 1);
  channels.push_back(std::move(channel)); rebuildOrder(); return true;
}
bool Host::setLoopbackMode(int index, bool enabled) {
  const juce::ScopedLock guard(lock); if (!juce::isPositiveAndBelow(index, static_cast<int>(channels.size()))) return false;
  auto& c = *channels[index];
  c.loopback = enabled; c.windowsInput = {}; c.asioBindings = {}; c.input = c.inputRight = c.output = c.outputRight = -1;
  rebuildOrder(); syncSystemLoopback(); error.clear(); return true;
}
bool Host::setBindings(int index, int inputLeft, int inputRight, int outputLeft, int outputRight) {
  return changeBindings(index, inputLeft, inputRight, outputLeft, outputRight, true, true);
}
bool Host::changeBindings(int index, int inputLeft, int inputRight, int outputLeft, int outputRight, bool changeInput, bool changeOutput) {
  const juce::ScopedLock guard(lock); if (!juce::isPositiveAndBelow(index, static_cast<int>(channels.size()))) return false;
  auto& c = *channels[index]; const std::array<int, 4> previous{c.input, c.inputRight, c.output, c.outputRight};
  c.input = inputLeft; c.inputRight = inputRight; c.output = outputLeft; c.outputRight = outputRight; rebuildOrder();
  if (order.empty()) { c.input = previous[0]; c.inputRight = previous[1]; c.output = previous[2]; c.outputRight = previous[3]; rebuildOrder(); error = utf8("这个输入跳线会形成反馈环路，未连接。"); return false; }
  const auto& inputs = c.loopback ? outputNames : inputNames;
  const auto& outputs = c.loopback ? inputNames : outputNames;
  const auto currentDriver = layoutDriver.isEmpty() ? driverName : layoutDriver;
  if (changeInput) {
    c.asioBindings[0] = AsioBinding::fromIndex(inputLeft, inputs, currentDriver);
    c.asioBindings[1] = AsioBinding::fromIndex(inputRight, inputs, currentDriver);
  }
  if (changeOutput) {
    c.asioBindings[2] = AsioBinding::fromIndex(outputLeft, outputs, currentDriver);
    c.asioBindings[3] = AsioBinding::fromIndex(outputRight, outputs, currentDriver);
  }
  syncSystemLoopback(); error.clear(); return true;
}
bool Host::setAsioInput(int index, int left, int right) {
  const juce::ScopedLock guard(lock);
  if (!juce::isPositiveAndBelow(index, static_cast<int>(channels.size()))) return false;
  auto& channel = *channels[index]; const auto previous = channel.windowsInput; channel.windowsInput = {};
  if (changeBindings(index, left, right, channel.output, channel.outputRight, true, false)) return true;
  channel.windowsInput = previous; rebuildOrder(); return false;
}
bool Host::setAsioOutput(int index, int left, int right) {
  const juce::ScopedLock guard(lock);
  if (!juce::isPositiveAndBelow(index, static_cast<int>(channels.size()))) return false;
  const auto& channel = *channels[index];
  return changeBindings(index, channel.input, channel.inputRight, left, right, false, true);
}
bool Host::setWindowsInput(int index, const WindowsLoopbackInput& input) {
  const juce::ScopedLock guard(lock);
  if (!juce::isPositiveAndBelow(index, static_cast<int>(channels.size())) || !channels[index]->loopback
      || input.endpoint.isEmpty() || input.endpoint == "off" || input.left < 0 || input.left >= 64 || input.right < 0 || input.right >= 64) return false;
  const auto endpoints = windowsLoopbackOutputs();
  const auto found = std::find_if(endpoints.begin(), endpoints.end(), [&](const auto& endpoint) { return endpoint.id == input.endpoint; });
  if (found == endpoints.end() || input.left >= found->channels || input.right >= found->channels) {
    error = utf8("这个 Windows 输出当前不可用，请刷新设备列表后重新选择。"); return false;
  }
  auto& channel = *channels[index]; const auto previous = channel.windowsInput;
  channel.windowsInput = input;
  if (changeBindings(index, -1, -1, channel.output, channel.outputRight, true, false)) return true;
  channel.windowsInput = previous; rebuildOrder(); return false;
}
bool Host::setSendTap(int channel, int sendIndex, bool preFx) {
  const juce::ScopedLock guard(lock);
  if (!juce::isPositiveAndBelow(channel, static_cast<int>(channels.size())) || sendIndex < 0 || sendIndex > 512) return false;
  auto& sends = channels[channel]->sends;
  if (sendIndex >= static_cast<int>(sends.size())) sends.resize(sendIndex+1);
  sends[sendIndex].preFx = preFx; return true;
}
void Host::rebuildOrder() {
  std::vector<std::vector<Route>> routes; for (const auto& channel : channels) routes.push_back(channel->sends);
  // Internal loopback output writes into host input bindings; include these
  // dependencies so plugins and sends still run once, with no feedback cycle.
  // A loopback source can tap all Mini DAW channels writing to the selected ASIO output.
  for (int source = 0; source < static_cast<int>(channels.size()); ++source) if (!channels[source]->loopback)
    for (int target = 0; target < static_cast<int>(channels.size()); ++target) if (channels[target]->loopback) {
      const int outputs[] = {channels[source]->output, channels[source]->outputRight};
      const int taps[] = {channels[target]->input, channels[target]->inputRight};
      for (int output : outputs) for (int tap : taps) if (output >= 0 && output == tap && systemSource(tap).includeHost) routes[source].push_back({target, -1, 0, 1});
    }
  for (int source = 0; source < static_cast<int>(channels.size()); ++source) if (channels[source]->loopback)
    for (int target = 0; target < static_cast<int>(channels.size()); ++target) if (!channels[target]->loopback) {
      const auto& a = *channels[source]; const auto& b = *channels[target];
      const int destinations[] = {a.output, a.outputRight}; const int bindings[] = {b.input, b.inputRight};
      for (int destination : destinations) for (int binding : bindings) if (destination >= 0 && destination == binding) routes[source].push_back({target, -1, 0, 1});
    }
  order = routingOrder(routes);
  plans.clear(); plans.resize(channels.size());
  for (int index = 0; index < static_cast<int>(channels.size()); ++index) {
    const auto& channel = *channels[index];
    const int offset = outputNames.size()+index*2;
    plans[index].systemInputs = channel.windowsInput.endpoint.isNotEmpty()
      ? std::array<int, 2>{offset, offset+1} : std::array<int, 2>{channel.input, channel.inputRight};
  }
  for (int source = 0; source < static_cast<int>(channels.size()); ++source) if (!channels[source]->loopback)
    for (int target = 0; target < static_cast<int>(channels.size()); ++target) if (channels[target]->loopback) {
      const int outputs[] = {channels[source]->output, channels[source]->outputRight};
      const int taps[] = {channels[target]->input, channels[target]->inputRight};
      for (int a = 0; a < 2; ++a) for (int b = 0; b < 2; ++b) if (outputs[a] >= 0 && outputs[a] == taps[b] && systemSource(taps[b]).includeHost) plans[target].patches.push_back({source, a, b});
    }
  for (int source = 0; source < static_cast<int>(channels.size()); ++source) if (channels[source]->loopback)
    for (int target = 0; target < static_cast<int>(channels.size()); ++target) if (!channels[target]->loopback) {
      const int destinations[] = {channels[source]->output, channels[source]->outputRight}; const int bindings[] = {channels[target]->input, channels[target]->inputRight};
      for (int a = 0; a < 2; ++a) for (int b = 0; b < 2; ++b) if (destinations[a] >= 0 && destinations[a] == bindings[b]) plans[target].patches.push_back({source, a, b});
    }
  for (int source = 0; source < static_cast<int>(channels.size()); ++source)
    for (int send = 0; send < static_cast<int>(channels[source]->sends.size()); ++send) {
      const auto& r = channels[source]->sends[send];
      if (!juce::isPositiveAndBelow(r.target, static_cast<int>(plans.size()))) continue;
      if (r.slot < 0) plans[r.target].main.push_back({source, send});
      else if (r.slot < slotCount) plans[r.target].sidechains[r.slot].push_back({source, send});
    }
}
bool Host::setRoute(int source, int index, Route route) {
  const juce::ScopedLock guard(lock);
  if (source < 0 || source >= static_cast<int>(channels.size()) || route.target < 0 || route.target >= static_cast<int>(channels.size()) || index < 0
      || route.slot < -1 || route.slot >= slotCount || route.bus < 0 || !std::isfinite(route.gain) || route.gain < 0) return false;
  auto& sends = channels[source]->sends;
  auto previous = sends;
  if (index >= static_cast<int>(sends.size())) sends.resize(index+1);
  sends[index] = route;
  rebuildOrder();
  if (order.empty()) { sends = std::move(previous); rebuildOrder(); error = utf8("该发送会形成音频反馈环路，未连接。"); return false; }
  error.clear();
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
bool Host::loadPreferredPlugin(int channel, int index, const juce::PluginDescription& description) {
  auto versions = matchingPluginVersions(description, catalog.getTypes());
  if (versions.isEmpty()) versions.add(description);
  juce::StringArray failures;
  for (const auto& version : versions) {
    if (loadPlugin(channel, index, version)) return true;
    failures.add(version.pluginFormatName+": "+error);
  }
  error = failures.joinIntoString("\n"); return false;
}
bool Host::loadPlugin(int channel, int index, const juce::PluginDescription& description, const juce::MemoryBlock* state) {
  if (!isEffectCandidate(description)) { error = utf8("仅允许效果插件"); return false; }
  juce::String reason;
  auto plugin = formats.createPluginInstance(description, rate, frames, reason);
  if (!plugin) { error = utf8("加载插件失败：") + reason; return false; }
  if (plugin->getBusCount(true) == 0 || plugin->getBusCount(false) == 0) { error = utf8("插件没有音频效果输入/输出"); return false; }
  if (state && state->getSize()) plugin->setStateInformation(state->getData(), static_cast<int>(state->getSize()));
  auto layout = plugin->getBusesLayout();
  // Prefer stereo mains. Extra input buses use the plug-in's own accepted layouts.
  layout.inputBuses.set(0, juce::AudioChannelSet::stereo()); layout.outputBuses.set(0, juce::AudioChannelSet::stereo());
  if (plugin->checkBusesLayoutSupported(layout)) plugin->setBusesLayout(layout);
  // Some effects (including Valhalla) declare their main buses inactive by
  // default. The host must negotiate/enable them before checking channel counts.
  plugin->getBus(true, 0)->enable(true); plugin->getBus(false, 0)->enable(true);
  if (plugin->getMainBusNumInputChannels() == 0 || plugin->getMainBusNumOutputChannels() == 0) {
    error = utf8("插件没有可用的音频主输入/输出"); return false;
  }
  for (int bus = 1; bus < plugin->getBusCount(true); ++bus) {
    auto* b = plugin->getBus(true, bus);
    if (!b->isEnabled()) b->enable(true);
  }
  const juce::ScopedLock guard(lock);
  auto& slot = channels[channel]->slots[index];
  if (slot.plugin) slot.plugin->releaseResources();
  slot.plugin = std::move(plugin); slot.description = description; slot.enabled = true;
  slot.description.numInputChannels = slot.plugin->getTotalNumInputChannels(); slot.description.numOutputChannels = slot.plugin->getTotalNumOutputChannels();
  catalog.addType(slot.description); ++catalogRevision;
  slot.editorScale = 1; slot.editorWidth = slot.editorHeight = 0;
  // A replacement may have fewer auxiliary buses than the previous effect.
  for (auto& ch : channels) std::erase_if(ch->sends, [&](const Route& r) {
    return r.target == channel && r.slot == index &&
      (r.bus <= 0 || r.bus >= slot.plugin->getBusCount(true) || !slot.plugin->getBus(true, r.bus)->isEnabled());
  });
  rebuildOrder();
  error.clear();
  auto& c = *channels[channel];
  if (!c.colourAssigned) { c.colour = juce::Colour::fromHSV(juce::Random::getSystemRandom().nextFloat(), 0.48f, 0.88f, 1.0f); c.colourAssigned = true; }
  prepare(slot); return true;
}
void Host::removePlugin(int channel, int index) {
  const juce::ScopedLock guard(lock);
  auto& slot = channels[channel]->slots[index]; if (slot.plugin) slot.plugin->releaseResources(); slot.plugin.reset();
  for (auto& ch : channels) std::erase_if(ch->sends, [&](const Route& route) { return route.target == channel && route.slot == index; });
  rebuildOrder();
}
void Host::controlPanel() {
  const bool resume = wantAudio;
  if (!device && !start()) return;
  if (device) device->showControlPanel();
  if (!resume) stop();
}
double Host::deviceLatencyMs() const {
  return device && rate > 0 ? 1000.0*(device->getInputLatencyInSamples()+device->getOutputLatencyInSamples())/rate : 0;
}
int Host::pluginLatencySamples() const {
  const juce::ScopedLock guard(lock);
  std::vector<int> latency(channels.size()); int maximum = 0;
  for (int c : order) {
    for (const auto& r : plans[c].main) latency[c] = std::max(latency[c], latency[r.source]);
    for (int s = 0; s < slotCount; ++s) {
      for (const auto& r : plans[c].sidechains[s]) latency[c] = std::max(latency[c], latency[r.source]);
      const auto& slot = channels[c]->slots[s];
      if (slot.plugin && slot.enabled) latency[c] += slot.plugin->getLatencySamples();
    }
    if (channels[c]->output >= 0 || channels[c]->outputRight >= 0) maximum = std::max(maximum, latency[c]);
  }
  return maximum;
}
juce::Array<double> Host::sampleRates() const { return device ? device->getAvailableSampleRates() : juce::Array<double>(); }
bool Host::setSampleRate(double value) { if (!sampleRates().contains(value)) return false; requestedRate = value; return start(); }
juce::Array<int> Host::bufferSizes() const { return device ? device->getAvailableBufferSizes() : juce::Array<int>(); }
bool Host::setBufferSize(int size) {
  if (!bufferSizes().contains(size)) return false;
  requestedFrames = size; return start();
}
juce::String Host::channelLabel(int index) const { return juce::String(index+1).paddedLeft('0', 2)+"  "+channels[index]->name; }
void Host::audioDeviceAboutToStart(juce::AudioIODevice* d) {
  if (d->getInputChannelNames() != inputNames || d->getOutputChannelNames() != outputNames)
    reconcileChannels(d->getInputChannelNames(), d->getOutputChannelNames());
  const juce::ScopedLock guard(lock); rate = d->getCurrentSampleRate(); frames = d->getCurrentBufferSizeSamples();
  for (auto& channel : channels) for (auto& slot : channel->slots) prepare(slot);
  juce::MessageManager::callAsync([this, token = alive] { if (token->load() && changed) changed(); });
}
void Host::audioDeviceStopped() { rebuildRequested = true; }
void Host::audioDeviceError(const juce::String& message) {
  rebuildRequested = true;
  juce::MessageManager::callAsync([this, token = alive, message] {
    if (!token->load()) return;
    if (persistent) logAudioError(driverName, message);
    error = audioErrorText(message); if (changed) changed();
  });
}
void Host::audioDeviceIOCallbackWithContext(const float* const* input, int inputs, float* const* output, int outputs, int count, const juce::AudioIODeviceCallbackContext&) {
  const juce::ScopedNoDenormals noDenormals;
  ++callbackCount;
  const juce::ScopedTryLock guard(lock);
  if (!guard.isLocked() || count > 8192 || count <= 0) {
    for (int i = 0; i < outputs; ++i) if (output[i] && count > 0) juce::FloatVectorOperations::clear(output[i], count);
    return;
  }
  systemLoopback.read(count);
  processAudio(input, inputs, output, outputs, count, systemLoopback.signals());
}
void Host::processAudio(const float* const* input, int inputs, float* const* output, int outputs, int count, std::span<const float* const> system) {
  for (int i = 0; i < outputs; ++i) if (output[i]) juce::FloatVectorOperations::clear(output[i], count);
  for (auto& channel : channels) channel->signal.clear(0, count);
  for (int index : order) {
    auto& channel = *channels[index];
    const int bindings[] = {channel.input, channel.inputRight};
    if (channel.loopback) for (int side = 0; side < 2; ++side) {
      const int source = plans[index].systemInputs[side];
      if (juce::isPositiveAndBelow(source, static_cast<int>(system.size())) && system[source]) channel.signal.addFrom(side, 0, system[source], count);
    }
    for (int side = 0; !channel.loopback && side < 2; ++side) {
      const int binding = bindings[side];
      if (binding >= 0 && binding < inputs && input[binding]) channel.signal.addFrom(side, 0, input[binding], count);
    }
    for (const auto& patch : plans[index].patches) {
      channel.signal.addFrom(patch.destinationSide, 0, channels[patch.source]->signal, patch.sourceSide, 0, count);
    }
    // All upstream channels are already processed, including sidechain sources.
    for (const auto& incoming : plans[index].main) {
      const auto& route = channels[incoming.source]->sends[incoming.send];
      const auto& signal = route.preFx ? channels[incoming.source]->preFxSignal : channels[incoming.source]->signal;
      for (int side = 0; side < 2; ++side)
        channel.signal.addFrom(side, 0, signal, side, 0, count, route.gain);
    }
    for (int side = 0; side < 2; ++side) channel.preFxSignal.copyFrom(side, 0, channel.signal, side, 0, count);
    if (channel.mute.load()) channel.preFxSignal.clear(0, count);
    for (int s = 0; s < slotCount; ++s) {
      auto& slot = channel.slots[s]; if (!slot.plugin || !slot.enabled.load()) continue;
      auto& plugin = *slot.plugin; slot.work.clear(0, count);
      juce::AudioBuffer<float> work(slot.work.getArrayOfWritePointers(), slot.work.getNumChannels(), count);
      auto main = plugin.getBusBuffer(work, true, 0);
      for (int side = 0; side < main.getNumChannels(); ++side) main.copyFrom(side, 0, channel.signal, std::min(side, 1), 0, count);
      for (const auto& incoming : plans[index].sidechains[s]) {
        const auto& route = channels[incoming.source]->sends[incoming.send];
        if (route.bus <= 0 || route.bus >= plugin.getBusCount(true)) continue;
        auto bus = plugin.getBusBuffer(work, true, route.bus);
        const auto& signal = route.preFx ? channels[incoming.source]->preFxSignal : channels[incoming.source]->signal;
        for (int side = 0; side < bus.getNumChannels(); ++side) bus.addFrom(side, 0, signal, std::min(side, 1), 0, count, route.gain);
      }
      midi.clear(); plugin.processBlock(work, midi);
      auto result = plugin.getBusBuffer(work, false, 0);
      channel.signal.clear(0, count);
      if (result.getNumChannels()) for (int side = 0; side < 2; ++side) channel.signal.copyFrom(side, 0, result, std::min(side, result.getNumChannels() - 1), 0, count);
    }
    for (int side = 0; side < 2; ++side) {
      channel.signal.applyGain(side, 0, count, channel.mute.load() ? 0 : gainForPosition(channel.fader.load()));
      channel.peak[side] = channel.signal.getMagnitude(side, 0, count);
    }
    const int destinations[] = {channel.output, channel.outputRight};
    for (int side = 0; side < 2 && !channel.loopback; ++side) {
      const int binding = destinations[side];
      if (binding >= 0 && binding < outputs && output[binding])
        juce::FloatVectorOperations::add(output[binding], channel.signal.getReadPointer(side), count);
    }
  }
}
void Host::save() {
  if (!persistent) return;
  saveProject(stateFile());
  if (auto xml = catalog.createXml()) xml->writeTo(catalogFile());
}
std::unique_ptr<juce::XmlElement> Host::projectXml() {
  const juce::ScopedLock guard(lock);
  auto xml = std::make_unique<juce::XmlElement>("MINI_DAW"); auto& root = *xml;
  root.setAttribute("version", 4); root.setAttribute("driver", driverName);
  root.setAttribute("windowHeight", windowHeight);
  root.setAttribute("bufferSize", requestedFrames); root.setAttribute("sampleRate", requestedRate);
  const auto& inputs = savedInputs.isEmpty() ? inputNames : savedInputs;
  const auto& outputs = savedOutputs.isEmpty() ? outputNames : savedOutputs;
  for (const auto& name : inputs) root.createNewChildElement("ASIO_INPUT")->setAttribute("name", name);
  for (const auto& name : outputs) root.createNewChildElement("ASIO_OUTPUT")->setAttribute("name", name);
  for (const auto& [key, source] : systemSources) {
    auto* item = root.createNewChildElement("SYSTEM_LOOPBACK"); item->setAttribute("key", key);
    item->setAttribute("endpoint", source.endpoint); item->setAttribute("endpointName", source.name);
    item->setAttribute("channel", source.channel); item->setAttribute("includeHost", source.includeHost);
  }
  for (const auto& channel : channels) root.addChildElement(channelXml(*channel).release());
  return xml;
}
std::unique_ptr<juce::XmlElement> Host::channelXml(const Channel& value) {
    const auto* channel = &value;
    auto xml = std::make_unique<juce::XmlElement>("CHANNEL"); auto* c = xml.get(); c->setAttribute("name", channel->name);
    c->setAttribute("loopback", channel->loopback);
    if (channel->loopback) c->setAttribute("loopbackInputSource", channel->windowsInput.endpoint.isNotEmpty() ? "windows-output" : "asio-output");
    if (channel->loopback && channel->windowsInput.endpoint.isNotEmpty()) {
      c->setAttribute("windowsEndpoint", channel->windowsInput.endpoint); c->setAttribute("windowsEndpointName", channel->windowsInput.name);
      c->setAttribute("windowsLeft", channel->windowsInput.left); c->setAttribute("windowsRight", channel->windowsInput.right);
    }
    c->setAttribute("colour", channel->colour.toString()); c->setAttribute("colourAssigned", channel->colourAssigned);
    c->setAttribute("stripWidth", channel->stripWidth); c->setAttribute("stripHeight", channel->stripHeight); c->setAttribute("stripScale", channel->stripScale);
    c->setAttribute("input", channel->input); c->setAttribute("inputR", channel->inputRight);
    c->setAttribute("output", channel->output); c->setAttribute("outputR", channel->outputRight);
    const std::array<int, 4> indices{channel->input, channel->inputRight, channel->output, channel->outputRight};
    for (int i = 0; i < 4; ++i) {
      auto binding = channel->asioBindings[i];
      if (binding.name.isEmpty()) {
        const bool inputPort = (i < 2) != channel->loopback;
        const auto& names = inputPort ? (savedInputs.isEmpty() ? inputNames : savedInputs) : (savedOutputs.isEmpty() ? outputNames : savedOutputs);
        binding = AsioBinding::fromIndex(indices[i], names, layoutDriver.isEmpty() ? driverName : layoutDriver);
      }
      if (binding.name.isEmpty()) continue;
      auto* item = c->createNewChildElement("ASIO_BINDING"); item->setAttribute("side", i);
      item->setAttribute("driver", binding.driver); item->setAttribute("name", binding.name); item->setAttribute("occurrence", binding.occurrence);
    }
    c->setAttribute("fader", channel->fader.load()); c->setAttribute("faderDb", std::isfinite(positionToDb(channel->fader.load())) ? positionToDb(channel->fader.load()) : -1000.0f); c->setAttribute("mute", channel->mute.load());
    for (int i = 0; i < slotCount; ++i) {
      const auto& slot = channel->slots[i]; if (!slot.plugin) continue;
      auto* p = c->createNewChildElement("SLOT"); p->setAttribute("index", i); p->setAttribute("enabled", slot.enabled.load());
      p->setAttribute("editorScale", slot.editorScale); p->setAttribute("editorWidth", slot.editorWidth); p->setAttribute("editorHeight", slot.editorHeight);
      p->addChildElement(slot.description.createXml().release());
      juce::MemoryBlock state; slot.plugin->getStateInformation(state); p->setAttribute("state", state.toBase64Encoding());
    }
    for (const auto& route : channel->sends) {
      auto* r = c->createNewChildElement("SEND"); r->setAttribute("target", route.target); r->setAttribute("slot", route.slot); r->setAttribute("bus", route.bus); r->setAttribute("gain", route.gain);
      r->setAttribute("preFx", route.preFx);
    }
  return xml;
}
bool Host::saveProject(const juce::File& file) {
  auto root = projectXml();
  file.getParentDirectory().createDirectory();
  if (root->writeTo(file)) return true;
  error = utf8("无法保存项目预设：")+file.getFullPathName(); return false;
}
void Host::restore() {
  auto root = juce::XmlDocument::parse(stateFile()); if (!root || !root->hasTagName("MINI_DAW")) return;
  restoreXml(*root);
}
bool Host::loadProject(const juce::File& file) {
  auto root = juce::XmlDocument::parse(file);
  if (!root || !root->hasTagName("MINI_DAW") || root->getChildByName("CHANNEL") == nullptr) {
    error = utf8("不是有效的 Mini DAW 项目预设"); return false;
  }
  const bool resume = wantAudio;
  pendingEdit.reset(); undoHistory.clear(); redoHistory.clear();
  stop(); error.clear(); channels.clear(); restoreXml(*root);
  if (channels.empty()) addChannel();
  // A stopped device can reuse its known layout only for the same driver.
  if (layoutDriver.isNotEmpty() && layoutDriver != driverName) reconcileChannels({}, {});
  else if (!inputNames.isEmpty() || !outputNames.isEmpty()) reconcileChannels(inputNames, outputNames, layoutDriver);
  if (resume) start();
  recentPresets.addFile(file);
  if (persistent) {
    const auto history = stateFile().getSiblingFile("recent-presets.txt");
    history.getParentDirectory().createDirectory(); history.replaceWithText(recentPresets.toString());
  }
  if (changed) changed();
  return true;
}
void Host::restoreXml(const juce::XmlElement& xml) {
  const auto* root = &xml;
  savedInputs.clear(); savedOutputs.clear();
  systemSources.clear();
  for (auto* item : root->getChildWithTagNameIterator("SYSTEM_LOOPBACK")) {
    const auto key = item->getStringAttribute("key");
    if (key.isNotEmpty()) systemSources[key] = {item->getStringAttribute("endpoint"), juce::jlimit(0, 63, item->getIntAttribute("channel")), item->getBoolAttribute("includeHost", true), item->getStringAttribute("endpointName")};
  }
  for (auto* c : root->getChildWithTagNameIterator("ASIO_INPUT")) savedInputs.add(c->getStringAttribute("name"));
  for (auto* c : root->getChildWithTagNameIterator("ASIO_OUTPUT")) savedOutputs.add(c->getStringAttribute("name"));
  needsDefaultInput = false;
  driverName = root->getStringAttribute("driver", "Aggregate Audio"); requestedRate = root->getDoubleAttribute("sampleRate", 0);
  requestedFrames = juce::jlimit(0, 8192, root->getIntAttribute("bufferSize"));
  windowHeight = juce::jlimit(550, 2100, root->getIntAttribute("windowHeight", 751));
  for (auto* c : root->getChildWithTagNameIterator("CHANNEL")) {
    if (!addChannel()) break;
    restoreChannel(static_cast<int>(channels.size())-1, *c, savedInputs, savedOutputs, driverName);
  }
  // Discard invalid session edges and cycles instead of enabling feedback on startup.
  for (auto& ch : channels) for (auto& r : ch->sends)
    if (r.target >= static_cast<int>(channels.size()) || r.slot >= slotCount || r.slot < -1 || r.bus < 0 || !std::isfinite(r.gain)) r = {};
  rebuildOrder(); if (order.empty()) { for (auto& ch : channels) ch->sends.clear(); rebuildOrder(); }
}
void Host::restoreChannel(int channelIndex, const juce::XmlElement& element, const juce::StringArray& inputs, const juce::StringArray& outputs, const juce::String& driver) {
    const auto* c = &element;
    auto& ch = *channels[channelIndex]; ch.loopback = c->getBoolAttribute("loopback"); ch.name = c->getStringAttribute("name", ch.name);
    ch.stripWidth = juce::jlimit(176, 460, c->getIntAttribute("stripWidth", 194));
    ch.stripHeight = juce::jlimit(0, 2160, c->getIntAttribute("stripHeight"));
    ch.stripScale = juce::jlimit(0.6f, 2.0f, static_cast<float>(c->getDoubleAttribute("stripScale", 1)));
    ch.input = c->getIntAttribute("input", -1); ch.inputRight = c->getIntAttribute("inputR", -1);
    ch.windowsInput = {};
    if (ch.loopback && c->getStringAttribute("loopbackInputSource") == "windows-output")
      ch.windowsInput = {c->getStringAttribute("windowsEndpoint"), c->getStringAttribute("windowsEndpointName"),
        juce::jlimit(0, 63, c->getIntAttribute("windowsLeft")), juce::jlimit(0, 63, c->getIntAttribute("windowsRight", 1))};
    if (ch.loopback && c->getStringAttribute("loopbackInputSource") != "asio-output") ch.input = ch.inputRight = -1;
    ch.output = c->getIntAttribute("output", -1); ch.outputRight = c->getIntAttribute("outputR", -1);
    ch.asioBindings = {
      AsioBinding::fromIndex(ch.input, ch.loopback ? outputs : inputs, driver),
      AsioBinding::fromIndex(ch.inputRight, ch.loopback ? outputs : inputs, driver),
      AsioBinding::fromIndex(ch.output, ch.loopback ? inputs : outputs, driver),
      AsioBinding::fromIndex(ch.outputRight, ch.loopback ? inputs : outputs, driver)};
    for (auto* item : c->getChildWithTagNameIterator("ASIO_BINDING")) {
      const int side = item->getIntAttribute("side", -1);
      if (juce::isPositiveAndBelow(side, 4)) ch.asioBindings[side] = {item->getStringAttribute("driver", driver),
        item->getStringAttribute("name"), juce::jmax(0, item->getIntAttribute("occurrence"))};
    }
    if (ch.loopback && c->getStringAttribute("loopbackInputSource") != "asio-output") ch.asioBindings[0] = ch.asioBindings[1] = {};
    const float oldPosition = static_cast<float>(c->getDoubleAttribute("fader", c->getDoubleAttribute("left", 72.0/84.0)));
    const float savedDb = static_cast<float>(c->getDoubleAttribute("faderDb", oldPosition <= 0 ? -1000.0 : -72.0+84.0*oldPosition));
    ch.fader = savedDb <= -1000 ? 0 : dbToPosition(savedDb); ch.mute = c->getBoolAttribute("mute");
    for (auto* p : c->getChildWithTagNameIterator("SLOT")) {
      int index = p->getIntAttribute("index", -1); auto* desc = p->getChildByName("PLUGIN");
      if (!juce::isPositiveAndBelow(index, slotCount) || !desc) continue;
      juce::PluginDescription d; if (!d.loadFromXml(*desc)) continue;
      juce::MemoryBlock state; state.fromBase64Encoding(p->getStringAttribute("state"));
      if (loadPlugin(channelIndex, index, d, &state)) {
        auto& slot = ch.slots[index]; slot.enabled = p->getBoolAttribute("enabled", true);
        slot.editorScale = juce::jlimit(0.75f, 2.5f, static_cast<float>(p->getDoubleAttribute("editorScale", 1)));
        slot.editorWidth = p->getIntAttribute("editorWidth"); slot.editorHeight = p->getIntAttribute("editorHeight");
      }
    }
    for (auto* r : c->getChildWithTagNameIterator("SEND")) ch.sends.push_back({r->getIntAttribute("target", -1), r->getIntAttribute("slot", -1), r->getIntAttribute("bus"), static_cast<float>(r->getDoubleAttribute("gain")), r->getBoolAttribute("preFx")});
    if (c->hasAttribute("colour")) { ch.colour = juce::Colour::fromString(c->getStringAttribute("colour")); ch.colourAssigned = c->getBoolAttribute("colourAssigned", true); }
}
bool Host::moveChannel(int from, int to) {
  const juce::ScopedLock guard(lock);
  if (!juce::isPositiveAndBelow(from, static_cast<int>(channels.size()))
      || !juce::isPositiveAndBelow(to, static_cast<int>(channels.size())) || from == to) return false;
  auto channel = std::move(channels[from]); channels.erase(channels.begin()+from);
  channels.insert(channels.begin()+to, std::move(channel));
  for (auto& c : channels) for (auto& r : c->sends) r.target = movedChannelIndex(r.target, from, to);
  rebuildOrder(); syncSystemLoopback(); return true;
}
bool Host::swapChannels(int a, int b) {
  const juce::ScopedLock guard(lock);
  if (!juce::isPositiveAndBelow(a, static_cast<int>(channels.size())) || !juce::isPositiveAndBelow(b, static_cast<int>(channels.size())) || a == b) return false;
  std::swap(channels[a], channels[b]);
  for (auto& c : channels) for (auto& r : c->sends) r.target = swappedIndex(r.target, a, b);
  rebuildOrder(); syncSystemLoopback(); return true;
}
juce::String Host::copyChannel(int index) {
  const juce::ScopedLock guard(lock);
  juce::XmlElement root("MINI_DAW_CHANNEL");
  root.setAttribute("driver", driverName);
  root.addChildElement(channelXml(*channels[index]).release());
  for (const auto& name : inputNames) root.createNewChildElement("ASIO_INPUT")->setAttribute("name", name);
  for (const auto& name : outputNames) root.createNewChildElement("ASIO_OUTPUT")->setAttribute("name", name);
  return root.toString();
}
bool Host::pasteChannel(int index, const juce::String& text) {
  auto xml = juce::XmlDocument::parse(text);
  if (!xml || !xml->hasTagName("MINI_DAW_CHANNEL") || !xml->getChildByName("CHANNEL")) {
    error = utf8("剪贴板中没有 Mini DAW 通道设置"); return false;
  }
  const juce::ScopedLock pasteGuard(lock);
  error.clear();
  // Keep the Channel object alive: the meter and UI may still reference it.
  for (int s = 0; s < slotCount; ++s) removePlugin(index, s);
  { const juce::ScopedLock guard(lock); channels[index]->sends.clear(); }
  juce::StringArray previousInputs, previousOutputs;
  for (auto* p : xml->getChildWithTagNameIterator("ASIO_INPUT")) previousInputs.add(p->getStringAttribute("name"));
  for (auto* p : xml->getChildWithTagNameIterator("ASIO_OUTPUT")) previousOutputs.add(p->getStringAttribute("name"));
  restoreChannel(index, *xml->getChildByName("CHANNEL"), previousInputs, previousOutputs, xml->getStringAttribute("driver", driverName));
  const juce::ScopedLock guard(lock);
  auto& c = *channels[index];
  const auto currentDriver = layoutDriver.isEmpty() ? driverName : layoutDriver;
  c.input = c.asioBindings[0].resolve(c.loopback ? outputNames : inputNames, currentDriver);
  c.inputRight = c.asioBindings[1].resolve(c.loopback ? outputNames : inputNames, currentDriver);
  c.output = c.asioBindings[2].resolve(c.loopback ? inputNames : outputNames, currentDriver);
  c.outputRight = c.asioBindings[3].resolve(c.loopback ? inputNames : outputNames, currentDriver);
  auto routes = std::move(c.sends); c.sends.clear(); rebuildOrder();
  int skipped = 0;
  if (order.empty() && c.loopback) {
    c.output = c.outputRight = -1; rebuildOrder(); ++skipped;
  }
  for (int i = 0; i < static_cast<int>(routes.size()); ++i) {
    const auto& r = routes[i];
    if (r.target < 0) { c.sends.resize(i+1); c.sends[i] = r; continue; }
    if (!setRoute(index, i, r)) ++skipped;
  }
  if (skipped) error = utf8("通道已粘贴，跳过 ")+juce::String(skipped)+utf8(" 个无效或反馈连接");
  syncSystemLoopback();
  return true;
}
void Host::swapSlots(Slot& a, Slot& b) {
  std::swap(a.plugin, b.plugin); std::swap(a.work, b.work); std::swap(a.description, b.description);
  std::swap(a.editorScale, b.editorScale); std::swap(a.editorWidth, b.editorWidth); std::swap(a.editorHeight, b.editorHeight);
  const bool enabled = a.enabled; a.enabled = b.enabled.load(); b.enabled = enabled;
}
bool Host::movePlugin(int channel, int from, int insertion) {
  const int to = juce::jlimit(0, slotCount-1, insertion > from ? insertion-1 : insertion);
  if (!juce::isPositiveAndBelow(from, slotCount) || from == to) return false;
  const juce::ScopedLock guard(lock); auto& slots = channels[channel]->slots;
  if (!slots[from].plugin) return false;
  for (int i = from; i != to; i += to > from ? 1 : -1) swapSlots(slots[i], slots[i+(to > from ? 1 : -1)]);
  for (auto& c : channels) for (auto& r : c->sends) if (r.target == channel && r.slot >= 0) r.slot = movedChannelIndex(r.slot, from, to);
  rebuildOrder();
  return true;
}
bool Host::swapPlugins(int source, int slot, int target, int other) {
  const juce::ScopedLock guard(lock);
  if (!juce::isPositiveAndBelow(source, static_cast<int>(channels.size())) || !juce::isPositiveAndBelow(target, static_cast<int>(channels.size()))
      || !juce::isPositiveAndBelow(slot, slotCount) || !juce::isPositiveAndBelow(other, slotCount) || (source == target && slot == other)) return false;
  if (!channels[source]->slots[slot].plugin) return false;
  auto remap = [&] {
    for (auto& c : channels) for (auto& r : c->sends) {
      if (r.target == source && r.slot == slot) { r.target = target; r.slot = other; }
      else if (r.target == target && r.slot == other) { r.target = source; r.slot = slot; }
    }
    rebuildOrder();
  };
  remap();
  if (order.empty()) { remap(); error = utf8("交换插件会形成侧链反馈环路，未交换。"); return false; }
  swapSlots(channels[source]->slots[slot], channels[target]->slots[other]); error.clear(); return true;
}
bool Host::copyPlugin(int source, int slot, int target, int insertion) {
  insertion = juce::jlimit(0, slotCount-1, insertion);
  auto& origin = channels[source]->slots[slot];
  if (!origin.plugin) return false;
  int empty = insertion;
  while (empty < slotCount && channels[target]->slots[empty].plugin) ++empty;
  if (empty == slotCount) { error = utf8("目标插入位置之后没有空槽，无法插入插件"); return false; }
  juce::MemoryBlock state;
  { const juce::ScopedLock guard(lock); origin.plugin->getStateInformation(state); }
  const auto description = origin.description; const bool enabled = origin.enabled;
  if (!loadPlugin(target, empty, description, &state)) return false;
  const juce::ScopedLock guard(lock); auto& slots = channels[target]->slots; slots[empty].enabled = enabled;
  slots[empty].editorScale = origin.editorScale; slots[empty].editorWidth = origin.editorWidth; slots[empty].editorHeight = origin.editorHeight;
  for (int i = empty; i > insertion; --i) swapSlots(slots[i], slots[i-1]);
  for (auto& c : channels) for (auto& r : c->sends) if (r.target == target && r.slot >= 0) r.slot = movedChannelIndex(r.slot, empty, insertion);
  rebuildOrder();
  return true;
}
bool Host::replaceWithCopy(int source, int slot, int target, int other) {
  auto& origin = channels[source]->slots[slot]; if (!origin.plugin || source == target) return false;
  juce::MemoryBlock state;
  { const juce::ScopedLock guard(lock); origin.plugin->getStateInformation(state); }
  const auto description = origin.description; const bool enabled = origin.enabled;
  const float scale = origin.editorScale; const int width = origin.editorWidth, height = origin.editorHeight;
  if (!loadPlugin(target, other, description, &state)) return false;
  auto& destination = channels[target]->slots[other]; destination.enabled = enabled;
  destination.editorScale = scale; destination.editorWidth = width; destination.editorHeight = height; return true;
}
Host::EditState Host::captureEditState() {
  const juce::ScopedLock guard(lock); EditState state;
  state.inputs = inputNames; state.outputs = outputNames;
  state.windowHeight = windowHeight; state.systemSources = systemSources;
  for (const auto& c : channels) {
    EditChannel snapshot{c->name, c->loopback, c->colour, c->colourAssigned, c->mute.load(), c->input, c->inputRight, c->output, c->outputRight, c->fader.load(), c->stripWidth, c->stripHeight, c->stripScale, c->sends, {}, c->windowsInput, c->asioBindings};
    for (int s = 0; s < slotCount; ++s) {
      const auto& slot = c->slots[s];
      snapshot.slots[s] = {slot.plugin, slot.description, slot.enabled.load(), slot.editorScale, slot.editorWidth, slot.editorHeight};
    }
    state.channels.push_back(std::move(snapshot));
  }
  return state;
}
bool Host::sameEditState(const EditState& a, const EditState& b) {
  if (a.channels.size() != b.channels.size() || a.windowHeight != b.windowHeight || a.systemSources != b.systemSources) return false;
  for (size_t i = 0; i < a.channels.size(); ++i) {
    const auto& x = a.channels[i]; const auto& y = b.channels[i];
    if (x.name != y.name || x.loopback != y.loopback || x.colour != y.colour || x.colourAssigned != y.colourAssigned || x.mute != y.mute || x.fader != y.fader
        || x.input != y.input || x.inputRight != y.inputRight || x.output != y.output || x.outputRight != y.outputRight || x.sends.size() != y.sends.size()
        || x.stripWidth != y.stripWidth || x.stripHeight != y.stripHeight || x.stripScale != y.stripScale || x.windowsInput != y.windowsInput || x.asioBindings != y.asioBindings) return false;
    for (size_t r = 0; r < x.sends.size(); ++r) {
      const auto& p = x.sends[r]; const auto& q = y.sends[r];
      if (p.target != q.target || p.slot != q.slot || p.bus != q.bus || p.gain != q.gain || p.preFx != q.preFx) return false;
    }
    for (int s = 0; s < slotCount; ++s) {
      const auto& p = x.slots[s]; const auto& q = y.slots[s];
      if (p.plugin != q.plugin || p.enabled != q.enabled || p.scale != q.scale || p.width != q.width || p.height != q.height) return false;
    }
  }
  return true;
}
void Host::beginEdit() { if (!pendingEdit) pendingEdit = std::make_unique<EditState>(captureEditState()); }
void Host::endEdit(const juce::String& label) {
  if (!pendingEdit) return;
  auto before = std::move(*pendingEdit); pendingEdit.reset(); auto after = captureEditState();
  if (sameEditState(before, after)) return;
  redoHistory.clear(); undoHistory.push_back({std::move(before), std::move(after), label});
  if (undoHistory.size() > 32) undoHistory.erase(undoHistory.begin());
}
void Host::applyEditState(const EditState& state) {
  const juce::ScopedLock guard(lock);
  windowHeight = state.windowHeight; systemSources = state.systemSources;
  channels.clear();
  for (const auto& c : state.channels) {
    auto channel = std::make_unique<Channel>();
    channel->name = c.name; channel->loopback = c.loopback; channel->colour = c.colour; channel->colourAssigned = c.colourAssigned; channel->mute = c.mute;
    channel->input = c.input; channel->inputRight = c.inputRight; channel->output = c.output; channel->outputRight = c.outputRight;
    channel->windowsInput = c.windowsInput;
    channel->asioBindings = c.asioBindings;
    channel->fader = c.fader; channel->sends = c.sends;
    channel->stripWidth = c.stripWidth; channel->stripHeight = c.stripHeight; channel->stripScale = c.stripScale;
    for (int s = 0; s < slotCount; ++s) {
      const auto& snapshot = c.slots[s]; auto& slot = channel->slots[s];
      slot.plugin = snapshot.plugin; slot.description = snapshot.description; slot.enabled = snapshot.enabled;
      slot.editorScale = snapshot.scale; slot.editorWidth = snapshot.width; slot.editorHeight = snapshot.height;
      prepare(slot);
    }
    channels.push_back(std::move(channel));
  }
  savedInputs = state.inputs; savedOutputs = state.outputs; reconcileChannels(inputNames, outputNames, layoutDriver);
  rebuildOrder(); syncSystemLoopback(); error.clear();
}
bool Host::undo() {
  if (!canUndo() || pendingEdit) return false;
  auto entry = std::move(undoHistory.back()); undoHistory.pop_back(); applyEditState(entry.before); redoHistory.push_back(std::move(entry)); return true;
}
bool Host::redo() {
  if (!canRedo() || pendingEdit) return false;
  auto entry = std::move(redoHistory.back()); redoHistory.pop_back(); applyEditState(entry.after); undoHistory.push_back(std::move(entry)); return true;
}
}
