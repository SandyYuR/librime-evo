//
// Copyright RIME Developers
// Distributed under the BSD License
//
// 2011-07-03 GONG Chen <chen.sst@gmail.com>
//
#include <gtest/gtest.h>
#include <fstream>
#include <chrono>
#include <cstdio>
#include <climits>
#include <rime/dict/userdb_tsv.h>
#include <rime/dict/manual_stamp_codec.h>
#include <rime/algo/syllabifier.h>
#include <rime/dict/text_db.h>
#include <rime/dict/user_db.h>
#include <rime/dict/level_db.h>
#include <rime/dict/user_manual_order.h>
#include <rime/dict/user_dictionary.h>

using namespace rime;

namespace {
std::string Stamp(uint64_t value) {
  return manual_stamp_codec::Format(value);
}
}  // namespace

using TestDb = UserDbWrapper<TextDb>;

TEST(RimeUserDbTest, AccessRecordByKey) {
  TestDb db(path{"user_db_test.txt"}, "user_db_test");
  if (db.Exists())
    db.Remove();
  ASSERT_FALSE(db.Exists());
  db.Open();
  EXPECT_TRUE(db.loaded());
  EXPECT_TRUE(db.Update("abc", "ZYX"));
  EXPECT_TRUE(db.Update("zyx", "CBA"));
  EXPECT_TRUE(db.Update("zyx", "ABC"));
  string value;
  EXPECT_TRUE(db.Fetch("abc", &value));
  EXPECT_EQ("ZYX", value);
  value.clear();
  EXPECT_TRUE(db.Fetch("zyx", &value));
  EXPECT_EQ("ABC", value);
  value.clear();
  EXPECT_FALSE(db.Fetch("wvu", &value));
  EXPECT_TRUE(value.empty());
  value.clear();
  EXPECT_TRUE(db.Erase("zyx"));
  EXPECT_FALSE(db.Fetch("zyx", &value));
  EXPECT_TRUE(value.empty());
  EXPECT_TRUE(db.Close());
  ASSERT_FALSE(db.loaded());
}

TEST(RimeUserDbTest, Query) {
  TestDb db(path{"user_db_test.txt"}, "user_db_test");
  if (db.Exists())
    db.Remove();
  ASSERT_FALSE(db.Exists());
  db.Open();
  EXPECT_TRUE(db.Update("abc", "ZYX"));
  EXPECT_TRUE(db.Update("abc\tdef", "ZYX WVU"));
  EXPECT_TRUE(db.Update("zyx", "ABC"));
  EXPECT_TRUE(db.Update("wvu", "DEF"));
  {
    an<DbAccessor> accessor = db.Query("abc");
    ASSERT_TRUE(bool(accessor));
    EXPECT_FALSE(accessor->exhausted());
    string key, value;
    EXPECT_TRUE(accessor->GetNextRecord(&key, &value));
    EXPECT_EQ("abc", key);
    EXPECT_EQ("ZYX", value);
    key.clear();
    value.clear();
    EXPECT_TRUE(accessor->GetNextRecord(&key, &value));
    EXPECT_EQ("abc\tdef", key);
    EXPECT_EQ("ZYX WVU", value);
    key.clear();
    value.clear();
    EXPECT_FALSE(accessor->GetNextRecord(&key, &value));
    // key, value contain invalid contents
    EXPECT_EQ("", key);
    EXPECT_EQ("", value);
  }
  {
    an<DbAccessor> accessor = db.Query("wvu\tt");
    ASSERT_TRUE(bool(accessor));
    EXPECT_TRUE(accessor->exhausted());
    string key, value;
    EXPECT_FALSE(accessor->GetNextRecord(&key, &value));
  }
  {
    an<DbAccessor> accessor = db.Query("z");
    ASSERT_TRUE(bool(accessor));
    EXPECT_FALSE(accessor->exhausted());
    string key, value;
    EXPECT_TRUE(accessor->GetNextRecord(&key, &value));
    EXPECT_EQ("zyx", key);
    EXPECT_EQ("ABC", value);
    EXPECT_FALSE(accessor->GetNextRecord(&key, &value));
  }
  db.Close();
}

