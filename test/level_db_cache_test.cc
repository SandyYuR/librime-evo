// Copyright RIME Developers
// Distributed under the BSD License
//
#include <algorithm>
#include <filesystem>
#include <random>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <gtest/gtest.h>
#include <rime/dict/level_db.h>
#include <rime/dict/user_db.h>
#include <rime/dict/user_dictionary.h>

using namespace rime;

class RimeLevelDbCacheTest : public ::testing::TestWithParam<bool> {
 protected:
  void SetUp() override {
    dir_ = path(std::filesystem::temp_directory_path()) /
           boost::uuids::to_string(boost::uuids::random_generator()());
    ASSERT_TRUE(std::filesystem::create_directory(dir_));
    db_ = New<UserDbWrapper<LevelDb>>(dir_ / "cache.userdb", "cache");
    ASSERT_TRUE(db_->Open());
    ASSERT_TRUE(db_->Update("a", "base-a"));
    ASSERT_TRUE(db_->Update("ab", "base-ab"));
    ASSERT_TRUE(db_->Update("b", "base-b"));
    if (GetParam()) {
      ASSERT_TRUE(db_->EnableQueryCache());
      ASSERT_EQ(3u, Records(db_->QueryAll()).size());
    }
  }
  void TearDown() override {
    if (db_ && db_->loaded())
      db_->Close();
    std::filesystem::remove_all(dir_);
  }
  static vector<pair<string, string>> Records(an<DbAccessor> cursor) {
    vector<pair<string, string>> result;
    string key, value;
    while (cursor->GetNextRecord(&key, &value))
      result.emplace_back(key, value);
    EXPECT_TRUE(cursor->exhausted());
    return result;
  }
  string Value(const string& key) {
    auto records = Records(db_->Query(key));
    return !records.empty() && records.front().first == key
               ? records.front().second
               : string();
  }
  path dir_;
  an<UserDbWrapper<LevelDb>> db_;
};

TEST_P(RimeLevelDbCacheTest, MergesOverwritesDeletionsAndSeeks) {
  ASSERT_TRUE(db_->Update("ab", "changed-ab"));
  ASSERT_TRUE(db_->Erase("a"));
  ASSERT_TRUE(db_->Update("aa", "new-aa"));
  ASSERT_TRUE(db_->Update("ac", "new-ac"));
  ASSERT_TRUE(db_->Erase("not-present"));
  auto cursor = db_->Query("a");
  const vector<pair<string, string>> expected{
      {"aa", "new-aa"}, {"ab", "changed-ab"}, {"ac", "new-ac"}};
  EXPECT_EQ(expected, Records(cursor));
  ASSERT_TRUE(cursor->Reset());
  EXPECT_EQ(expected, Records(cursor));
  ASSERT_TRUE(cursor->Jump("ab"));
  EXPECT_EQ(
      (vector<pair<string, string>>{{"ab", "changed-ab"}, {"ac", "new-ac"}}),
      Records(cursor));
  ASSERT_TRUE(cursor->Jump("b"));
  EXPECT_TRUE(cursor->exhausted());
  ASSERT_TRUE(cursor->Jump("a"));
  EXPECT_EQ(expected, Records(cursor));
}

TEST_P(RimeLevelDbCacheTest, AccessorsRetainCommittedSnapshots) {
  auto before = db_->Query("a");
  ASSERT_TRUE(db_->Update("a", "changed"));
  auto after = db_->Query("a");
  ASSERT_TRUE(db_->Erase("ab"));
  EXPECT_EQ((vector<pair<string, string>>{{"a", "base-a"}, {"ab", "base-ab"}}),
            Records(before));
  EXPECT_EQ((vector<pair<string, string>>{{"a", "changed"}, {"ab", "base-ab"}}),
            Records(after));
  EXPECT_EQ((vector<pair<string, string>>{{"a", "changed"}}),
            Records(db_->Query("a")));
}

