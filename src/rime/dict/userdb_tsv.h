// 统一使用带元数据的 TSV 快照。
#ifndef RIME_LEVER_USERDB_TSV_H_
#define RIME_LEVER_USERDB_TSV_H_

#include <string>
#include <rime/common.h>

namespace rime {
struct UserDbState;
namespace userdb_tsv {

// 每行三列：编码、词条、五字段值。
bool FormatUserEntry(const std::string& key,
                     const std::string& value,
                     std::string* line,
                     UserDbState* parsed_state = nullptr);

// 导入前校验整份快照，有错即拒绝。
bool ValidateSnapshot(const path& snapshot,
                      size_t* records,
                      std::string* error = nullptr);

}  // namespace userdb_tsv
}  // namespace rime
#endif  // RIME_LEVER_USERDB_TSV_H_