TEST(RimeUserDbValueTest, Denormal) {
  const string denormal_entry =
      "commits=16 dynamic=9.88131e-324 last_tick=1449225";
  UserDbValue v(denormal_entry);
  EXPECT_EQ(16, v.commits);
  EXPECT_EQ(1449225u, v.last_tick);
  EXPECT_GE(v.dynamic, 0.0);
  EXPECT_FALSE(std::isnan(v.dynamic));
}

TEST(RimeUserDbValueTest, NamedFieldsRoundTripAndRejectInvalid) {
  UserDbValue value;
  value.commits = -2;
  value.dynamic = 0.75;
  value.last_tick = 42;
  EXPECT_EQ("commits=-2 dynamic=0.75 last_tick=42", value.Pack());
  UserDbValue parsed;
  ASSERT_TRUE(parsed.Unpack(value.Pack()));
  EXPECT_EQ(value.Pack(), parsed.Pack());
  EXPECT_FALSE(parsed.Unpack("unknown=-2 dynamic=0.75 last_tick=42"));
  EXPECT_FALSE(parsed.Unpack("commits=2 dynamic=1"));
  EXPECT_FALSE(parsed.Unpack("commits=2 dynamic=1 last_tick=abc"));
  EXPECT_FALSE(parsed.Unpack("commits=2 dynamic=1 last_tick=3 junk=1"));
}

// 五字段：前三个自动学习，后两个手排。
TEST(RimeUserDbStateTest, FiveNumbersAndUpstreamDynamics) {
  UserDbState value;
  EXPECT_EQ(value.Pack(), "0,0,0,0,0");
  EXPECT_FALSE(value.Unpack("u1||0"));
  EXPECT_FALSE(value.Unpack("commits=1 dynamic=0 last_tick=0"));
  EXPECT_FALSE(value.Unpack("2,1,0,0"));
  EXPECT_FALSE(value.Unpack("0,0,0,4,0"));
  ASSERT_TRUE(value.Learn(1, 1));
  ASSERT_TRUE(value.Learn(1, 2));
  EXPECT_EQ(value.commits, 2);
  EXPECT_EQ(value.last_tick, 2u);
  EXPECT_GT(value.dynamic, 1.9);
  EXPECT_LT(value.Score(402), value.Score(3));
  UserDbState decoded;
  ASSERT_TRUE(decoded.Unpack(value.Pack()));
  EXPECT_EQ(decoded.Pack(), value.Pack());
}
TEST(RimeUserDbStateTest, ManualStampAndClear) {
  UserDbState a, b, c;
  a.SetManualVersion(3, 1);
  b.SetManualVersion(6, 2);
  c.SetManualVersion(4, 3);
  a.MergeManual(b);
  a.MergeManual(c);
  a.MergeManual(b);
  EXPECT_EQ(a.position, 4);
  EXPECT_EQ(a.manual_stamp, 3u);
  UserDbState cleared;
  cleared.SetManualVersion(0, 4);
  a.MergeManual(cleared);
  a.MergeManual(c);
  EXPECT_EQ(a.position, 0);
  EXPECT_EQ(a.manual_stamp, 4u);
}

TEST(RimeUserDbStateTest, UpstreamAutomaticMergerUsesEachDatabaseTick) {
  // 本机按 /tick 衰减，远端取原值；动态值取较大者。
  UserDbState local, remote;
  ASSERT_TRUE(local.Unpack("10,4,10,1," + Stamp(1700000000)));
  ASSERT_TRUE(remote.Unpack("5,2,50,3," + Stamp(1700000001)));
  local.Merge(remote, 210, 50);
  EXPECT_EQ(local.commits, 10);
  EXPECT_DOUBLE_EQ(local.dynamic, 2.0);
  EXPECT_EQ(local.last_tick, 210u);
  EXPECT_EQ(local.position, 3);
  EXPECT_EQ(local.manual_stamp, 1700000001u);
  // 重复同步不能增加学习强度。
  const auto after = local.Pack();
  local.Merge(remote, 210, 50);
  EXPECT_EQ(local.Pack(), after);
}

