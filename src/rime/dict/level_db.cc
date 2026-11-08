//
// Copyright RIME Developers
// Distributed under the BSD License
//
// 2014-12-04 Chen Gong <chen.sst@gmail.com>
//

#include <algorithm>
#include <map>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <leveldb/cache.h>
#include <leveldb/db.h>
#include <leveldb/write_batch.h>
#include <rime/common.h>
#include <rime/service.h>
#include <rime/dict/level_db.h>
#include <rime/dict/user_db.h>

namespace rime {

static const char* kMetaCharacter = "\x01";

static leveldb::Cache* SharedBlockCache() {
  // All user dictionaries share the disk block cache; queries use sorted data.
  static leveldb::Cache* cache = leveldb::NewLRUCache(2 << 20);
  return cache;
}

// A compact immutable base plus a bounded overlay avoids copying the full
// dictionary on each learning update. Accessors retain their own snapshots.
struct LevelDbSnapshot {
  struct Record {
    size_t offset, key_size, value_size;
  };
  string data;
  vector<Record> records;
  std::string_view Key(size_t i) const {
    const auto& r = records[i];
    return {data.data() + r.offset, r.key_size};
  }
  std::string_view Value(size_t i) const {
    const auto& r = records[i];
    return {data.data() + r.offset + r.key_size, r.value_size};
  }
};
using LevelDbChanges = map<string, std::optional<string>>;

class CachedLevelDbAccessor : public DbAccessor {
 public:
  CachedLevelDbAccessor(an<const LevelDbSnapshot> base,
                        an<const LevelDbChanges> changes,
                        const string& prefix)
      : DbAccessor(prefix),
        base_(std::move(base)),
        changes_(std::move(changes)) {
    Reset();
  }
  bool Reset() override { return Jump(prefix_); }
  bool Jump(const string& key) override {
    auto it = std::lower_bound(
        base_->records.begin(), base_->records.end(), key,
        [&](const LevelDbSnapshot::Record& r, const string& needle) {
          return std::string_view(base_->data.data() + r.offset, r.key_size) <
                 needle;
        });
    index_ = it - base_->records.begin();
    changed_ = changes_->lower_bound(key);
    return true;
  }
  bool exhausted() override {
    while (index_ < base_->records.size() || changed_ != changes_->end()) {
      const bool has_base = index_ < base_->records.size();
      use_change_ = changed_ != changes_->end() &&
                    (!has_base || changed_->first <= base_->Key(index_));
      if (use_change_ && has_base && changed_->first == base_->Key(index_))
        ++index_;
      if (use_change_ && !changed_->second) {
        ++changed_;
        continue;
      }
      const auto key =
          use_change_ ? std::string_view(changed_->first) : base_->Key(index_);
      return key.size() < prefix_.size() ||
             key.substr(0, prefix_.size()) != prefix_;
    }
    return true;
  }
  bool GetNextRecord(string* key, string* value) override {
    if (!key || !value || exhausted())
      return false;
    if (use_change_) {
      *key = changed_->first;
      *value = *changed_->second;
      ++changed_;
    } else {
      key->assign(base_->Key(index_));
      value->assign(base_->Value(index_));
      ++index_;
    }
    if (prefix_ == kMetaCharacter)
      key->erase(0, 1);
    return true;
  }

 private:
  an<const LevelDbSnapshot> base_;
  an<const LevelDbChanges> changes_;
  size_t index_ = 0;
  LevelDbChanges::const_iterator changed_;
  bool use_change_ = false;
};

struct LevelDbCursor {
  leveldb::Iterator* iterator = nullptr;

  LevelDbCursor(leveldb::DB* db) {
    leveldb::ReadOptions options;
    options.fill_cache = false;
    iterator = db->NewIterator(options);
  }

  bool IsValid() const { return iterator && iterator->Valid(); }

  string GetKey() const { return iterator->key().ToString(); }

  string GetValue() const { return iterator->value().ToString(); }

  void Next() { iterator->Next(); }

  bool Jump(const string& key) {
    if (!iterator) {
      return false;
    }
    iterator->Seek(key);
    return true;
  }

  void Release() {
    delete iterator;
    iterator = nullptr;
  }
};

struct LevelDbWrapper {
  leveldb::DB* ptr = nullptr;
  leveldb::WriteBatch batch;
  an<LevelDbSnapshot> snapshot;
  an<LevelDbChanges> changes = New<LevelDbChanges>();

  bool BuildQueryCache() {
    auto next = New<LevelDbSnapshot>();
    leveldb::ReadOptions options;
    options.fill_cache = false;
    std::unique_ptr<leveldb::Iterator> it(ptr->NewIterator(options));
    for (it->SeekToFirst(); it->Valid(); it->Next()) {
      const auto key = it->key();
      const auto value = it->value();
      next->records.push_back({next->data.size(), key.size(), value.size()});
      next->data.append(key.data(), key.size());
      next->data.append(value.data(), value.size());
    }
    if (!it->status().ok())
      return false;
    snapshot = std::move(next);
    changes = New<LevelDbChanges>();
    return true;
  }

