// Copyright RIME Developers
// Distributed under the BSD License
#ifndef RIME_GEAR_USER_RANK_ACTIONS_H_
#define RIME_GEAR_USER_RANK_ACTIONS_H_

namespace rime {
class Context;
class Engine;
namespace user_rank {
enum class EditAction { kMoveUp, kMoveDown, kResetAuto, kResetManual };
bool ApplyEditAction(Engine* engine, Context* ctx, EditAction action);
}  // namespace user_rank
}  // namespace rime
#endif  // RIME_GEAR_USER_RANK_ACTIONS_H_