TEST(RimeUserDbStateTest, OriginalSignedDeletionWinsByAbsoluteCount) {
  UserDbState local, remote;
  ASSERT_TRUE(local.Unpack("3,3,40,2," + Stamp(1800000001)));
  ASSERT_TRUE(remote.Unpack("-5,1,30,0," + Stamp(1800000002)));
  local.Merge(remote, 40, 50);
  EXPECT_EQ(local.commits, -5);
  EXPECT_EQ(local.last_tick, 50u);
  EXPECT_EQ(local.position, 0);
  EXPECT_EQ(local.manual_stamp, 1800000002u);
  EXPECT_FALSE(local.has_learning());
  UserDbState pinned;
  pinned.SetManualVersion(1, 1800000003);
  local.Merge(pinned, 50, 0);
  EXPECT_EQ(local.commits, -5);
  EXPECT_TRUE(local.has_manual_position());
  EXPECT_TRUE(local.is_auto_deleted());
  EXPECT_FALSE(local.has_learning());
}

TEST(RimeUserDbStateTest, DeletionDoesNotEraseManualAndSelectionRevives) {
  UserDbState state;
  ASSERT_TRUE(state.Unpack("2,1.5,10,2," + Stamp(1700000000)));
  ASSERT_TRUE(state.MarkDeleted(20));
  EXPECT_EQ(state.commits, -2);
  EXPECT_EQ(state.last_tick, 20u);
  EXPECT_TRUE(state.has_manual_position());
  EXPECT_FALSE(state.has_learning());
  ASSERT_TRUE(state.Learn(1, 21));
  EXPECT_EQ(state.commits, 3);
  EXPECT_TRUE(state.has_learning());
  EXPECT_EQ(state.position, 2);
  EXPECT_EQ(state.manual_stamp, 1700000000u);
}

TEST(RimeUserDbStateTest, ManualClearDoesNotOverwriteAutomaticData) {
  UserDbState a, b;
  ASSERT_TRUE(a.Unpack("3,2,40,1," + Stamp(1800000000)));
  ASSERT_TRUE(b.Unpack("8,4,42,0," + Stamp(1800000001)));
  a.Merge(b, 40, 42);
  EXPECT_EQ(a.commits, 8);
  EXPECT_EQ(a.dynamic, 4.0);
  EXPECT_EQ(a.last_tick, 42u);
  EXPECT_EQ(a.position, 0);
  EXPECT_EQ(a.manual_stamp, 1800000001u);
}

TEST(RimeUserDbStateTest, OriginalAbsoluteCountHandlesMinInt) {
  UserDbState local, remote;
  ASSERT_TRUE(local.Unpack("-2147483648,0,1,2," + Stamp(10)));
  ASSERT_TRUE(remote.Unpack("3,1,2,0," + Stamp(11)));
  local.Merge(remote, 1, 2);
  EXPECT_EQ(local.commits, INT_MIN);
  EXPECT_EQ(local.position, 0);
  EXPECT_FALSE(local.Learn(1, 3));  // revival would overflow int
}

TEST(RimeUserDbStateTest, SignedCountRoundtripAndSnapshot) {
  UserDbState deleted;
  ASSERT_TRUE(deleted.Unpack("-2,0.5,0,1," + Stamp(100)));
  EXPECT_EQ(deleted.Pack(), "-2,0.5,0,1," + Stamp(100));
  EXPECT_TRUE(deleted.has_manual_position());
  EXPECT_FALSE(deleted.has_learning());
  std::string row;
  EXPECT_TRUE(userdb_tsv::FormatUserEntry("ce shi \t侧室",
                                          "-2,0.5,0,1," + Stamp(100), &row));
  EXPECT_EQ(row, "ce shi\t侧室\t-2,0.5,0,1," + Stamp(100) + "\n");
}

