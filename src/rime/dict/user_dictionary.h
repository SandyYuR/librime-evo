//
// Copyright RIME Developers
// Distributed under the BSD License
//
// 2011-10-30 GONG Chen <chen.sst@gmail.com>
//
#ifndef RIME_USER_DICTIONARY_H_
#define RIME_USER_DICTIONARY_H_

#ifndef RIME_USER_DICT_CACHE_ENABLED
#define RIME_USER_DICT_CACHE_ENABLED 1
#endif

#include <time.h>
#include <atomic>
#include <memory>
#include <rime/common.h>
#include <rime/component.h>
#include <rime/dict/user_db.h>
#include <rime/dict/vocabulary.h>

namespace rime {

class RIME_DLL UserDictEntryIterator : public DictEntryFilterBinder {
 public:
  UserDictEntryIterator() = default;

  void Add(an<DictEntry>&& entry);
  void SetEntries(DictEntryList&& entries);
  void SortRange(size_t start, size_t count);

  void AddFilter(DictEntryFilter filter) override;
  an<DictEntry> Peek();
  bool Next();
  bool exhausted() const { return index_ >= cache_.size(); }
  size_t cache_size() const { return cache_.size(); }
  DictEntryList& entries() { return cache_; }
  const DictEntryList& entries() const { return cache_; }
  void SortAll();
  bool Allows(const an<DictEntry>& entry) const {
    return !filter_ || filter_(entry);
  }

 protected:
  bool FindNextEntry();

  DictEntryList cache_;
  size_t index_ = 0;
};

using UserDictEntryCollector = map<size_t, UserDictEntryIterator>;

class Schema;
class Table;
class Prism;
class Db;
struct SyllableGraph;
struct DfsState;
struct Ticket;

class UserDictionary : public Class<UserDictionary, const Ticket&> {
 public:
  UserDictionary(const string& name,
                 an<Db> db,
                 std::shared_ptr<std::atomic<TickCount>> shared_tick = nullptr);
  virtual ~UserDictionary();

  void Attach(const an<Table>& table, const an<Prism>& prism);
  bool Load();
  // Revalidate metadata and rebuild the shared query snapshot.
  RIME_DLL bool Reload();
  bool loaded() const;
  bool readonly() const;
  void SetWriteOwner(bool owner) {
    if (owner && !write_owner_)
      tick_ = shared_tick_->load(std::memory_order_relaxed);
    write_owner_ = owner;
  }
  bool write_owner() const { return write_owner_; }
  bool HasPendingTransaction() const { return transaction_active_; }

  an<UserDictEntryCollector> Lookup(const SyllableGraph& syllable_graph,
                                    size_t start_pos,
                                    size_t depth_limit = 0,
                                    size_t predict_word_from_depth = 0,
                                    double initial_credibility = 0.0);
  size_t LookupWords(UserDictEntryIterator* result,
                     const string& input,
                     bool predictive,
                     size_t limit = 0,
                     string* resume_key = NULL);
  bool UpdateEntry(const DictEntry& entry, int commits);
  bool UpdateEntry(const DictEntry& entry,
                   int commits,
                   const string& new_entry_prefix);
  // 同一记录包含自动学习和手排数据。
  bool GetUserState(const DictEntry& entry, UserDbState* state);
  // 按所属词典解析编码，跨翻译器手排必须先规范化编码。
  bool GetEntryCode(const DictEntry& entry, string* code);
  bool SharesDatabaseWith(const UserDictionary& other) const {
    return db_ && db_.get() == other.db_.get();
  }
  const Db* storage_identity() const { return db_.get(); }
  // 只移动相邻候选；目标词无需预先入库。
  bool MoveManualOneStep(const DictEntry& selected,
                         const DictEntry& neighbor,
                         int source_position,
                         int direction,
                         bool neighbor_visible = false);
  bool SetManualPosition(const DictEntry& entry, int position);
  // 自动学习与手排分别清除。
  bool ResetAutoLearning(
      const DictEntry& entry);  // mark commits<0; retain dynamic/last_tick and
                                // position/manual_stamp
  bool ResetManualPosition(
      const DictEntry& entry);  // position=0, new manual_stamp; keep
                                // commits/dynamic/last_tick
  bool UpdateTickCount(TickCount increment);
  void SetUserDictMode(const string& mode) { user_dict_mode_ = mode; }
  const string& user_dict_mode() const { return user_dict_mode_; }
  bool automatic_learning_enabled() const { return user_dict_mode_ == "auto"; }

  bool NewTransaction();
  bool RevertRecentTransaction();
  bool CommitPendingTransaction();
  bool AbortPendingTransaction();

  const string& name() const { return name_; }
  TickCount tick() const {
    return write_owner_ ? tick_ : shared_tick_->load(std::memory_order_relaxed);
  }

  RIME_DLL static an<DictEntry> CreateDictEntry(const string& key,
                                                const string& value,
                                                TickCount present_tick,
                                                double credibility = 0.0,
                                                double quality_len = 0.0,
                                                string* full_code = nullptr);
  RIME_DLL static an<DictEntry> CreateDictEntry(const string& key,
                                                const string& value,
                                                TickCount present_tick,
                                                double credibility,
                                                double quality_len,
                                                string* full_code,
                                                bool use_automatic);

 protected:
  bool TranslateCodeToString(const Code& code, string* result);
  void DfsLookup(const SyllableGraph& syll_graph,
                 size_t current_pos,
                 const string& current_prefix,
                 DfsState* state);

 private:
  string name_;
  an<Db> db_;
  std::shared_ptr<std::atomic<TickCount>> shared_tick_;
  an<Table> table_;
  an<Prism> prism_;
  hash_map<string, SyllableId> syllabary_;
  hash_map<SyllableId, string> rev_syllabary_;
  TickCount tick_ = 0;
  string user_dict_mode_ = "auto";
  time_t transaction_time_ = 0;
  bool load_attempted_ = false;
  bool load_valid_ = false;
  bool write_owner_ = true;
  bool transaction_active_ = false;
};

class UserDictionaryComponent : public UserDictionary::Component {
 public:
  UserDictionaryComponent();
  UserDictionary* Create(const Ticket& ticket);
  UserDictionary* Create(const string& dict_name, const string& db_class);

 private:
  hash_map<string, weak<Db>> db_pool_;
  hash_map<string, std::weak_ptr<std::atomic<TickCount>>> tick_pool_;
};

}  // namespace rime

#endif  // RIME_USER_DICTIONARY_H_
