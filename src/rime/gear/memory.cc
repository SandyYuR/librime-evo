//
// Copyright RIME Developers
// Distributed under the BSD License
//
// 2013-01-02 GONG Chen <chen.sst@gmail.com>
//
#include <rime/candidate.h>
#include <rime/context.h>
#include <rime/composition.h>
#include <rime/engine.h>
#include <rime/key_event.h>
#include <rime/language.h>
#include <rime/schema.h>
#include <rime/ticket.h>
#include <rime/dict/dictionary.h>
#include <rime/dict/db.h>
#include <rime/dict/user_dictionary.h>
#include <algorithm>
#include <map>
#include <mutex>
#include <rime/gear/memory.h>
#include <rime/gear/table_translator.h>
#include <rime/gear/translator_commons.h>

namespace rime {

void CommitEntry::Clear() {
  text.clear();
  code.clear();
  elements.clear();
}

void CommitEntry::AppendPhrase(const an<Phrase>& phrase) {
  text += phrase->text();
  code.insert(code.end(), phrase->code().begin(), phrase->code().end());
  if (auto sentence = As<Sentence>(phrase)) {
    for (const DictEntry& e : sentence->components()) {
      elements.push_back(&e);
    }
  } else {
    elements.push_back(&phrase->entry());
  }
}

bool CommitEntry::Save() const {
  if (memory && !empty()) {
    if (!memory->StartSession())
      return false;
    DLOG(INFO) << "memorize commit entry: " << text;
    return memory->Memorize(*this);
  }
  return false;
}

int CommitEntry::Length() const {
  int length = 0;
  for (const DictEntry* e : elements) {
    if (e) {
      length += e->code.size();
    }
  }
  return length;
}

namespace {
struct MemoryRegistry {
  std::mutex mutex;
  std::map<Engine*, std::vector<Memory*>> by_engine;
  // Registration order defines the sole writer for each shared UserDB.
  std::map<const Db*, std::vector<Memory*>> by_storage;
  std::map<const Db*, Memory*> writer_owner;
  std::map<const Db*, Memory*> session_owner;
};
MemoryRegistry& MemoryRegistryInstance() {
  static MemoryRegistry sources;
  return sources;
}
}  // namespace

Memory::Memory(const Ticket& ticket) : manual_engine_(ticket.engine) {
  if (!ticket.engine)
    return;

  if (auto dictionary = Dictionary::Require("dictionary")) {
    dict_.reset(dictionary->Create(ticket));
    if (dict_)
      dict_->Load();
  }

  if (auto user_dictionary = UserDictionary::Require("user_dictionary")) {
    user_dict_.reset(user_dictionary->Create(ticket));
    if (user_dict_) {
      user_dict_->Load();
      if (dict_)
        user_dict_->Attach(dict_->primary_table(), dict_->prism());
    }
  }

  // 用户词库名称优先，词典名称只取语言部分。
  language_.reset(
      user_dict_ ? new Language{user_dict_->name()}
      : dict_    ? new Language{Language::get_language_component(dict_->name())}
                 : nullptr);

  if (user_dict_ && user_dict_->loaded()) {
    auto& sources = MemoryRegistryInstance();
    std::lock_guard<std::mutex> lock(sources.mutex);
    sources.by_engine[ticket.engine].push_back(this);
    const Db* storage = user_dict_->storage_identity();
    sources.by_storage[storage].push_back(this);
    auto owner = sources.writer_owner.find(storage);
    if (owner == sources.writer_owner.end() && !user_dict_->readonly()) {
      sources.writer_owner.emplace(storage, this);
      user_db_writer_ = true;
    } else {
      user_db_writer_ =
          owner != sources.writer_owner.end() && owner->second == this;
    }
    user_dict_->SetWriteOwner(user_db_writer_);
  }

  Context* ctx = ticket.engine->context();
  commit_connection_ =
      ctx->commit_notifier().connect([this](Context* ctx) { OnCommit(ctx); });
  delete_connection_ = ctx->delete_notifier().connect(
      [this](Context* ctx) { OnDeleteEntry(ctx); });
  unhandled_key_connection_ = ctx->unhandled_key_notifier().connect(
      [this](Context* ctx, const KeyEvent& key) { OnUnhandledKey(ctx, key); });
}

Memory::~Memory() {
  commit_connection_.disconnect();
  delete_connection_.disconnect();
  unhandled_key_connection_.disconnect();
  if (!user_dict_)
    return;

  auto& sources = MemoryRegistryInstance();
  std::lock_guard<std::mutex> lock(sources.mutex);
  const Db* storage = user_dict_->storage_identity();

  // Abort only this handle's own batch. This is deliberately unconditional:
  // the registry entry may already have gone stale, but the handle still
  // knows whether it opened the transaction.
  if (user_dict_->HasPendingTransaction())
    user_dict_->AbortPendingTransaction();
  session_active_ = false;
  auto session = sources.session_owner.find(storage);
  if (session != sources.session_owner.end() && session->second == this)
    sources.session_owner.erase(session);

  if (manual_engine_) {
    auto engine = sources.by_engine.find(manual_engine_);
    if (engine != sources.by_engine.end()) {
      auto& memories = engine->second;
      memories.erase(std::remove(memories.begin(), memories.end(), this),
                     memories.end());
      if (memories.empty())
        sources.by_engine.erase(engine);
    }
  }

  auto storage_list = sources.by_storage.find(storage);
  if (storage_list != sources.by_storage.end()) {
    auto& memories = storage_list->second;
    memories.erase(std::remove(memories.begin(), memories.end(), this),
                   memories.end());
    if (memories.empty())
      sources.by_storage.erase(storage_list);
  }

  auto writer = sources.writer_owner.find(storage);
  if (writer != sources.writer_owner.end() && writer->second == this) {
    sources.writer_owner.erase(writer);
    auto remaining = sources.by_storage.find(storage);
    if (remaining != sources.by_storage.end()) {
      for (Memory* candidate : remaining->second) {
        if (candidate && candidate->user_dict_ &&
            candidate->user_dict_->loaded() &&
            !candidate->user_dict_->readonly()) {
          sources.writer_owner[storage] = candidate;
          candidate->user_db_writer_ = true;
          candidate->user_dict_->SetWriteOwner(true);
          break;
        }
      }
    }
  }
  user_db_writer_ = false;
  user_dict_->SetWriteOwner(false);
}

Memory* Memory::FindManualSource(Engine* engine, const Language* source) {
  if (!engine || !source)
    return nullptr;
  auto& sources = MemoryRegistryInstance();
  std::lock_guard<std::mutex> lock(sources.mutex);
  auto it = sources.by_engine.find(engine);
  if (it == sources.by_engine.end())
    return nullptr;
  for (Memory* memory : it->second) {
    if (memory->language() == source && memory->user_dict() &&
        memory->user_dict()->loaded() && !memory->user_dict()->readonly())
      return memory;
  }
  return nullptr;
}

bool Memory::OwnsPhrase(const an<Phrase>& phrase) const {
  return phrase && OwnsLanguage(phrase->language());
}

bool Memory::StartSession() {
  if (!user_dict_ || !user_db_writer_ || !user_dict_->write_owner())
    return false;
  auto& sources = MemoryRegistryInstance();
  std::lock_guard<std::mutex> lock(sources.mutex);
  const Db* storage = user_dict_->storage_identity();
  auto writer = sources.writer_owner.find(storage);
  if (writer == sources.writer_owner.end() || writer->second != this)
    return false;

  auto owner = sources.session_owner.find(storage);
  const auto* tx = dynamic_cast<const Transactional*>(storage);
  if (!tx)
    return false;
  if (tx->in_transaction()) {
    session_active_ = owner != sources.session_owner.end() &&
                      owner->second == this &&
                      user_dict_->HasPendingTransaction();
    return session_active_;
  }
  if (owner != sources.session_owner.end())
    sources.session_owner.erase(owner);  // stale owner after an external close
  if (!user_dict_->NewTransaction())
    return false;
  sources.session_owner[storage] = this;
  session_active_ = true;
  return true;
}

bool Memory::FinishSession() {
  if (!user_dict_ || !user_db_writer_ || !session_active_)
    return true;
  auto& sources = MemoryRegistryInstance();
  std::lock_guard<std::mutex> lock(sources.mutex);
  const Db* storage = user_dict_->storage_identity();
  auto it = sources.session_owner.find(storage);
  const auto* tx = dynamic_cast<const Transactional*>(storage);
  if (!tx)
    return false;
  if (it == sources.session_owner.end() || it->second != this) {
    if (!tx->in_transaction())
      session_active_ = false;
    return false;
  }
  if (!tx->in_transaction()) {
    user_dict_->AbortPendingTransaction();
    sources.session_owner.erase(it);
    session_active_ = false;
    return true;
  }
  if (!user_dict_->CommitPendingTransaction())
    return false;
  sources.session_owner.erase(it);
  session_active_ = false;
  return true;
}

bool Memory::DiscardSession() {
  if (!user_dict_ || !user_db_writer_ || !session_active_)
    return false;
  auto& sources = MemoryRegistryInstance();
  std::lock_guard<std::mutex> lock(sources.mutex);
  const Db* storage = user_dict_->storage_identity();
  auto it = sources.session_owner.find(storage);
  const auto* tx = dynamic_cast<const Transactional*>(storage);
  if (!tx)
    return false;
  if (it == sources.session_owner.end()) {
    if (!tx->in_transaction())
      session_active_ = false;
    return !tx->in_transaction();
  }
  if (it->second != this)
    return false;
  if (!tx->in_transaction()) {
    user_dict_->AbortPendingTransaction();
    sources.session_owner.erase(it);
    session_active_ = false;
    return true;
  }
  if (!user_dict_->RevertRecentTransaction())
    return false;
  sources.session_owner.erase(it);
  session_active_ = false;
  return true;
}

bool Memory::AbortSession() {
  if (!user_dict_ || !user_db_writer_ || !session_active_)
    return false;
  auto& sources = MemoryRegistryInstance();
  std::lock_guard<std::mutex> lock(sources.mutex);
  const Db* storage = user_dict_->storage_identity();
  auto it = sources.session_owner.find(storage);
  const auto* tx = dynamic_cast<const Transactional*>(storage);
  if (!tx)
    return false;
  if (it == sources.session_owner.end()) {
    if (!tx->in_transaction())
      session_active_ = false;
    return !tx->in_transaction();
  }
  if (it->second != this)
    return false;
  if (!user_dict_->AbortPendingTransaction())
    return false;
  sources.session_owner.erase(it);
  session_active_ = false;
  return true;
}

bool Memory::FinishSharedSession(UserDictionary* user_dict) {
  if (!user_dict)
    return false;
  auto& sources = MemoryRegistryInstance();
  std::lock_guard<std::mutex> lock(sources.mutex);
  const Db* storage = user_dict->storage_identity();
  const auto* tx = dynamic_cast<const Transactional*>(storage);
  if (!tx)
    return false;

  auto writer = sources.writer_owner.find(storage);
  if (writer == sources.writer_owner.end() || !writer->second ||
      !writer->second->user_dict_)
    return false;
  Memory* owner = writer->second;
  auto session = sources.session_owner.find(storage);
  if (!tx->in_transaction()) {
    if (session != sources.session_owner.end() && session->second == owner) {
      owner->user_dict_->AbortPendingTransaction();
      sources.session_owner.erase(session);
      owner->session_active_ = false;
    }
    return true;
  }
  if (session == sources.session_owner.end() || session->second != owner ||
      !owner->user_dict_->HasPendingTransaction()) {
    LOG(WARNING) << "UserDB manual action blocked by an unowned transaction: "
                 << user_dict->name();
    return false;
  }
  if (!owner->user_dict_->CommitPendingTransaction())
    return false;
  sources.session_owner.erase(session);
  owner->session_active_ = false;
  return true;
}

bool Memory::ProcessSegmentOnCommit(CommitEntry& commit_entry,
                                    const Segment& seg) {
  auto phrase =
      As<Phrase>(Candidate::GetGenuineCandidate(seg.GetSelectedCandidate()));
  const bool recognized = OwnsPhrase(phrase);
  if (recognized)
    commit_entry.AppendPhrase(phrase);
  if (!recognized || seg.status >= Segment::kConfirmed) {
    if (!commit_entry.empty() && !commit_entry.Save())
      return false;
    commit_entry.Clear();
  }
  return true;
}

bool Memory::FlushPendingCommit(CommitEntry& commit_entry) {
  if (commit_entry.empty())
    return true;
  const bool saved = commit_entry.Save();
  commit_entry.Clear();
  return saved;
}

void Memory::OnCommit(Context* ctx) {
  if (!ctx || !user_dict_ || !user_dict_->loaded() || user_dict_->readonly() ||
      !user_db_writer_ || !user_dict_->automatic_learning_enabled())
    return;
  // Start lazily in CommitEntry::Save(): process the composition once and
  // avoid opening an empty transaction for foreign or non-learning entries.
  CommitEntry commit_entry(this);
  bool ok = true;
  for (const auto& seg : ctx->composition()) {
    if (!ProcessSegmentOnCommit(commit_entry, seg)) {
      ok = false;
      break;
    }
  }
  if (ok)
    ok = FlushPendingCommit(commit_entry);
  if (!ok && !AbortSession())
    LOG(ERROR) << "failed to abort UserDB learning transaction";
}

void Memory::OnDeleteEntry(Context* ctx) {
  if (!ctx || !user_dict_ || !user_db_writer_ || user_dict_->readonly() ||
      !user_dict_->automatic_learning_enabled() || !ctx->HasMenu())
    return;
  auto phrase =
      As<Phrase>(Candidate::GetGenuineCandidate(ctx->GetSelectedCandidate()));
  if (OwnsPhrase(phrase) && user_dict_->UpdateEntry(phrase->entry(), -1)) {
    ctx->RefreshNonConfirmedComposition();
  }
}

void Memory::OnUnhandledKey(Context*, const KeyEvent& key) {
  if (!user_dict_ || !user_db_writer_ || user_dict_->readonly())
    return;
  if (session_active_ && (key.modifier() & ~kShiftMask) == 0) {
    if (key.keycode() == XK_BackSpace && DiscardSession())
      return;
    FinishSession();
  }
}

}  // namespace rime
