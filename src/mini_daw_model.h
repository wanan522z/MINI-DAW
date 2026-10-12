#pragma once
#include <algorithm>
#include <cmath>
#include <vector>

namespace mini {
constexpr int slotCount = 12;
constexpr int maximumChannels = 32;
// The endpoint at the bottom is an exact digital mute, not a finite dB value.
inline float positionToDb(float position) { return position <= 0 ? -INFINITY : -72.0f + position * 84.0f; }
inline float dbToPosition(float db) { return std::isfinite(db) ? std::clamp((db + 72) / 84, 0.0f, 1.0f) : 0; }
inline float gainForPosition(float position) { return position <= 0 ? 0 : std::pow(10.0f, positionToDb(position) / 20); }
struct Route {
  int target = -1;
  int slot = -1; // -1 = main input; otherwise the actual plug-in slot
  int bus = 0;   // plug-in input bus index (bus 0 = main, bus 1 = SC 1)
  float gain = 0;
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
}
