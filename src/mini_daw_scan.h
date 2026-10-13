#pragma once
#include <juce_audio_utils/juce_audio_utils.h>
#include <atomic>
#include <thread>

namespace mini {
bool scanPluginFactory(const juce::File&, juce::OwnedArray<juce::PluginDescription>&);
juce::Array<juce::PluginDescription> preferredEffectTypes(const juce::Array<juce::PluginDescription>&);
juce::Array<juce::PluginDescription> matchingPluginVersions(const juce::PluginDescription&, const juce::Array<juce::PluginDescription>&);
juce::FileSearchPath defaultPluginSearchPaths();
inline juce::File canonicalPluginFile(juce::File file) {
  if (file.getParentDirectory().getFileName() == "x86_64-win") {
    const auto bundle = file.getParentDirectory().getParentDirectory().getParentDirectory();
    if (bundle.hasFileExtension("vst3")) return bundle;
  }
  return file;
}
inline juce::Time pluginTimestamp(const juce::File& file) {
  auto time = file.getLastModificationTime();
  if (file.isDirectory()) {
    for (const auto& library : file.getChildFile("Contents/x86_64-win").findChildFiles(juce::File::findFiles, false, "*.vst3")) time = std::max(time, library.getLastModificationTime());
    for (const auto& path : {"Contents/Resources/moduleinfo.json", "Contents/moduleinfo.json"}) time = std::max(time, file.getChildFile(path).getLastModificationTime());
  }
  return time;
}
// VST3 manifests omit bus counts. Validate actual audio buses when loading.
inline bool isEffectCandidate(const juce::PluginDescription& d) {
  return !d.isInstrument && ((d.numInputChannels > 0 && d.numOutputChannels > 0)
      || (d.numInputChannels == 0 && d.numOutputChannels == 0
          && d.category.containsIgnoreCase("Fx")));
}
struct PluginScanJob {
  juce::CriticalSection lock;
  juce::Array<juce::PluginDescription> ready;
  juce::String current, failures;
  std::atomic<bool> cancelled{false}, done{false};
  std::atomic<int> total{0}, completed{0}, failed{0}, cached{0};
};
inline std::shared_ptr<PluginScanJob> scanPlugins(const juce::FileSearchPath& paths,
                                                 juce::Array<juce::PluginDescription> cachedTypes) {
  auto job = std::make_shared<PluginScanJob>();
  std::thread([job, paths, cachedTypes] {
    juce::VST3PluginFormat format;
    juce::VSTPluginFormat vst2;
    auto files = format.searchPathsForPlugins(paths, true, false);
    for (const auto& path : vst2.searchPathsForPlugins(paths, true, false)) {
      bool insideBundle = false;
      for (auto parent = juce::File(path).getParentDirectory(); parent != parent.getParentDirectory(); parent = parent.getParentDirectory())
        if (parent.hasFileExtension("vst3;aaxplugin")) { insideBundle = true; break; }
      if (!insideBundle) files.add(path);
    }
    files.removeDuplicates(true); job->total = files.size();
    juce::StringArray pending;
    for (const auto& path : files) {
      if (job->cancelled) break;
      const juce::File file(path);
      bool cached = false;
      for (const auto& d : cachedTypes)
        if (canonicalPluginFile(juce::File(d.fileOrIdentifier)) == file && d.lastFileModTime == pluginTimestamp(file)) { cached = true; break; }
      if (cached) { ++job->cached; ++job->completed; continue; }
      // Read manifests first, so these plugins appear before any slow binary probes.
      const auto manifest = file.getChildFile("Contents/Resources/moduleinfo.json");
      const auto legacy = file.getChildFile("Contents/moduleinfo.json");
      const auto json = juce::JSON::parse((manifest.existsAsFile() ? manifest : legacy).loadFileAsString());
      if (file.hasFileExtension("vst3") && json.isObject() && json.getProperty("Classes", {}).isArray()) {
        juce::OwnedArray<juce::PluginDescription> found;
        format.findAllTypesForFile(found, path);
        if (!found.isEmpty()) {
          const juce::ScopedLock guard(job->lock);
          for (auto* d : found) { d->lastFileModTime = pluginTimestamp(file); job->ready.add(*d); }
          ++job->completed; continue;
        }
      }
      pending.add(path);
    }
    // Isolated probes run on their own message thread. A bad plugin cannot stall
    // the DAW, hide all other results, or hold its message-thread lock.
    std::atomic<int> next{0};
    std::vector<std::thread> workers;
    for (int w = 0; w < std::min(3, pending.size()); ++w) workers.emplace_back([&] {
      for (;;) {
        const int i = next++;
        if (i >= pending.size() || job->cancelled) return;
        const auto path = pending[i];
        { const juce::ScopedLock guard(job->lock); job->current = juce::File(path).getFileNameWithoutExtension(); }
        const auto report = juce::File::getSpecialLocation(juce::File::tempDirectory)
                            .getNonexistentChildFile("minidaw-scan-"+juce::Uuid().toString(), ".xml");
        juce::ChildProcess process;
        const juce::StringArray args{juce::File::getSpecialLocation(juce::File::currentExecutableFile).getFullPathName(),
                                    "--scan-plugin", path, report.getFullPathName()};
        const bool started = process.start(args, 0);
        const auto start = juce::Time::getMillisecondCounter();
        while (started && process.isRunning() && !job->cancelled
               && juce::Time::getMillisecondCounter()-start < 15000) juce::Thread::sleep(30);
        if (process.isRunning()) process.kill();
        const auto xml = juce::XmlDocument::parse(report);
        bool found = xml && xml->hasTagName("SCAN_RESULT") && xml->getBoolAttribute("factoryValid");
        if (xml && xml->hasTagName("SCAN_RESULT")) {
          const juce::ScopedLock guard(job->lock);
          for (auto* p : xml->getChildWithTagNameIterator("PLUGIN")) {
            juce::PluginDescription d;
            if (d.loadFromXml(*p)) { job->ready.add(d); found = true; }
          }
        }
        report.deleteFile();
        if (!found && !job->cancelled) {
          ++job->failed;
          const juce::ScopedLock guard(job->lock); job->failures += path+"\n";
        }
        ++job->completed;
      }
    });
    for (auto& worker : workers) worker.join();
    job->done = true;
  }).detach();
  return job;
}
}
