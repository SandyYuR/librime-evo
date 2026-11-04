// Copyright RIME Developers
// Distributed under the BSD License
#ifndef RIME_GEAR_USER_RANK_MATH_H_
#define RIME_GEAR_USER_RANK_MATH_H_
#include <cmath>
#include <cstddef>
#include <limits>
namespace rime {
namespace user_rank {
// Both inputs are in the *same* log-weight domain as DictEntry.weight.
// high >= low, +/- infinity means there is no natural neighbor.
inline double WeightBetween(double high, double low) {
  if (!std::isfinite(high) && std::isfinite(low))
    return std::nextafter(low, std::numeric_limits<double>::infinity());
  if (std::isfinite(high) && !std::isfinite(low))
    return std::nextafter(high, -std::numeric_limits<double>::infinity());
  if (!std::isfinite(high) && !std::isfinite(low))
    return 0;
  double middle = high / 2 + low / 2;
  // A plateau (or adjacent floats) has no representable interior point;
  // the query's observed position is the secondary comparison key.
  return middle < high && middle > low ? middle : low;
}
inline bool PreferPinnedOnTie(int position, size_t observed_one_based) {
  return position > 0 && observed_one_based >= static_cast<size_t>(position);
}
}  // namespace user_rank
}  // namespace rime
#endif
