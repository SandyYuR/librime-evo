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
static ReverseLookupTable BuildAuxiliaryReverseLookup(
    const path& schema_file,
    const vector<path>& files,
    const string& translator_namespace,
    const string& auxiliary_type);

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

static bool get_auxiliary_file_from_schema(vector<path>* auxiliary_files,
                                           const path& schema_file,
                                           ResourceResolver* source_resolver,
                                           bool enabled) {
  if (schema_file.empty() || !enabled)
    return true;
  Config schema;
  if (!schema.LoadFromFile(schema_file)) {
    LOG(ERROR)
        << "failed to load schema while resolving speller/aux_dictionary.";
    return false;
  }
  string name;
  if (!schema.GetString("speller/aux_dictionary", &name) || name.empty())
    return true;

  if (!boost::iends_with(name, ".yaml"))
    name += ".yaml";
  auto file = source_resolver->ResolvePath(name);
  if (std::filesystem::exists(file)) {
    auxiliary_files->push_back(file);
    return true;
  }
  LOG(ERROR) << "auxiliary source '" << name << "' does not exist.";
  return false;
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
  vector<path> auxiliary_files;
  if (!get_auxiliary_file_from_schema(&auxiliary_files, schema_file,
                                      source_resolver_.get(),
                                      !auxiliary_type_.empty())) {
    return false;
  }
  const bool aux_active = !auxiliary_files.empty();
  ReverseLookupTable auxiliary_reverse;
  if (aux_active) {
    auxiliary_reverse = BuildAuxiliaryReverseLookup(
        schema_file, auxiliary_files, translator_namespace_, auxiliary_type_);
  }
  uint32_t schema_file_checksum =
      schema_file.empty() ? 0 : Checksum(schema_file);
  uint32_t dict_file_checksum =
      compute_dict_file_checksum(0, dict_files, settings);
  if (aux_active) {
    ChecksumComputer aux_checksum(dict_file_checksum);
    for (const auto& file : auxiliary_files)
      aux_checksum.ProcessFile(file);
    dict_file_checksum = aux_checksum.Checksum();
  }
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
    // A Prism from an older AUX format cannot describe the current metadata.
    if (build_table_from_source && aux_active &&
        !prism_->SupportsAuxiliaryCharacters()) {
      rebuild_prism = true;
    }
    prism_->Close();
  } else {
    rebuild_prism = true;
    // No compatible Prism means the companion AUX descriptors must be rebuilt.
  }
  // The AUX reverse projection is stored in the reverse database, so rebuild
  // the primary table along with the Prism when its schema changes.
  if (build_table_from_source && aux_active && rebuild_prism)
    rebuild_table = true;
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
    if (!BuildTable(0, collector, &settings, dict_files, dict_file_checksum,
                    &auxiliary_reverse)) {
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
      !BuildPrism(schema_file, dict_file_checksum, schema_file_checksum,
                  aux_active ? &dict_files : nullptr,
                  aux_active ? &auxiliary_files : nullptr)) {
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
                              uint32_t dict_file_checksum,
                              const ReverseLookupTable* auxiliary_reverse) {
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
      !BuildReverseDb(settings, collector, vocabulary, dict_file_checksum,
                      auxiliary_reverse)) {
    return false;
  }
  return true;
}

