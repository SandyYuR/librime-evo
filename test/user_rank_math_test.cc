#include <cmath>
#include <limits>
#include <gtest/gtest.h>
#include <rime/dict/user_dictionary.h>
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

TEST(UserRankMathTest, TiedPinsKeepOrdinaryEntriesInTheirStableSlots) {
  rime::UserDictEntryIterator entries;
  auto add = [&](const char* text, int position, uint64_t version) {
    auto entry = rime::New<rime::DictEntry>();
    entry->text = text;
    entry->weight = 1.0;
    entry->manual_position = position;
    entry->manual_version = version;
    entries.Add(std::move(entry));
  };
  add("later", 4, 10);
  add("ordinary first", 0, 0);
  add("older", 1, 5);
  add("ordinary second", 0, 0);
  add("newer", 1, 6);
  entries.SortAll();
  for (const char* expected :
       {"newer", "ordinary first", "older", "ordinary second", "later"}) {
    ASSERT_FALSE(entries.exhausted());
    EXPECT_EQ(expected, entries.Peek()->text);
    entries.Next();
  }
  EXPECT_TRUE(entries.exhausted());
}
