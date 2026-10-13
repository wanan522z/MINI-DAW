#include "mini_daw_scan.h"
#include <juce_audio_processors_headless/format_types/VST3_SDK/pluginterfaces/base/ipluginbase.h>

namespace mini {
juce::FileSearchPath defaultPluginSearchPaths() {
  juce::VST3PluginFormat vst3; juce::VSTPluginFormat vst2;
  auto paths = vst3.getDefaultLocationsToSearch(); paths.addPath(vst2.getDefaultLocationsToSearch());
  const auto programFiles = juce::File::getSpecialLocation(juce::File::globalApplicationsDirectory);
  for (const auto* folder : {"Common Files/VST2", "VSTPlugins", "Steinberg/VSTPlugins"}) paths.addIfNotAlreadyThere(programFiles.getChildFile(folder));
  paths.removeRedundantPaths(); return paths;
}
namespace {
juce::String normalisedPluginName(juce::String value) {
  value = value.toLowerCase().trim();
  for (const auto* suffix : {" (vst3)", " (vst2)", " (vst)", " vst3", " vst2"})
    if (value.endsWith(suffix)) { value = value.dropLastCharacters(static_cast<int>(std::strlen(suffix))).trim(); break; }
  return value.removeCharacters(" \t_-./:");
}
juce::String pluginProductName(const juce::PluginDescription& d) {
  auto name = d.name.trim(); const auto vendor = d.manufacturerName.trim();
  if (vendor.isNotEmpty() && name.startsWithIgnoreCase(vendor) && name.length() > vendor.length()) {
    const auto separator = name[vendor.length()];
    if (juce::CharacterFunctions::isWhitespace(separator) || separator == ':' || separator == '-') name = name.substring(vendor.length()).trim();
  }
  return normalisedPluginName(name);
}
bool samePlugin(const juce::PluginDescription& a, const juce::PluginDescription& b) {
  if (a.isDuplicateOf(b)) return true;
  if (a.manufacturerName.isEmpty() || b.manufacturerName.isEmpty()) return false;
  return a.isInstrument == b.isInstrument && pluginProductName(a) == pluginProductName(b)
      && normalisedPluginName(a.manufacturerName) == normalisedPluginName(b.manufacturerName)
      && (a.pluginFormatName != b.pluginFormatName || a.uniqueId == b.uniqueId);
}
bool supportedEffect(const juce::PluginDescription& d) {
  return (d.pluginFormatName == "VST3" || d.pluginFormatName == "VST") && isEffectCandidate(d) && juce::File(d.fileOrIdentifier).exists();
}
bool preferPlugin(const juce::PluginDescription& a, const juce::PluginDescription& b) {
  if (a.pluginFormatName != b.pluginFormatName) return a.pluginFormatName == "VST3";
  if (a.version != b.version) return a.version.compareNatural(b.version) > 0;
  return a.fileOrIdentifier.compareIgnoreCase(b.fileOrIdentifier) < 0;
}
}
juce::Array<juce::PluginDescription> preferredEffectTypes(const juce::Array<juce::PluginDescription>& types) {
  juce::Array<juce::PluginDescription> result;
  for (const auto& d : types) {
    if (!supportedEffect(d)) continue;
    auto existing = std::find_if(result.begin(), result.end(), [&](const auto& other) { return samePlugin(d, other); });
    if (existing == result.end()) result.add(d); else if (preferPlugin(d, *existing)) *existing = d;
  }
  std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) {
    const int name = a.name.compareNatural(b.name); return name != 0 ? name < 0 : a.manufacturerName.compareNatural(b.manufacturerName) < 0;
  });
  return result;
}
juce::Array<juce::PluginDescription> matchingPluginVersions(const juce::PluginDescription& selected, const juce::Array<juce::PluginDescription>& types) {
  juce::Array<juce::PluginDescription> result;
  for (const auto& d : types) if (supportedEffect(d) && samePlugin(selected, d)) result.add(d);
  std::sort(result.begin(), result.end(), preferPlugin);
  return result;
}
// Enumerate factory class metadata without constructing or initializing DSP.
// Run only inside --scan-plugin, so DLL load/exit remains isolated and bounded.
bool scanPluginFactory(const juce::File& bundle, juce::OwnedArray<juce::PluginDescription>& found) {
  if (bundle.hasFileExtension("dll")) {
    juce::VSTPluginFormat vst2; vst2.findAllTypesForFile(found, bundle.getFullPathName());
    for (auto* d : found) d->lastFileModTime = pluginTimestamp(bundle);
    return !found.isEmpty();
  }
  if (!bundle.hasFileExtension("vst3")) return false;
  using namespace Steinberg;
  auto file = bundle;
  if (file.isDirectory()) {
    const auto libraries = file.getChildFile("Contents/x86_64-win").findChildFiles(juce::File::findFiles, false, "*.vst3");
    if (libraries.isEmpty()) return false;
    file = libraries[0];
  }
  juce::DynamicLibrary library;
  if (!library.open(file.getFullPathName())) return false;
  using ModuleFunction = bool (PLUGIN_API*)();
  auto exit = reinterpret_cast<ModuleFunction>(library.getFunction("ExitDll"));
  auto init = reinterpret_cast<ModuleFunction>(library.getFunction("InitDll"));
  if (init && !init()) return false;
  using FactoryFunction = IPluginFactory* (PLUGIN_API*)();
  auto getFactory = reinterpret_cast<FactoryFunction>(library.getFunction("GetPluginFactory"));
  IPluginFactory* factory = getFactory ? getFactory() : nullptr;
  if (factory) {
    IPluginFactory2* factory2 = nullptr;
    IPluginFactory3* factory3 = nullptr;
    factory->queryInterface(INLINE_UID_OF(IPluginFactory2), reinterpret_cast<void**>(&factory2));
    factory->queryInterface(INLINE_UID_OF(IPluginFactory3), reinterpret_cast<void**>(&factory3));
    PFactoryInfo factoryInfo{}; factory->getFactoryInfo(&factoryInfo);
    const int classes = factory->countClasses();
    for (int i = 0; i < classes; ++i) {
      PClassInfo info{};
      if (factory->getClassInfo(i, &info) != kResultOk || std::strcmp(info.category, "Audio Module Class") != 0) continue;
      juce::PluginDescription d;
      d.name = juce::String::fromUTF8(info.name); d.manufacturerName = juce::String::fromUTF8(factoryInfo.vendor);
      d.category = "Fx"; // old factories have no subcategory; actual buses are checked on load
      PClassInfoW wide{}; PClassInfo2 extended{};
      if (factory3 && factory3->getClassInfoUnicode(i, &wide) == kResultOk) {
        d.name = juce::String(reinterpret_cast<const wchar_t*>(wide.name));
        d.manufacturerName = juce::String(reinterpret_cast<const wchar_t*>(wide.vendor));
        d.version = juce::String(reinterpret_cast<const wchar_t*>(wide.version));
        d.category = juce::String::fromUTF8(wide.subCategories);
      } else if (factory2 && factory2->getClassInfo2(i, &extended) == kResultOk) {
        d.manufacturerName = juce::String::fromUTF8(extended.vendor);
        d.version = juce::String::fromUTF8(extended.version); d.category = juce::String::fromUTF8(extended.subCategories);
      }
      if (d.category.isEmpty()) d.category = "Fx";
      if (d.manufacturerName.isEmpty()) d.manufacturerName = juce::String::fromUTF8(factoryInfo.vendor);
      d.descriptiveName = d.name; d.pluginFormatName = "VST3";
      d.fileOrIdentifier = bundle.getFullPathName(); d.lastFileModTime = pluginTimestamp(bundle); d.lastInfoUpdateTime = juce::Time::getCurrentTime();
      d.isInstrument = d.category.containsIgnoreCase("Instrument");
      uint32_t legacy = 0;
      for (char b : info.cid) legacy = legacy*31+static_cast<uint32_t>(b);
      d.deprecatedUid = static_cast<int>(legacy);
      const auto* b = reinterpret_cast<const uint8_t*>(info.cid);
      auto word = [&](int a, int c, int e, int f) { return (uint32_t(b[a])<<24) | (uint32_t(b[c])<<16) | (uint32_t(b[e])<<8) | b[f]; };
      uint32_t uid = 0;
      for (uint32_t part : {word(3, 2, 1, 0), word(5, 4, 7, 6), word(8, 9, 10, 11), word(12, 13, 14, 15)}) uid = uid*31+part;
      d.uniqueId = static_cast<int>(uid);
      found.add(new juce::PluginDescription(d));
    }
    if (factory3) factory3->release(); if (factory2) factory2->release(); factory->release();
  }
  if (exit) exit();
  return factory != nullptr;
}
}
