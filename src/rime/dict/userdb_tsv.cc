// 同步和导出共用五字段 TSV 快照。
#include <fstream>
#include <set>
#include <algorithm>
#include <rime/dict/user_db.h>
#include <rime/dict/manual_stamp_codec.h>
#include <rime/dict/userdb_tsv.h>

namespace rime {
namespace userdb_tsv {

bool FormatUserEntry(const std::string& key,
                     const std::string& value,
                     std::string* line,
                     UserDbState* parsed_state) {
  if (!line)
    return false;
  const size_t tab = key.find('\t');
  if (tab == std::string::npos || tab < 2 || tab + 1 >= key.size() ||
      key[tab - 1] != ' ' || key.find('\t', tab + 1) != std::string::npos)
    return false;
  const std::string code = key.substr(0, tab - 1);
  const std::string word = key.substr(tab + 1);
  if (code.empty() || word.empty() ||
      code.find_first_of("\r\n\t") != std::string::npos ||
      word.find_first_of("\r\n\t") != std::string::npos ||
      code.front() == '#' || code.back() == ' ')
    return false;
  UserDbState state;
  if (!state.Unpack(value))
    return false;
  if (parsed_state)
    *parsed_state = state;
  *line = code + '\t' + word + '\t' + state.Pack() + '\n';
  return true;
}

bool ValidateSnapshot(const path& snapshot,
                      size_t* records,
                      std::string* error) {
  if (records)
    *records = 0;
  if (error)
    error->clear();
  auto fail = [&](const std::string& reason, size_t line_number) {
    if (error) {
      *error = reason;
      if (line_number)
        *error += "（第 " + std::to_string(line_number) + " 行）";
    }
    return false;
  };
  std::ifstream input(static_cast<const std::filesystem::path&>(snapshot),
                      std::ios::binary);
  if (!input)
    return fail("无法打开快照文件", 0);
  std::string line;
  size_t line_number = 0;
  size_t count = 0;
  std::set<std::string> metadata;
  bool format_seen = false, type_seen = false, user_seen = false;
  bool name_seen = false, version_seen = false;
  bool count_seen = false, clock_seen = false, rows_started = false;
  std::string expected_count, db_name;
  uint64_t manual_clock = 0;
  uint64_t max_revision = 0;
  uint64_t max_last_tick = 0;
  uint64_t global_tick = 0;
  bool tick_seen = false, fields_seen = false;
  while (std::getline(input, line)) {
    ++line_number;
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    if (line.empty())
      continue;
    if (line.rfind("# sync_complete", 0) == 0)
      return fail("快照仍处于未完成状态", line_number);
    if (line.rfind("#@", 0) == 0) {
      if (rows_started)
        return fail("元数据出现在词条记录之后", line_number);
      const size_t tab = line.find('\t', 2);
      if (tab == std::string::npos ||
          line.find('\t', tab + 1) != std::string::npos)
        return fail("元数据列数不正确", line_number);
      const std::string key = line.substr(2, tab - 2);
      const std::string val = line.substr(tab + 1);
      if (!metadata.insert(key).second || key.empty() || key.front() != '/' ||
          key.size() > 129 ||
          key.find_first_not_of("/abcdefghijklmnopqrstuvwxyz0123456789_") !=
              std::string::npos ||
          val.find_first_of("\t\r\n") != std::string::npos)
        return fail("元数据名称重复或格式非法", line_number);
      if (key == "/db_name") {
        if (val.empty() || val.find_first_of("/\\\r\n\t") != std::string::npos)
          return fail("/db_name 无效", line_number);
        db_name = val;
        name_seen = true;
      } else if (key == "/rime_version") {
        if (val.empty())
          return fail("/rime_version 为空", line_number);
        version_seen = true;
      } else if (key == "/userdb_format") {
        if (val != kUserDbFormat)
          return fail("/userdb_format 与当前格式不匹配", line_number);
        format_seen = true;
      } else if (key == "/value_fields") {
        if (val != "commits,dynamic,last_tick,position,manual_stamp")
          return fail("/value_fields 与五字段定义不匹配", line_number);
        fields_seen = true;
      } else if (key == "/tick") {
        if (val.empty() || val.size() > 20 ||
            val.find_first_not_of("0123456789") != std::string::npos)
          return fail("/tick 不是有效的非负整数", line_number);
        try {
          size_t used = 0;
          global_tick = std::stoull(val, &used);
          if (used != val.size() || std::to_string(global_tick) != val)
            return fail("/tick 数值格式不规范", line_number);
        } catch (...) {
          return fail("/tick 超出数值范围", line_number);
        }
        tick_seen = true;
      } else if (key == "/manual_clock") {
        if (!manual_stamp_codec::Parse(val, &manual_clock))
          return fail("/manual_clock 无效", line_number);
        clock_seen = true;
      } else if (key == "/db_type") {
        if (val != "userdb")
          return fail("/db_type 不是 userdb", line_number);
        type_seen = true;
      } else if (key == "/user_id") {
        if (val.empty())
          return fail("/user_id 为空", line_number);
        user_seen = true;
      } else if (key == "/record_count") {
        if (val.empty() || val.size() > 20 ||
            val.find_first_not_of("0123456789") != std::string::npos)
          return fail("/record_count 不是有效的非负整数", line_number);
        expected_count = val;
        count_seen = true;
      } else if (key == "/file_name") {
        return fail("快照不允许使用保留元数据 /file_name", line_number);
      }
      continue;
    }
    if (line == "# no comment")
      return fail("发现旧式空注释标记", line_number);
    if (line[0] == '#')
      continue;
    rows_started = true;
    const size_t first = line.find('\t');
    if (first == std::string::npos || first == 0)
      return fail("词条缺少编码列或编码为空", line_number);
    const size_t second = line.find('\t', first + 1);
    if (second == std::string::npos || second == first + 1 ||
        line.find('\t', second + 1) != std::string::npos)
      return fail("词条必须恰好包含三列", line_number);
    const std::string code = line.substr(0, first);
    const std::string word = line.substr(first + 1, second - first - 1);
    const std::string value = line.substr(second + 1);
    if (code.empty() || code.front() == '#' || code.back() == ' ' ||
        word.empty() || code.find_first_of("\r\n") != std::string::npos)
      return fail("词条编码或词语内容无效", line_number);
    std::string normalized;
    UserDbState state;
    if (!FormatUserEntry(code + " \t" + word, value, &normalized, &state))
      return fail("词条五字段值无法解析", line_number);
    if (normalized != line + "\n")
      return fail("词条格式不是规范 TSV 格式", line_number);
    max_revision = (std::max)(max_revision, state.manual_stamp);
    max_last_tick = (std::max)(max_last_tick, state.last_tick);
    ++count;
  }
  if (input.bad())
    return fail("读取快照时发生 I/O 错误", line_number);
  if (!input.eof())
    return fail("快照读取未正常结束", line_number);
  if (!name_seen)
    return fail("缺少元数据 /db_name", 0);
  if (!version_seen)
    return fail("缺少元数据 /rime_version", 0);
  if (!format_seen)
    return fail("缺少元数据 /userdb_format", 0);
  if (!fields_seen)
    return fail("缺少元数据 /value_fields", 0);
  if (!type_seen)
    return fail("缺少元数据 /db_type", 0);
  if (!user_seen)
    return fail("缺少元数据 /user_id", 0);
  if (!count_seen)
    return fail("缺少元数据 /record_count", 0);
  if (!clock_seen)
    return fail("缺少元数据 /manual_clock", 0);
  if (!tick_seen)
    return fail("缺少元数据 /tick", 0);
  if (expected_count != std::to_string(count))
    return fail("/record_count 与实际词条数不一致（声明 " + expected_count +
                    "，实际 " + std::to_string(count) + "）",
                0);
  if (manual_clock < max_revision)
    return fail("/manual_clock 小于词条中的最大手排时间", 0);
  if (global_tick < max_last_tick)
    return fail("/tick 小于词条中的最大 last_tick", 0);
  const std::string filename = snapshot.filename().to_utf8_string();
  const std::string suffix = UserDb::snapshot_extension();
  if (filename.size() <= suffix.size() ||
      filename.compare(filename.size() - suffix.size(), suffix.size(),
                       suffix) != 0)
    return fail("快照文件扩展名不正确", 0);
  const std::string name = filename.substr(0, filename.size() - suffix.size());
  if (name.empty() || name.find_first_of("/\\\r\n\t") != std::string::npos ||
      db_name != name)
    return fail("文件名与 /db_name 不一致", 0);
  if (records)
    *records = count;
  return true;
}

}  // namespace userdb_tsv
}  // namespace rime
