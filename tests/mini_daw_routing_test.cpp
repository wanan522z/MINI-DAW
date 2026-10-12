#include "mini_daw_model.h"
#include <cstdio>
int main() {
  auto require = [](bool value, const char* label) { if (!value) std::fprintf(stderr, "%s failed\n", label); return value; };
  using namespace mini;
  if (!require(std::abs(gainForPosition(dbToPosition(0)) - 1) < 0.00001f, "unity default")) return 1;
  if (!require(gainForPosition(0) == 0 && std::isinf(positionToDb(0)), "minus infinity")) return 1;
  if (!require(positionToDb(1) == 12, "12 dB maximum")) return 1;
  std::vector<std::vector<Route>> routes(3);
  routes[2].push_back({0, 1, 1, 0.5f}); routes[0].push_back({1, -1, 0, 1});
  if (!require(routingOrder(routes) == std::vector<int>{2, 0, 1}, "sidechain dependency order")) return 1;
  routes[1].push_back({2, -1, 0, 0});
  if (!require(routingOrder(routes).empty(), "reject cycles even at zero send")) return 1;
  if (!require(sendControlCount(0) == 2 && sendControlCount(2) == 3 && sendControlCount(5) == 6, "growing sends")) return 1;
  return 0;
}
