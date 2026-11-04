//
// Copyright RIME Developers
// Distributed under the BSD License
//
// 2011-11-02 GONG Chen <chen.sst@gmail.com>
//
#include <algorithm>
#include <cstdio>
#include <memory>
#include <cstdlib>
#include <sstream>
#include <cfloat>
#include <fstream>
#include <cmath>
#include <limits>
#include <set>
#include <vector>
#include <tuple>
#include <climits>
#include <boost/algorithm/string.hpp>
#include <rime/service.h>
#include <rime/algo/dynamics.h>
#include <rime/dict/text_db.h>
#include <rime/dict/user_db.h>
#include <rime/dict/manual_stamp_codec.h>
#include <rime/dict/userdb_tsv.h>

namespace rime {

UserDbValue::UserDbValue(const string& value) {
  Unpack(value);
}

string UserDbValue::Pack() const {
  std::ostringstream packed;
  packed << "commits=" << commits << " dynamic=" << dynamic
         << " last_tick=" << last_tick;
  return packed.str();
}

bool UserDbValue::Unpack(const string& value) {
  UserDbValue decoded;
  bool has_commits = false;
  bool has_dynamic = false;
  bool has_last_tick = false;
  vector<string> fields;
  boost::split(fields, value, boost::is_any_of(" "));
  try {
    for (const string& field : fields) {
      const size_t separator = field.find('=');
      if (separator == string::npos || separator == 0 ||
          separator + 1 == field.size()) {
        return false;
      }
      const string name = field.substr(0, separator);
      const string number = field.substr(separator + 1);
      size_t consumed = 0;
      if (name == "commits" && !has_commits) {
        decoded.commits = std::stoi(number, &consumed);
        has_commits = true;
      } else if (name == "dynamic" && !has_dynamic) {
        try {
          const double parsed_dynamic = std::stod(number, &consumed);
          if (!std::isfinite(parsed_dynamic) || parsed_dynamic < 0)
            return false;
          decoded.dynamic = (std::min)(10000.0, parsed_dynamic);
        } catch (const std::out_of_range&) {
          // 极小动态值下溢时按零处理。
          decoded.dynamic = 0;
          consumed = number.size();
        }
        has_dynamic = true;
      } else if (name == "last_tick" && !has_last_tick) {
        if (number.front() < '0' || number.front() > '9')
          return false;
        decoded.last_tick = std::stoull(number, &consumed);
        has_last_tick = true;
      } else {
        return false;
      }
      if (consumed != number.size())
        return false;
    }
  } catch (const std::exception&) {
    return false;
  }
  if (!has_commits || !has_dynamic || !has_last_tick ||
      !std::isfinite(decoded.dynamic) || decoded.dynamic < 0) {
    return false;
  }
  *this = decoded;
  return true;
}

namespace {
bool UnsignedNumber(const string& s, uint64_t* out) {
  if (s.empty())
    return false;
  uint64_t v = 0;
  for (unsigned char c : s) {
    if (c < '0' || c > '9' || v > (UINT64_MAX - (c - '0')) / 10)
      return false;
    v = v * 10 + c - '0';
  }
  *out = v;
  return true;
}
bool SignedCount(const string& s, int* out) {
  const bool negative = !s.empty() && s.front() == '-';
  const string digits = negative ? s.substr(1) : s;
  uint64_t magnitude = 0;
  if (!UnsignedNumber(digits, &magnitude) ||
      magnitude > (negative ? uint64_t(INT_MAX) + 1 : uint64_t(INT_MAX)))
    return false;
  const int64_t result = negative ? -int64_t(magnitude) : int64_t(magnitude);
  *out = static_cast<int>(result);
  return true;
}
bool NonnegativePosition(const string& s, int* out) {
  uint64_t n = 0;
  if (!UnsignedNumber(s, &n) || n > INT_MAX)
    return false;
  *out = static_cast<int>(n);
  return true;
}
}  // namespace

bool UserDbState::Unpack(const string& raw) {
  if (raw.empty() || raw.size() > 120)
    return false;
  string fields[5];
  size_t begin = 0;
  for (int i = 0; i < 5; ++i) {
    const size_t end = raw.find(',', begin);
    if ((i < 4 && end == string::npos) || (i == 4 && end != string::npos))
      return false;
    fields[i] = raw.substr(begin, end == string::npos ? end : end - begin);
    if (end != string::npos)
      begin = end + 1;
  }
  UserDbState parsed;
  if (!SignedCount(fields[0], &parsed.commits) ||
      !UnsignedNumber(fields[2], &parsed.last_tick) ||
      !NonnegativePosition(fields[3], &parsed.position) ||
      !manual_stamp_codec::Parse(fields[4], &parsed.manual_stamp))
    return false;
  try {
    size_t used = 0;
    parsed.dynamic = std::stod(fields[1], &used);
    if (used != fields[1].size() || !std::isfinite(parsed.dynamic) ||
        parsed.dynamic < 0 || parsed.dynamic > 10000)
      return false;
  } catch (...) {
    return false;
  }
  // last_tick 允许为 0；手排位置大于 0 时必须有版本。
  if (!parsed.manual_stamp && parsed.position != 0)
    return false;
  *this = parsed;
  return true;
}

string UserDbState::Pack() const {
  std::ostringstream stream;
  // Match the original UserDbValue serialization precision (six significant
  // digits) while keeping the five-field format stable.
  stream << commits << ',' << dynamic << ',' << last_tick << ',' << position
         << ',' << manual_stamp_codec::Format(manual_stamp);
  return stream.str();
}

void UserDbState::MergeAutomatic(const UserDbState& other,
                                 TickCount local_tick,
                                 TickCount remote_tick) {
  // 双方分别按各自 /tick 衰减，再按原版规则合并。
  double local_dynamic = dynamic;
  if (last_tick < local_tick)
    local_dynamic = algo::formula_d(0.0, double(local_tick), local_dynamic,
                                    double(last_tick));
  double remote_dynamic = other.dynamic;
  if (other.last_tick < remote_tick)
    remote_dynamic = algo::formula_d(0.0, double(remote_tick), remote_dynamic,
                                     double(other.last_tick));

  // 比较次数绝对值；先扩宽整数，避免 INT_MIN 溢出。
  const auto magnitude = [](int count) -> int64_t {
    return count < 0 ? -int64_t(count) : int64_t(count);
  };
  if (magnitude(commits) < magnitude(other.commits))
    commits = other.commits;
  dynamic = (std::max)(local_dynamic, remote_dynamic);
  last_tick = (std::max)(local_tick, remote_tick);
}

void UserDbState::MergeManual(const UserDbState& other) {
  // 清除手排只更新位置和版本；同版本冲突按固定规则处理。
  if (std::tie(other.manual_stamp, other.position) >
      std::tie(manual_stamp, position)) {
    manual_stamp = other.manual_stamp;
    position = other.position;
  }
}

void UserDbState::Merge(const UserDbState& other,
                        TickCount local_tick,
                        TickCount remote_tick) {
  MergeAutomatic(other, local_tick, remote_tick);
  MergeManual(other);
}

bool UserDbState::Learn(int selection_count, TickCount present_tick) {
  if (selection_count < 0 || present_tick < last_tick)
    return false;
  // 再次选中可恢复已删除词条。
  if (selection_count > 0 && commits == INT_MIN)
    return false;
  const int restored = selection_count > 0 && commits < 0 ? -commits : commits;
  if (selection_count > 0 && restored > INT_MAX - selection_count)
    return false;
  const double increment = selection_count ? double(selection_count) : 0.1;
  const double next_dynamic = algo::formula_d(increment, double(present_tick),
                                              dynamic, double(last_tick));
  if (!std::isfinite(next_dynamic))
    return false;
  dynamic = (std::min)(10000.0, next_dynamic);
  commits = restored + selection_count;
  last_tick = present_tick;
  return true;
}

bool UserDbState::MarkDeleted(TickCount present_tick) {
  if (present_tick < last_tick)
    return false;
  const double next_dynamic =
      algo::formula_d(0.0, double(present_tick), dynamic, double(last_tick));
  if (!std::isfinite(next_dynamic))
    return false;
  dynamic = (std::min)(10000.0, next_dynamic);
  // 重复删除时，负次数归为 -1。
  commits = commits > 0 ? -commits : -1;
  last_tick = present_tick;
  return true;
}

double UserDbState::Score(TickCount present_tick) const {
  // 自动删除不出候选；有效手排另行处理。
  if (!has_learning())
    return std::log(DBL_EPSILON);
  const TickCount current_tick = (std::max)(TickCount(1), present_tick);
  const double decayed =
      algo::formula_d(0.0, double(current_tick), dynamic, double(last_tick));
  const double probability =
      algo::formula_p(0.0, double(commits) / double(current_tick),
                      double(current_tick), decayed);
  return std::log(probability > 0 ? probability : DBL_EPSILON);
}

void UserDbState::SetManualVersion(int new_position, uint64_t version) {
  if (new_position < 0 || version == 0)
    return;
  position = new_position;
  manual_stamp = version;
}

// 同步快照使用带元数据的 TSV；本地数据库仍为 .userdb。
static const string plain_userdb_extension(".userdb.tsv");

template <>
string UserDbComponent<TextDb>::extension() const {
  return plain_userdb_extension;
}

string UserDb::snapshot_extension() {
  return plain_userdb_extension;
}

// key ::= code <space> <Tab> phrase

static bool userdb_entry_parser(const Tsv& row, string* key, string* value) {
  if (row.size() < 2 || row[0].empty() || row[1].empty()) {
    return false;
  }
  string code(row[0]);
  // 数据库键保留编码末尾的空格。
  if (code[code.length() - 1] != ' ')
    code += ' ';
  *key = code + "\t" + row[1];
  if (row.size() >= 3)
    *value = row[2];
  else
    value->clear();
  return true;
}

static bool userdb_entry_formatter(const string& key,
                                   const string& value,
                                   Tsv* tsv) {
  Tsv& row(*tsv);
  boost::algorithm::split(row, key, boost::algorithm::is_any_of("\t"));
  if (row.size() != 2 || row[0].empty() || row[1].empty())
    return false;
  row.push_back(value);
  return true;
}

static TextFormat plain_userdb_format = {
    userdb_entry_parser,
    userdb_entry_formatter,
    "Rime user dictionary",
};

template <>
RIME_DLL UserDbWrapper<TextDb>::UserDbWrapper(const path& file_path,
                                              const string& db_name)
    : TextDb(file_path, db_name, "userdb", plain_userdb_format) {}

bool UserDbHelper::UpdateUserInfo() {
  Deployer& deployer(Service::instance().deployer());
  // 同步时通过格式标记检查五字段结构。
  string clock;
  return db_->MetaUpdate("/user_id", deployer.user_id) &&
         db_->MetaUpdate("/userdb_format", kUserDbFormat) &&
         (db_->MetaFetch("/manual_clock", &clock) ||
          db_->MetaUpdate("/manual_clock", "0")) &&
         (db_->MetaFetch("/tick", &clock) || db_->MetaUpdate("/tick", "0"));
}

bool UserDbHelper::IsUniformFormat(const path& file_path) {
  return boost::ends_with(file_path.filename().to_utf8_string(),
                          plain_userdb_extension);
}

bool UserDbHelper::UniformBackup(const path& snapshot_file) {
  if (!db_ || !db_->loaded())
    return false;
  auto entries = db_->QueryAll();
  if (!entries)
    return false;

  // Query the database once. A temporary spool lets us write the record count
  // before the rows without retaining the full UserDB in memory.
  std::unique_ptr<std::FILE, int (*)(std::FILE*)> body(std::tmpfile(),
                                                       &std::fclose);
  if (!body) {
    LOG(ERROR) << "cannot create temporary UserDB snapshot spool";
    return false;
  }
  size_t expected_count = 0;
  string key, value, row;
  while (entries->GetNextRecord(&key, &value)) {
    if (!userdb_tsv::FormatUserEntry(key, value, &row)) {
      LOG(ERROR) << "invalid userdb entry in snapshot " << db_->name()
                 << " key=" << key;
      return false;
    }
    if (std::fwrite(row.data(), 1, row.size(), body.get()) != row.size()) {
      LOG(ERROR) << "failed writing temporary UserDB snapshot spool";
      return false;
    }
    ++expected_count;
  }
  if (std::fflush(body.get()) != 0 || std::ferror(body.get()))
    return false;
  std::rewind(body.get());

  std::ofstream out(static_cast<const std::filesystem::path&>(snapshot_file),
                    std::ios::binary | std::ios::trunc);
  if (!out) {
    LOG(ERROR) << "cannot create UserDB snapshot: " << snapshot_file;
    return false;
  }
  // 导出真实元数据，/record_count 仅用于快照校验。
  string db_name, db_type, rime_version, user_id, format, clock_value;
  if (!db_->MetaFetch("/db_name", &db_name)) {
    LOG(ERROR) << "UserDB snapshot missing metadata /db_name: " << db_->name();
    return false;
  }
  if (db_name != db_->name() || db_name.empty() ||
      db_name.find_first_of("/\\\r\n\t") != string::npos) {
    LOG(ERROR) << "UserDB snapshot has invalid /db_name: " << db_->name()
               << " value=" << db_name;
    return false;
  }
  if (!db_->MetaFetch("/rime_version", &rime_version) || rime_version.empty()) {
    LOG(ERROR) << "UserDB snapshot missing or invalid /rime_version: "
               << db_->name();
    return false;
  }
  if (!db_->MetaFetch("/db_type", &db_type) || db_type != "userdb") {
    LOG(ERROR) << "UserDB snapshot has invalid /db_type: " << db_->name();
    return false;
  }
  if (!db_->MetaFetch("/user_id", &user_id) || user_id.empty()) {
    LOG(ERROR) << "UserDB snapshot missing or invalid /user_id: "
               << db_->name();
    return false;
  }
  if (!db_->MetaFetch("/userdb_format", &format) || format != kUserDbFormat) {
    LOG(ERROR) << "UserDB snapshot has unsupported /userdb_format: "
               << db_->name() << " value=" << format;
    return false;
  }
  uint64_t clock = 0;
  const bool has_clock = db_->MetaFetch("/manual_clock", &clock_value);
  if (has_clock && !manual_stamp_codec::Parse(clock_value, &clock)) {
    LOG(ERROR) << "UserDB snapshot has invalid /manual_clock: " << db_->name();
    return false;
  }
  auto metadata = db_->QueryMetadata();
  if (!metadata) {
    LOG(ERROR) << "cannot read UserDB metadata for snapshot: " << db_->name();
    return false;
  }
  std::vector<std::pair<string, string>> meta_rows;
  std::set<string> meta_keys;
  while (metadata->GetNextRecord(&key, &value)) {
    // 元数据必须能原样写入 TSV。
    if (key.empty() || key.front() != '/' || key.size() > 129 ||
        key.find_first_not_of("/abcdefghijklmnopqrstuvwxyz0123456789_") !=
            string::npos ||
        value.find_first_of("\t\r\n") != string::npos ||
        key == "/record_count" || !meta_keys.insert(key).second) {
      LOG(ERROR) << "invalid metadata while snapshotting UserDB " << db_->name()
                 << " key=" << key;
      return false;
    }
    meta_rows.emplace_back(key, value);
  }
  // 没有手排记录时，/manual_clock 默认为 0。
  if (!has_clock)
    meta_rows.emplace_back("/manual_clock", "0");
  if (!db_->MetaFetch("/tick", &clock_value))
    meta_rows.emplace_back("/tick", "0");
  out << "#@/value_fields\tcommits,dynamic,last_tick,position,manual_stamp\n";
  for (const auto& [meta_key, meta_value] : meta_rows)
    out << "#@" << meta_key << '\t' << meta_value << '\n';
  out << "#@/record_count\t" << expected_count << '\n';
  if (!out) {
    LOG(ERROR) << "failed writing UserDB snapshot header: " << snapshot_file;
    return false;
  }
  char buffer[64 * 1024];
  size_t bytes = 0;
  while ((bytes = std::fread(buffer, 1, sizeof(buffer), body.get())) > 0) {
    out.write(buffer, static_cast<std::streamsize>(bytes));
    if (!out) {
      LOG(ERROR) << "failed writing UserDB snapshot body: " << snapshot_file;
      return false;
    }
  }
  if (std::ferror(body.get())) {
    LOG(ERROR) << "failed reading temporary UserDB snapshot spool";
    return false;
  }
  out.flush();
  if (!out) {
    LOG(ERROR) << "failed flushing UserDB snapshot: " << snapshot_file;
    return false;
  }
  out.close();
  if (!out) {
    LOG(ERROR) << "failed closing UserDB snapshot: " << snapshot_file;
    return false;
  }
  return true;
}

bool UserDbHelper::UniformRestore(const path& snapshot_file) {
  size_t validated_records = 0;
  string validation_error;
  if (!userdb_tsv::ValidateSnapshot(snapshot_file, &validated_records,
                                    &validation_error)) {
    LOG(ERROR) << "invalid UserDB snapshot " << snapshot_file << ": "
               << validation_error;
    return false;
  }
  TsvReader reader(snapshot_file, plain_userdb_format.parser);
  // 导入临时库时分批提交，避免逐条写日志或批次过大。
  class SnapshotSink : public DbSink {
   public:
    explicit SnapshotSink(Db* db)
        : DbSink(db), transaction_(dynamic_cast<Transactional*>(db)) {}
    ~SnapshotSink() override {
      if (transaction_ && transaction_->in_transaction())
        transaction_->AbortTransaction();
    }
    bool Begin() {
      if (!transaction_)
        return true;
      return !transaction_->in_transaction() &&
             transaction_->BeginTransaction();
    }
    bool MetaPut(const string& key, const string& value) override {
      if (failed_)
        return false;
      if (key == "/record_count" || key == "/value_fields")
        return true;  // snapshot-only header
      return RecordWrite(DbSink::MetaPut(key, value));
    }
    bool Put(const string& key, const string& value) override {
      if (failed_)
        return false;
      return RecordWrite(DbSink::Put(key, value));
    }
    bool Finish() {
      if (failed_)
        return false;
      if (!transaction_ || !transaction_->in_transaction())
        return true;
      const bool ok = transaction_->CommitTransaction();
      if (!ok)
        failed_ = true;
      return ok;
    }
    bool failed() const { return failed_; }

   private:
    bool RecordWrite(bool written) {
      if (!written) {
        failed_ = true;
        return false;
      }
      if (!transaction_ || ++batch_size_ < 1024)
        return true;
      // 每 1024 条提交一次。
      const bool ok =
          transaction_->CommitTransaction() && transaction_->BeginTransaction();
      if (!ok) {
        failed_ = true;
        return false;
      }
      batch_size_ = 0;
      return true;
    }
    Transactional* transaction_;
    size_t batch_size_ = 0;
    bool failed_ = false;
  } sink(db_);
  if (!sink.Begin())
    return false;
  try {
    if (reader >> sink != static_cast<int>(validated_records) || sink.failed())
      return false;
    if (!sink.Finish())
      return false;
  } catch (std::exception& ex) {
    LOG(ERROR) << ex.what();
    return false;
  }
  // 快照已校验时钟与记录版本，无需再次扫描。
  return true;
}

bool UserDbHelper::IsUserDb() {
  string db_type;
  return db_->MetaFetch("/db_type", &db_type) && (db_type == "userdb");
}

string UserDbHelper::GetDbName() {
  string name;
  if (!db_->MetaFetch("/db_name", &name))
    return name;
  auto ext = boost::find_last(name, ".userdb");
  if (!ext.empty()) {
    // remove ".userdb.*"
    name.erase(ext.begin(), name.end());
  }
  return name;
}

string UserDbHelper::GetUserId() {
  string user_id("unknown");
  db_->MetaFetch("/user_id", &user_id);
  return user_id;
}

string UserDbHelper::GetRimeVersion() {
  string version;
  db_->MetaFetch("/rime_version", &version);
  return version;
}

UserDbMerger::UserDbMerger(Db* db) : db_(db) {
  // 双方各自的 /tick 分别用于衰减。
  string raw;
  if (!db_ || !db_->MetaFetch("/tick", &raw) ||
      !UnsignedNumber(raw, &local_tick_)) {
    failed_ = true;
  }
  max_tick_ = local_tick_;
}
UserDbMerger::~UserDbMerger() {
  CloseMerge();
}
void UserDbMerger::ObserveTick(uint64_t tick) {
  remote_tick_ = tick;
  max_tick_ = (std::max)(local_tick_, remote_tick_);
  remote_tick_ready_ = true;
}
void UserDbMerger::ObserveClock(uint64_t clock) {
  observed_manual_stamp_ = (std::max)(observed_manual_stamp_, clock);
}
bool UserDbMerger::MetaPut(const string&, const string&) {
  return true;
}
bool UserDbMerger::Put(const string& key, const string& value) {
  if (!db_ || failed_ || !remote_tick_ready_) {
    failed_ = true;
    return false;
  }
  UserDbState incoming, local;
  if (!incoming.Unpack(value) || incoming.last_tick > remote_tick_) {
    failed_ = true;
    return false;
  }
  string raw;
  const bool existing = db_->Fetch(key, &raw);
  if (existing && (!local.Unpack(raw) || local.last_tick > local_tick_)) {
    failed_ = true;
    return false;
  }
  local.Merge(incoming, local_tick_, remote_tick_);
  observed_manual_stamp_ =
      (std::max)(observed_manual_stamp_, local.manual_stamp);
  const string merged = local.Pack();
  // 内容未变化时不重复写库。
  if ((!existing || merged != raw) && !db_->Update(key, merged)) {
    failed_ = true;
    return false;
  }
  return true;
}
void UserDbMerger::CloseMerge() {
  if (closed_)
    return;
  closed_ = true;
  if (db_ && !failed_ && remote_tick_ready_) {
    // 全局 /tick 取最大值，不覆盖更新后的较大值。
    string raw;
    uint64_t saved = 0;
    if (db_->MetaFetch("/tick", &raw) && !UnsignedNumber(raw, &saved))
      failed_ = true;
    if (!failed_ && max_tick_ > saved &&
        !db_->MetaUpdate("/tick", std::to_string(max_tick_)))
      failed_ = true;
  }
  if (db_ && !failed_ && observed_manual_stamp_) {
    uint64_t saved = 0;
    string raw;
    if (db_->MetaFetch("/manual_clock", &raw) &&
        !manual_stamp_codec::Parse(raw, &saved))
      failed_ = true;
    if (!failed_ && observed_manual_stamp_ > saved &&
        !db_->MetaUpdate("/manual_clock",
                         manual_stamp_codec::Format(observed_manual_stamp_)))
      failed_ = true;
  }
}

UserDbImporter::UserDbImporter(Db* db) : db_(db) {}

bool UserDbImporter::MetaPut(const string& key, const string& value) {
  return true;
}

bool UserDbImporter::Put(const string& key, const string& value) {
  if (!db_)
    return false;
  // 普通导入与快照同步使用不同合并入口。
  UserDbState incoming, local;
  if (!incoming.Unpack(value))
    return false;
  string raw;
  const bool exists = db_->Fetch(key, &raw);
  if (exists && !local.Unpack(raw))
    return false;
  if (incoming.commits > 0) {
    local.commits = (std::max)(local.commits, incoming.commits);
    local.dynamic = (std::max)(local.dynamic, incoming.dynamic);
  } else if (incoming.commits < 0) {
    const int64_t magnitude =
        local.commits < 0 ? -int64_t(local.commits) : int64_t(local.commits);
    local.commits =
        static_cast<int>((std::min)(int64_t(incoming.commits), -magnitude));
  }
  local.MergeManual(incoming);
  const string merged = local.Pack();
  return (exists && merged == raw) || db_->Update(key, merged);
}

}  // namespace rime