TEST(RimeUserDbStateTest, RejectNegativeManualPosition) {
  UserDbState state;
  EXPECT_FALSE(state.Unpack("2,1.5,4,-1,5"));
  std::string row;
  EXPECT_FALSE(
      userdb_tsv::FormatUserEntry("cè shì \t侧室", "2,1.5,4,-1,5", &row));
}

TEST(RimeUserDbStateTest, CanonicalSnapshotOnly) {
  EXPECT_EQ(".userdb.tsv", UserDb::snapshot_extension());
  EXPECT_TRUE(UserDbHelper::IsUniformFormat(path("zc.userdb.tsv")));
  EXPECT_FALSE(UserDbHelper::IsUniformFormat(path("zc.tsv")));
  const path file("zc.userdb.tsv");
  const std::string header =
      "# Rime UserDB snapshot (sync and inspection)\n"
      "#@/value_fields\tcommits,dynamic,last_tick,position,manual_stamp\n";
  const std::string metadata =
      "#@/db_name\tzc\n"
      "#@/rime_version\t1.17.0\n"
      "#@/db_type\tuserdb\n"
      "#@/user_id\ttest\n#@/userdb_format\tuserdb-v1\n"
      "#@/manual_clock\t5\n"
      "#@/tick\t10\n";
  const std::string row = "cè shì\t侧室\t2,1.5,8,2,5\n";
  size_t count = 0;
  {
    std::ofstream out(file);
    out << header << metadata << "#@/record_count\t1\n" << row;
  }
  EXPECT_TRUE(userdb_tsv::ValidateSnapshot(file, &count));
  EXPECT_EQ(1u, count);
  // 未发布格式固定为 userdb-v1。
  {
    std::string unsupported = metadata;
    const auto pos = unsupported.find("userdb-v1");
    ASSERT_NE(std::string::npos, pos);
    unsupported.replace(pos, std::string("userdb-v1").size(), "userdb-v3");
    std::ofstream out(file);
    out << header << unsupported << "#@/record_count\t1\n" << row;
  }
  EXPECT_FALSE(userdb_tsv::ValidateSnapshot(file, &count));  // duplicate tick
  {
    std::ofstream out(file);
    out << header << metadata << "#@/record_count\t2\n" << row;
  }
  EXPECT_FALSE(userdb_tsv::ValidateSnapshot(file, &count));
  {
    std::ofstream out(file);
    out << header << metadata << row << "#@/record_count\t1\n";
  }
  EXPECT_FALSE(userdb_tsv::ValidateSnapshot(file, &count));
  {
    std::ofstream out(file);
    out << header << metadata << "#@/record_count\t1\n#@/record_count\t1\n"
        << row;
  }
  EXPECT_FALSE(userdb_tsv::ValidateSnapshot(file, &count));
  {
    std::ofstream out(file);
    out << header << "# sync_complete\t1\n"
        << metadata << "#@/record_count\t1\n"
        << row;
  }
  EXPECT_FALSE(userdb_tsv::ValidateSnapshot(file, &count));
  {
    std::ofstream out(file);
    out << header << metadata << "#@/record_count\t1\n#@/tick\t0\n" << row;
  }
  EXPECT_FALSE(userdb_tsv::ValidateSnapshot(file, &count));
  {
    std::ofstream out(file);
    out << header << metadata
        << "#@/record_count\t1\n#@/manual_clock\t58006561152\n"
        << row;
  }
  EXPECT_FALSE(userdb_tsv::ValidateSnapshot(file, &count));  // duplicated clock
  {
    std::ofstream out(file);
    out << header << metadata << "#@/record_count\t1\n#@/db_name\tzc\n" << row;
  }
  EXPECT_FALSE(userdb_tsv::ValidateSnapshot(file, &count));  // duplicate name
  {
    std::ofstream out(file);
    out << header << metadata << "#@/record_count\t1\n#@/new_meta\tretained\n"
        << row;
  }
  EXPECT_TRUE(userdb_tsv::ValidateSnapshot(
      file, &count));  // real DB metadata is allowed
  {
    std::ofstream out(file);
    out << header << "#@/db_name\tother\n"
        << metadata.substr(metadata.find("#@/rime_version"))
        << "#@/record_count\t1\n"
        << row;
  }
  EXPECT_FALSE(userdb_tsv::ValidateSnapshot(file, &count));  // name mismatch
  {
    std::ofstream out(file);
    out << header << "#@/db_name\tzc\n"
        << metadata.substr(metadata.find("#@/db_type"))
        << "#@/record_count\t1\n"
        << row;
  }
  EXPECT_FALSE(
      userdb_tsv::ValidateSnapshot(file, &count));  // missing rime_version
  {
    std::ofstream out(file);
    out << header << metadata
        << "#@/record_count\t1\n#@/file_name\tzc.userdb.tsv\n"
        << row;
  }
  EXPECT_FALSE(
      userdb_tsv::ValidateSnapshot(file, &count));  // reserved metadata
  {
    std::ofstream out(file);
    out << header
        << "#@/db_type\tuserdb\n#@/user_id\ttest\n#@/"
           "userdb_format\tuserdb-v1\n#@/record_count\t1\n"
        << row;
  }
  EXPECT_FALSE(userdb_tsv::ValidateSnapshot(file, &count));  // missing clock
  {
    std::ofstream out(file);
    out << header
        << "#@/db_type\tuserdb\n#@/user_id\ttest\n#@/"
           "userdb_format\tuserdb-v1\n#@/manual_clock\t1\n#@/record_count\t1\n"
        << row;
  }
  EXPECT_FALSE(
      userdb_tsv::ValidateSnapshot(file, &count));  // clock < row revision
  {
    std::ofstream out(file);
    out << header << metadata << "#@/record_count\t1\n"
        << "cè shì\t侧室\t2,1.5,8,-1,5\n";
  }
  EXPECT_FALSE(userdb_tsv::ValidateSnapshot(file, &count));
  std::remove(file.string().c_str());
  {
    std::ofstream out("zc.tsv");
    out << header << metadata << "#@/record_count\t1\n" << row;
  }
  EXPECT_FALSE(userdb_tsv::ValidateSnapshot(path("zc.tsv"), &count));
  std::remove("zc.tsv");
}

