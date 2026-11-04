// Copyright RIME Developers
// Distributed under the BSD License
#include <algorithm>
#include <set>
#include <rime/candidate.h>
#include <rime/composition.h>
#include <rime/context.h>
#include <rime/dict/user_dictionary.h>
#include <rime/engine.h>
#include <rime/gear/memory.h>
#include <rime/gear/translator_commons.h>
#include <rime/menu.h>
#include <rime/gear/user_rank_actions.h>

namespace rime {
namespace user_rank {
namespace {

// 保留每个候选原本的数据库编码；编码可能对应不同读音或含有 Table 编码前缀。
bool StorageCode(UserDictionary* db, const DictEntry& entry, string* code) {
  return db && db->GetEntryCode(entry, code) && !code->empty();
}

}  // namespace

bool ApplyEditAction(Engine* engine, Context* ctx, EditAction action) {
  const int direction = action == EditAction::kMoveUp     ? -1
                        : action == EditAction::kMoveDown ? 1
                                                          : 0;
  const bool reset_auto = action == EditAction::kResetAuto;
  const bool reset_manual = action == EditAction::kResetManual;
  // 菜单动作只作用于当前可编辑候选。
  if (!ctx || !ctx->IsComposing() || !ctx->HasMenu() ||
      ctx->composition().empty()) {
    return true;
  }
  Segment& seg = ctx->composition().back();
  if (!seg.menu)
    return true;
  const size_t source = seg.selected_index;
  const auto from =
      As<Phrase>(Candidate::GetGenuineCandidate(seg.GetCandidateAt(source)));
  if (!from) {
    return true;
  }
  size_t target = source;
  an<Phrase> neighbor;
  if (direction != 0) {
    if (direction < 0 && source == 0) {
      return true;
    }
    target = direction > 0 ? source + 1 : source - 1;
    if (seg.menu->Prepare((std::max)(source, target) + 1) <= target) {
      return true;
    }
    neighbor =
        As<Phrase>(Candidate::GetGenuineCandidate(seg.GetCandidateAt(target)));
    if (neighbor && (from->start() != neighbor->start() ||
                     from->end() != neighbor->end())) {
      return true;
    }
  }

  Memory* source_memory = Memory::FindManualSource(engine, from->language());
  if (!source_memory || !source_memory->user_dict()) {
    LOG(WARNING)
        << "manual action ignored: selected phrase has no UserDB source";
    return true;
  }
  UserDictionary* db = source_memory->user_dict();
  if (!db->loaded() || db->readonly() || !source_memory->IsUserDbWriter())
    return true;
  // 写入和读取固定位置之前，先提交共享库上尚未落盘的学习事务。
  if (!Memory::FinishSharedSession(db)) {
    LOG(ERROR) << "cannot finish pending UserDB session before manual action: "
               << db->name();
    return true;
  }

  // 仅使用当前词自己的数据库编码；邻词不是手排写入的必要条件。
  DictEntry selected_entry = from->entry();
  string code;
  if (!StorageCode(db, selected_entry, &code)) {
    LOG(WARNING) << "manual action ignored: selected phrase code unavailable";
    return true;
  }
  selected_entry.custom_code = code;

  DictEntry neighbor_entry;
  UserDbState neighbor_state;
  bool neighbor_pinned = false;
  if (neighbor) {
    // 同库邻词若已手排，可以在位置冲突时交换；获取失败不拦截当前词移动。
    Memory* neighbor_memory =
        Memory::FindManualSource(engine, neighbor->language());
    if (neighbor_memory && neighbor_memory->user_dict() &&
        db->SharesDatabaseWith(*neighbor_memory->user_dict())) {
      string adjacent_code;
      neighbor_entry = neighbor->entry();
      if (StorageCode(neighbor_memory->user_dict(), neighbor_entry,
                      &adjacent_code)) {
        neighbor_entry.custom_code = adjacent_code;
        neighbor_pinned = db->GetUserState(neighbor_entry, &neighbor_state) &&
                          neighbor_state.has_manual_position();
      } else {
        neighbor_entry = DictEntry{};
      }
    }
  }

  // 按当前输入覆盖区间计算可见位置，而不是按内部读音编码分组。
  int local_position = 0;
  std::set<string> seen_texts;
  for (size_t i = 0; i <= source; ++i) {
    const auto previous =
        As<Phrase>(Candidate::GetGenuineCandidate(seg.GetCandidateAt(i)));
    if (!previous || previous->start() != from->start() ||
        previous->end() != from->end())
      continue;
    auto* previous_memory =
        Memory::FindManualSource(engine, previous->language());
    if (!previous_memory || !previous_memory->user_dict() ||
        !db->SharesDatabaseWith(*previous_memory->user_dict()))
      continue;
    if (seen_texts.insert(previous->text()).second)
      ++local_position;
  }
  if (local_position < 1)
    return true;

  if (direction != 0) {
    // 已手排词以存储的绝对位置为准；未手排词以菜单位置为准。
    UserDbState state;
    if (db->GetUserState(selected_entry, &state) &&
        state.has_manual_position()) {
      local_position = state.position;
    } else if (neighbor_pinned) {
      // 当前词尚未手排时，借用相邻固定位置推导插入点。
      local_position = neighbor_state.position - direction;
    }
    if (local_position < 1 || local_position + direction < 1)
      return true;
  }

  bool committed = false;
  if (reset_auto) {
    committed = db->ResetAutoLearning(selected_entry);
  } else if (reset_manual) {
    committed = db->ResetManualPosition(selected_entry);
  } else {
    committed =
        db->MoveManualOneStep(selected_entry, neighbor_entry, local_position,
                              direction, direction != 0);
  }
  if (!committed && direction != 0)
    LOG(WARNING) << "UserDB manual move failed: db=" << db->name()
                 << " position=" << local_position;
  if (committed) {
    ctx->RefreshNonConfirmedComposition();
    if (ctx->HasMenu())
      ctx->Highlight(target);
  }
  return true;
}

}  // namespace user_rank
}  // namespace rime
