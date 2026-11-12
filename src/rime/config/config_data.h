//
// Copyright RIME Developers
// Distributed under the BSD License
//

#ifndef RIME_CONFIG_DATA_H_
#define RIME_CONFIG_DATA_H_

#include <iostream>
#include <rime/common.h>

namespace rime {

class ConfigCompiler;
class ConfigItem;

class ConfigData {
 public:
  ConfigData() = default;
  ~ConfigData();

  // returns whether actually saved to file.
  bool Save();
  bool LoadFromStream(std::istream& stream);
  bool SaveToStream(std::ostream& stream);
  bool LoadFromFile(const path& file_path, ConfigCompiler* compiler);
  bool SaveToFile(const path& file_path);
  bool TraverseWrite(const string& path, an<ConfigItem> item);
  an<ConfigItem> Traverse(const string& path);

  static vector<string> SplitPath(const string& path);
  static string JoinPath(const vector<string>& keys);
  static bool IsListItemReference(const string& key);
  static string FormatListIndex(size_t index);
  static size_t ResolveListIndex(an<ConfigItem> list,
                                 const string& key,
                                 bool read_only = false);

  const path& file_path() const { return file_path_; }
  bool modified() const { return modified_; }
  void set_modified() { modified_ = true; }
  void set_auto_save(bool auto_save) { auto_save_ = auto_save; }
  bool build_successful() const { return build_successful_; }
  void set_build_successful(bool successful) { build_successful_ = successful; }

  an<ConfigItem> root;

 protected:
  path file_path_;
  bool modified_ = false;
  bool auto_save_ = false;
  // This result belongs to the current in-memory compilation, never a previous
  // artifact or source-supplied __build_info. Partial data remains readable.
  bool build_successful_ = false;
};

}  // namespace rime

#endif  // RIME_CONFIG_DATA_H_