TEST(RimeUserDbStateTest, MergeDatabaseClockUsesMaximum) {
  const path test_file("user_db_clock_test.txt");
  TestDb db(test_file, "user_db_clock_test");
  if (db.Exists())
    ASSERT_TRUE(db.Remove());
  ASSERT_TRUE(db.Open());
  ASSERT_TRUE(db.MetaUpdate("/manual_clock", "100"));
  {
    UserDbMerger merger(&db);
    merger.ObserveClock(58006561152ULL);
    merger.ObserveTick(2);
    ASSERT_TRUE(merger.Put("cè shì \t侧室", "0,0,2,0," + Stamp(57890237595)));
    merger.CloseMerge();
    ASSERT_FALSE(merger.failed());
  }
  std::string clock;
  ASSERT_TRUE(db.MetaFetch("/manual_clock", &clock));
  EXPECT_EQ(clock, Stamp(58006561152));
  ASSERT_TRUE(db.Close());
  ASSERT_TRUE(db.Remove());
}

TEST(RimeUserDbStateTest, RealMergerPreservesIndependentManualAndAutomatic) {
  const path test_file("user_db_original_merge_test.txt");
  TestDb db(test_file, "user_db_original_merge_test");
  if (db.Exists())
    ASSERT_TRUE(db.Remove());
  ASSERT_TRUE(db.Open());
  ASSERT_TRUE(db.MetaUpdate("/tick", "210"));
  ASSERT_TRUE(db.MetaUpdate("/manual_clock", "100"));
  const string key("ce shi \t侧室");
  ASSERT_TRUE(db.Update(key, "10,4,10,2," + Stamp(100)));
  {
    UserDbMerger merger(&db);
    merger.ObserveTick(50);
    merger.ObserveClock(110);
    ASSERT_TRUE(merger.Put(key, "-12,2,50,1," + Stamp(110)));
    merger.CloseMerge();
    ASSERT_FALSE(merger.failed());
  }
  string raw;
  ASSERT_TRUE(db.Fetch(key, &raw));
  UserDbState merged;
  ASSERT_TRUE(merged.Unpack(raw));
  EXPECT_EQ(merged.commits, -12);
  EXPECT_DOUBLE_EQ(merged.dynamic, 2.0);
  EXPECT_EQ(merged.last_tick, 210u);
  EXPECT_EQ(merged.position, 1);
  EXPECT_EQ(merged.manual_stamp, 110u);
  string tick;
  ASSERT_TRUE(db.MetaFetch("/tick", &tick));
  EXPECT_EQ(tick, "210");
  ASSERT_TRUE(db.Close());
  ASSERT_TRUE(db.Remove());
}

