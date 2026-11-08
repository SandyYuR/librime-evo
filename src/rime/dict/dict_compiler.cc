//
// Copyright RIME Developers
// Distributed under the BSD License
//
// 2011-11-27 GONG Chen <chen.sst@gmail.com>
//
#include <filesystem>
#include <limits>
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
#include <rime/dict/dict_compiler.h>
#include <rime/dict/dict_settings.h>
#include <rime/dict/dictionary.h>
#include <rime/dict/entry_collector.h>
#include <rime/dict/preset_vocabulary.h>
#include <rime/dict/prism.h>
#include <rime/dict/prism_config.h>
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

uint64_t DictCompiler::EstimateMemory(const string& dict_name, bool auxiliary) {
  constexpr uint64_t kEstimatedBytesPerSourceByte = 32;
  the<ResourceResolver> resolver(
      Service::instance().CreateResourceResolver({"source_file", "", ""}));
  const path source =
      resolver->ResolvePath(dict_name + (auxiliary ? ".yaml" : ".dict.yaml"));
  std::ifstream stream(source);
  DictSettings settings;
  set<path> sources{source};
  if (!auxiliary && stream && settings.LoadDictHeader(stream)) {
    if (auto tables = settings.GetTables()) {
      for (const auto& item : *tables) {
        if (auto value = As<ConfigValue>(item))
          sources.insert(resolver->ResolvePath(value->str() + ".dict.yaml"));
      }
    }
    if (settings.use_preset_vocabulary())
      sources.insert(PresetVocabulary::DictFilePath(settings.vocabulary()));
  }
  uint64_t size = 0;
  const auto maximum = std::numeric_limits<uint64_t>::max();
  for (const auto& file : sources) {
    std::error_code ec;
    const uint64_t bytes = std::filesystem::file_size(file, ec);
    if (!ec)
      size = bytes > maximum - size ? maximum : size + bytes;
  }
  return size > maximum / kEstimatedBytesPerSourceByte
             ? maximum
             : size * kEstimatedBytesPerSourceByte;
}

bool DictCompiler::ReadDictionarySpecs(Config* schema,
                                       vector<DictionarySpec>* definitions) {
  if (!schema || !definitions || !PrismConfig::ValidateReferences(schema))
    return false;
  definitions->clear();
  if (schema->IsNull("build/dictionaries"))
    return true;
  auto dictionaries = schema->GetMap("build/dictionaries");
  if (!dictionaries) {
    LOG(ERROR) << "build/dictionaries must be a map keyed by dictionary name";
    return false;
  }

  hash_set<string> table_names;
  hash_set<string> prism_names;
  for (const auto& item : *dictionaries) {
    auto entry = As<ConfigMap>(item.second);
    DictionarySpec definition;
    definition.name = item.first;
    if (definition.name.empty() || !entry) {
      LOG(ERROR) << "build/dictionaries entries require a nonempty dictionary "
                    "name and a map of build settings";
      return false;
    }
    if (!table_names.insert(definition.name).second) {
      LOG(ERROR) << "duplicate table build: " << definition.name;
      return false;
    }
    if (auto packs_item = entry->Get("packs")) {
      auto packs = As<ConfigList>(packs_item);
      if (!packs) {
        LOG(ERROR) << "build dictionary '" << definition.name
                   << "' packs must be a list";
        return false;
      }
      for (const auto& pack_item : *packs) {
        auto pack = As<ConfigValue>(pack_item);
        if (!pack || pack->str().empty()) {
          LOG(ERROR) << "invalid pack in build dictionary '" << definition.name
                     << "'";
          return false;
        }
        if (!table_names.insert(pack->str()).second) {
          LOG(ERROR) << "duplicate table build: " << pack->str();
          return false;
        }
        definition.packs.push_back(pack->str());
      }
    }
    auto algebra = entry->Get("algebra");
    if (!algebra || As<ConfigList>(algebra)) {
      definition.prisms.emplace_back(definition.name, As<ConfigList>(algebra));
    } else if (auto groups = As<ConfigMap>(algebra)) {
      if (groups->empty()) {
        LOG(ERROR) << "build dictionary '" << definition.name
                   << "' algebra contains no groups";
        return false;
      }
      for (const auto& group : *groups) {
        auto rules = As<ConfigList>(group.second);
        if (group.first.empty() || !rules) {
          LOG(ERROR) << "invalid algebra group in build dictionary '"
                     << definition.name << "'";
          return false;
        }
        definition.prisms.emplace_back(definition.name + "." + group.first,
                                       rules);
      }
    } else {
      LOG(ERROR) << "build dictionary '" << definition.name
                 << "' algebra must be a list or a map of lists";
      return false;
    }
    for (const auto& prism : definition.prisms) {
      if (!prism_names.insert(prism.first).second) {
        LOG(ERROR) << "duplicate prism build: " << prism.first;
        return false;
      }
    }
    definitions->push_back(std::move(definition));
  }

  return true;
}

