#include <cmath>
#include <limits>
#include <gtest/gtest.h>
#include <rime/gear/user_rank_math.h>

TEST(UserRankMathTest, Midpoint) {
  const auto w = rime::user_rank::WeightBetween(10.0, 8.0);
  EXPECT_GT(w, 8.0);
  EXPECT_LT(w, 10.0);
}
TEST(UserRankMathTest, EqualWeightUsesPosition) {
  using namespace rime::user_rank;
  EXPECT_EQ(WeightBetween(8.0, 8.0), 8.0);
  EXPECT_FALSE(PreferPinnedOnTie(50, 49));
  EXPECT_TRUE(PreferPinnedOnTie(50, 50));
}
TEST(UserRankMathTest, EdgeAndAdjacentFloat) {
  using namespace rime::user_rank;
  const double x = 8.0;
  const double y = std::nextafter(x, std::numeric_limits<double>::infinity());
  EXPECT_EQ(WeightBetween(y, x), x);
  EXPECT_GT(WeightBetween(std::numeric_limits<double>::infinity(), x), x);
  EXPECT_LT(WeightBetween(x, -std::numeric_limits<double>::infinity()), x);
}
