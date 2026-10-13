#pragma once
#include <juce_audio_utils/juce_audio_utils.h>
#include <span>

namespace mini {
struct WindowsOutput {
  juce::String id, name, interfaceName, description;
  int channels = 0;
  bool sharedAvailable = true;
  bool operator==(const WindowsOutput&) const = default;
};
struct LoopbackSource {
  // Empty = automatic, "off" = host only; otherwise a stable MMDevice ID.
  juce::String endpoint;
  int channel = 0;
  bool includeHost = true;
  juce::String name;
  int endpointChannels = 0;
  bool operator==(const LoopbackSource&) const = default;
};
std::vector<WindowsOutput> windowsAudioOutputs(const juce::String& asioDriver = {}, const juce::StringArray& asioOutputs = {});
class WindowsAudioDeviceWatcher final {
public:
  WindowsAudioDeviceWatcher();
  ~WindowsAudioDeviceWatcher();
  bool consumeChanges();
private:
  struct Impl;
  std::unique_ptr<Impl> impl;
};
bool asioUsesWindowsOutput(const juce::String& driver, const juce::StringArray& outputs, const WindowsOutput& endpoint);
LoopbackSource automaticLoopbackSource(const juce::String& driver, const juce::StringArray& outputs,
                                     int index, const std::vector<WindowsOutput>& endpoints);
class SystemLoopback final {
public:
  SystemLoopback();
  ~SystemLoopback();
  void configure(const std::vector<LoopbackSource>&, double sampleRate);
  void stop();
  // One read per ASIO callback; shared endpoints are consumed only once.
  void read(int samples);
  std::span<const float* const> signals() const { return outputSignals; }
  const float* signal(int output) const;
  juce::String status(int output) const;
private:
  struct Capture;
  std::vector<std::unique_ptr<Capture>> captures;
  struct Binding { int capture = -1, channel = 0; };
  std::vector<Binding> bindings;
  std::vector<LoopbackSource> configuration;
  std::vector<const float*> outputSignals;
  double rate = 0;
};
}
