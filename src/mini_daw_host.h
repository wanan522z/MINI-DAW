#pragma once
#include <juce_audio_utils/juce_audio_utils.h>
#include "mini_daw_model.h"
#include <array>
#include <atomic>

namespace mini {
inline juce::String utf8(const char* text) { return juce::String::fromUTF8(text); }
struct Slot {
  std::unique_ptr<juce::AudioPluginInstance> plugin;
  juce::AudioBuffer<float> work;
  juce::PluginDescription description;
  std::atomic<bool> enabled{true};
};
struct Channel {
  juce::String name;
  int input = -1, inputRight = -1, output = -1, outputRight = -1;
  std::array<Slot, slotCount> slots;
  std::vector<Route> sends;
  std::atomic<float> fader{dbToPosition(0)};
  std::array<std::atomic<float>, 2> peak{0, 0};
  std::atomic<bool> mute{false};
  juce::AudioBuffer<float> signal{2, 8192};
};
class Host final : public juce::AudioIODeviceCallback {
public:
  explicit Host(bool persistent = true);
  ~Host() override;
  juce::CriticalSection lock;
  std::vector<std::unique_ptr<Channel>> channels;
  juce::AudioPluginFormatManager formats;
  juce::KnownPluginList catalog;
  juce::StringArray inputNames, outputNames;
  juce::String error;
  std::atomic<uint64_t> callbackCount{0};
  std::function<void()> changed;
  bool start();
  void stop();
  bool isRunning() const { return device && device->isPlaying(); }
  double sampleRate() const { return rate; }
  int blockSize() const { return frames; }
  juce::StringArray drivers();
  juce::String driverName = "Aggregate Audio";
  bool addChannel();
  bool setRoute(int channel, int sendIndex, Route route);
  void removeRoute(int channel, int sendIndex);
  void rebuildOrder();
  bool loadPlugin(int channel, int slot, const juce::PluginDescription& description, const juce::MemoryBlock* state = nullptr);
  void removePlugin(int channel, int slot);
  void controlPanel();
  void save();
  void restore();
  juce::File stateFile() const;
  juce::File catalogFile() const;
  void audioDeviceIOCallbackWithContext(const float* const*, int, float* const*, int, int, const juce::AudioIODeviceCallbackContext&) override;
  void audioDeviceAboutToStart(juce::AudioIODevice*) override;
  void audioDeviceStopped() override;
  void audioDeviceError(const juce::String&) override;
private:
  std::unique_ptr<juce::AudioIODeviceType> type;
  std::unique_ptr<juce::AudioIODevice> device;
  std::vector<int> order;
  double rate = 48000;
  int frames = 128;
  juce::MidiBuffer midi;
  bool persistent;
  bool needsDefaultInput = true;
  std::shared_ptr<std::atomic<bool>> alive = std::make_shared<std::atomic<bool>>(true);
  void prepare(Slot&);
  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(Host)
};
}
