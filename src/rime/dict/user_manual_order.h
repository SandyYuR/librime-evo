// Copyright RIME Developers
// Distributed under the BSD License
#ifndef RIME_USER_MANUAL_ORDER_H_
#define RIME_USER_MANUAL_ORDER_H_
#include <rime/common.h>
namespace rime {
class Db;
namespace user_manual_order {
// 编码以空格结尾；已有位置不能被直接覆盖。
bool SetManual(Db* db, const string& code, const string& text, int position);
// 自动学习和手排可独立清除；手排清除仍更新版本。
bool ResetAuto(Db* db, const string& code, const string& text);
bool ResetManual(Db* db, const string& code, const string& text);
// 单次只移动一位，最多写两条记录。
bool MoveAdjacent(Db* db,
                  const string& code,
                  const string& selected,
                  const string& neighbor_code,
                  const string& neighbor,
                  int source_position,
                  int direction,
                  bool neighbor_visible);
inline bool MoveAdjacent(Db* db,
                         const string& code,
                         const string& selected,
                         const string& neighbor_code,
                         const string& neighbor,
                         int source_position,
                         int direction) {
  return MoveAdjacent(db, code, selected, neighbor_code, neighbor,
                      source_position, direction, false);
}
// 同一编码调用者继续使用原入口。
inline bool MoveAdjacent(Db* db,
                         const string& code,
                         const string& selected,
                         const string& neighbor,
                         int source_position,
                         int direction,
                         bool neighbor_visible = false) {
  return MoveAdjacent(db, code, selected, code, neighbor, source_position,
                      direction, neighbor_visible);
}
}  // namespace user_manual_order
}  // namespace rime
#endif