TEST(RimeUserDbStateTest, DeletedEntryStillAppearsIfPinned) {
  const string key("ce shi \t侧室");
  EXPECT_FALSE(bool(UserDictionary::CreateDictEntry(key, "-2,1,20,0,100", 30)));
  auto pinned = UserDictionary::CreateDictEntry(key, "-2,1,20,2,100", 30);
  ASSERT_TRUE(bool(pinned));
  EXPECT_EQ(pinned->manual_position, 2);
  EXPECT_EQ(pinned->commit_count, 0);
  EXPECT_FALSE(bool(UserDictionary::CreateDictEntry(key, "-2,1,20,0,101", 30)));
}

TEST(RimeUserDbStateTest, ManualModeDoesNotWriteAutomaticDeletion) {
  using RankDb = UserDbWrapper<LevelDb>;
  const path file("manual_mode_deletion.userdb");
  auto db = New<RankDb>(file, "manual_mode_deletion");
  if (db->Exists())
    ASSERT_TRUE(db->Remove());
  ASSERT_TRUE(db->Open());
  const string key("ce shi \t测试");
  const string original("3,1.5,10,2,1");
  ASSERT_TRUE(db->Update(key, original));
  UserDictionary dictionary("manual_mode_deletion", db);
  ASSERT_TRUE(dictionary.Load());
  DictEntry entry;
  entry.text = "测试";
  entry.custom_code = "ce shi ";
  dictionary.SetUserDictMode("manual");
  EXPECT_FALSE(dictionary.UpdateEntry(entry, -1));
  string value;
  ASSERT_TRUE(db->Fetch(key, &value));
  EXPECT_EQ(value, original);
  ASSERT_TRUE(db->Close());
  ASSERT_TRUE(db->Remove());
}

// 手排使用所选词的实际 UserDB 键；邻词不参与写入许可判断。
TEST(RimeUserDbManualOrderTest, MoveSelectedWithoutNeighborRecord) {
  using RankDb = UserDbWrapper<LevelDb>;
  RankDb db(path{"manual_position_single.userdb"}, "manual_position_single");
  if (db.Exists())
    ASSERT_TRUE(db.Remove());
  ASSERT_TRUE(db.Open());
  ASSERT_TRUE(db.Update("ni \t泥", "3,1,10,0,0"));
  ASSERT_TRUE(user_manual_order::MoveAdjacent(&db, "ni ", "泥", "", "", 3, -1));
  string value;
  ASSERT_TRUE(db.Fetch("ni \t泥", &value));
  UserDbState state;
  ASSERT_TRUE(state.Unpack(value));
  EXPECT_EQ(state.position, 2);
  EXPECT_EQ(state.commits, 3);
  EXPECT_GT(state.manual_stamp, 0u);
  ASSERT_TRUE(db.Close());
  ASSERT_TRUE(db.Remove());
}

