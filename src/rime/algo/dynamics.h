#include <cmath>

namespace rime {
namespace algo {

inline double formula_d(double increment,
                        double current_tick,
                        double previous_dynamic,
                        double previous_tick) {
  return increment +
         previous_dynamic * exp((previous_tick - current_tick) / 200);
}

inline double formula_p(double baseline,
                        double frequency,
                        double current_tick,
                        double dynamic) {
  const double kM = 1 / (1 - exp(-0.005));
  const double initial =
      baseline -
      (baseline - frequency) * pow((1 - exp(-current_tick / 10000)), 10);
  return (dynamic < 20)
             ? initial + (0.5 - initial) * (dynamic / kM)
             : initial + (1 - initial) * (pow(4, (dynamic / kM)) - 1) / 3;
}

}  // namespace algo
}  // namespace rime