bool DictCompiler::CompileDictionary(const DictionarySpec& definition,
                                     const path& schema_file,
                                     int options) {
  if (definition.name.empty() || definition.prisms.empty())
    return false;
  DictionaryComponent component;
  the<ResourceResolver> prism_resolver(
      Service::instance().CreateDeployedResourceResolver(
          {"prism", "", ".prism.bin"}));
  the<Dictionary> dictionary(component.Create(
      definition.name, definition.prisms.front().first, definition.packs));
  DictCompiler compiler(dictionary.get());
  compiler.set_options(options);
  for (const auto& prism : definition.prisms) {
    compiler.prism_specs_.push_back(
        {New<Prism>(prism_resolver->ResolvePath(prism.first)), prism.second});
  }
  return compiler.Compile(schema_file);
}

bool DictCompiler::CompileDictionaries(const path& schema_file, int options) {
  Config schema;
  if (!schema.LoadFromFile(schema_file))
    return false;
  vector<DictionarySpec> definitions;
  if (!ReadDictionarySpecs(&schema, &definitions))
    return false;
  bool success = true;
  for (const auto& definition : definitions) {
    if (!CompileDictionary(definition, schema_file, options))
      success = false;
  }
  return success;
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
  uint32_t dict_file_checksum =
      compute_dict_file_checksum(0, dict_files, settings);
  bool rebuild_table = false;
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
  Syllabary syllabary;
  if (rebuild_table) {
    if (!build_table_from_source) {
      LOG(ERROR) << "cannot rebuild dictionary '" << dict_name_
                 << "' without its source";
      return false;
    }
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
  if (prism_specs_.empty())
    prism_specs_.push_back({prism_, nullptr});
  for (const auto& spec : prism_specs_) {
    prism_ = spec.prism;
    boost::crc_32_type checksum;
    if (spec.algebra) {
      for (const auto& item : *spec.algebra) {
        auto rule = As<ConfigValue>(item);
        if (!rule) {
          LOG(ERROR) << "algebra rules must be strings";
          return false;
        }
        checksum.process_bytes(rule->str().c_str(), rule->str().size() + 1);
      }
    }
    const uint32_t algebra_checksum = checksum.checksum();
    bool rebuild_prism = true;
    if (prism_->Exists() && prism_->Load()) {
      rebuild_prism = prism_->dict_file_checksum() != dict_file_checksum ||
                      prism_->schema_file_checksum() != algebra_checksum;
      prism_->Close();
    }
    if ((rebuild_prism || (options_ & kRebuildPrism)) &&
        !BuildPrism(spec.algebra, dict_file_checksum, algebra_checksum))
      return false;
  }
  for (int table_index = 1; table_index < tables_.size(); ++table_index) {
    const auto& pack_name = packs_[table_index - 1];
    auto pack_table = tables_[table_index];
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
    pack_table->Close();
    EntryCollector collector(std::move(syllabary));
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

  if (!schema.IsNull("build/auxiliary") && !schema.GetMap("build/auxiliary")) {
    LOG(ERROR) << "build/auxiliary must be a map";
    return false;
  }
  vector<pair<string, an<ConfigList>>> groups;
  if (auto rules = schema.GetList("build/auxiliary/algebra")) {
    groups.emplace_back("auto", rules);
  } else if (auto named = schema.GetMap("build/auxiliary/algebra")) {
    for (const auto& item : *named) {
      auto rules = As<ConfigList>(item.second);
      if (!rules) {
        LOG(ERROR) << "build/auxiliary/algebra group '" << item.first
                   << "' is not a list";
        return false;
      }
      groups.emplace_back(item.first, rules);
    }
  } else if (!schema.IsNull("build/auxiliary/algebra")) {
    LOG(ERROR) << "build/auxiliary/algebra must be a list or a map of lists";
    return false;
  }
  if (groups.empty())
    return true;

  string source_name;
  if (!schema.GetString("build/auxiliary/dictionary", &source_name) ||
      source_name.empty()) {
    LOG(ERROR)
        << "build/auxiliary/dictionary is required for auxiliary algebra";
    return false;
  }
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
        LOG(ERROR) << "build/auxiliary/algebra group '" << group.first
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

bool DictCompiler::BuildPrism(const an<ConfigList>& algebra,
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
  Script script;
  if (algebra) {
    for (const auto& item : *algebra) {
      auto formula = As<ConfigValue>(item);
      if (!formula)
        return false;
      const string& text = formula->str();
      if (text.size() > 3 && text.compare(0, 3, "aux") == 0 &&
          !std::isalpha(static_cast<unsigned char>(text[3]))) {
        LOG(ERROR) << "dictionary algebra cannot contain AUX rule: " << text;
        return false;
      }
    }
    Projection projection;
    if (!projection.Load(algebra)) {
      LOG(ERROR) << "failed to load spelling algebra";
      return false;
    }
    for (const auto& syllable : syllabary)
      script.AddSyllable(syllable);
    projection.Apply(&script);
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
