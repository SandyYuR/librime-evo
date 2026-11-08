//
// Copyright RIME Developers
// Distributed under the BSD License
//
// 2011-11-27 GONG Chen <chen.sst@gmail.com>
//
#ifndef RIME_DICT_COMPILER_H_
#define RIME_DICT_COMPILER_H_

#include <rime_api.h>
#include <rime/common.h>
#include <rime/dict/vocabulary.h>

namespace rime {

class Dictionary;
class ConfigList;
class Prism;
class Table;
class ReverseDb;
class DictSettings;
class EntryCollector;
class Vocabulary;
class ResourceResolver;

class DictCompiler {
 public:
  enum Options {
    kRebuildPrism = 1,
    kRebuildTable = 2,
    kRebuild = kRebuildPrism | kRebuildTable,
    kDump = 4,
  };

  RIME_DLL explicit DictCompiler(Dictionary* dictionary);
  RIME_DLL virtual ~DictCompiler();

  RIME_DLL bool Compile(const path& schema_file);
  RIME_DLL static bool CompileDictionaries(const path& schema_file,
                                           int options = 0);
  RIME_DLL static bool CompileAuxiliary(const path& schema_file);
  void set_options(int options) { options_ = options; }

 private:
  struct PrismSpec {
    an<Prism> prism;
    an<ConfigList> algebra;
  };

  bool BuildTable(int table_index,
                  EntryCollector& collector,
                  DictSettings* settings,
                  const vector<path>& dict_files,
                  uint32_t dict_file_checksum);
  bool BuildPrism(const an<ConfigList>& algebra,
                  uint32_t dict_file_checksum,
                  uint32_t schema_file_checksum);
  bool BuildReverseDb(DictSettings* settings,
                      const EntryCollector& collector,
                      const Vocabulary& vocabulary,
                      uint32_t dict_file_checksum);

  const string& dict_name_;
  const vector<string>& packs_;
  an<Prism> prism_;
  vector<of<Table>> tables_;
  int options_ = 0;
  vector<PrismSpec> prism_specs_;
  the<ResourceResolver> source_resolver_;
  the<ResourceResolver> target_resolver_;
};

}  // namespace rime

#endif  // RIME_DICT_COMPILER_H_
