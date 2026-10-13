#pragma once
#include <algorithm>
#include <cmath>
#include <vector>

namespace mini {
constexpr int slotCount = 12;
constexpr int maximumChannels = 32;
// The endpoint at the bottom is an exact digital mute, not a finite dB value.
inline float positionToDb(float position) { if (position <= 0) return -INFINITY; if (position >= 1) return 12; return position >= 0.78f ? (position-0.78f)*(12.0f/0.22f) : position >= 0.30f ? (position-0.78f)*75.0f : -36.0f+24.0f*std::log2(position/0.30f); }
inline float dbToPosition(float db) { return std::isfinite(db) ? std::clamp(db >= 0 ? 0.78f+db*(0.22f/12.0f) : db >= -36 ? 0.78f+db/75.0f : 0.30f*std::exp2((db+36)/24.0f), 0.0f, 1.0f) : (db > 0 ? 1.0f : 0.0f); }
inline float gainForPosition(float position) { return position <= 0 ? 0 : std::pow(10.0f, positionToDb(position) / 20); }
// Only the initial stationary phase of each press can become a drag-and-drop.
struct InitialHoldGesture {
  double pressedAt = 0;
  bool waiting = false, fired = false;
  void press(double now) { pressedAt = now; waiting = true; fired = false; }
  void move(float distance) { if (distance > 5.0f) waiting = false; }
  bool trigger(double now) { if (!waiting || fired || now-pressedAt < 550) return false; fired = true; waiting = false; return true; }
  void release() { waiting = false; fired = false; }
};
struct Route {
  int target = -1;
  int slot = -1; // -1 = main input; otherwise the actual plug-in slot
  int bus = 0;   // plug-in input bus index (bus 0 = main, bus 1 = SC 1)
  float gain = 0;
  bool preFx = false;
};
inline std::vector<int> routingOrder(const std::vector<std::vector<Route>>& routes) {
  const int count = static_cast<int>(routes.size());
  std::vector<int> degrees(count), order;
  for (const auto& sends : routes) for (const auto& send : sends)
    if (send.target >= 0 && send.target < count) ++degrees[send.target];
  for (int i = 0; i < count; ++i) if (!degrees[i]) order.push_back(i);
  for (size_t i = 0; i < order.size(); ++i) for (const auto& send : routes[order[i]])
    if (send.target >= 0 && send.target < count && --degrees[send.target] == 0) order.push_back(send.target);
  return order.size() == routes.size() ? order : std::vector<int>{};
}
inline int sendControlCount(int used) { return std::max(2, used + 1); }
inline int movedChannelIndex(int index, int from, int to) {
  if (index == from) return to;
  if (from < to && index > from && index <= to) return index-1;
  if (to < from && index >= to && index < from) return index+1;
  return index;
}
inline int swappedIndex(int index, int a, int b) { return index == a ? b : index == b ? a : index; }
inline float snappedFaderPosition(float position, float pixelHeight) {
  const auto unity = dbToPosition(0);
  return std::abs(position-unity)*pixelHeight <= 5 ? unity : std::clamp(position, 0.0f, 1.0f);
}
}
