#pragma once
#include "mini_daw_host.h"

namespace mini {
// A predictable effect lets the host checks distinguish both audio taps and
// exercise a real auxiliary bus without requiring a licensed external plugin.
class VerificationEffect final : public juce::AudioPluginInstance {
public:
  explicit VerificationEffect(bool sidechain = false)
    : AudioPluginInstance(BusesProperties().withInput("Main", juce::AudioChannelSet::stereo(), true)
        .withInput("Sidechain", juce::AudioChannelSet::stereo(), true)
        .withOutput("Main", juce::AudioChannelSet::stereo(), true)), useSidechain(sidechain) {}
  const juce::String getName() const override { return "Verification effect"; }
  void fillInPluginDescription(juce::PluginDescription& d) const override { d.name = getName(); }
  void prepareToPlay(double, int) override {}
  void releaseResources() override {}
  void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override {
    auto main = getBusBuffer(buffer, false, 0);
    if (useSidechain) {
      auto aux = getBusBuffer(buffer, true, 1);
      for (int side = 0; side < 2; ++side) main.copyFrom(side, 0, aux, side, 0, buffer.getNumSamples());
    } else main.applyGain(0.5f);
  }
  double getTailLengthSeconds() const override { return 0; }
  bool acceptsMidi() const override { return false; }
  bool producesMidi() const override { return false; }
  bool hasEditor() const override { return false; }
  juce::AudioProcessorEditor* createEditor() override { return nullptr; }
  int getNumPrograms() override { return 1; }
  int getCurrentProgram() override { return 0; }
  void setCurrentProgram(int) override {}
  const juce::String getProgramName(int) override { return {}; }
  void changeProgramName(int, const juce::String&) override {}
  void getStateInformation(juce::MemoryBlock&) override {}
  void setStateInformation(const void*, int) override {}
private:
  bool useSidechain;
};
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
  bool immediate = true;
  for (int frames : {17, 128, 63, 32, 128}) {
    host.audioDeviceIOCallbackWithContext(ins, 1, outs, 2, frames, context);
    for (int i = 0; i < frames; ++i) immediate &= std::abs(a[i]-input[i]) < 0.000001f && std::abs(b[i]-input[i]*0.25f) < 0.000001f;
  }
  check("varying blocks and sends add no host buffer latency", immediate);
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
  {
    Host taps(false); taps.inputNames = {"ASIO virtual input"}; taps.outputNames = {"Direct", "Main send", "Sidechain"};
    auto& drySource = *taps.channels[0];
    drySource.input = drySource.inputRight = 0; drySource.output = 0; drySource.fader = dbToPosition(-6);
    auto effect = std::make_shared<VerificationEffect>(); drySource.slots[0].plugin = effect; drySource.slots[0].work.setSize(4, 8192);
    taps.channels[1]->output = 1; taps.channels[2]->output = 2;
    taps.channels[2]->slots[0].plugin = std::make_shared<VerificationEffect>(true); taps.channels[2]->slots[0].work.setSize(4, 8192);
    taps.setRoute(0, 0, {1, -1, 0, 0.25f}); taps.setRoute(0, 1, {2, 0, 1, 0.75f});
    std::array<float, 128> sidechainOutput{}; float* tapOutputs[]{a.data(), b.data(), sidechainOutput.data()};
    taps.audioDeviceIOCallbackWithContext(ins, 1, tapOutputs, 3, 128, context);
    const float wet = input[32]*0.5f*gainForPosition(dbToPosition(-6));
    check("Post-FX default sends processed audio to main and sidechain", !drySource.sends[0].preFx
      && std::abs(a[32]-wet) < 0.000001f && std::abs(b[32]-wet*0.25f) < 0.000001f && std::abs(sidechainOutput[32]-wet*0.75f) < 0.000001f);
    taps.beginEdit(); taps.setSendTap(0, 0, true); taps.setSendTap(0, 1, true); taps.endEdit("signal tap");
    taps.audioDeviceIOCallbackWithContext(ins, 1, tapOutputs, 3, 128, context);
    check("Pre-FX bypasses source effects and fader for both input buses", std::abs(a[32]-wet) < 0.000001f
      && std::abs(b[32]-input[32]*0.25f) < 0.000001f && std::abs(sidechainOutput[32]-input[32]*0.75f) < 0.000001f);
    drySource.mute = true; taps.audioDeviceIOCallbackWithContext(ins, 1, tapOutputs, 3, 128, context);
    check("channel mute also silences Pre-FX sends", b[32] == 0 && sidechainOutput[32] == 0); drySource.mute = false;
    check("send tap can be undone and redone", taps.undo() && !taps.channels[0]->sends[0].preFx && !taps.channels[0]->sends[1].preFx
      && taps.redo() && taps.channels[0]->sends[0].preFx && taps.channels[0]->sends[1].preFx);
    taps.beginEdit(); const bool converted = taps.setLoopbackMode(0, true); taps.endEdit("convert populated channel");
    check("populated channel converts while retaining plugins and sends and clearing devices", converted && taps.channels[0]->slots[0].plugin == effect
      && taps.channels[0]->input == -1 && taps.channels[0]->inputRight == -1 && taps.channels[0]->output == -1 && taps.channels[0]->outputRight == -1
      && taps.channels[0]->sends[0].preFx);
    check("undo conversion restores bindings and the same plugin instance", taps.undo() && !taps.channels[0]->loopback
      && taps.channels[0]->input == 0 && taps.channels[0]->output == 0 && taps.channels[0]->slots[0].plugin == effect);
  }
  if (pluginPath.isNotEmpty()) {
    juce::OwnedArray<juce::PluginDescription> types;
    for (auto* format : host.formats.getFormats())
      if (format->fileMightContainThisPluginType(pluginPath)) format->findAllTypesForFile(types, pluginPath);
    const juce::PluginDescription* effect = nullptr;
    for (const auto* d : types) if (isEffectCandidate(*d)) { effect = d; break; }
    check("real effect discovered in its native format", effect != nullptr);
    if (effect) {
      report.setAttribute("plugin", effect->name);
      juce::OwnedArray<juce::PluginDescription> fastTypes; scanPluginFactory(juce::File(pluginPath), fastTypes);
      const juce::PluginDescription* fast = nullptr;
      for (auto* d : fastTypes) if (d->uniqueId == effect->uniqueId) fast = d;
      check("factory-only scan matches loadable JUCE identity", fast != nullptr);
      if (fast) effect = fast;
      bool loaded = host.loadPlugin(1, 0, *effect); check("real plugin instance loaded", loaded);
      if (loaded && effect->pluginFormatName == "VST") {
        Host fallback(false); fallback.catalog.addType(*effect);
        auto incompatible = *effect; incompatible.pluginFormatName = "VST3"; ++incompatible.uniqueId;
        fallback.catalog.addType(incompatible);
        check("failed preferred format falls back to the actual compatible VST2 effect", fallback.loadPreferredPlugin(0, 0, incompatible)
          && fallback.channels[0]->slots[0].description.pluginFormatName == "VST");
      }
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
        check("reload real plugin", host.loadPlugin(1, 0, *effect));
        receiver.slots[0].enabled = false;
        auto* original = receiver.slots[0].plugin.get();
        check("move plugin without recreating instance", host.movePlugin(1, 0, 3) && receiver.slots[2].plugin.get() == original && !receiver.slots[2].enabled);
        check("copy plugin into another channel", host.copyPlugin(1, 2, 2, 0) && host.channels[2]->slots[0].plugin && host.channels[2]->slots[0].plugin.get() != original && !host.channels[2]->slots[0].enabled);
        if (receiver.slots[2].plugin->getBusCount(true) > 1) {
          check("route to moved plugin", host.setRoute(0, 2, {1, 2, 1, 0.5f}));
          check("plugin reorder keeps sidechain target", host.movePlugin(1, 2, 0) && source.sends[2].slot == 0 && receiver.slots[0].plugin.get() == original);
        } else host.movePlugin(1, 2, 0);
        const auto pluginPreset = reportFile.getSiblingFile(reportFile.getFileNameWithoutExtension()+"-plugin-roundtrip.minidaw");
        check("save real plugin preset", host.saveProject(pluginPreset));
        Host roundtrip(false);
        check("restore real plugin state", roundtrip.loadProject(pluginPreset) && roundtrip.channels[1]->slots[0].plugin && !roundtrip.channels[1]->slots[0].enabled);
        auto retainedSource = roundtrip.channels[1]->slots[0].plugin;
        auto retainedTarget = roundtrip.channels[2]->slots[0].plugin;
        roundtrip.beginEdit(); const bool replaced = roundtrip.replaceWithCopy(1, 0, 2, 0); roundtrip.endEdit("replace copy");
        auto replacement = roundtrip.channels[2]->slots[0].plugin;
        check("copy replace retains source and creates independent destination", replaced && roundtrip.channels[1]->slots[0].plugin == retainedSource
          && replacement && replacement != retainedSource && replacement != retainedTarget && !roundtrip.channels[2]->slots[0].enabled);
        check("undo restores original plugin instance after replacement", roundtrip.undo() && roundtrip.channels[2]->slots[0].plugin == retainedTarget
          && roundtrip.channels[1]->slots[0].plugin == retainedSource);
        check("redo restores copied replacement instance", roundtrip.redo() && roundtrip.channels[2]->slots[0].plugin == replacement);
        roundtrip.beginEdit(); roundtrip.removePlugin(2, 0); roundtrip.endEdit("remove");
        check("undo unload restores exact plugin and state", roundtrip.undo() && roundtrip.channels[2]->slots[0].plugin == replacement && !roundtrip.channels[2]->slots[0].enabled);
        auto* copy = host.channels[2]->slots[0].plugin.get();
        check("plugin body swap keeps both instances", host.swapPlugins(1, 0, 1, 3) && receiver.slots[3].plugin.get() == original && !receiver.slots[0].plugin);
        if (aux > 0) check("plugin swap follows sidechain", source.sends[2].slot == 3);
        check("cross channel occupied body swaps instead of discarding", host.swapPlugins(1, 3, 2, 0) && receiver.slots[3].plugin.get() == copy && host.channels[2]->slots[0].plugin.get() == original);
        if (aux > 0) check("sidechain follows plugin across channels", source.sends[2].target == 2 && source.sends[2].slot == 0);
        host.swapPlugins(1, 3, 2, 0); host.swapPlugins(1, 3, 1, 0);
        if (aux > 0) {
          host.setRoute(2, 0, {1, 0, 1, 0.5f});
          check("cross channel swap rejects resulting feedback and retains instances", !host.swapPlugins(1, 0, 2, 0)
            && receiver.slots[0].plugin.get() == original && host.channels[2]->slots[0].plugin.get() == copy && host.channels[2]->sends[0].target == 1);
          host.removeRoute(2, 0);
        }
      } else report.setAttribute("error", host.error);
    }
  }
  Host patch(false); patch.inputNames = {"Mic", "Patch"}; patch.outputNames = {"Speaker L", "Speaker R", "Any output"};
  check("blank channel converts to loopback", patch.setLoopbackMode(3, true));
  patch.channels[0]->input = patch.channels[0]->inputRight = 0;
  patch.channels[1]->input = patch.channels[1]->inputRight = 1; patch.channels[1]->output = 1;
  check("loopback output selects internal input destination", patch.setBindings(3, -1, -1, 1, -1));
  patch.channels[3]->fader = dbToPosition(-6);
  check("loopback supports regular sends", patch.setRoute(0, 0, {3, -1, 0, 0.5f}));
  std::array<float,128> silent{}; const float* patchInputs[]{input.data(), silent.data()};
  patch.audioDeviceIOCallbackWithContext(patchInputs, 2, outs, 2, 128, context);
  check("input jumpers follow processed signal before destination plugins", std::abs(b[32]-input[32]*0.5f*gainForPosition(dbToPosition(-6))) < 0.000001f);
  check("loopback input jumpers reject feedback", !patch.setRoute(1, 0, {0, -1, 0, 1}));
  patch.removeRoute(0, 0);
  check("ASIO output capture is configured through channel bindings", patch.setBindings(0, 0, 0, 2, -1) && patch.setBindings(3, 2, 2, 1, -1));
  patch.audioDeviceIOCallbackWithContext(patchInputs, 2, outs, 2, 128, context);
  check("selected ASIO output reaches the input jumper in the same block", patch.channels[3]->peak[0] > 0
    && std::abs(b[32]-input[32]*gainForPosition(dbToPosition(-6))) < 0.000001f);
  patch.channels[3]->slots[0].plugin = std::make_shared<VerificationEffect>(); patch.channels[3]->slots[0].work.setSize(4, 8192);
  patch.channels[2]->output = 0; patch.setRoute(3, 0, {2, -1, 0, 0.25f});
  patch.audioDeviceIOCallbackWithContext(patchInputs, 2, outs, 2, 128, context);
  check("ASIO loopback input passes through plugins, sends and internal patch", std::abs(b[32]-input[32]*0.5f*gainForPosition(dbToPosition(-6))) < 0.000001f
    && std::abs(a[32]-b[32]*0.25f) < 0.000001f);
  patch.removePlugin(3, 0); patch.removeRoute(3, 0);
  auto patchFile = reportFile.getSiblingFile(reportFile.getFileNameWithoutExtension()+"-loopback-roundtrip.minidaw");
  check("loopback configuration saves", patch.saveProject(patchFile));
  check("ASIO output selection survives project reload", patch.loadProject(patchFile) && patch.channels[3]->loopback && patch.channels[3]->input == 2 && patch.channels[3]->inputRight == 2 && patch.channels[3]->output == 1);
  if (auto legacy = juce::parseXML(patchFile)) {
    auto* mapping = legacy->createNewChildElement("ASIO_LOOPBACK"); mapping->setAttribute("output", 2); mapping->setAttribute("input", 1);
    auto legacyFile = reportFile.getSiblingFile(reportFile.getFileNameWithoutExtension()+"-obsolete-mapping.minidaw");
    const bool restored = legacy->writeTo(legacyFile) && patch.loadProject(legacyFile);
    patch.audioDeviceIOCallbackWithContext(patchInputs, 2, outs, 2, 128, context);
    check("obsolete global mapping does not override channel routing", restored && std::abs(b[32]-input[32]*gainForPosition(dbToPosition(-6))) < 0.000001f);
  } else check("obsolete global mapping fixture loads", false);
  check("opening a preset promotes it without duplicate history entries", patch.loadProject(patchFile) && patch.recentPresets.getNumFiles() == 2
    && patch.recentPresets.getFile(0) == patchFile);
  const auto previousHistory = patch.recentPresets.getAllFilenames();
  check("failed preset opens do not change recent history", !patch.loadProject(reportFile.getSiblingFile("missing-preset.minidaw"))
    && patch.recentPresets.getAllFilenames() == previousHistory);
  check("loopback channel copy preserves output identity", patch.pasteChannel(5, patch.copyChannel(3)) && patch.channels[5]->input == 2);
  patch.setBindings(5, -1, -1, -1, -1);
  patch.beginEdit(); patch.setBindings(3, 0, 0, 1, -1); patch.endEdit("output capture binding");
  check("output capture binding can be undone and redone", patch.undo() && patch.channels[3]->input == 2 && patch.redo() && patch.channels[3]->input == 0);
  patch.setBindings(3, 2, 2, 1, -1);
  patch.reconcileChannels({"Patch", "Mic"}, {"Any output", "Speaker R", "Speaker L"});
  check("ASIO reorder preserves output capture and input destination", patch.channels[3]->input == 0 && patch.channels[3]->output == 0);
  patch.reconcileChannels({"Mic", "Patch"}, {"Speaker L", "Speaker R", "Any output"});
  patch.setRoute(0, 0, {3, -1, 0, 0.5f});
  patch.setLoopbackMode(4, true); patch.setBindings(4, -1, -1, 0, -1);
  const auto patchSettings = patch.copyChannel(4); patch.setLoopbackMode(4, false);
  check("pasting loopback skips input patches that would form feedback", patch.pasteChannel(3, patchSettings) && patch.channels[3]->loopback && patch.channels[3]->output == -1);
  patch.setBindings(3, -1, -1, 1, -1);
  patch.beginEdit(); patch.setLoopbackMode(3, false); patch.endEdit("mode");
  check("loopback mode conversion can be undone", patch.undo() && patch.channels[3]->loopback && patch.channels[3]->output == 1);
  {
    Host taps(false); taps.inputNames = {"Source", "Destination"}; taps.outputNames = {"Chosen output"};
    taps.channels[0]->input = taps.channels[0]->inputRight = 0; taps.channels[0]->output = 0;
    taps.channels[1]->input = taps.channels[1]->inputRight = 1; taps.channels[1]->output = 0;
    taps.setLoopbackMode(3, true); taps.setBindings(3, 0, 0, -1, -1);
    taps.audioDeviceIOCallbackWithContext(patchInputs, 2, outs, 2, 128, context);
    check("output tap captures Mini DAW output without reading the same-index input", std::abs(taps.channels[3]->signal.getSample(0, 32)-input[32]) < 0.000001f);
    check("output capture to input jumper rejects a feedback loop", !taps.setBindings(3, 0, 0, 1, -1));
    taps.setBindings(3, -1, -1, -1, -1); taps.audioDeviceIOCallbackWithContext(patchInputs, 2, outs, 2, 128, context);
    check("unassigned loopback output selector stays silent", taps.channels[3]->peak[0] == 0);
  }
  juce::PluginDescription metadata; metadata.category = "Fx|Reverb";
  check("manifest reverb visible before bus validation", isEffectCandidate(metadata));
  metadata.isInstrument = true; check("manifest instrument excluded", !isEffectCandidate(metadata));
  {
    juce::PluginDescription vst2; vst2.name = "Test Vendor Echo-X"; vst2.manufacturerName = "Test Vendor"; vst2.category = "Fx";
    vst2.pluginFormatName = "VST"; vst2.uniqueId = 1; vst2.numInputChannels = vst2.numOutputChannels = 2;
    vst2.fileOrIdentifier = juce::File::getSpecialLocation(juce::File::currentExecutableFile).getFullPathName();
    auto vst3 = vst2; vst3.name = "Echo X (VST3)"; vst3.pluginFormatName = "VST3"; vst3.uniqueId = 2;
    auto mono = vst2; mono.name = "Test Vendor Echo-X (Mono)"; mono.uniqueId = 3;
    auto otherVendor = vst2; otherVendor.manufacturerName = "Another Vendor"; otherVendor.uniqueId = 4;
    auto missing = vst2; missing.name = "Deleted effect"; missing.fileOrIdentifier = reportFile.getSiblingFile("missing-plugin.dll").getFullPathName();
    auto shell = vst3; shell.uniqueId = 5;
    auto unicodeA = vst2; unicodeA.name = utf8("混响"); unicodeA.uniqueId = 6;
    auto unicodeB = unicodeA; unicodeB.name = utf8("延迟"); unicodeB.uniqueId = 7;
    const juce::Array<juce::PluginDescription> all{vst2, vst3, mono, otherVendor, missing, shell, unicodeA, unicodeB};
    const auto preferred = preferredEffectTypes(all), versions = matchingPluginVersions(vst3, all);
    check("cross-format duplicates prefer VST3 while mono, vendors, shell classes and Unicode names stay distinct", preferred.size() == 6);
    check("compatible alternatives retain VST2 after preferred VST3", versions.size() == 2 && versions[0].pluginFormatName == "VST3" && versions[1].pluginFormatName == "VST");
  }
  Host fixture(false);
  fixture.inputNames = {"Mic A", "Mic B", "Mic C"}; fixture.outputNames = {"Speaker A", "Speaker B"};
  fixture.channels[0]->input = 0; fixture.channels[0]->inputRight = 1;
  fixture.channels[0]->output = 1;
  fixture.reconcileChannels({"Mic C", "Mic A"}, {"Speaker B", "Speaker A"});
  check("device reorder preserves existing channel identity", fixture.channels[0]->input == 1 && fixture.channels[0]->output == 0);
  check("removed device channel left empty", fixture.channels[0]->inputRight == -1);
  auto* channel = fixture.channels[0].get();
  fixture.setRoute(0, 0, {1, -1, 0, 1});
  fixture.setSendTap(0, 0, true); fixture.setSendTap(0, 1, true);
  auto* target = fixture.channels[1].get();
  auto* unaffected = fixture.channels[2].get();
  check("channel body swaps two objects without shifting neighbours", fixture.swapChannels(0, 1) && fixture.channels[1].get() == channel
    && fixture.channels[0].get() == target && fixture.channels[2].get() == unaffected && channel->sends[0].target == 0);
  fixture.swapChannels(0, 1);
  check("channel drag retains object and rewrites routing", fixture.moveChannel(0, 3) && fixture.channels[3].get() == channel && fixture.channels[channel->sends[0].target].get() == target);
  fixture.moveChannel(3, 0);
  fixture.channels[0]->name = "Preset microphone"; fixture.channels[0]->fader = dbToPosition(-6); fixture.channels[0]->mute = true;
  fixture.channels[0]->colour = juce::Colour(0xffcf87b0); fixture.channels[0]->colourAssigned = true;
  fixture.channels[0]->stripWidth = 264; fixture.channels[0]->stripHeight = 520; fixture.channels[0]->stripScale = 0.8f; fixture.windowHeight = 640;
  const auto presetFile = reportFile.getSiblingFile(reportFile.getFileNameWithoutExtension()+"-routing-roundtrip.minidaw");
  check("save project preset", fixture.saveProject(presetFile));
  fixture.channels[0]->name = "changed";
  check("load project restores settings and routes", fixture.loadProject(presetFile) && fixture.channels[0]->name == "Preset microphone"
        && fixture.channels[0]->mute && fixture.channels[0]->colour == juce::Colour(0xffcf87b0) && fixture.channels[0]->sends[0].target == 1);
  check("channel copy paste keeps colour and fader", fixture.pasteChannel(2, fixture.copyChannel(0))
        && fixture.channels[2]->colour == fixture.channels[0]->colour && fixture.channels[2]->fader == fixture.channels[0]->fader.load());
  check("channel dimensions survive project reload and copy paste", fixture.channels[0]->stripWidth == 264 && fixture.channels[2]->stripWidth == 264
    && fixture.channels[2]->stripHeight == 520 && std::abs(fixture.channels[2]->stripScale-0.8f) < 0.00001f && fixture.windowHeight == 640);
  check("connected and unassigned send taps survive project and channel copy", fixture.channels[0]->sends.size() == 2 && fixture.channels[2]->sends.size() == 2
    && fixture.channels[0]->sends[0].preFx && fixture.channels[0]->sends[1].preFx && fixture.channels[2]->sends[0].preFx && fixture.channels[2]->sends[1].preFx);
  check("paste rejects feedback without discarding other settings", fixture.pasteChannel(1, fixture.copyChannel(0))
    && std::none_of(fixture.channels[1]->sends.begin(), fixture.channels[1]->sends.end(), [](const Route& r) { return r.target >= 0; }));
  auto* retained = fixture.channels[0].get();
  check("renamed channels keep numeric display prefix", fixture.channelLabel(0).startsWith("01  Preset microphone"));
  const auto invalidFile = reportFile.getSiblingFile(reportFile.getFileNameWithoutExtension()+"-invalid-project.xml"); invalidFile.replaceWithText("<wrong/>");
  check("invalid preset leaves existing project intact", !fixture.loadProject(invalidFile) && fixture.channels[0].get() == retained);
  Host history(false); history.inputNames = {"Mic A", "Mic B"}; history.outputNames = {"Speaker A"};
  history.beginEdit(); history.channels[0]->name = "Undo microphone"; history.channels[0]->fader = dbToPosition(-9);
  history.channels[0]->colour = juce::Colour(0xffd97780); history.channels[0]->mute = true; history.channels[0]->input = 1; history.setRoute(0, 0, {1, -1, 0, 1}); history.endEdit("channel settings");
  check("undo groups channel controls and routes", history.undo() && history.channels[0]->name == utf8("通道 1")
    && history.channels[0]->fader == dbToPosition(0) && !history.channels[0]->mute && history.channels[0]->sends.empty());
  check("redo restores grouped controls and routes", history.redo() && history.channels[0]->name == "Undo microphone"
    && history.channels[0]->fader == dbToPosition(-9) && history.channels[0]->mute && history.channels[0]->sends[0].target == 1 && history.channels[0]->input == 1);
  history.undo(); history.beginEdit(); history.channels[0]->name = "New branch"; history.endEdit("rename");
  check("new edit after undo clears redo", !history.canRedo());
  history.beginEdit(); history.endEdit("unchanged"); history.undo();
  check("unchanged click creates no undo entry", history.channels[0]->name == utf8("通道 1") && !history.canUndo());
  // Dense routing and short blocks stress the precomputed incoming-route plan.
  Host performance(false); performance.channels[0]->input = performance.channels[0]->inputRight = 0;
  for (int c = 0; c < 11; ++c) performance.setRoute(c, 0, {c+1, -1, 0, 1});
  performance.channels[11]->output = 0;
  std::vector<double> timings; timings.reserve(2000);
  for (int block = 0; block < 2100; ++block) {
    const auto begin = juce::Time::getMillisecondCounterHiRes();
    performance.audioDeviceIOCallbackWithContext(ins, 1, outs, 2, 32, context);
    if (block >= 100) timings.push_back(juce::Time::getMillisecondCounterHiRes()-begin);
  }
  check("dense multihop route processes in current block", std::abs(a[16]-input[16]) < 0.00001f);
  std::sort(timings.begin(), timings.end());
  report.setAttribute("host32SampleMedianMs", timings[timings.size()/2]); report.setAttribute("host32SampleP99Ms", timings[timings.size()*99/100]);
  report.setAttribute("passed", passed); reportFile.getParentDirectory().createDirectory(); report.writeTo(reportFile);
  return passed ? 0 : 1;
}
}
