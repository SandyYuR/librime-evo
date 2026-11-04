//
// Copyright RIME Developers
// Distributed under the BSD License
//
// 固定短语沿用 TableDb 三列格式，内部值字段不等于 UserDB 五字段。
//
#include <gtest/gtest.h>
#include <rime/dict/table_db.h>
#include <rime/dict/user_db.h>

using namespace rime;

TEST(TableDb, UnweightedCustomPhraseRoundTrip) {
  const Tsv original = {"自定义短语", "zdydu"};
  string key, encoded;
  ASSERT_TRUE(TableDb::format.parser(original, &key, &encoded));
  EXPECT_EQ("zdydu \t自定义短语", key);
  EXPECT_EQ("commits=0 dynamic=0 last_tick=0", encoded);
  UserDbValue parsed;
  ASSERT_TRUE(parsed.Unpack(encoded));
  EXPECT_EQ(0, parsed.commits);
  EXPECT_EQ(0, parsed.last_tick);
  Tsv formatted;
  ASSERT_TRUE(TableDb::format.formatter(key, encoded, &formatted));
  ASSERT_EQ(3u, formatted.size());
  EXPECT_EQ("自定义短语", formatted[0]);
  EXPECT_EQ("zdydu", formatted[1]);
  EXPECT_EQ("0", formatted[2]);
}

TEST(TableDb, WeightedCustomPhraseRoundTrip) {
  const Tsv original = {"测试", "ce shi", "100"};
  string key, encoded;
  ASSERT_TRUE(TableDb::format.parser(original, &key, &encoded));
  UserDbValue value;
  ASSERT_TRUE(value.Unpack(encoded));
  EXPECT_EQ(100, value.commits);
  EXPECT_GT(value.dynamic, 0.0);
  Tsv formatted;
  ASSERT_TRUE(TableDb::format.formatter(key, encoded, &formatted));
  EXPECT_EQ(original, formatted);
  EXPECT_FALSE(TableDb::format.formatter(
      key, "unknown=100 dynamic=0 last_tick=0", &formatted));
}