TEST(RimeUserDbManualOrderTest, SwapAlreadyPinnedDifferentCodes) {
  using RankDb = UserDbWrapper<LevelDb>;
  RankDb db(path{"manual_position_cross_code.userdb"},
            "manual_position_cross_code");
  if (db.Exists())
    ASSERT_TRUE(db.Remove());
  ASSERT_TRUE(db.Open());
  ASSERT_TRUE(db.Update("ni \t泥", "3,1,10,3,1"));
  ASSERT_TRUE(db.Update("ni3 \t尼", "5,2,12,2,2"));
  ASSERT_TRUE(
      user_manual_order::MoveAdjacent(&db, "ni ", "泥", "ni3 ", "尼", 3, -1));
  string first, second;
  ASSERT_TRUE(db.Fetch("ni \t泥", &first));
  ASSERT_TRUE(db.Fetch("ni3 \t尼", &second));
  UserDbState a, b;
  ASSERT_TRUE(a.Unpack(first));
  ASSERT_TRUE(b.Unpack(second));
  EXPECT_EQ(a.position, 2);
  EXPECT_EQ(b.position, 3);
  EXPECT_EQ(a.commits, 3);
  EXPECT_EQ(b.commits, 5);
  ASSERT_TRUE(db.Close());
  ASSERT_TRUE(db.Remove());
}

TEST(RimeUserDbManualOrderTest, SameCodeOccupiedPositionSwaps) {
  using RankDb = UserDbWrapper<LevelDb>;
  RankDb db(path{"manual_position_collision.userdb"},
            "manual_position_collision");
  if (db.Exists())
    ASSERT_TRUE(db.Remove());
  ASSERT_TRUE(db.Open());
  ASSERT_TRUE(db.Update("ni \t泥", "3,1,10,3,1"));
  ASSERT_TRUE(db.Update("ni \t你", "1,2,11,2,2"));
  ASSERT_TRUE(user_manual_order::MoveAdjacent(&db, "ni ", "泥", "", "", 3, -1));
  string first, second;
  ASSERT_TRUE(db.Fetch("ni \t泥", &first));
  ASSERT_TRUE(db.Fetch("ni \t你", &second));
  UserDbState a, b;
  ASSERT_TRUE(a.Unpack(first));
  ASSERT_TRUE(b.Unpack(second));
  EXPECT_EQ(a.position, 2);
  EXPECT_EQ(b.position, 3);
  ASSERT_TRUE(db.Close());
  ASSERT_TRUE(db.Remove());
}

TEST(RimeUserDbManualOrderTest, UnpinnedNeighborWithDifferentCodeIsIgnored) {
  using RankDb = UserDbWrapper<LevelDb>;
  RankDb db(path{"manual_position_other_code.userdb"},
            "manual_position_other_code");
  if (db.Exists())
    ASSERT_TRUE(db.Remove());
  ASSERT_TRUE(db.Open());
  ASSERT_TRUE(db.Update("ni \t泥", "3,1,10,0,0"));
  ASSERT_TRUE(db.Update("ni3 \t尼", "1,2,11,0,0"));
  ASSERT_TRUE(
      user_manual_order::MoveAdjacent(&db, "ni ", "泥", "ni3 ", "尼", 3, -1));
  string first, second;
  ASSERT_TRUE(db.Fetch("ni \t泥", &first));
  ASSERT_TRUE(db.Fetch("ni3 \t尼", &second));
  UserDbState a, b;
  ASSERT_TRUE(a.Unpack(first));
  ASSERT_TRUE(b.Unpack(second));
  EXPECT_EQ(a.position, 2);
  EXPECT_EQ(b.position, 0);
  ASSERT_TRUE(db.Close());
  ASSERT_TRUE(db.Remove());
}
