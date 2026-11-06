//
// Copyright RIME Developers
// Distributed under the BSD License
//
// 2012-03-23 GONG Chen <chen.sst@gmail.com>
//
#include <algorithm>
#include <climits>
#include <fstream>
#include <system_error>
#include <boost/algorithm/string.hpp>
#include <filesystem>
#include <boost/scope_exit.hpp>
#include <rime/common.h>
#include <rime/deployer.h>
#include <rime/algo/utilities.h>
#include <rime/dict/db_utils.h>
#include <rime/dict/user_db.h>
#include <rime/dict/manual_stamp_codec.h>
#include <rime/lever/user_dict_manager.h>
#include <rime/dict/userdb_tsv.h>
#include <rime/path_resolver.h>

namespace fs = std::filesystem;

namespace rime {

UserDictManager::UserDictManager(Deployer* deployer)
    : deployer_(deployer), user_db_component_(UserDb::Require("userdb")) {
  if (deployer) {
    path_ = deployer->user_data_dir;
  }
}

namespace {
bool IsMergeUserDb(Db* db) {
  // 按数据库元数据识别用户词库，不遍历词条。
  string db_type, format;
  return db && db->MetaFetch("/db_type", &db_type) && db_type == "userdb" &&
         db->MetaFetch("/userdb_format", &format) && format == kUserDbFormat;
}
}  // namespace

bool UserDictManager::GetUserDictList(UserDictList* user_dict_list,
                                      UserDb::Component* component) {
  if (!user_dict_list) {
    LOG(ERROR) << "UserDB discovery failed: output list is null.";
    return false;
  }
  user_dict_list->clear();
  if (!component)
    component = user_db_component_;
  if (!component) {
    LOG(ERROR) << "UserDB discovery failed: userdb component is unavailable.";
    return false;
  }
  std::error_code ec;
  const bool exists = fs::exists(path_, ec);
  if (ec) {
    LOG(ERROR) << "UserDB discovery cannot access directory " << path_ << ": "
               << ec.message();
    return false;
  }
  if (!exists) {
    LOG(INFO) << "UserDB discovery: directory does not exist yet: " << path_;
    return true;
  }
  if (!fs::is_directory(path_, ec) || ec) {
    LOG(ERROR) << "UserDB discovery cannot scan directory " << path_
               << (ec ? ": " + ec.message() : ": not a directory");
    return false;
  }

  // 只检查 .userdb 目录，并逐个打开读取元数据，不同时持有多个数据库。
  const string extension = component->extension();
  bool complete = true;
  size_t ignored = 0;
  for (fs::directory_iterator it(path_, ec), end; !ec && it != end;
       it.increment(ec)) {
    const path entry = it->path();
    std::error_code entry_error;
    if (it->is_symlink(entry_error))
      continue;
    if (entry_error) {
      LOG(ERROR) << "UserDB discovery cannot inspect " << entry << ": "
                 << entry_error.message();
      complete = false;
      continue;
    }
    if (!it->is_directory(entry_error)) {
      if (entry_error) {
        LOG(ERROR) << "UserDB discovery cannot inspect directory entry "
                   << entry << ": " << entry_error.message();
        complete = false;
      }
      continue;
    }
    string name = entry.filename().to_utf8_string();
    if (!boost::ends_with(name, extension))
      continue;
    boost::erase_last(name, extension);
    if (name.empty() || name.front() == '.' ||
        !safe_path::ValidRelativeName(name)) {
      ++ignored;
      continue;
    }
    if (!safe_path::SafeDatabaseLocation(path_, entry)) {
      LOG(ERROR) << "UserDB discovery rejected unsafe database path: " << entry;
      complete = false;
      continue;
    }

    if (component == user_db_component_) {
      the<Db> db(component->Create(name));
      if (!db || !db->Exists()) {
        LOG(ERROR) << "UserDB discovery found a directory without a database: "
                   << entry;
        complete = false;
        continue;
      }
      if (!db->OpenReadOnly()) {
        LOG(ERROR) << "UserDB discovery cannot open database for header check: "
                   << "name=" << name << " path=" << db->file_path();
        complete = false;
        continue;
      }
      const bool valid =
          IsMergeUserDb(db.get()) && UserDbHelper(db).GetDbName() == name;
      const bool closed = db->Close();
      if (!closed) {
        LOG(ERROR) << "UserDB discovery cannot close inspected database: "
                   << db->file_path();
        complete = false;
        continue;
      }
      if (!valid) {
        ++ignored;
        continue;
      }
    }
    user_dict_list->push_back(name);
  }
  if (ec) {
    LOG(ERROR) << "UserDB discovery failed while enumerating " << path_ << ": "
               << ec.message();
    complete = false;
  }
  std::sort(user_dict_list->begin(), user_dict_list->end());
  user_dict_list->erase(
      std::unique(user_dict_list->begin(), user_dict_list->end()),
      user_dict_list->end());
  LOG(INFO) << "UserDB discovery finished: found=" << user_dict_list->size()
            << " ignored=" << ignored << " complete=" << complete;
  return complete;
}

bool UserDictManager::Backup(const string& dict_name) {
  if (!deployer_ || !safe_path::ValidRelativeName(dict_name)) {
    LOG(ERROR) << "UserDB snapshot rejected invalid database name: "
               << dict_name;
    return false;
  }
  the<Db> db(user_db_component_->Create(dict_name));
  if (!db) {
    LOG(ERROR) << "UserDB snapshot cannot create database handle: "
               << dict_name;
    return false;
  }
  if (!safe_path::SafeDatabaseLocation(path_, db->file_path())) {
    LOG(ERROR) << "UserDB snapshot rejected unsafe database path: "
               << db->file_path();
    return false;
  }
  if (!db->OpenReadOnly()) {
    LOG(ERROR) << "UserDB snapshot cannot open database: " << db->file_path();
    return false;
  }
  // 设备 ID 变化时只更新归属信息，不重建其他元数据。
  const string old_user_id = UserDbHelper(db).GetUserId();
  if (old_user_id != deployer_->user_id) {
    LOG(INFO) << "UserDB snapshot updates device id: name=" << dict_name
              << " old=" << old_user_id << " new=" << deployer_->user_id;
    if (!db->Close()) {
      LOG(ERROR) << "UserDB snapshot cannot close before owner update: "
                 << dict_name;
      return false;
    }
    if (!db->Open()) {
      LOG(ERROR) << "UserDB snapshot cannot reopen for owner update: "
                 << dict_name;
      return false;
    }
    if (!db->MetaUpdate("/user_id", deployer_->user_id)) {
      LOG(ERROR) << "UserDB snapshot cannot update /user_id: " << dict_name;
      return false;
    }
    if (!db->Close()) {
      LOG(ERROR) << "UserDB snapshot cannot close after owner update: "
                 << dict_name;
      return false;
    }
    if (!db->OpenReadOnly()) {
      LOG(ERROR)
          << "UserDB snapshot cannot reopen read-only after owner update: "
          << dict_name;
      return false;
    }
  }

  const path dir(deployer_->user_data_sync_dir());
  std::error_code ec;
  if (!fs::exists(dir, ec)) {
    if (ec) {
      LOG(ERROR) << "UserDB snapshot cannot inspect device directory " << dir
                 << ": " << ec.message();
      return false;
    }
    fs::create_directories(dir, ec);
    if (ec) {
      LOG(ERROR) << "UserDB snapshot cannot create device directory " << dir
                 << ": " << ec.message();
      return false;
    }
  } else if (!fs::is_directory(dir, ec) || ec) {
    LOG(ERROR) << "UserDB snapshot device path is not a directory: " << dir
               << (ec ? ": " + ec.message() : "");
    return false;
  }
  const path target = safe_path::WritableFile(
      dir, dict_name + UserDb::snapshot_extension(), true);
  const path pending = safe_path::WritableFile(
      dir, dict_name + ".pending" + UserDb::snapshot_extension(), true);
  if (target.empty() || pending.empty()) {
    LOG(ERROR) << "UserDB snapshot destination is unsafe: " << dir
               << " name=" << dict_name;
    return false;
  }
  ec.clear();
  if (!db->Backup(pending)) {
    LOG(ERROR) << "UserDB snapshot export failed: " << pending;
    std::error_code remove_error;
    fs::remove(pending, remove_error);
    if (remove_error)
      LOG(WARNING) << "cannot remove incomplete UserDB snapshot " << pending
                   << ": " << remove_error.message();
    return false;
  }
  fs::rename(pending, target, ec);
  if (ec) {
    LOG(ERROR) << "UserDB snapshot cannot publish " << target << ": "
               << ec.message();
    std::error_code remove_error;
    fs::remove(pending, remove_error);
    if (remove_error)
      LOG(WARNING) << "cannot remove pending UserDB snapshot " << pending
                   << ": " << remove_error.message();
    return false;
  }
  LOG(INFO) << "UserDB snapshot completed: name=" << dict_name
            << " path=" << target;
  return true;
}

bool UserDictManager::Restore(const path& snapshot_file) {
  if (!deployer_)
    return false;
  const path resolved =
      safe_path::ExistingFileNoSymlinks(deployer_->sync_dir, snapshot_file);
  if (resolved.empty()) {
    LOG(ERROR) << "snapshot outside sync root or through a symlink: "
               << snapshot_file;
    return false;
  }
  UserDictList names;
  if (!GetUserDictList(&names))
    return false;
  for (const string& name : names) {
    if (resolved.filename().to_utf8_string() ==
        name + UserDb::snapshot_extension())
      return RestoreMerge(resolved, name);
  }
  LOG(ERROR) << "snapshot does not belong to a local UserDB: " << snapshot_file;
  return false;
}

namespace {
int SnapshotEntryCount(const path& text_file) {
  // 直接读取头部记录数，避免重复遍历数据库。
  std::ifstream input(static_cast<const std::filesystem::path&>(text_file),
                      std::ios::binary);
  if (!input)
    return -1;
  string line;
  constexpr char kRecordCount[] = "#@/record_count\t";
  while (std::getline(input, line)) {
    if (line.compare(0, sizeof(kRecordCount) - 1, kRecordCount) == 0) {
      const string raw = line.substr(sizeof(kRecordCount) - 1);
      if (raw.empty() || raw.find_first_not_of("0123456789") != string::npos)
        return -1;
      try {
        const auto count = std::stoull(raw);
        return count <= INT_MAX ? static_cast<int>(count) : -1;
      } catch (...) {
        return -1;
      }
    }
    if (!line.empty() && line[0] != '#')
      break;
  }
  return -1;
}
}  // namespace

int UserDictManager::Export(const string& dict_name, const path& text_file) {
  if (!boost::ends_with(text_file.filename().to_utf8_string(),
                        UserDb::snapshot_extension())) {
    LOG(ERROR) << "UserDB TSV export requires *.userdb.tsv";
    return -1;
  }
  the<Db> db(user_db_component_->Create(dict_name));
  if (!db || !db->OpenReadOnly())
    return -1;
  const bool ok = db->Backup(text_file);
  db->Close();
  return ok ? SnapshotEntryCount(text_file) : -1;
}

int UserDictManager::Import(const string& dict_name, const path& text_file) {
  // 普通导入仅处理指定的五字段 UserDB。
  UserDictList names;
  if (!GetUserDictList(&names) ||
      std::find(names.begin(), names.end(), dict_name) == names.end())
    return -1;
  size_t count = 0;
  if (!RestoreMerge(text_file, dict_name, &count))
    return -1;
  return static_cast<int>(count);
}

bool UserDictManager::RestoreMerge(const path& snapshot_file,
                                   const string& name,
                                   size_t* source_records_out) {
  // UniformRestore performs full validation before writing. Read only the
  // declared count here; avoid a second full-file validation scan.
  const int declared_records = SnapshotEntryCount(snapshot_file);
  if (declared_records < 0) {
    LOG(ERROR) << "UserDB merge snapshot has no valid record count: "
               << snapshot_file;
    return false;
  }
  const size_t source_records = static_cast<size_t>(declared_records);
  if (source_records_out)
    *source_records_out = source_records;

  the<Db> temp(user_db_component_->Create(".userdb_sync_staging"));
  if (!temp) {
    LOG(ERROR) << "UserDB merge cannot create staging database handle.";
    return false;
  }
  if (!safe_path::SafeDatabaseLocation(path_, temp->file_path())) {
    LOG(ERROR) << "UserDB merge rejected unsafe staging path: "
               << temp->file_path();
    return false;
  }
  if (temp->Exists() && !temp->Remove()) {
    LOG(ERROR) << "UserDB merge cannot remove stale staging database: "
               << temp->file_path();
    return false;
  }
  if (!temp->Open()) {
    LOG(ERROR) << "UserDB merge cannot open staging database: "
               << temp->file_path();
    return false;
  }
  BOOST_SCOPE_EXIT((&temp)) {
    temp->Close();
    temp->Remove();
  }
  BOOST_SCOPE_EXIT_END

  if (snapshot_file.filename().to_utf8_string() !=
      name + UserDb::snapshot_extension()) {
    LOG(ERROR) << "UserDB merge snapshot filename does not match database: "
               << snapshot_file
               << " expected=" << name + UserDb::snapshot_extension();
    return false;
  }
  if (!temp->Restore(snapshot_file)) {
    LOG(ERROR) << "UserDB merge cannot restore snapshot into staging database: "
               << snapshot_file;
    return false;
  }
  if (!UserDbHelper(temp).IsUserDb()) {
    LOG(ERROR) << "UserDB merge staging database has invalid /db_type: "
               << snapshot_file;
    return false;
  }
  const string staged_name = UserDbHelper(temp).GetDbName();
  if (staged_name != name) {
    LOG(ERROR) << "UserDB merge staging database name mismatch: expected="
               << name << " actual=" << staged_name;
    return false;
  }

  string remote_clock_raw;
  uint64_t remote_clock = 0;
  if (!temp->MetaFetch("/manual_clock", &remote_clock_raw)) {
    LOG(ERROR) << "UserDB merge snapshot missing /manual_clock: "
               << snapshot_file;
    return false;
  }
  if (!manual_stamp_codec::Parse(remote_clock_raw, &remote_clock)) {
    LOG(ERROR) << "UserDB merge snapshot has invalid /manual_clock: "
               << snapshot_file;
    return false;
  }
  string remote_tick_raw;
  uint64_t remote_tick = 0;
  if (!temp->MetaFetch("/tick", &remote_tick_raw)) {
    LOG(ERROR) << "UserDB merge snapshot missing /tick: " << snapshot_file;
    return false;
  }
  try {
    size_t used = 0;
    remote_tick = std::stoull(remote_tick_raw, &used);
    if (used != remote_tick_raw.size()) {
      LOG(ERROR) << "UserDB merge snapshot has malformed /tick: "
                 << snapshot_file;
      return false;
    }
  } catch (const std::exception&) {
    LOG(ERROR) << "UserDB merge snapshot has out-of-range /tick: "
               << snapshot_file;
    return false;
  }

  the<Db> dest(user_db_component_->Create(name));
  if (!dest || !safe_path::SafeDatabaseLocation(path_, dest->file_path())) {
    LOG(ERROR) << "UserDB merge rejected unsafe destination database: " << name;
    return false;
  }
  if (!dest->Open()) {
    LOG(ERROR) << "UserDB merge cannot open destination database: "
               << dest->file_path();
    return false;
  }
  BOOST_SCOPE_EXIT((&dest)) {
    dest->Close();
  }
  BOOST_SCOPE_EXIT_END
  if (!IsMergeUserDb(dest.get()) || UserDbHelper(dest).GetDbName() != name) {
    LOG(ERROR) << "UserDB merge destination metadata mismatch: "
               << dest->file_path();
    return false;
  }
  auto transaction = dynamic_cast<Transactional*>(dest.get());
  if (!transaction) {
    LOG(ERROR) << "UserDB merge destination does not support transactions: "
               << dest->file_path();
    return false;
  }
  if (!transaction->BeginTransaction()) {
    LOG(ERROR) << "UserDB merge cannot begin destination transaction: "
               << dest->file_path();
    return false;
  }

  bool ok = false;
  try {
    DbSource source(temp.get());
    UserDbMerger merger(dest.get());
    merger.ObserveClock(remote_clock);
    merger.ObserveTick(remote_tick);
    source >> merger;
    ok = !merger.failed();
    merger.CloseMerge();
    ok = ok && !merger.failed();
  } catch (const std::exception& e) {
    LOG(ERROR) << "UserDB merge threw an exception for " << snapshot_file
               << ": " << e.what();
  }
  if (!ok) {
    if (!transaction->AbortTransaction())
      LOG(ERROR) << "UserDB merge also failed to roll back destination: "
                 << dest->file_path();
    LOG(ERROR) << "UserDB merge failed while applying snapshot: "
               << snapshot_file << " records=" << source_records;
    return false;
  }
  if (!transaction->CommitTransaction()) {
    LOG(ERROR) << "UserDB merge cannot commit destination transaction: "
               << dest->file_path();
    return false;
  }
  LOG(INFO) << "UserDB merge completed: name=" << name
            << " records=" << source_records;
  return true;
}

bool UserDictManager::SynchronizeMerge(const string& name) {
  if (!deployer_ || !safe_path::ValidRelativeName(name)) {
    LOG(ERROR) << "UserDB synchronization rejected invalid database name: "
               << name;
    return false;
  }
  path local_path;
  {
    the<Db> local(user_db_component_->Create(name));
    if (!local) {
      LOG(ERROR)
          << "UserDB synchronization cannot create local database handle: "
          << name;
      return false;
    }
    local_path = local->file_path();
    if (!safe_path::SafeDatabaseLocation(path_, local_path)) {
      LOG(ERROR) << "UserDB synchronization rejected unsafe local path: "
                 << local_path;
      return false;
    }
    if (local->Exists()) {
      if (!local->OpenReadOnly()) {
        LOG(ERROR) << "UserDB synchronization cannot inspect local database: "
                   << local_path;
        return false;
      }
      const bool valid =
          IsMergeUserDb(local.get()) && UserDbHelper(local).GetDbName() == name;
      if (!local->Close()) {
        LOG(ERROR) << "UserDB synchronization cannot close local database: "
                   << local_path;
        return false;
      }
      if (!valid) {
        LOG(ERROR)
            << "UserDB synchronization local database metadata mismatch: "
            << local_path;
        return false;
      }
    }
  }

  bool success = true;
  const path sync_dir(deployer_->sync_dir);
  std::error_code ec;
  fs::create_directories(sync_dir, ec);
  if (ec) {
    LOG(ERROR) << "UserDB synchronization cannot create sync directory "
               << sync_dir << ": " << ec.message();
    return false;
  }
  if (!fs::is_directory(sync_dir, ec) || ec) {
    LOG(ERROR) << "UserDB synchronization path is not a directory: " << sync_dir
               << (ec ? ": " + ec.message() : "");
    return false;
  }

  const string snapshot = name + UserDb::snapshot_extension();
  size_t devices_checked = 0;
  size_t snapshots_found = 0;
  for (fs::directory_iterator it(sync_dir, ec), end; !ec && it != end;
       it.increment(ec)) {
    const path device_dir = it->path();
    if (!deployer_->user_id.empty() &&
        device_dir.filename().to_utf8_string() == deployer_->user_id)
      continue;
    std::error_code status_error;
    const auto status = it->symlink_status(status_error);
    if (status_error) {
      LOG(ERROR) << "UserDB synchronization cannot inspect sync entry "
                 << device_dir << ": " << status_error.message();
      success = false;
      continue;
    }
    if (fs::is_symlink(status) || !fs::is_directory(status))
      continue;
    ++devices_checked;
    const path candidate = device_dir / snapshot;
    std::error_code check_error;
    if (!fs::exists(candidate, check_error)) {
      if (check_error) {
        LOG(ERROR) << "UserDB synchronization cannot inspect snapshot "
                   << candidate << ": " << check_error.message();
        success = false;
      }
      continue;
    }
    const path file = safe_path::ExistingFileNoSymlinks(sync_dir, candidate);
    if (file.empty()) {
      LOG(ERROR) << "UserDB synchronization rejected unsafe snapshot path: "
                 << candidate;
      success = false;
      continue;
    }
    ++snapshots_found;
    if (!RestoreMerge(file, name))
      success = false;
  }
  if (ec) {
    LOG(ERROR) << "UserDB synchronization cannot enumerate sync directory "
               << sync_dir << ": " << ec.message();
    success = false;
  }

  the<Db> db(user_db_component_->Create(name));
  if (!db || !db->Exists()) {
    LOG(ERROR) << "UserDB disappeared during synchronization: "
               << (db ? db->file_path() : local_path);
    return false;
  }
  if (success && !Backup(name))
    success = false;
  LOG(INFO) << "UserDB synchronization finished: name=" << name
            << " devices=" << devices_checked
            << " snapshots=" << snapshots_found
            << " result=" << (success ? "success" : "failed");
  return success;
}

bool UserDictManager::Synchronize(const string& dict_name) {
  UserDictList names;
  if (!GetUserDictList(&names))
    return false;
  if (std::find(names.begin(), names.end(), dict_name) == names.end()) {
    LOG(WARNING) << "skipping non-UserDB synchronization request: "
                 << dict_name;
    return false;
  }
  return SynchronizeMerge(dict_name);
}

bool UserDictManager::SynchronizeAll() {
  if (!deployer_) {
    LOG(ERROR) << "UserDB synchronization has no deployer.";
    return false;
  }
  UserDictList names;
  if (!GetUserDictList(&names))
    return false;
  if (names.empty()) {
    LOG(INFO) << "UserDB synchronization finished: no local UserDB found.";
    return true;
  }
  int failures = 0;
  for (const string& name : names) {
    if (!SynchronizeMerge(name))
      ++failures;
  }
  if (failures)
    LOG(ERROR) << "UserDB synchronization finished: databases=" << names.size()
               << " failures=" << failures;
  else
    LOG(INFO) << "UserDB synchronization finished: databases=" << names.size()
              << " failures=0";
  return failures == 0;
}

}  // namespace rime