  void CacheChange(const string& key, const std::optional<string>& value) {
    if (!snapshot)
      return;
    if (!changes.unique())
      changes = New<LevelDbChanges>(*changes);
    (*changes)[key] = value;
  }

  void CompactQueryCache() {
    if (snapshot && changes->size() > 1000)
      BuildQueryCache();
  }
  // LevelDB Get() cannot see a pending WriteBatch. A bounded per-batch
  // overlay guarantees read-your-writes for repeated learning in one commit.
  std::unordered_map<string, std::optional<string>> pending;

  leveldb::Status Open(const path& file_path, bool readonly) {
    leveldb::Options options;
    options.create_if_missing = !readonly;
    options.block_cache = SharedBlockCache();
    options.write_buffer_size = 2 << 20;
    return leveldb::DB::Open(options, file_path.string(), &ptr);
  }

  void Release() {
    snapshot.reset();
    changes = New<LevelDbChanges>();
    delete ptr;
    ptr = nullptr;
  }

  LevelDbCursor* CreateCursor() { return new LevelDbCursor(ptr); }

  bool Fetch(const string& key, string* value) {
    if (auto it = pending.find(key); it != pending.end()) {
      if (!it->second)
        return false;
      *value = *it->second;
      return true;
    }
    auto status = ptr->Get(leveldb::ReadOptions(), key, value);
    return status.ok();
  }

  bool Update(const string& key, const string& value, bool write_batch) {
    if (write_batch) {
      batch.Put(key, value);
      pending[key] = value;
      return true;
    }
    auto status = ptr->Put(leveldb::WriteOptions(), key, value);
    if (status.ok()) {
      CacheChange(key, value);
      CompactQueryCache();
    }
    return status.ok();
  }

  bool Erase(const string& key, bool write_batch) {
    if (write_batch) {
      batch.Delete(key);
      pending[key] = std::nullopt;
      return true;
    }
    auto status = ptr->Delete(leveldb::WriteOptions(), key);
    if (status.ok()) {
      CacheChange(key, std::nullopt);
      CompactQueryCache();
    }
    return status.ok();
  }

  void ClearBatch() {
    batch.Clear();
    pending.clear();
  }

