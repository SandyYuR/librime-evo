//
// Copyright RIME Developers
// Distributed under the BSD License
//
// 2013-01-02 GONG Chen <chen.sst@gmail.com>
//
#ifndef RIME_MEMORY_H_
#define RIME_MEMORY_H_

#include <rime/common.h>
#include <rime/dict/vocabulary.h>

namespace rime {

class KeyEvent;
class Context;
class Engine;
class Dictionary;
class UserDictionary;
class Language;
class Phrase;
class Segment;
class Memory;

struct CommitEntry : DictEntry {
  vector<const DictEntry*> elements;
  Memory* memory;

  CommitEntry(Memory* a_memory = NULL) : memory(a_memory) {}
  bool empty() const { return text.empty(); }
  void Clear();
  void AppendPhrase(const an<Phrase>& phrase);
  bool Save() const;
  int Length() const;
};

class Memory {
 public:
  Memory(const Ticket& ticket);
  virtual ~Memory();

  virtual bool Memorize(const CommitEntry& commit_entry) = 0;
  virtual bool ProcessSegmentOnCommit(CommitEntry& commit_entry,
                                      const Segment& seg);
  bool StartSession();
  bool FinishSession();
  bool DiscardSession();
  bool AbortSession();
  // 手排前提交该共享数据库当前真正持有的学习事务。
  static bool FinishSharedSession(UserDictionary* user_dict);
  // 提交前处理最后一个选中片段。
  virtual bool FlushPendingCommit(CommitEntry& commit_entry);

  Dictionary* dict() const { return dict_.get(); }
  UserDictionary* user_dict() const { return user_dict_.get(); }

  const Language* language() const { return language_.get(); }
  static Memory* FindManualSource(Engine* engine, const Language* source);
  bool IsUserDbWriter() const { return user_db_writer_; }

 protected:
  bool OwnsLanguage(const Language* source) const {
    return source && source == language();
  }
  bool OwnsPhrase(const an<Phrase>& phrase) const;
  void OnCommit(Context* ctx);
  void OnUnhandledKey(Context* ctx, const KeyEvent& key);
  void OnDeleteEntry(Context* ctx);

  the<Dictionary> dict_;
  the<UserDictionary> user_dict_;
  the<Language> language_;
  Engine* manual_engine_ = nullptr;
  bool user_db_writer_ = false;
  bool session_active_ = false;

 private:
  connection commit_connection_;
  connection unhandled_key_connection_;
  connection delete_connection_;
};

}  // namespace rime

#endif  // RIME_MEMORY_H_
