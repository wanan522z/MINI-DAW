#pragma once
#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_gui_extra/juce_gui_extra.h>
#include "mini_daw_model.h"
#include "mini_daw_scan.h"
#include "mini_daw_loopback.h"
#include <array>
#include <atomic>
#include <map>
#include <future>

namespace mini {
inline juce::String utf8(const char* text) { return juce::String::fromUTF8(text); }
struct Slot {
  std::shared_ptr<juce::AudioPluginInstance> plugin;
  juce::AudioBuffer<float> work;
  juce::PluginDescription description;
  std::atomic<bool> enabled{true};
  float editorScale = 1;
  int editorWidth = 0, editorHeight = 0;
};
struct WindowsLoopbackInput {
  juce::String endpoint, name;
  int left = 0, right = 1;
  bool operator==(const WindowsLoopbackInput&) const = default;
};
struct AsioBinding {
  juce::String driver, name;
  int occurrence = 0;
  static AsioBinding fromIndex(int, const juce::StringArray&, const juce::String& driver);
  int resolve(const juce::StringArray&, const juce::String& driver) const;
  bool operator==(const AsioBinding&) const = default;
};
struct Channel {
  juce::String name;
  bool loopback = false;
  juce::Colour colour{0xff489ada};
  bool colourAssigned = false;
  int stripWidth = 194, stripHeight = 0;
  float stripScale = 1;
  int input = -1, inputRight = -1, output = -1, outputRight = -1;
  // Desired identities survive unplugging; the indices above are active ports only.
  std::array<AsioBinding, 4> asioBindings;
  WindowsLoopbackInput windowsInput;
  std::array<Slot, slotCount> slots;
  std::vector<Route> sends;
  std::atomic<float> fader{dbToPosition(0)};
  std::array<std::atomic<float>, 2> peak{0, 0};
  std::atomic<bool> mute{false};
  juce::AudioBuffer<float> signal{2, 8192};
  juce::AudioBuffer<float> preFxSignal{2, 8192};
};
class Host final : public juce::AudioIODeviceCallback, private juce::Timer,
                   private juce::AudioIODeviceType::Listener {
public:
  explicit Host(bool persistent = true);
  ~Host() override;
  juce::CriticalSection lock;
  std::vector<std::unique_ptr<Channel>> channels;
  juce::AudioPluginFormatManager formats;
  juce::KnownPluginList catalog;
  juce::StringArray inputNames, outputNames;
  juce::RecentlyOpenedFilesList recentPresets;
  juce::String error;
  std::atomic<uint64_t> callbackCount{0};
  unsigned deviceGeneration = 0;
  unsigned systemDeviceGeneration = 0;
  std::function<void()> changed;
  bool start();
  void stop();
  bool isRunning() const { return device && device->isPlaying(); }
  double sampleRate() const { return rate; }
  int blockSize() const { return frames; }
  double deviceLatencyMs() const;
  int pluginLatencySamples() const;
  juce::Array<double> sampleRates() const;
  bool setSampleRate(double);
  juce::Array<int> bufferSizes() const;
  bool setBufferSize(int);
  juce::String channelLabel(int) const;
  juce::StringArray drivers();
  juce::String driverName = "Aggregate Audio";
  int windowHeight = 751;
  bool addChannel();
  bool setLoopbackMode(int channel, bool enabled);
  bool setBindings(int channel, int inputLeft, int inputRight, int outputLeft, int outputRight);
  bool setAsioInput(int channel, int left, int right);
  bool setAsioOutput(int channel, int left, int right);
  bool setWindowsInput(int channel, const WindowsLoopbackInput&);
  std::vector<WindowsOutput> systemOutputs;
  std::vector<WindowsOutput> windowsLoopbackOutputs() const;
  void refreshSystemOutputs();
  juce::String loopbackInputStatus(int channel) const;
  LoopbackSource systemSource(int output, bool resolveAutomatic = false) const;
  bool setSystemSource(int output, const LoopbackSource&);
  juce::String systemSourceStatus(int output) const;
  bool setSendTap(int channel, int sendIndex, bool preFx);
  bool setRoute(int channel, int sendIndex, Route route);
  void removeRoute(int channel, int sendIndex);
  void rebuildOrder();
  bool loadPlugin(int channel, int slot, const juce::PluginDescription& description, const juce::MemoryBlock* state = nullptr);
  bool loadPreferredPlugin(int channel, int slot, const juce::PluginDescription& description);
  void removePlugin(int channel, int slot);
  void controlPanel();
  bool saveOnExit = true;
  void save();
  void restore();
  bool saveProject(const juce::File&);
  bool loadProject(const juce::File&);
  bool moveChannel(int from, int to);
  bool swapChannels(int a, int b);
  juce::String copyChannel(int);
  bool pasteChannel(int, const juce::String&);
  bool movePlugin(int channel, int from, int insertion);
  bool copyPlugin(int source, int slot, int target, int insertion);
  bool replaceWithCopy(int source, int slot, int target, int other);
  bool swapPlugins(int source, int slot, int target, int other);
  void beginEdit();
  void endEdit(const juce::String& label);
  bool undo();
  bool redo();
  bool canUndo() const { return !pendingEdit && !undoHistory.empty(); }
  bool canRedo() const { return !pendingEdit && !redoHistory.empty(); }
  juce::String undoLabel() const { return canUndo() ? undoHistory.back().label : juce::String(); }
  juce::String redoLabel() const { return canRedo() ? redoHistory.back().label : juce::String(); }
  void swapSlots(Slot&, Slot&);
  juce::FileSearchPath scanPaths() const;
  void setScanPaths(const juce::FileSearchPath&);
  void beginScan(const juce::FileSearchPath&);
  std::shared_ptr<PluginScanJob> scanJob;
  uint64_t catalogRevision = 0;
  // Rebind saved channel identities after an ASIO layout change.
  void reconcileChannels(const juce::StringArray&, const juce::StringArray&, const juce::String& driver = {});
  juce::File stateFile() const;
  juce::File catalogFile() const;
  void audioDeviceIOCallbackWithContext(const float* const*, int, float* const*, int, int, const juce::AudioIODeviceCallbackContext&) override;
  void audioDeviceAboutToStart(juce::AudioIODevice*) override;
  void audioDeviceStopped() override;
  void audioDeviceError(const juce::String&) override;
private:
  void processAudio(const float* const*, int, float* const*, int, int, std::span<const float* const>);
  std::unique_ptr<juce::AudioIODeviceType> type;
  std::unique_ptr<juce::AudioIODevice> device;
  std::vector<int> order;
  struct IncomingRoute { int source, send; };
  struct InputPatch { int source, sourceSide, destinationSide; };
  struct ChannelPlan {
    std::array<int, 2> systemInputs{-1, -1};
    std::vector<InputPatch> patches;
    std::vector<IncomingRoute> main;
    std::array<std::vector<IncomingRoute>, slotCount> sidechains;
  };
  std::vector<ChannelPlan> plans;
  SystemLoopback systemLoopback;
  std::unique_ptr<WindowsAudioDeviceWatcher> systemDeviceWatcher;
  double systemRefreshAt = 0;
  std::future<std::vector<WindowsOutput>> systemOutputRefresh;
  bool systemRefreshAgain = false;
  unsigned systemRefreshGeneration = 0, pendingSystemRefreshGeneration = 0;
  void queueSystemOutputRefresh();
  void applySystemOutputs(std::vector<WindowsOutput>);
  juce::String layoutDriver;
  std::map<juce::String, LoopbackSource> systemSources;
  void syncSystemLoopback();
  bool changeBindings(int channel, int inputLeft, int inputRight, int outputLeft, int outputRight, bool changeInput, bool changeOutput);
  struct EditSlot {
    std::shared_ptr<juce::AudioPluginInstance> plugin;
    juce::PluginDescription description;
    bool enabled;
    float scale;
    int width, height;
  };
  struct EditChannel {
    juce::String name;
    bool loopback;
    juce::Colour colour;
    bool colourAssigned, mute;
    int input, inputRight, output, outputRight;
    float fader;
    int stripWidth, stripHeight;
    float stripScale;
    std::vector<Route> sends;
    std::array<EditSlot, slotCount> slots;
    WindowsLoopbackInput windowsInput;
    std::array<AsioBinding, 4> asioBindings;
  };
  struct EditState { std::vector<EditChannel> channels; juce::StringArray inputs, outputs; int windowHeight = 751; std::map<juce::String, LoopbackSource> systemSources; };
  struct EditEntry { EditState before, after; juce::String label; };
  std::unique_ptr<EditState> pendingEdit;
  std::vector<EditEntry> undoHistory, redoHistory;
  EditState captureEditState();
  void applyEditState(const EditState&);
  static bool sameEditState(const EditState&, const EditState&);
  double requestedRate = 0;
  double rate = 48000;
  int frames = 128;
  int requestedFrames = 0;
  juce::MidiBuffer midi;
  bool persistent;
  bool needsDefaultInput = true;
  bool wantAudio = false, opening = false;
  std::atomic<bool> rebuildRequested{false};
  uint64_t lastCallbacks = 0;
  double lastAudioTime = 0, lastStartTime = 0, lastScanSave = 0;
  void timerCallback() override;
  void audioDeviceListChanged() override;
  void closeDevice();
  std::unique_ptr<juce::XmlElement> projectXml();
  std::unique_ptr<juce::XmlElement> channelXml(const Channel&);
  void restoreChannel(int, const juce::XmlElement&, const juce::StringArray&, const juce::StringArray&, const juce::String& driver);
  void restoreXml(const juce::XmlElement&);
  juce::StringArray savedInputs, savedOutputs;
  std::shared_ptr<std::atomic<bool>> alive = std::make_shared<std::atomic<bool>>(true);
  void prepare(Slot&);
  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(Host)
};
}
