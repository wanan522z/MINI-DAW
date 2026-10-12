#pragma once
#include "mini_daw_host.h"

namespace mini {
// Deterministic host checks: no hardware capture or app-data writes.
inline int verifyHost(const juce::String& pluginPath, const juce::File& reportFile) {
  Host host(false);
  juce::XmlElement report("HOST_VERIFICATION"); bool passed = true;
  auto check = [&](const juce::String& name, bool condition) {
    auto* item = report.createNewChildElement("CHECK"); item->setAttribute("name", name); item->setAttribute("passed", condition); passed &= condition;
  };
  check("default 12 channels", host.channels.size() == 12);
  auto& source = *host.channels[0]; auto& receiver = *host.channels[1];
  source.input = source.inputRight = 0; source.output = 0; receiver.output = 1;
  check("connect main send", host.setRoute(0, 0, {1, -1, 0, 0.25f}));
  std::array<float, 128> input{}, a{}, b{};
  for (int i = 0; i < 128; ++i) input[i] = 0.2f*std::sin(static_cast<float>(i)*0.13f);
  const float* ins[] = {input.data()}; float* outs[] = {a.data(), b.data()};
  juce::AudioIODeviceCallbackContext context{};
  host.audioDeviceIOCallbackWithContext(ins, 1, outs, 2, 128, context);
  check("direct ASIO channel mapping", std::abs(a[32]-input[32]) < 0.000001f);
  check("send amount in audio path", std::abs(b[32]-input[32]*0.25f) < 0.000001f);
  source.fader = 0;
  host.audioDeviceIOCallbackWithContext(ins, 1, outs, 2, 128, context);
  check("minus infinity mutes direct and sends", a[32] == 0 && b[32] == 0);
  source.fader = dbToPosition(0); source.mute = true;
  host.audioDeviceIOCallbackWithContext(ins, 1, outs, 2, 128, context);
  check("channel mute", a[32] == 0 && b[32] == 0); source.mute = false;
  check("reject feedback cycle", !host.setRoute(1, 0, {0, -1, 0, 1}));
  host.removeRoute(0, 0);
  check("second knob keeps its index", host.setRoute(0, 1, {1, -1, 0, 0.5f}) && source.sends.size() == 2 && source.sends[0].target == -1);
  check("third knob appears", sendControlCount(static_cast<int>(source.sends.size())) == 3);
  if (pluginPath.isNotEmpty()) {
    juce::OwnedArray<juce::PluginDescription> types;
    host.formats.getFormat(0)->findAllTypesForFile(types, pluginPath);
    const juce::PluginDescription* effect = nullptr;
    for (const auto* d : types) if (!d->isInstrument && d->numInputChannels > 0 && d->numOutputChannels > 0) { effect = d; break; }
    check("real VST3 effect discovered", effect != nullptr);
    if (effect) {
      report.setAttribute("plugin", effect->name);
      bool loaded = host.loadPlugin(1, 0, *effect); check("real VST3 instance loaded", loaded);
      if (loaded) {
        auto& slot = receiver.slots[0];
        int aux = slot.plugin->getBusCount(true)-1; report.setAttribute("sidechainBuses", aux);
        if (aux > 0) {
          check("actual sidechain bus enabled", slot.plugin->getBus(true, 1)->isEnabled());
          check("connect actual sidechain", host.setRoute(0, 2, {1, 0, 1, 1}));
          report.setAttribute("SC1", slot.plugin->getBus(true, 1)->getName());
        }
        for (int block = 0; block < 30; ++block) host.audioDeviceIOCallbackWithContext(ins, 1, outs, 2, 128, context);
        bool finite = true; for (float sample : b) finite &= std::isfinite(sample);
        check("real plugin processes finite audio", finite);
        auto* editor = slot.plugin->createEditorIfNeeded();
        check("real plugin editor created", editor && editor->getWidth() > 0 && editor->getHeight() > 0); delete editor;
        juce::MemoryBlock state; slot.plugin->getStateInformation(state); check("plugin state captured", state.getSize() > 0);
        slot.enabled = false;
        host.audioDeviceIOCallbackWithContext(ins, 1, outs, 2, 128, context);
        check("slot bypass preserves main send", std::abs(b[32]-input[32]*0.5f) < 0.000001f);
        host.removePlugin(1, 0);
        check("unload removes sidechain routes", source.sends.size() == 2 && !receiver.slots[0].plugin);
      } else report.setAttribute("error", host.error);
    }
  }
  report.setAttribute("passed", passed); reportFile.getParentDirectory().createDirectory(); report.writeTo(reportFile);
  return passed ? 0 : 1;
}
}