bool DictCompiler::BuildReverseDb(DictSettings* settings,
                                  const EntryCollector& collector,
                                  const Vocabulary& vocabulary,
                                  uint32_t dict_file_checksum,
                                  const ReverseLookupTable* auxiliary_reverse) {
  // build .reverse.bin
  auto target_path = target_resolver_->ResolvePath(dict_name_ + ".reverse.bin");
  ReverseDb reverse_db(target_path);
  if (!reverse_db.Build(settings, collector.syllabary, vocabulary,
                        collector.stems, dict_file_checksum,
                        auxiliary_reverse) ||
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

// Read the pure single-character pronunciations from the main dictionary.
// AUX is a separate imported table, so the main source must not carry a
// separator or any AUX suffix.
static ReverseLookupTable ReadPureCharacterSources(const vector<path>& files) {
  ReverseLookupTable characters;
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
      if (word.empty() || code.empty() || code.find(';') != string::npos)
        continue;
      auto pos = word.begin();
      utf8::next(pos, word.end());
      if (pos == word.end()) {
        characters[word].insert(code);
      }
    }
  }
  return characters;
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

static ReverseLookupTable BuildAuxiliaryReverseLookup(
    const path& schema_file,
    const vector<path>& files,
    const string& translator_namespace,
    const string& auxiliary_type) {
  ReverseLookupTable result;
  if (schema_file.empty() || files.empty())
    return result;

  Config schema;
  if (!schema.LoadFromFile(schema_file))
    return result;
  string aux_path = translator_namespace + "/aux_algebra";
  if (schema.IsNull(aux_path))
    aux_path = "speller/aux_algebra";
  an<ConfigList> aux_algebra;
  if (!get_speller_algebra(schema, aux_path, &aux_algebra, auxiliary_type) ||
      !aux_algebra)
    return result;
  Projection projection;
  if (!projection.Load(aux_algebra))
    return result;
  const auto sources = ReadAuxiliarySources(files);
  for (const auto& item : sources) {
    for (const auto& raw_code : item.second) {
      Script projected;
      projected.AddSyllable(raw_code);
      projection.Apply(&projected);
      for (const auto& spelling : projected) {
        if (!IsAsciiPrismKey(spelling.first))
          continue;
        for (const auto& variant : spelling.second) {
          if (variant.properties.auxiliary)
            result[item.first].insert(spelling.first);
        }
      }
    }
  }
  return result;
}

bool DictCompiler::BuildPrism(const path& schema_file,
                              uint32_t dict_file_checksum,
                              uint32_t schema_file_checksum,
                              const vector<path>* source_files,
                              const vector<path>* auxiliary_files) {
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
  AuxiliaryCharacterMap auxiliary_characters;
  size_t imported_aux_entries = 0;
  bool aux_enabled = false;
  if (!schema_file.empty()) {
    Config config;
    if (!config.LoadFromFile(schema_file)) {
      LOG(ERROR) << "error loading prism definition from " << schema_file;
      return false;
    }
    Projection p;
    Projection aux_projection;
    an<ConfigList> algebra;
    string algebra_path = translator_namespace_ + "/algebra";
    if (config.IsNull(algebra_path))
      algebra_path = "speller/algebra";
    if (!get_speller_algebra(config, algebra_path, &algebra, input_type_))
      return false;
    an<ConfigList> aux_algebra;
    if (auxiliary_files) {
      string aux_path = translator_namespace_ + "/aux_algebra";
      if (config.IsNull(aux_path))
        aux_path = "speller/aux_algebra";
      if (!get_speller_algebra(config, aux_path, &aux_algebra, auxiliary_type_))
        return false;
      aux_enabled = static_cast<bool>(aux_algebra);
    }
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
    const bool aux_loaded = !aux_algebra || aux_projection.Load(aux_algebra);
    if (!ordinary_loaded || !aux_loaded) {
      LOG(ERROR) << "failed to load spelling algebra";
      return false;
    }
    for (const auto& x : syllabary) {
      script.AddSyllable(x);
    }
    if (ordinary_loaded)
      p.Apply(&script);
    if (source_files && auxiliary_files && aux_algebra && ordinary_loaded) {
      const auto source_characters = ReadPureCharacterSources(*source_files);
      const auto auxiliary_characters_by_text =
          ReadAuxiliarySources(*auxiliary_files);
      for (const auto& item : auxiliary_characters_by_text) {
        set<string> projected_aux_codes;
        map<string, SpellingProperties> projected_aux_properties;
        for (const auto& raw_aux_code : item.second) {
          Script projected_aux;
          projected_aux.AddSyllable(raw_aux_code);
          aux_projection.Apply(&projected_aux);
          for (const auto& aux_item : projected_aux) {
            for (const auto& spelling : aux_item.second) {
              if (spelling.properties.auxiliary &&
                  IsAsciiPrismKey(aux_item.first)) {
                projected_aux_codes.insert(aux_item.first);
                auto it = projected_aux_properties.find(aux_item.first);
                if (it == projected_aux_properties.end() ||
                    spelling.properties.type < it->second.type ||
                    (spelling.properties.type == it->second.type &&
                     spelling.properties.credibility >
                         it->second.credibility)) {
                  projected_aux_properties[aux_item.first] =
                      spelling.properties;
                }
              }
            }
          }
        }
        if (projected_aux_codes.empty())
          continue;
        const auto source_it = source_characters.find(item.first);
        if (source_it == source_characters.end())
          continue;
        for (const auto& base : source_it->second) {
          if (syllabary.find(base) == syllabary.end())
            continue;
          Script projected_base;
          projected_base.AddSyllable(base);
          p.Apply(&projected_base);
          for (const auto& base_item : projected_base) {
            bool normal_projection = false;
            for (const auto& normal : base_item.second) {
              if (!normal.properties.auxiliary &&
                  normal.properties.type == kNormalSpelling) {
                normal_projection = true;
                break;
              }
            }
            if (!normal_projection || !IsAsciiPrismKey(base_item.first))
              continue;
            for (const auto& aux_code : projected_aux_codes) {
              const string aux_key = base_item.first + aux_code;
              Spelling remapped(base);
              const auto aux_properties =
                  projected_aux_properties.find(aux_code);
              if (aux_properties != projected_aux_properties.end())
                remapped.properties = aux_properties->second;
              remapped.properties.auxiliary = true;
              script.Merge(aux_key, SpellingProperties(), {remapped});
              ++imported_aux_entries;
              auto& allowed = auxiliary_characters[{aux_key, base}];
              allowed.insert(item.first);
            }
          }
        }
      }
    }

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
  if (auxiliary_files && aux_enabled) {
    size_t accepted = 0;
    for (const auto& item : auxiliary_characters)
      accepted += item.second.size();
    if (imported_aux_entries && !accepted)
      LOG(ERROR) << "no AUX matches mapped to the phonetic syllabary";
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
                       dict_file_checksum, schema_file_checksum,
                       aux_enabled ? &auxiliary_characters : nullptr) ||
        !prism_->Save()) {
      return false;
    }
  }

  return true;
}

}  // namespace rime
