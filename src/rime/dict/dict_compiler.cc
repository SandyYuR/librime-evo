//
// Copyright RIME Developers
// Distributed under the BSD License
//
// 2011-11-27 GONG Chen <chen.sst@gmail.com>
//
#include <filesystem>
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <fstream>
#include <cctype>
#include <boost/algorithm/string.hpp>
#include <rime/algo/strings.h>
#include <utf8.h>
#include <rime/algo/algebra.h>
#include <rime/algo/utilities.h>
#include <rime/dict/auxiliary_db.h>
#include <rime/dict/corrector.h>
#include <rime/dict/dict_compiler.h>
#include <rime/dict/dict_settings.h>
#include <rime/dict/dictionary.h>
#include <rime/dict/entry_collector.h>
#include <rime/dict/preset_vocabulary.h>
#include <rime/dict/prism.h>
#include <rime/dict/reverse_lookup_dictionary.h>
#include <rime/dict/table.h>
#include <rime/resource.h>
#include <rime/service.h>

namespace rime {

static bool IsAsciiPrismKey(const string& key);
static ReverseLookupTable ReadAuxiliarySources(const vector<path>& files);
static bool BuildAuxiliaryDatabase(const ReverseLookupTable& sources,
                                   an<ConfigList> algebra,
                                   const path& target,
                                   uint32_t checksum);

static bool get_speller_algebra(Config& config,
                                const string& path,
                                an<ConfigList>* algebra,
                                const string& group_name = {}) {
  *algebra = config.GetList(path);
  if (*algebra)
    return true;

  auto groups = config.GetMap(path);
  if (!groups)
    return true;

  if (group_name.empty()) {
    LOG(ERROR) << path << " contains named groups but no group was selected";
    return false;
  }
  string selected_group = group_name;
  if (selected_group == "auto") {
    auto it = groups->begin();
    if (it == groups->end()) {
      LOG(ERROR) << path << " contains no named groups";
      return false;
    }
    auto next = it;
    ++next;
    if (next != groups->end()) {
      LOG(ERROR) << path
                 << " has multiple named groups; selector 'auto' is "
                    "ambiguous";
      return false;
    }
    selected_group = it->first;
  }
  *algebra = As<ConfigList>(groups->Get(selected_group));
  if (!*algebra) {
    LOG(ERROR) << path << " group '" << selected_group
               << "' is missing or "
                  "not a list";
    return false;
  }
  return true;
}

DictCompiler::DictCompiler(Dictionary* dictionary)
    : dict_name_(dictionary->name()),
      packs_(dictionary->packs()),
      prism_(dictionary->prism()),
      tables_(dictionary->tables()),
      source_resolver_(
          Service::instance().CreateResourceResolver({"source_file", "", ""})),
      target_resolver_(Service::instance().CreateStagingResourceResolver(
          {"target_file", "", ""})) {}

DictCompiler::~DictCompiler() {}

static bool load_dict_settings_from_file(DictSettings* settings,
                                         const path& dict_file) {
  std::ifstream fin(dict_file.c_str());
  bool success = settings->LoadDictHeader(fin);
  fin.close();
  return success;
}

static bool get_dict_files_from_settings(vector<path>* dict_files,
                                         DictSettings& settings,
                                         ResourceResolver* source_resolver) {
  if (auto tables = settings.GetTables()) {
    for (auto it = tables->begin(); it != tables->end(); ++it) {
      string dict_name = As<ConfigValue>(*it)->str();
      auto dict_file = source_resolver->ResolvePath(dict_name + ".dict.yaml");
      if (!std::filesystem::exists(dict_file)) {
        LOG(ERROR) << "source file '" << dict_file << "' does not exist.";
        return false;
      }
      dict_files->push_back(dict_file);
    }
  }
  return true;
}

static uint32_t compute_dict_file_checksum(uint32_t initial_checksum,
                                           const vector<path>& dict_files,
                                           DictSettings& settings) {
  if (dict_files.empty()) {
    return initial_checksum;
  }
  ChecksumComputer cc(initial_checksum);
  for (const auto& file_path : dict_files) {
    cc.ProcessFile(file_path);
  }
  if (settings.use_preset_vocabulary()) {
    cc.ProcessFile(PresetVocabulary::DictFilePath(settings.vocabulary()));
  }
  return cc.Checksum();
}

bool DictCompiler::Compile(const path& schema_file) {
  LOG(INFO) << "compiling dictionary for " << schema_file;
  bool build_table_from_source = true;
  DictSettings settings;
  auto dict_file = source_resolver_->ResolvePath(dict_name_ + ".dict.yaml");
  if (!std::filesystem::exists(dict_file)) {
    LOG(ERROR) << "source file '" << dict_file << "' does not exist.";
    build_table_from_source = false;
  } else if (!load_dict_settings_from_file(&settings, dict_file)) {
    LOG(ERROR) << "failed to load settings from '" << dict_file << "'.";
    return false;
  }
  vector<path> dict_files;
  if (!get_dict_files_from_settings(&dict_files, settings,
                                    source_resolver_.get())) {
    return false;
  }
  uint32_t schema_file_checksum =
      schema_file.empty() ? 0 : Checksum(schema_file);
  uint32_t dict_file_checksum =
      compute_dict_file_checksum(0, dict_files, settings);
  bool rebuild_table = false;
  bool rebuild_prism = false;
  const auto& primary_table = tables_[0];
  if (primary_table->Exists() && primary_table->Load()) {
    if (build_table_from_source) {
      rebuild_table = primary_table->dict_file_checksum() != dict_file_checksum;
    } else {
      dict_file_checksum = primary_table->dict_file_checksum();
      LOG(INFO) << "reuse existing table: " << primary_table->file_path();
    }
    primary_table->Close();
  } else if (build_table_from_source) {
    rebuild_table = true;
  } else {
    LOG(ERROR) << "neither " << dict_name_ << ".dict.yaml nor " << dict_name_
               << ".table.bin exists.";
    return false;
  }
  if (prism_->Exists() && prism_->Load()) {
    rebuild_prism = prism_->dict_file_checksum() != dict_file_checksum ||
                    prism_->schema_file_checksum() != schema_file_checksum;
    prism_->Close();
  } else {
    rebuild_prism = true;
  }
  LOG(INFO) << dict_file << "[" << dict_files.size() << " file(s)]"
            << " (" << dict_file_checksum << ")";
  LOG(INFO) << schema_file << " (" << schema_file_checksum << ")";
  {
    the<ResourceResolver> resolver(
        Service::instance().CreateDeployedResourceResolver(
            {"find_reverse_db", "", ".reverse.bin"}));
    ReverseDb reverse_db(resolver->ResolvePath(dict_name_));
    if (!reverse_db.Exists() || !reverse_db.Load() ||
        reverse_db.dict_file_checksum() != dict_file_checksum) {
      rebuild_table = true;
    }
  }
  if (build_table_from_source && (options_ & kRebuildTable)) {
    rebuild_table = true;
  }
  if (options_ & kRebuildPrism) {
    rebuild_prism = true;
  }
  Syllabary syllabary;
  if (rebuild_table) {
    EntryCollector collector;
    if (!BuildTable(0, collector, &settings, dict_files, dict_file_checksum)) {
      return false;
    }
    syllabary = std::move(collector.syllabary);
  } else if (packs_.size() > 0) {
    if (primary_table->Load() && primary_table->GetSyllabary(&syllabary))
      primary_table->Close();
    else
      LOG(WARNING) << "couldn't load syllabary from '" << schema_file << "'";
  }
  if (rebuild_prism &&
      !BuildPrism(schema_file, dict_file_checksum, schema_file_checksum)) {
    return false;
  }
  for (int table_index = 1; table_index < tables_.size(); ++table_index) {
    const auto& pack_name = packs_[table_index - 1];
    auto pack_table = tables_[table_index];
    EntryCollector collector(std::move(syllabary));
    DictSettings settings;
    auto dict_file = source_resolver_->ResolvePath(pack_name + ".dict.yaml");
    if (!std::filesystem::exists(dict_file)) {
      if (pack_table->Exists())
        LOG(INFO) << "pack source file '" << dict_file
                  << "' does not exist, using prebuilt table '"
                  << pack_table->file_path() << "'";
      else
        LOG(ERROR) << "neither pack source file '" << dict_file
                   << "' nor a prebuilt table exists";
      continue;
    }
    if (!load_dict_settings_from_file(&settings, dict_file)) {
      LOG(ERROR) << "failed to load settings from '" << dict_file << "'.";
      continue;
    }
    vector<path> dict_files;
    if (!get_dict_files_from_settings(&dict_files, settings,
                                      source_resolver_.get())) {
      continue;
    }
    uint32_t pack_file_checksum =
        compute_dict_file_checksum(dict_file_checksum, dict_files, settings);
    bool rebuild_pack = true;
    if (pack_table->Exists() && pack_table->Load()) {
      rebuild_pack = pack_table->dict_file_checksum() != pack_file_checksum;
    }
    if (rebuild_pack) {
      LOG(INFO) << "rebuilding pack '" << pack_name << "'";
      if (!BuildTable(table_index, collector, &settings, dict_files,
                      pack_file_checksum)) {
        LOG(ERROR) << "failed to build pack: " << pack_name;
      }
    } else {
      LOG(INFO) << "pack '" << pack_name << "' reuses up-to-date table '"
                << pack_table->file_path() << "'";
    }
    syllabary = std::move(collector.syllabary);
    pack_table->Close();
  }
  // done!
  return true;
}

static path relocate_target(const path& source_path,
                            ResourceResolver* target_resolver) {
  auto resource_id = source_path.filename().u8string();
  return target_resolver->ResolvePath(resource_id);
}

bool DictCompiler::BuildTable(int table_index,
                              EntryCollector& collector,
                              DictSettings* settings,
                              const vector<path>& dict_files,
                              uint32_t dict_file_checksum) {
  auto& table = tables_[table_index];
  auto target_path =
      relocate_target(table->file_path(), target_resolver_.get());
  LOG(INFO) << "building table: " << target_path;
  table = New<Table>(target_path);

  collector.Configure(settings);
  collector.Collect(dict_files);
  if (options_ & kDump) {
    path dump_path(table->file_path());
    dump_path.replace_extension(".txt");
    collector.Dump(dump_path);
  }
  Vocabulary vocabulary;
  // build .table.bin
  {
    map<string, SyllableId> syllable_to_id;
    SyllableId syllable_id = 0;
    for (const auto& s : collector.syllabary) {
      syllable_to_id[s] = syllable_id++;
    }
    for (const auto& r : collector.entries) {
      Code code;
      for (const auto& s : r->raw_code) {
        code.push_back(syllable_to_id[s]);
      }
      // release memory in time to reduce memory usage
      RawCode().swap(r->raw_code);
      auto ls = vocabulary.LocateEntries(code);
      if (!ls) {
        LOG(ERROR) << "Error locating entries in vocabulary.";
        continue;
      }
      auto e = New<ShortDictEntry>();
      e->code.swap(code);
      e->text.swap(r->text);
      e->weight = log(r->weight > 0 ? r->weight : DBL_EPSILON);
      ls->push_back(e);
    }
    // release memory in time to reduce memory usage
    vector<of<RawDictEntry>>().swap(collector.entries);
    if (settings->sort_order() != "original") {
      vocabulary.SortHomophones();
    }
    table->Remove();
    if (!table->Build(collector.syllabary, vocabulary, collector.num_entries,
                      dict_file_checksum) ||
        !table->Save()) {
      return false;
    }
  }
  // build reverse db for the primary table
  if (table_index == 0 &&
      !BuildReverseDb(settings, collector, vocabulary, dict_file_checksum)) {
    return false;
  }
  return true;
}

bool DictCompiler::BuildReverseDb(DictSettings* settings,
                                  const EntryCollector& collector,
                                  const Vocabulary& vocabulary,
                                  uint32_t dict_file_checksum) {
  // build .reverse.bin
  auto target_path = target_resolver_->ResolvePath(dict_name_ + ".reverse.bin");
  ReverseDb reverse_db(target_path);
  if (!reverse_db.Build(settings, collector.syllabary, vocabulary,
                        collector.stems, dict_file_checksum) ||
      !reverse_db.Save()) {
    LOG(ERROR) << "error building reversedb.";
    return false;
  }
  return true;
}

static bool IsAsciiPrismKey(const string& key) {
  if (key.empty())
    return false;
  return std::all_of(key.begin(), key.end(), [](unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '/';
  });
}

// Read the independent AUX dictionary. Each row is simply character<TAB>code;
// YAML headers are optional, and multiple rows may define multiple variants.
static ReverseLookupTable ReadAuxiliarySources(const vector<path>& files) {
  ReverseLookupTable auxiliary;
  for (const auto& file : files) {
    std::ifstream in(file.c_str());
    DictSettings header;
    if (!header.LoadDictHeader(in, true))
      continue;
    const int text_col = header.GetColumnIndex("text");
    const int code_col = header.GetColumnIndex("code");
    if (text_col < 0 || code_col < 0)
      continue;
    string line;
    while (std::getline(in, line)) {
      boost::algorithm::trim_right(line);
      if (line.empty() || line[0] == '#')
        continue;
      auto row = strings::split(line, "\t");
      if (static_cast<int>(row.size()) <= std::max(text_col, code_col))
        continue;
      const string& word = row[text_col];
      const string& code = row[code_col];
      auto pos = word.begin();
      if (word.empty() || code.empty())
        continue;
      utf8::next(pos, word.end());
      if (pos != word.end() || !IsAsciiPrismKey(code))
        continue;
      auxiliary[word].insert(code);
    }
  }
  return auxiliary;
}

static bool BuildAuxiliaryDatabase(const ReverseLookupTable& sources,
                                   an<ConfigList> aux_algebra,
                                   const path& target,
                                   uint32_t checksum) {
  Projection projection;
  if (!projection.Load(aux_algebra))
    return false;
  map<pair<string, string>, SpellingProperties> entries;
  for (const auto& item : sources) {
    for (const auto& raw_code : item.second) {
      Script projected;
      projected.AddSyllable(raw_code);
      projection.Apply(&projected);
      for (const auto& spelling : projected) {
        if (!IsAsciiPrismKey(spelling.first))
          continue;
        for (const auto& variant : spelling.second) {
          if (variant.properties.auxiliary) {
            auto key = std::make_pair(item.first, spelling.first);
            auto it = entries.find(key);
            if (it == entries.end() ||
                variant.properties.type < it->second.type ||
                (variant.properties.type == it->second.type &&
                 variant.properties.credibility > it->second.credibility)) {
              entries[key] = variant.properties;
            }
          }
        }
      }
    }
  }
  map<string, vector<AuxiliaryCode>> by_character;
  map<string, vector<AuxiliaryCharacter>> by_code;
  for (const auto& item : entries) {
    const auto& props = item.second;
    by_character[item.first.first].push_back(
        {item.first.second, props.type, props.credibility});
    by_code[item.first.second].push_back(
        {item.first.first, props.type, props.credibility});
  }
  AuxiliaryDb database(target);
  database.Remove();
  return database.Build(by_character, by_code, checksum) && database.Save();
}

bool DictCompiler::CompileAuxiliary(const path& schema_file) {
  if (schema_file.empty())
    return true;
  Config schema;
  if (!schema.LoadFromFile(schema_file))
    return false;

  vector<pair<string, an<ConfigList>>> groups;
  if (auto rules = schema.GetList("speller/aux_algebra")) {
    groups.emplace_back("auto", rules);
  } else if (auto named = schema.GetMap("speller/aux_algebra")) {
    for (const auto& item : *named) {
      auto rules = As<ConfigList>(item.second);
      if (!rules) {
        LOG(ERROR) << "speller/aux_algebra group '" << item.first
                   << "' is not a list";
        return false;
      }
      groups.emplace_back(item.first, rules);
    }
  }
  if (groups.empty())
    return true;

  string source_name;
  if (!schema.GetString("speller/aux_dictionary", &source_name) ||
      source_name.empty())
    return true;
  if (!boost::iends_with(source_name, ".yaml"))
    source_name += ".yaml";
  the<ResourceResolver> source_resolver(
      Service::instance().CreateResourceResolver({"source_file", "", ""}));
  const auto source = source_resolver->ResolvePath(source_name);
  if (!std::filesystem::exists(source)) {
    LOG(ERROR) << "auxiliary source '" << source_name << "' does not exist";
    return false;
  }
  const uint32_t source_checksum = Checksum(source);
  the<ResourceResolver> target_resolver(
      Service::instance().CreateStagingResourceResolver(
          {"auxiliary_db", "", ".aux.bin"}));
  ReverseLookupTable sources;
  bool sources_loaded = false;
  for (const auto& group : groups) {
    // Each artifact depends only on the shared source and this group's rules.
    boost::crc_32_type checksum(source_checksum);
    for (const auto& item : *group.second) {
      auto rule = As<ConfigValue>(item);
      if (!rule) {
        LOG(ERROR) << "speller/aux_algebra group '" << group.first
                   << "' contains a non-string rule";
        return false;
      }
      checksum.process_bytes(rule->str().c_str(), rule->str().size() + 1);
    }
    const auto target = target_resolver->ResolvePath(group.first);
    AuxiliaryDb database(target);
    if (database.Exists() && database.Load()) {
      const bool current = database.checksum() == checksum.checksum();
      database.Close();
      if (current)
        continue;
    }
    if (!sources_loaded) {
      sources = ReadAuxiliarySources({source});
      sources_loaded = true;
    }
    if (!BuildAuxiliaryDatabase(sources, group.second, target,
                                checksum.checksum())) {
      LOG(ERROR) << "failed to compile auxiliary group '" << group.first << "'";
      return false;
    }
  }
  return true;
}

bool DictCompiler::BuildPrism(const path& schema_file,
                              uint32_t dict_file_checksum,
                              uint32_t schema_file_checksum) {
  LOG(INFO) << "building prism...";
  auto target_path =
      relocate_target(prism_->file_path(), target_resolver_.get());
  prism_ = New<Prism>(target_path);

  // get syllabary from primary table, which may not be rebuilt
  Syllabary syllabary;
  const auto& primary_table = tables_[0];
  if (!primary_table->Load() || !primary_table->GetSyllabary(&syllabary) ||
      syllabary.empty())
    return false;
  // apply spelling algebra and prepare corrections (if enabled)
  Script script;
  if (!schema_file.empty()) {
    Config config;
    if (!config.LoadFromFile(schema_file)) {
      LOG(ERROR) << "error loading prism definition from " << schema_file;
      return false;
    }
    Projection p;
    an<ConfigList> algebra;
    string algebra_path = translator_namespace_ + "/algebra";
    if (config.IsNull(algebra_path))
      algebra_path = "speller/algebra";
    if (!get_speller_algebra(config, algebra_path, &algebra, input_type_))
      return false;
    if (algebra) {
      for (size_t i = 0; i < algebra->size(); ++i) {
        auto formula = algebra->GetValueAt(i);
        if (!formula)
          continue;
        const string& text = formula->str();
        if (text.size() > 3 && text.compare(0, 3, "aux") == 0 &&
            !std::isalpha(static_cast<unsigned char>(text[3]))) {
          LOG(ERROR) << "ordinary speller/algebra cannot contain AUX rule: "
                     << text;
          return false;
        }
      }
    }
    const bool ordinary_loaded = !algebra || p.Load(algebra);
    if (!ordinary_loaded) {
      LOG(ERROR) << "failed to load spelling algebra";
      return false;
    }
    for (const auto& x : syllabary) {
      script.AddSyllable(x);
    }
    if (ordinary_loaded)
      p.Apply(&script);

#if 0
    // build corrector
    bool enable_correction = false; // Avoid if initializer to comfort compilers
    if (config.GetBool("translator/enable_correction", &enable_correction) &&
        enable_correction) {
      path corrector_path(prism_->file_path());
      corrector_path.replace_extension("");
      corrector_path.replace_extension(".correction.bin");
      auto target_path = relocate_target(corrector_path,
                                         target_resolver_.get());
      correction_ = New<EditDistanceCorrector>(target_path);
      if (correction_->Exists()) {
        correction_->Remove();
      }
      if (!correction_->Build(syllabary, &script,
                         dict_file_checksum, schema_file_checksum) ||
          !correction_->Save()) {
        return false;
      }
    }
#endif
  }
  if ((options_ & kDump) && !script.empty()) {
    path dump_path(prism_->file_path());
    dump_path.replace_extension(".txt");
    script.Dump(dump_path);
  }
  // build .prism.bin
  {
    prism_->Remove();
    if (!prism_->Build(syllabary, script.empty() ? nullptr : &script,
                       dict_file_checksum, schema_file_checksum) ||
        !prism_->Save()) {
      return false;
    }
  }

  return true;
}

}  // namespace rime