  bool CommitBatch() {
    auto status = ptr->Write(leveldb::WriteOptions(), &batch);
    if (status.ok()) {
      // Query iterators see committed records, just like LevelDB snapshots.
      for (const auto& change : pending)
        CacheChange(change.first, change.second);
      CompactQueryCache();
    }
    return status.ok();
  }
};

// LevelDbAccessor members

LevelDbAccessor::LevelDbAccessor() {}

LevelDbAccessor::LevelDbAccessor(LevelDbCursor* cursor, const string& prefix)
    : DbAccessor(prefix),
      cursor_(cursor),
      is_metadata_query_(prefix == kMetaCharacter) {
  Reset();
}

LevelDbAccessor::~LevelDbAccessor() {
  cursor_->Release();
}

bool LevelDbAccessor::Reset() {
  return cursor_->Jump(prefix_);
}

bool LevelDbAccessor::Jump(const string& key) {
  return cursor_->Jump(key);
}

bool LevelDbAccessor::GetNextRecord(string* key, string* value) {
  if (!cursor_->IsValid() || !key || !value)
    return false;
  *key = cursor_->GetKey();
  if (!MatchesPrefix(*key)) {
    return false;
  }
  if (is_metadata_query_) {
    key->erase(0, 1);  // remove meta character
  }
  *value = cursor_->GetValue();
  cursor_->Next();
  return true;
}

bool LevelDbAccessor::exhausted() {
  return !cursor_->IsValid() || !MatchesPrefix(cursor_->GetKey());
}

// LevelDb members

LevelDb::LevelDb(const path& file_path,
                 const string& db_name,
                 const string& db_type)
    : Db(file_path, db_name), db_type_(db_type) {}

LevelDb::~LevelDb() {
  if (loaded())
    Close();
}

void LevelDb::Initialize() {
  db_.reset(new LevelDbWrapper);
}

an<DbAccessor> LevelDb::QueryMetadata() {
  return Query(kMetaCharacter);
}

an<DbAccessor> LevelDb::QueryAll() {
  an<DbAccessor> all = Query("");
  if (all)
    all->Jump(" ");  // skip metadata
  return all;
}

an<DbAccessor> LevelDb::Query(const string& key) {
  if (!loaded())
    return nullptr;
  if (query_cache_enabled_ && (db_->snapshot || db_->BuildQueryCache()))
    return New<CachedLevelDbAccessor>(db_->snapshot, db_->changes, key);
  return New<LevelDbAccessor>(db_->CreateCursor(), key);
}

bool LevelDb::EnableQueryCache() {
  if (!loaded())
    return false;
  // Populate on the first query, so unused translator handles do not scan DBs.
  query_cache_enabled_ = true;
  return true;
}

bool LevelDb::RebuildQueryCache() {
  return loaded() && (!query_cache_enabled_ || db_->BuildQueryCache());
}

bool LevelDb::Fetch(const string& key, string* value) {
  if (!value || !loaded())
    return false;
  return db_->Fetch(key, value);
}

bool LevelDb::Update(const string& key, const string& value) {
  if (!loaded() || readonly())
    return false;
  DLOG(INFO) << "update db entry: " << key << " => " << value;
  return db_->Update(key, value, in_transaction());
}

bool LevelDb::Erase(const string& key) {
  if (!loaded() || readonly())
    return false;
  DLOG(INFO) << "erase db entry: " << key;
  return db_->Erase(key, in_transaction());
}

bool LevelDb::Backup(const path& snapshot_file) {
  if (!loaded())
    return false;
  LOG(INFO) << "backing up db '" << name() << "' to " << snapshot_file;
  // TODO(chen): suppose we only use this method for user dbs.
  bool success = UserDbHelper(this).UniformBackup(snapshot_file);
  if (!success) {
    LOG(ERROR) << "failed to create snapshot file '" << snapshot_file
               << "' for db '" << name() << "'.";
  }
  return success;
}

bool LevelDb::Restore(const path& snapshot_file) {
  if (!loaded() || readonly())
    return false;
  // TODO(chen): suppose we only use this method for user dbs.
  bool success = UserDbHelper(this).UniformRestore(snapshot_file);
  if (!success) {
    LOG(ERROR) << "failed to restore db '" << name() << "' from '"
               << snapshot_file << "'.";
  }
  return success;
}

bool LevelDb::Recover() {
  LOG(INFO) << "trying to recover db '" << name() << "'.";
  leveldb::Options options;
  options.block_cache = SharedBlockCache();
  auto status = leveldb::RepairDB(file_path().string(), options);
  if (status.ok()) {
    LOG(INFO) << "repair finished.";
    return true;
  }
  LOG(ERROR) << "db recovery failed: " << status.ToString();
  return false;
}

bool LevelDb::Remove() {
  if (loaded()) {
    LOG(ERROR) << "attempt to remove opened db '" << name() << "'.";
    return false;
  }
  auto status = leveldb::DestroyDB(file_path().string(), leveldb::Options());
  if (!status.ok()) {
    LOG(ERROR) << "Error removing db '" << name() << "': " << status.ToString();
    return false;
  }
  return true;
}

bool LevelDb::Open() {
  if (loaded())
    return false;
  Initialize();
  readonly_ = false;
  auto status = db_->Open(file_path(), readonly_);
  loaded_ = status.ok();

  if (loaded_) {
    string db_name;
    if (!MetaFetch("/db_name", &db_name)) {
      if (!CreateMetadata()) {
        LOG(ERROR) << "error creating metadata.";
        Close();
      }
    }
  } else {
    LOG(ERROR) << "Error opening db '" << name() << "': " << status.ToString();
  }
  return loaded_;
}

bool LevelDb::OpenReadOnly() {
  if (loaded())
    return false;
  Initialize();
  readonly_ = true;
  auto status = db_->Open(file_path(), readonly_);
  loaded_ = status.ok();

  if (!loaded_) {
    LOG(ERROR) << "Error opening db '" << name()
               << "' read-only: " << status.ToString();
  }
  return loaded_;
}

bool LevelDb::Close() {
  if (!loaded())
    return false;

  db_->Release();

  LOG(INFO) << "closed db '" << name() << "'.";
  loaded_ = false;
  readonly_ = false;
  in_transaction_ = false;
  return true;
}

bool LevelDb::CreateMetadata() {
  return Db::CreateMetadata() && MetaUpdate("/db_type", db_type_);
}

bool LevelDb::MetaFetch(const string& key, string* value) {
  return Fetch(kMetaCharacter + key, value);
}

bool LevelDb::MetaUpdate(const string& key, const string& value) {
  return Update(kMetaCharacter + key, value);
}

bool LevelDb::BeginTransaction() {
  // A nested BeginTransaction used to silently discard an uncommitted batch.
  // A read-only handle may never acquire a write transaction.
  if (!loaded() || readonly() || in_transaction())
    return false;
  db_->ClearBatch();
  in_transaction_ = true;
  return true;
}

bool LevelDb::AbortTransaction() {
  if (!loaded() || !in_transaction())
    return false;
  db_->ClearBatch();
  in_transaction_ = false;
  return true;
}

bool LevelDb::CommitTransaction() {
  if (!loaded() || !in_transaction())
    return false;
  bool ok = db_->CommitBatch();
  db_->ClearBatch();
  in_transaction_ = false;
  return ok;
}

template <>
RIME_DLL string UserDbComponent<LevelDb>::extension() const {
  return ".userdb";
}

template <>
RIME_DLL UserDbWrapper<LevelDb>::UserDbWrapper(const path& file_path,
                                               const string& db_name)
    : LevelDb(file_path, db_name, "userdb") {}

}  // namespace rime