TEST_P(RimeLevelDbCacheTest, PendingWritesCommitAndRollback) {
  ASSERT_TRUE(db_->BeginTransaction());
  ASSERT_TRUE(db_->Update("a", "pending"));
  ASSERT_TRUE(db_->Erase("ab"));
  ASSERT_TRUE(db_->Update("ac", "new"));
  string value;
  ASSERT_TRUE(db_->Fetch("a", &value));
  EXPECT_EQ("pending", value);
  EXPECT_EQ("base-a", Value("a"));
  EXPECT_EQ("base-ab", Value("ab"));
  EXPECT_TRUE(Value("ac").empty());
  ASSERT_TRUE(db_->AbortTransaction());
  EXPECT_EQ("base-a", Value("a"));
  ASSERT_TRUE(db_->BeginTransaction());
  ASSERT_TRUE(db_->Update("a", "committed"));
  ASSERT_TRUE(db_->Erase("ab"));
  ASSERT_TRUE(db_->Update("ac", "new"));
  ASSERT_TRUE(db_->CommitTransaction());
  EXPECT_EQ("committed", Value("a"));
  EXPECT_TRUE(Value("ab").empty());
  EXPECT_EQ("new", Value("ac"));
}

TEST_P(RimeLevelDbCacheTest, MetadataAndReopen) {
  ASSERT_TRUE(db_->MetaUpdate("/tick", "100"));
  auto all = Records(db_->QueryAll());
  EXPECT_EQ(3u, all.size());
  auto metadata = Records(db_->QueryMetadata());
  EXPECT_NE(metadata.end(),
            std::find(metadata.begin(), metadata.end(),
                      std::make_pair(string("/tick"), string("100"))));
  auto raw = Records(db_->Query(""));
  EXPECT_EQ(all.size() + metadata.size(), raw.size());
  ASSERT_TRUE(db_->Close());
  ASSERT_TRUE(db_->Open());
  EXPECT_EQ("base-a", Value("a"));
  ASSERT_TRUE(db_->Close());
  ASSERT_TRUE(db_->OpenReadOnly());
  EXPECT_FALSE(db_->Update("a", "rejected"));
  EXPECT_FALSE(db_->Erase("a"));
  EXPECT_FALSE(db_->BeginTransaction());
  EXPECT_EQ("base-a", Value("a"));
}

TEST_P(RimeLevelDbCacheTest, CompactWithoutInvalidatingExistingAccessor) {
  auto before = db_->QueryAll();
  ASSERT_TRUE(db_->BeginTransaction());
  for (int i = 0; i < 1100; ++i)
    ASSERT_TRUE(db_->Update("new-" + std::to_string(i), "value"));
  ASSERT_TRUE(db_->CommitTransaction());
  EXPECT_EQ(3u, Records(before).size());
  EXPECT_EQ(1103u, Records(db_->QueryAll()).size());
  ASSERT_TRUE(db_->Update("new-100", "replaced"));
  ASSERT_TRUE(db_->Erase("new-101"));
  EXPECT_EQ("replaced", Value("new-100"));
  EXPECT_TRUE(Value("new-101").empty());
  EXPECT_EQ(1102u, Records(db_->QueryAll()).size());
}

TEST_P(RimeLevelDbCacheTest, SharedHandlesObserveManualChangesWithoutTick) {
  ASSERT_TRUE(db_->Update("ni \t你", "3,1,10,0,0"));
  UserDictionary writer("cache", db_);
  UserDictionary reader("cache", db_);
  ASSERT_TRUE(writer.Load());
  ASSERT_TRUE(reader.Load());
  writer.SetUserDictMode("manual");
  reader.SetUserDictMode("manual");
  reader.SetWriteOwner(false);
  DictEntry selected;
  selected.text = "你";
  selected.custom_code = "ni ";
  const auto tick = writer.tick();
  ASSERT_TRUE(writer.SetManualPosition(selected, 2));
  UserDictEntryIterator result;
  ASSERT_EQ(1u, reader.LookupWords(&result, "ni", false));
  ASSERT_FALSE(result.exhausted());
  EXPECT_EQ(2, result.Peek()->manual_position);
  EXPECT_EQ(tick, writer.tick());
  ASSERT_TRUE(writer.ResetManualPosition(selected));
  UserDictEntryIterator cleared;
  reader.LookupWords(&cleared, "ni", false);
  EXPECT_TRUE(cleared.exhausted());
  ASSERT_TRUE(reader.Reload());
}

