// Copyright RIME Developers
// Distributed under the BSD License
#include <rime/dict/user_manual_order.h>
#include <rime/dict/user_db.h>
#include <rime/dict/manual_stamp_codec.h>
#include <rime/dict/db.h>
#include <limits>
#include <chrono>
#include <algorithm>

namespace rime {
namespace user_manual_order {
namespace {
bool NextClock(Db* db, uint64_t* version) {
  uint64_t last = 0;
  string raw;
  if (db->MetaFetch("/manual_clock", &raw)) {
    if (!manual_stamp_codec::Parse(raw, &last))
      return false;
  }
  if (last == std::numeric_limits<uint64_t>::max())
    return false;
  // 手排时间转换为短版本，连续操作保证递增。
  const auto now = std::chrono::system_clock::now();
  const auto seconds =
      std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch())
          .count();
  if (seconds < 0)
    return false;
  const uint64_t wall_stamp =
      manual_stamp_codec::FromUnixSeconds(static_cast<uint64_t>(seconds));
  *version = (std::max)(wall_stamp, last + 1);
  return true;
}
bool SaveClock(Db* db, uint64_t version) {
  return db->MetaUpdate("/manual_clock", manual_stamp_codec::Format(version));
}
// 同码候选只扫描一次，避免重复遍历。
bool FindPinnedOwners(Db* db,
                      const string& code,
                      int source_position,
                      int target_position,
                      string* source_owner,
                      string* target_owner) {
  if (!source_owner || !target_owner)
    return false;
  source_owner->clear();
  target_owner->clear();
  auto cursor = db->Query(code + '\t');
  if (!cursor)
    return false;
  string key, value;
  while (cursor->GetNextRecord(&key, &value)) {
    UserDbState state;
    if (!state.Unpack(value))
      return false;
    if (!state.has_manual_position())
      continue;
    if (state.position == source_position) {
      if (!source_owner->empty())
        return false;
      *source_owner = key;
    }
    if (source_position != target_position &&
        state.position == target_position) {
      if (!target_owner->empty())
        return false;
      *target_owner = key;
    }
  }
  return true;
}
bool FindPinnedAt(Db* db,
                  const string& code,
                  int position,
                  string* occupant_key) {
  string ignored;
  return FindPinnedOwners(db, code, position, position, occupant_key, &ignored);
}
}  // namespace

bool SetManual(Db* db, const string& code, const string& text, int position) {
  if (!db || code.empty() || position < 0) {
    return false;
  }
  const string key = code + '\t' + text;
  auto tx = dynamic_cast<Transactional*>(db);
  if (!tx || tx->in_transaction() || !tx->BeginTransaction()) {
    return false;
  }
  string raw;
  UserDbState state;
  uint64_t version = 0;
  string occupant;
  bool ok = (!db->Fetch(key, &raw) || state.Unpack(raw)) &&
            (position <= 0 || (FindPinnedAt(db, code, position, &occupant) &&
                               (occupant.empty() || occupant == key))) &&
            NextClock(db, &version);
  if (ok) {
    state.SetManualVersion(position, version);
    ok = db->Update(key, state.Pack()) && SaveClock(db, version);
  }
  if (!ok) {
    tx->AbortTransaction();
    return false;
  }
  const bool committed = tx->CommitTransaction();
  return committed;
}

// 只标记自动删除，保留手排；不存在则不创建。
bool ResetAuto(Db* db, const string& code, const string& text) {
  if (!db || code.empty() || text.empty())
    return false;
  const string key = code + '\t' + text;
  auto tx = dynamic_cast<Transactional*>(db);
  if (!tx || tx->in_transaction() || !tx->BeginTransaction()) {
    return false;
  }
  string raw;
  UserDbState state;
  if (!db->Fetch(key, &raw) || !state.Unpack(raw)) {
    tx->AbortTransaction();
    return false;
  }
  // 负次数遵循原版合并规则，手排独立保留。
  string raw_tick;
  uint64_t global_tick = 0;
  if (!db->MetaFetch("/tick", &raw_tick)) {
    tx->AbortTransaction();
    return false;
  }
  try {
    size_t used = 0;
    global_tick = std::stoull(raw_tick, &used);
    if (used != raw_tick.size() || global_tick < state.last_tick) {
      tx->AbortTransaction();
      return false;
    }
  } catch (...) {
    tx->AbortTransaction();
    return false;
  }
  if (state.is_auto_deleted()) {
    tx->AbortTransaction();
    return true;
  }
  if (!state.MarkDeleted(global_tick)) {
    tx->AbortTransaction();
    return false;
  }
  const bool written = db->Update(key, state.Pack());
  if (!written) {
    tx->AbortTransaction();
    return false;
  }
  const bool committed = tx->CommitTransaction();
  return committed;
}

// 清除手排时保留自动学习，并更新版本防止旧排序复活。
bool ResetManual(Db* db, const string& code, const string& text) {
  if (!db || code.empty() || text.empty())
    return false;
  const string key = code + '\t' + text;
  auto tx = dynamic_cast<Transactional*>(db);
  if (!tx || tx->in_transaction() || !tx->BeginTransaction()) {
    return false;
  }
  string raw;
  UserDbState state;
  if (!db->Fetch(key, &raw) || !state.Unpack(raw)) {
    tx->AbortTransaction();
    return false;
  }
  uint64_t version = 0;
  const bool clock_ready = NextClock(db, &version);
  if (clock_ready)
    state.SetManualVersion(0, version);
  const bool written =
      clock_ready && db->Update(key, state.Pack()) && SaveClock(db, version);
  if (!written) {
    tx->AbortTransaction();
    return false;
  }
  const bool committed = tx->CommitTransaction();
  return committed;
}

// 手排只更新当前词；只有目标位置已有固定词时才额外交换记录。
bool MoveAdjacent(Db* db,
                  const string& code,
                  const string& selected,
                  const string& neighbor_code,
                  const string& neighbor,
                  int source_position,
                  int direction,
                  bool neighbor_visible) {
  if (!db || code.empty() || selected.empty() || source_position < 1 ||
      (direction != -1 && direction != 1))
    return false;
  const int target_position = source_position + direction;
  if (target_position < 1)
    return false;
  const string source_key = code + '\t' + selected;

  auto tx = dynamic_cast<Transactional*>(db);
  if (!tx || tx->in_transaction() || !tx->BeginTransaction())
    return false;

  string raw;
  UserDbState source_state;
  bool ok = !db->Fetch(source_key, &raw) || source_state.Unpack(raw);
  // 当前编码内的目标位置如果已有固定词，直接通过 DB 查询定位。
  string occupant_key;
  if (ok)
    ok = FindPinnedAt(db, code, target_position, &occupant_key);
  if (occupant_key == source_key)
    occupant_key.clear();

  // 相邻显示词可能是不同读音；只在它确实占据目标固定位置时用来交换。
  const string neighbor_key = neighbor_code.empty() || neighbor.empty()
                                  ? string()
                                  : neighbor_code + '\t' + neighbor;
  UserDbState neighbor_state;
  bool neighbor_at_target = false;
  if (ok && !neighbor_key.empty() && neighbor_key != source_key) {
    if (db->Fetch(neighbor_key, &raw)) {
      ok = neighbor_state.Unpack(raw);
      neighbor_at_target = ok && neighbor_state.has_manual_position() &&
                           neighbor_state.position == target_position;
    }
  }

  string swapped_key;
  UserDbState swapped_state;
  if (ok && !occupant_key.empty()) {
    if (neighbor_visible) {
      // When the menu supplies a visible neighbor, only that exact pinned
      // entry may be exchanged; a third item must never be displaced.
      if (neighbor_key.empty() || occupant_key != neighbor_key ||
          !neighbor_at_target) {
        ok = false;
      } else {
        swapped_key = neighbor_key;
        swapped_state = neighbor_state;
      }
    } else {
      // Legacy/direct callers without menu context may exchange the pinned
      // entry occupying the adjacent slot in this same code.
      ok = db->Fetch(occupant_key, &raw) && swapped_state.Unpack(raw) &&
           swapped_state.has_manual_position() &&
           swapped_state.position == target_position;
      if (ok)
        swapped_key = occupant_key;
    }
  } else if (ok && neighbor_at_target) {
    swapped_key = neighbor_key;
    swapped_state = neighbor_state;
  }

  uint64_t version = 0;
  if (ok)
    ok = NextClock(db, &version);
  if (ok) {
    source_state.SetManualVersion(target_position, version);
    ok = db->Update(source_key, source_state.Pack());
    if (ok && !swapped_key.empty()) {
      swapped_state.SetManualVersion(source_position, version);
      ok = db->Update(swapped_key, swapped_state.Pack());
    }
    if (ok)
      ok = SaveClock(db, version);
  }
  if (!ok) {
    tx->AbortTransaction();
    return false;
  }
  return tx->CommitTransaction();
}

}  // namespace user_manual_order
}  // namespace rime
