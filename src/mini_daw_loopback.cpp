#include "mini_daw_loopback.h"
#include <windows.h>
#include <initguid.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <endpointvolume.h>
#include <functiondiscoverykeys_devpkey.h>
#include <ksmedia.h>
#include <wrl/client.h>

namespace mini {
namespace {
using Microsoft::WRL::ComPtr;
struct ComScope {
  HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  ~ComScope() { if (SUCCEEDED(result)) CoUninitialize(); }
  bool usable() const { return SUCCEEDED(result) || result == RPC_E_CHANGED_MODE; }
};
juce::String property(IPropertyStore* store, const PROPERTYKEY& key) {
  PROPVARIANT value{};
  const auto hr = store->GetValue(key, &value);
  juce::String result;
  if (SUCCEEDED(hr) && value.vt == VT_LPWSTR && value.pwszVal) result = value.pwszVal;
  PropVariantClear(&value); return result;
}
juce::String failure(HRESULT hr) { return juce::String::fromUTF8("Windows 回送不可用：0x")+juce::String::toHexString(static_cast<int>(hr)); }
bool aggregateOutputMatches(const juce::String& name, const WindowsOutput& endpoint) {
  if (endpoint.interfaceName.isEmpty() || endpoint.description.isEmpty()) return false;
  const auto prefix = endpoint.interfaceName+" / "+endpoint.description+" (KS Pin ";
  const auto bytes = prefix.toUTF8();
  const auto shortPrefix = juce::String::fromUTF8(bytes.getAddress(), std::min(31, static_cast<int>(prefix.getNumBytesAsUTF8())));
  return name.startsWith(shortPrefix);
}
}
struct WindowsAudioDeviceWatcher::Impl {
  struct Listener final : IMMNotificationClient {
    explicit Listener(std::shared_ptr<std::atomic<bool>> flag) : changed(std::move(flag)) {}
    std::atomic<ULONG> references{1};
    std::shared_ptr<std::atomic<bool>> changed;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** result) override {
      if (!result) return E_POINTER;
      *result = nullptr;
      if (iid != __uuidof(IUnknown) && iid != __uuidof(IMMNotificationClient)) return E_NOINTERFACE;
      *result = static_cast<IMMNotificationClient*>(this); AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references; }
    ULONG STDMETHODCALLTYPE Release() override { const auto count = --references; if (!count) delete this; return count; }
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override { *changed = true; return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override { *changed = true; return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override { *changed = true; return S_OK; }
    // Bindings use explicit endpoint IDs. Changing the Windows default device
    // or unrelated driver properties must not interrupt an existing source.
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow, ERole, LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY key) override {
      if (IsEqualPropertyKey(key, PKEY_Device_FriendlyName) || IsEqualPropertyKey(key, PKEY_DeviceInterface_FriendlyName)
          || IsEqualPropertyKey(key, PKEY_Device_DeviceDesc) || IsEqualPropertyKey(key, PKEY_AudioEngine_DeviceFormat)) *changed = true;
      return S_OK;
    }
  };
  ComScope com;
  std::shared_ptr<std::atomic<bool>> changed = std::make_shared<std::atomic<bool>>(false);
  ComPtr<IMMDeviceEnumerator> enumerator;
  ComPtr<Listener> listener;
  Impl() {
    if (!com.usable() || FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator)))) return;
    listener.Attach(new Listener(changed));
    if (FAILED(enumerator->RegisterEndpointNotificationCallback(listener.Get()))) enumerator.Reset();
  }
  ~Impl() { if (enumerator) enumerator->UnregisterEndpointNotificationCallback(listener.Get()); }
};
WindowsAudioDeviceWatcher::WindowsAudioDeviceWatcher() : impl(std::make_unique<Impl>()) {}
WindowsAudioDeviceWatcher::~WindowsAudioDeviceWatcher() = default;
bool WindowsAudioDeviceWatcher::consumeChanges() { return impl->changed->exchange(false); }
std::vector<WindowsOutput> windowsAudioOutputs(const juce::String& asioDriver, const juce::StringArray& asioOutputs) {
  ComScope com; std::vector<WindowsOutput> result;
  ComPtr<IMMDeviceEnumerator> enumerator; ComPtr<IMMDeviceCollection> devices;
  if (!com.usable() || FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator)))
      || FAILED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &devices))) return result;
  UINT count = 0; devices->GetCount(&count);
  for (UINT i = 0; i < count; ++i) {
    ComPtr<IMMDevice> device; ComPtr<IPropertyStore> store; ComPtr<IAudioClient> client;
    if (FAILED(devices->Item(i, &device)) || FAILED(device->OpenPropertyStore(STGM_READ, &store))) continue;
    LPWSTR id = nullptr; if (FAILED(device->GetId(&id))) continue;
    WindowsOutput output{id, property(store.Get(), PKEY_Device_FriendlyName),
      property(store.Get(), PKEY_DeviceInterface_FriendlyName), property(store.Get(), PKEY_Device_DeviceDesc)};
    CoTaskMemFree(id);
    WAVEFORMATEX* format = nullptr;
    if (SUCCEEDED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client)) && SUCCEEDED(client->GetMixFormat(&format))) {
      output.channels = format->nChannels;
      // Opening a shared loopback client without starting it detects endpoints
      // that a native/KS ASIO driver has reserved exclusively. No audio is read.
      output.sharedAvailable = !asioUsesWindowsOutput(asioDriver, asioOutputs, output)
        && SUCCEEDED(client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, 500000, 0, format, nullptr));
      CoTaskMemFree(format);
    }
    if (output.channels > 0 && output.channels <= 64) result.push_back(std::move(output));
  }
  return result;
}
bool asioUsesWindowsOutput(const juce::String& driver, const juce::StringArray& outputs, const WindowsOutput& endpoint) {
  for (const auto& name : outputs) {
    if (driver == "Aggregate Audio" && aggregateOutputMatches(name, endpoint)) return true;
    if (name.equalsIgnoreCase(endpoint.name) || name.startsWithIgnoreCase(endpoint.name+" - Output ")) return true;
  }
  return false;
}
LoopbackSource automaticLoopbackSource(const juce::String& driver, const juce::StringArray& outputs,
                                     int index, const std::vector<WindowsOutput>& endpoints) {
  LoopbackSource result;
  // KS bypasses the Windows engine, so its host contribution must be added.
  // Other ASIO drivers require an explicit mapping and mix choice.
  if (driver != "Aggregate Audio" || !juce::isPositiveAndBelow(index, outputs.size())) return result;
  const auto name = outputs[index];
  const WindowsOutput* match = nullptr;
  for (const auto& endpoint : endpoints) {
    if (!aggregateOutputMatches(name, endpoint)) continue;
    if (match) return result; // Never silently capture another connector.
    match = &endpoint;
  }
  if (!match) return result;
  int channel = -1;
  if (name.contains(" - Output ")) channel = name.fromFirstOccurrenceOf(" - Output ", false, false).getIntValue()-1;
  else {
    // Truncated names are identical until JUCE adds " (1)", " (2)", etc.
    auto unnumbered = [](juce::String value) {
      const auto suffix = value.fromLastOccurrenceOf(" (", true, false);
      if (value.endsWithChar(')') && suffix.substring(2, suffix.length()-1).containsOnly("0123456789"))
        value = value.upToLastOccurrenceOf(" (", false, false);
      return value;
    };
    const auto group = unnumbered(name); int total = 0, ordinal = 0;
    for (int i = 0; i < outputs.size(); ++i) if (unnumbered(outputs[i]) == group) { if (i < index) ++ordinal; ++total; }
    // Subsets cannot be inferred from truncated labels. Ask for a manual map.
    if (total == match->channels) channel = ordinal;
  }
  if (juce::isPositiveAndBelow(channel, match->channels)) {
    result.endpoint = match->id; result.name = match->name; result.channel = channel; result.endpointChannels = match->channels;
  }
  return result;
}
struct SystemLoopback::Capture final : juce::Thread {
  Capture(juce::String endpointId, double sampleRate) : Thread("MiniDAW system loopback"), id(std::move(endpointId)), rate(sampleRate) { startThread(); }
  ~Capture() override { signalThreadShouldExit(); notify(); stopThread(-1); }
  juce::String id;
  double rate;
  juce::AbstractFifo fifo{65536};
  // Allocate on the worker before publishing ready. Never allocate in ASIO.
  juce::AudioBuffer<float> ring, block;
  std::atomic<bool> ready{false}, connected{false};
  std::atomic<float> windowsGain{1.0f};
  std::atomic<bool> windowsMuted{false};
  juce::SmoothedValue<float> volumeEnvelope{1.0f};
  std::atomic<unsigned> discontinuities{0}; unsigned lastDiscontinuity = 0;
  juce::CriticalSection statusLock; juce::String message = juce::String::fromUTF8("正在连接 Windows 输出…");
  void setStatus(juce::String value) { const juce::ScopedLock guard(statusLock); message = std::move(value); }
  void run() override {
    ComScope com;
    if (!com.usable()) { setStatus(failure(com.result)); return; }
    while (!threadShouldExit()) {
      const auto hr = capture(); connected = false; ++discontinuities;
      if (threadShouldExit()) break;
      setStatus(failure(hr)); wait(2000);
    }
  }
  HRESULT capture() {
    ComPtr<IMMDeviceEnumerator> enumerator; ComPtr<IMMDevice> device; ComPtr<IAudioClient> client; ComPtr<IAudioCaptureClient> input;
    ComPtr<IAudioEndpointVolume> endpointVolume;
    auto hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
    if (FAILED(hr)) return hr;
    if (FAILED(hr = enumerator->GetDevice(id.toWideCharPointer(), &device))) return hr;
    if (FAILED(hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client))) return hr;
    // Loopback taps the Windows mix before endpoint master attenuation. Read
    // the selected endpoint's controls on this worker, never on the ASIO thread.
    device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr, &endpointVolume);
    auto syncVolume = [&] {
      if (!endpointVolume) return false;
      float db = 0, scalar = 1; BOOL muted = FALSE;
      if (FAILED(endpointVolume->GetMasterVolumeLevel(&db)) || FAILED(endpointVolume->GetMasterVolumeLevelScalar(&scalar))
          || FAILED(endpointVolume->GetMute(&muted)) || !std::isfinite(db) || !std::isfinite(scalar)) return false;
      // The 0..1 slider is audio-tapered; its dB value gives the actual gain.
      windowsGain.store(juce::Decibels::decibelsToGain(db), std::memory_order_relaxed);
      windowsMuted.store(muted != FALSE || scalar <= 0, std::memory_order_relaxed);
      return true;
    };
    const bool volumeAvailable = syncVolume();
    WAVEFORMATEX* mix = nullptr; if (FAILED(hr = client->GetMixFormat(&mix))) return hr;
    const int channels = mix->nChannels;
    WAVEFORMATEXTENSIBLE format{};
    format.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE; format.Format.nChannels = static_cast<WORD>(channels);
    format.Format.nSamplesPerSec = static_cast<DWORD>(rate); format.Format.wBitsPerSample = 32;
    format.Format.nBlockAlign = static_cast<WORD>(channels*4); format.Format.nAvgBytesPerSec = format.Format.nSamplesPerSec*format.Format.nBlockAlign;
    format.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE)-sizeof(WAVEFORMATEX);
    format.Samples.wValidBitsPerSample = 32; format.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
    if (mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE && mix->cbSize >= 22)
      format.dwChannelMask = reinterpret_cast<WAVEFORMATEXTENSIBLE*>(mix)->dwChannelMask;
    CoTaskMemFree(mix);
    if (channels < 1 || channels > 64) return E_INVALIDARG;
    hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM
      | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY, 500000, 0, &format.Format, nullptr);
    if (FAILED(hr)) return hr;
    if (FAILED(hr = client->GetService(IID_PPV_ARGS(&input)))) return hr;
    if (!ready.load()) {
      ring.setSize(channels, fifo.getTotalSize()); block.setSize(channels, 8192);
      volumeEnvelope.reset(rate, 0.005);
      volumeEnvelope.setCurrentAndTargetValue(windowsMuted.load() ? 0.0f : windowsGain.load());
      ready.store(true, std::memory_order_release);
    }
    // A format change requires reconfiguration rather than touching a live buffer.
    if (ring.getNumChannels() != channels) return AUDCLNT_E_UNSUPPORTED_FORMAT;
    if (FAILED(hr = client->Start())) return hr;
    connected = true; setStatus(juce::String::fromUTF8(volumeAvailable ? "Windows 系统回送已连接 · 跟随系统音量" : "Windows 系统回送已连接 · 无法读取系统音量"));
    auto lastVolumeSync = juce::Time::getMillisecondCounterHiRes();
    while (!threadShouldExit()) {
      const auto now = juce::Time::getMillisecondCounterHiRes();
      if (now-lastVolumeSync >= 20) { syncVolume(); lastVolumeSync = now; }
      UINT packets = 0; if (FAILED(hr = input->GetNextPacketSize(&packets))) break;
      while (packets && !threadShouldExit()) {
        BYTE* data = nullptr; UINT samples = 0; DWORD flags = 0;
        if (FAILED(hr = input->GetBuffer(&data, &samples, &flags, nullptr, nullptr))) break;
        if (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) ++discontinuities;
        int a = 0, na = 0, b = 0, nb = 0;
        fifo.prepareToWrite(static_cast<int>(samples), a, na, b, nb);
        auto copy = [&](int offset, int size, int from) {
          for (int ch = 0; ch < channels; ++ch) {
            auto* dest = ring.getWritePointer(ch, offset);
            if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) || !data) juce::FloatVectorOperations::clear(dest, size);
            else for (int i = 0; i < size; ++i) dest[i] = reinterpret_cast<const float*>(data)[(from+i)*channels+ch];
          }
        };
        copy(a, na, 0); copy(b, nb, na); fifo.finishedWrite(na+nb);
        if (na+nb < static_cast<int>(samples)) ++discontinuities;
        if (FAILED(hr = input->ReleaseBuffer(samples))) break;
        if (FAILED(hr = input->GetNextPacketSize(&packets))) break;
      }
      if (FAILED(hr)) break;
      wait(2);
    }
    client->Stop(); return hr;
  }
  void read(int samples) {
    if (!ready.load(std::memory_order_acquire)) return;
    block.clear(0, samples);
    const auto generation = discontinuities.load(); const int available = fifo.getNumReady();
    const int maximumBacklog = std::max(samples*4, static_cast<int>(rate*0.04));
    if (!connected.load() || generation != lastDiscontinuity || available > maximumBacklog) {
      const int discard = !connected.load() || generation != lastDiscontinuity ? available : std::max(0, available-std::max(samples, static_cast<int>(rate*0.01)));
      fifo.finishedRead(discard); lastDiscontinuity = generation;
    }
    if (!connected.load()) return;
    int a = 0, na = 0, b = 0, nb = 0; fifo.prepareToRead(samples, a, na, b, nb);
    for (int ch = 0; ch < block.getNumChannels(); ++ch) {
      block.copyFrom(ch, 0, ring, ch, a, na); block.copyFrom(ch, na, ring, ch, b, nb);
    }
    fifo.finishedRead(na+nb);
    // Apply the latest volume after dequeueing so queued audio also responds
    // to mute immediately. Only the Windows contribution passes this gain.
    if (windowsMuted.load(std::memory_order_relaxed)) {
      block.clear(0, samples); volumeEnvelope.setCurrentAndTargetValue(0.0f);
    } else {
      volumeEnvelope.setTargetValue(windowsGain.load(std::memory_order_relaxed));
      if (!volumeEnvelope.isSmoothing()) block.applyGain(0, samples, volumeEnvelope.getCurrentValue());
      else for (int i = 0; i < samples; ++i) {
        const float gain = volumeEnvelope.getNextValue();
        for (int ch = 0; ch < block.getNumChannels(); ++ch) block.getWritePointer(ch)[i] *= gain;
      }
    }
  }
};
SystemLoopback::SystemLoopback() = default;
SystemLoopback::~SystemLoopback() = default;
void SystemLoopback::stop() { captures.clear(); bindings.clear(); configuration.clear(); outputSignals.clear(); rate = 0; }
void SystemLoopback::configure(const std::vector<LoopbackSource>& sources, double sampleRate) {
  if (sources == configuration && rate == sampleRate) return;
  stop(); configuration = sources; rate = sampleRate;
  outputSignals.resize(sources.size());
  for (const auto& source : sources) {
    Binding binding; binding.channel = source.channel;
    if (source.endpoint.isNotEmpty() && source.endpoint != "off") {
      int index = 0; for (; index < static_cast<int>(captures.size()); ++index) if (captures[index]->id == source.endpoint) break;
      if (index == static_cast<int>(captures.size())) captures.push_back(std::make_unique<Capture>(source.endpoint, rate));
      binding.capture = index;
    }
    bindings.push_back(binding);
  }
}
void SystemLoopback::read(int samples) {
  if (samples <= 0 || samples > 8192) return;
  for (auto& capture : captures) capture->read(samples);
  for (int i = 0; i < static_cast<int>(outputSignals.size()); ++i) outputSignals[i] = signal(i);
}
const float* SystemLoopback::signal(int output) const {
  if (!juce::isPositiveAndBelow(output, static_cast<int>(bindings.size()))) return nullptr;
  const auto& binding = bindings[output];
  if (binding.capture < 0) return nullptr;
  const auto& capture = *captures[binding.capture];
  return capture.ready.load(std::memory_order_acquire) && capture.connected.load()
    && juce::isPositiveAndBelow(binding.channel, capture.block.getNumChannels()) ? capture.block.getReadPointer(binding.channel) : nullptr;
}
juce::String SystemLoopback::status(int output) const {
  if (!juce::isPositiveAndBelow(output, static_cast<int>(bindings.size())) || bindings[output].capture < 0)
    return juce::String::fromUTF8("等待内录通道选择此输出");
  const auto& capture = *captures[bindings[output].capture]; const juce::ScopedLock guard(capture.statusLock); return capture.message;
}
}