TEST_P(RimeLevelDbCacheTest, RestoreRefreshesCachedRecordsAndMetadata) {
  for (const auto& key : {"a", "ab", "b"})
    ASSERT_TRUE(db_->Erase(key));
  ASSERT_TRUE(db_->MetaUpdate("/tick", "10"));
  ASSERT_TRUE(db_->MetaUpdate("/manual_clock", "1"));
  const string key = "ni \t你";
  const string state = "3,1,10,2,1";
  ASSERT_TRUE(db_->Update(key, state));
  const path backup = dir_ / ("cache" + UserDb::snapshot_extension());
  ASSERT_TRUE(db_->Backup(backup));
  ASSERT_TRUE(db_->Erase(key));
  ASSERT_TRUE(Value(key).empty());
  ASSERT_TRUE(db_->Restore(backup));
  EXPECT_EQ(state, Value(key));
  string persisted;
  ASSERT_TRUE(db_->Fetch(key, &persisted));
  EXPECT_EQ(persisted, Value(key));
}

TEST_P(RimeLevelDbCacheTest, SyncMergerRefreshesValuesAndClocks) {
  const string key = "ni \t你";
  ASSERT_TRUE(db_->MetaUpdate("/tick", "10"));
  ASSERT_TRUE(db_->MetaUpdate("/manual_clock", "1"));
  ASSERT_TRUE(db_->Update(key, "1,1,10,1,1"));
  {
    UserDbMerger merger(db_.get());
    merger.ObserveTick(20);
    merger.ObserveClock(2);
    ASSERT_TRUE(merger.Put(key, "3,2,20,2,2"));
    merger.CloseMerge();
    ASSERT_FALSE(merger.failed());
  }
  string persisted;
  ASSERT_TRUE(db_->Fetch(key, &persisted));
  EXPECT_EQ(persisted, Value(key));
  UserDbState state;
  ASSERT_TRUE(state.Unpack(Value(key)));
  EXPECT_EQ(3, state.commits);
  EXPECT_EQ(20u, state.last_tick);
  EXPECT_EQ(2, state.position);
  EXPECT_EQ(2u, state.manual_stamp);
  const auto metadata = Records(db_->QueryMetadata());
  EXPECT_NE(metadata.end(),
            std::find(metadata.begin(), metadata.end(),
                      std::make_pair(string("/tick"), string("20"))));
}

INSTANTIATE_TEST_SUITE_P(QueryBackends,
                         RimeLevelDbCacheTest,
                         ::testing::Values(false, true));

TEST(RimeLevelDbCacheParityTest, RandomUpdatesMatchLevelDbIterators) {
  const path dir(path(std::filesystem::temp_directory_path()) /
                 boost::uuids::to_string(boost::uuids::random_generator()()));
  ASSERT_TRUE(std::filesystem::create_directory(dir));
  {
    UserDbWrapper<LevelDb> plain(dir / "plain.userdb", "same");
    UserDbWrapper<LevelDb> cached(dir / "cached.userdb", "same");
    ASSERT_TRUE(plain.Open());
    ASSERT_TRUE(cached.Open());
    ASSERT_TRUE(cached.EnableQueryCache());
    std::mt19937 random(12345);
    for (int batch = 0; batch < 24; ++batch) {
      ASSERT_TRUE(plain.BeginTransaction());
      ASSERT_TRUE(cached.BeginTransaction());
      for (int i = 0; i < 120; ++i) {
        const string key = "key-" + std::to_string(random() % 1500);
        const string value = std::to_string(random());
        if (random() % 3) {
          ASSERT_TRUE(plain.Update(key, value));
          ASSERT_TRUE(cached.Update(key, value));
        } else {
          ASSERT_TRUE(plain.Erase(key));
          ASSERT_TRUE(cached.Erase(key));
        }
      }
      if (batch % 4 == 0) {
        ASSERT_TRUE(plain.AbortTransaction());
        ASSERT_TRUE(cached.AbortTransaction());
      } else {
        ASSERT_TRUE(plain.CommitTransaction());
        ASSERT_TRUE(cached.CommitTransaction());
      }
      for (const auto& prefix : {"", "key-1", "key-9", "absent"}) {
        auto a = plain.Query(prefix);
        auto b = cached.Query(prefix);
        for (const auto& seek :
             {string(prefix), string("key-53"), string("z")}) {
          ASSERT_TRUE(a->Jump(seek));
          ASSERT_TRUE(b->Jump(seek));
          string ak, av, bk, bv;
          while (a->GetNextRecord(&ak, &av)) {
            ASSERT_TRUE(b->GetNextRecord(&bk, &bv));
            EXPECT_EQ(ak, bk);
            EXPECT_EQ(av, bv);
          }
          EXPECT_FALSE(b->GetNextRecord(&bk, &bv));
        }
      }
    }
  }
  std::filesystem::remove_all(dir);
}
