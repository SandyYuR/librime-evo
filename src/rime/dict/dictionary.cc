//
// Copyright RIME Developers
// Distributed under the BSD License
//
// 2011-07-05 GONG Chen <chen.sst@gmail.com>
//
#include <filesystem>
#include <rime/algo/syllabifier.h>
#include <rime/common.h>
#include <rime/dict/dictionary.h>
#include <rime/resource.h>
#include <rime/schema.h>
#include <rime/service.h>
#include <rime/ticket.h>
#include <utf8.h>
#include <utility>
#include <tuple>

namespace rime {

namespace dictionary {

struct Chunk {
  Table* table = nullptr;
  Code code;
  const table::Entry* entries = nullptr;
  size_t size = 0;
  size_t cursor = 0;
  string remaining_code;  // for predictive queries
  size_t matching_code_size = 0;
  double credibility = 0.0;
  double quality_len = 0.0;
  bool abbreviated_auxiliary = false;
  std::shared_ptr<const AuxiliaryPath> auxiliary_path;

  Chunk() = default;
  Chunk(Table* t,
        const Code& c,
        const table::Entry* e,
        size_t m,
        double cr = 0.0,
        double q = 0.0,
        bool abbreviated = false)
      : table(t),
        code(c),
        entries(e),
        size(1),
        cursor(0),
        matching_code_size(m),
        credibility(cr),
        quality_len(q),
        abbreviated_auxiliary(abbreviated) {}
  Chunk(Table* t, const TableAccessor& a, double cr = 0.0, double q = 0.0)
      : Chunk(t, a, string(), cr, q) {}
  Chunk(Table* t,
        const TableAccessor& a,
        const string& r,
        double cr = 0.0,
        double q = 0.0)
      : table(t),
        code(a.index_code()),
        entries(a.entry()),
        size(a.remaining()),
        cursor(0),
        remaining_code(r),
        matching_code_size(a.index_code().size()),
        credibility(cr),
        quality_len(q),
        abbreviated_auxiliary(a.abbreviated_auxiliary()),
        auxiliary_path(a.auxiliary_path()) {}

  void SkipNonMatchingAuxiliary() {
    if (!auxiliary_path)
      return;
    while (cursor < size &&
           !auxiliary_path->Matches(table->GetEntryText(entries[cursor])))
      ++cursor;
  }

  bool is_exact_match() const { return matching_code_size == code.size(); }

  bool is_predictive_match() const { return matching_code_size < code.size(); }
};

struct QueryResult {
  vector<Chunk> chunks;
};

bool compare_chunk_by_head_element(const Chunk& a, const Chunk& b) {
  if (!a.entries || a.cursor >= a.size)
    return false;
  if (!b.entries || b.cursor >= b.size)
    return true;
  if (a.is_exact_match() != b.is_exact_match())
    return a.is_exact_match() > b.is_exact_match();
  if (a.remaining_code.length() != b.remaining_code.length())
    return a.remaining_code.length() < b.remaining_code.length();
  return a.credibility + a.entries[a.cursor].weight >
         b.credibility + b.entries[b.cursor].weight;  // by weight desc
}

struct CodeMatch {
  bool success;
  size_t depth;
  size_t end_pos;
};

CodeMatch match_extra_code(const table::Code* extra_code,
                           size_t depth,
                           const SyllableGraph& syll_graph,
                           size_t current_pos,
                           bool predict_word) {
  const CodeMatch kFailed{false, 0, 0};
  if (!extra_code || depth >= extra_code->size)
    return {true, depth, current_pos};
  if (current_pos >= syll_graph.interpreted_length) {
    if (predict_word)
      return {true, depth, syll_graph.interpreted_length};
    else
      return kFailed;
  }
  auto index = syll_graph.indices.find(current_pos);
  if (index == syll_graph.indices.end())
    return kFailed;
  SyllableId current_syll_id = extra_code->at[depth];
  auto spellings = index->second.find(current_syll_id);
  if (spellings == index->second.end())
    return kFailed;
  CodeMatch best_match = kFailed;
  for (const SpellingProperties* props : spellings->second) {
    CodeMatch match = match_extra_code(extra_code, depth + 1, syll_graph,
                                       props->end_pos, predict_word);
    if (!match.success)
      continue;
    if (match.end_pos > best_match.end_pos)
      best_match = match;
  }
  return best_match;
}

struct AuxiliaryCodeMatch {
  size_t depth;
  size_t end_pos;
  double credibility;
  bool abbreviated;
};

// Tail indices store four-syllable and longer entries outside the table trie.
// Match each tail against its own text, preserving the chosen AUX path's score.
void collect_extra_code_matches(
    const table::Code* extra_code,
    size_t depth,
    const SyllableGraph& graph,
    size_t pos,
    bool predict_word,
    const vector<string>& characters,
    size_t prefix_depth,
    double credibility,
    bool abbreviated,
    map<std::tuple<size_t, size_t, bool>, AuxiliaryCodeMatch>* matches,
    map<std::tuple<size_t, size_t, bool>, double>* seen) {
  auto state = std::make_tuple(depth, pos, abbreviated);
  auto best = seen->find(state);
  if (best != seen->end() && best->second >= credibility)
    return;
  (*seen)[state] = credibility;
  if (depth >= extra_code->size ||
      (predict_word && pos >= graph.interpreted_length)) {
    (*matches)[state] = {depth, pos, credibility, abbreviated};
    return;
  }
  auto index = graph.indices.find(pos);
  if (index == graph.indices.end())
    return;
  auto found = index->second.find(extra_code->at[depth]);
  if (found == index->second.end())
    return;
  for (const auto* props : found->second) {
    if (props->auxiliary_chars &&
        (prefix_depth + depth >= characters.size() ||
         !props->auxiliary_chars->count(characters[prefix_depth + depth])))
      continue;
    collect_extra_code_matches(
        extra_code, depth + 1, graph, props->end_pos, predict_word, characters,
        prefix_depth, credibility + props->credibility,
        abbreviated || (props->auxiliary_chars && props->type == kAbbreviation),
        matches, seen);
  }
}

}  // namespace dictionary

DictEntryIterator::DictEntryIterator()
    : query_result_(New<dictionary::QueryResult>()) {}

void DictEntryIterator::AddChunk(dictionary::Chunk&& chunk) {
  chunk.SkipNonMatchingAuxiliary();
  if (chunk.cursor >= chunk.size)
    return;
  has_abbreviated_auxiliary_ |= chunk.abbreviated_auxiliary;
  query_result_->chunks.push_back(std::move(chunk));
  entry_count_ += chunk.size;
}

void DictEntryIterator::Sort() {
  if (ordered_mode_ || exhausted())
    return;
  auto& chunks = query_result_->chunks;
  // partial-sort remaining chunks, move best match to chunk_index_
  std::partial_sort(chunks.begin() + chunk_index_,
                    chunks.begin() + chunk_index_ + 1, chunks.end(),
                    dictionary::compare_chunk_by_head_element);
}

std::map<Code, double> DictEntryIterator::TopWeightsByCode(
    bool separate_readings) const {
  std::map<Code, double> result;
  constexpr double kLogScale = 18.420680743952367;
  if (ordered_mode_) {
    for (size_t index = ordered_index_; index < ordered_entries_.size();
         ++index) {
      const auto& entry = ordered_entries_[index];
      if (!entry || !entry->IsExactMatch() || entry->remaining_code_length != 0)
        continue;
      const Code key = separate_readings ? entry->code : Code{};
      auto it = result.find(key);
      if (it == result.end())
        result[key] = entry->weight;
      else
        it->second = (std::max)(it->second, entry->weight);
    }
    return result;
  }
  const auto& chunks = query_result_->chunks;
  for (size_t index = chunk_index_; index < chunks.size(); ++index) {
    const auto& c = chunks[index];
    if (!c.entries || c.cursor >= c.size || !c.is_exact_match() ||
        !c.remaining_code.empty())
      continue;
    const Code key = separate_readings ? c.code : Code{};
    const double value = c.entries[c.cursor].weight - kLogScale + c.credibility;
    auto it = result.find(key);
    if (it == result.end())
      result[key] = value;
    else
      it->second = (std::max)(it->second, value);
  }
  return result;
}

DictEntryIterator DictEntryIterator::Fork() const {
  DictEntryIterator copy(*this);
  // The implicit copy shares QueryResult and would mutate live chunk cursors.
  copy.query_result_ = New<dictionary::QueryResult>(*query_result_);
  copy.entry_.reset();
  return copy;
}

namespace {

size_t Utf8Length(const string& text) {
  size_t length = 0;
  for (auto p = text.begin(); p != text.end();) {
    utf8::next(p, text.end());
    ++length;
  }
  return length;
}

}  // namespace

void DictEntryIterator::ApplyAuxiliaryOrdering() {
  if (!has_abbreviated_auxiliary_ || ordered_mode_)
    return;

  DictEntryIterator snapshot = Fork();
  snapshot.filter_ = nullptr;
  vector<an<DictEntry>> entries;
  entries.reserve(entry_count_);
  while (!snapshot.exhausted()) {
    snapshot.entry_.reset();
    entries.push_back(snapshot.Peek());
    if (!snapshot.FindNextEntry())
      break;
  }

  vector<an<DictEntry>> ordered;
  ordered.reserve(entries.size());
  for (size_t begin = 0; begin < entries.size();) {
    const bool exact = entries[begin]->IsExactMatch();
    const int remaining = entries[begin]->remaining_code_length;
    size_t end = begin + 1;
    while (end < entries.size() && entries[end]->IsExactMatch() == exact &&
           entries[end]->remaining_code_length == remaining) {
      ++end;
    }

    vector<an<DictEntry>> ordinary;
    vector<an<DictEntry>> auxiliary;
    for (size_t i = begin; i < end; ++i) {
      if (entries[i]->abbreviated_auxiliary)
        auxiliary.push_back(entries[i]);
      else
        ordinary.push_back(entries[i]);
    }
    vector<size_t> auxiliary_lengths;
    auxiliary_lengths.reserve(auxiliary.size());
    for (const auto& entry : auxiliary)
      auxiliary_lengths.push_back(Utf8Length(entry->text));
    vector<bool> emitted(auxiliary.size(), false);
    for (const auto& entry : ordinary) {
      const size_t text_length = Utf8Length(entry->text);
      for (size_t i = 0; i < auxiliary.size(); ++i) {
        if (!emitted[i] && auxiliary_lengths[i] == text_length) {
          ordered.push_back(auxiliary[i]);
          emitted[i] = true;
        }
      }
      ordered.push_back(entry);
    }
    for (size_t i = 0; i < auxiliary.size(); ++i) {
      if (!emitted[i])
        ordered.push_back(auxiliary[i]);
    }
    begin = end;
  }

  ordered_entries_ = std::move(ordered);
  ordered_index_ = 0;
  ordered_mode_ = true;
  entry_count_ = ordered_entries_.size();
  entry_.reset();
}

void DictEntryIterator::AddFilter(DictEntryFilter filter) {
  DictEntryFilterBinder::AddFilter(filter);
  // the introduced filter could invalidate the current or even all the
  // remaining entries
  while (!exhausted() && !filter_(Peek())) {
    entry_.reset();
    FindNextEntry();
  }
}

an<DictEntry> DictEntryIterator::Peek() {
  if (ordered_mode_)
    return ordered_index_ < ordered_entries_.size()
               ? ordered_entries_[ordered_index_]
               : nullptr;
  if (!entry_ && !exhausted()) {
    // get next entry from current chunk
    const auto& chunk = query_result_->chunks[chunk_index_];
    const auto& e = chunk.entries[chunk.cursor];
    DLOG(INFO) << "creating temporary dict entry '"
               << chunk.table->GetEntryText(e) << "'.";
    entry_ = New<DictEntry>();
    entry_->code = chunk.code;
    entry_->text = chunk.table->GetEntryText(e);
    const double kS = 18.420680743952367;  // log(1e8)
    entry_->weight = e.weight - kS + chunk.credibility;
    entry_->quality_len = chunk.quality_len;
    entry_->abbreviated_auxiliary = chunk.abbreviated_auxiliary;
    if (!chunk.remaining_code.empty()) {
      entry_->comment = "~" + chunk.remaining_code;
      entry_->remaining_code_length = chunk.remaining_code.length();
    }
    if (chunk.is_predictive_match()) {
      entry_->matching_code_size = chunk.matching_code_size;
    }
  }
  return entry_;
}

bool DictEntryIterator::FindNextEntry() {
  if (ordered_mode_) {
    if (ordered_index_ >= ordered_entries_.size())
      return false;
    ++ordered_index_;
    return !exhausted();
  }
  if (exhausted()) {
    return false;
  }
  auto& chunk = query_result_->chunks[chunk_index_];
  ++chunk.cursor;
  chunk.SkipNonMatchingAuxiliary();
  if (chunk.cursor >= chunk.size) {
    ++chunk_index_;
  }
  if (exhausted()) {
    return false;
  }
  // reorder chunks to move the one with the best entry to head
  Sort();
  return true;
}

bool DictEntryIterator::Next() {
  do {
    entry_.reset();
    if (!FindNextEntry()) {
      return false;
    }
  } while (filter_ && !filter_(Peek()));
  return true;
}

// Note: does not apply filters
bool DictEntryIterator::Skip(size_t num_entries) {
  if (ordered_mode_) {
    if (ordered_index_ + num_entries > ordered_entries_.size()) {
      ordered_index_ = ordered_entries_.size();
      return false;
    }
    ordered_index_ += num_entries;
    entry_.reset();
    return true;
  }
  while (num_entries > 0) {
    if (exhausted())
      return false;
    auto& chunk = query_result_->chunks[chunk_index_];
    if (chunk.cursor + num_entries < chunk.size) {
      chunk.cursor += num_entries;
      return true;
    }
    num_entries -= (chunk.size - chunk.cursor);
    ++chunk_index_;
  }
  return true;
}

bool DictEntryIterator::exhausted() const {
  if (ordered_mode_)
    return ordered_index_ >= ordered_entries_.size();
  return chunk_index_ >= query_result_->chunks.size();
}

// Dictionary members

Dictionary::Dictionary(string name,
                       vector<string> packs,
                       vector<of<Table>> tables,
                       an<Prism> prism)
    : name_(name),
      packs_(std::move(packs)),
      tables_(std::move(tables)),
      prism_(std::move(prism)) {}

Dictionary::~Dictionary() {
  // should not close shared table and prism objects
}

bool Dictionary::AddPrism(const string& name, const an<Prism>& prism) {
  if (name.empty() || !prism)
    return false;
  prisms_[name] = prism;
  return true;
}

bool Dictionary::SetPrism(const string& name) {
  auto it = prisms_.find(name);
  if (it == prisms_.end() || !it->second)
    return false;
  prism_ = it->second;
  return true;
}

static void lookup_table(Table* table,
                         DictEntryCollector* collector,
                         const SyllableGraph& syllable_graph,
                         size_t start_pos,
                         bool predict_word,
                         double initial_credibility) {
  TableQueryResult result;
  if (!table->Query(syllable_graph, start_pos, &result)) {
    return;
  }
  // copy result
  for (auto& v : result) {
    size_t end_pos = v.first;
    for (TableAccessor& a : v.second) {
      double cr = initial_credibility + a.credibility();
      double q = a.quality_len();
      if (a.extra_code()) {
        do {
          if (!syllable_graph.has_auxiliary) {
            dictionary::CodeMatch match = dictionary::match_extra_code(
                a.extra_code(), 0, syllable_graph, end_pos, predict_word);
            if (!match.success)
              continue;
            size_t matching_code_size = a.index_code().size() + match.depth;
            (*collector)[match.end_pos].AddChunk({table, a.code(), a.entry(),
                                                  matching_code_size, cr, 0.0,
                                                  a.abbreviated_auxiliary()});
            continue;
          }
          const Code code = a.code();
          const string text = table->GetEntryText(*a.entry());
          if (a.auxiliary_path() && !a.auxiliary_path()->Matches(text))
            continue;
          vector<string> characters;
          auto p = text.begin();
          while (p != text.end()) {
            auto begin = p;
            utf8::next(p, text.end());
            characters.emplace_back(begin, p);
          }
          map<std::tuple<size_t, size_t, bool>, dictionary::AuxiliaryCodeMatch>
              matches;
          map<std::tuple<size_t, size_t, bool>, double> seen;
          dictionary::collect_extra_code_matches(
              a.extra_code(), 0, syllable_graph, end_pos, predict_word,
              characters, a.index_code().size(), cr, a.abbreviated_auxiliary(),
              &matches, &seen);
          for (const auto& item : matches) {
            const auto& match = item.second;
            const size_t matching = a.index_code().size() + match.depth;
            (*collector)[match.end_pos].AddChunk({table, code, a.entry(),
                                                  matching, match.credibility,
                                                  0.0, match.abbreviated});
          }
        } while (a.Next());
      } else {
        (*collector)[end_pos].AddChunk({table, a, cr, q});
      }
    }
  }
}

an<DictEntryCollector> Dictionary::Lookup(const SyllableGraph& syllable_graph,
                                          size_t start_pos,
                                          const hash_set<string>* blacklist,
                                          bool predict_word,
                                          double initial_credibility) {
  if (!loaded())
    return nullptr;
  auto collector = New<DictEntryCollector>();
  for (const auto& table : tables_) {
    if (!table->IsOpen())
      continue;
    lookup_table(table.get(), collector.get(), syllable_graph, start_pos,
                 predict_word, initial_credibility);
  }
  if (collector->empty())
    return nullptr;
  // for each group of equal code length, sort it and filter words
  for (auto& v : *collector) {
    v.second.Sort();
    v.second.ApplyAuxiliaryOrdering();
    if (blacklist && !blacklist->empty()) {
      v.second.AddFilter([blacklist](an<DictEntry> entry) {
        return entry && !blacklist->count(entry->text);
      });
    }
  }
  return collector;
}

size_t Dictionary::LookupWords(DictEntryIterator* result,
                               const string& str_code,
                               bool predictive,
                               size_t expand_search_limit,
                               const hash_set<string>* blacklist) {
  DLOG(INFO) << "lookup: " << str_code;
  if (!loaded())
    return 0;
  vector<Prism::Match> keys;
  if (predictive) {
    prism_->ExpandSearch(str_code, &keys, expand_search_limit);
  } else {
    Prism::Match match{0, 0};
    if (prism_->GetValue(str_code, &match.value)) {
      keys.push_back(match);
    }
  }
  DLOG(INFO) << "found " << keys.size() << " matching keys thru the prism.";
  size_t code_length(str_code.length());
  for (auto& match : keys) {
    SpellingAccessor accessor(prism_->QuerySpelling(match.value));
    while (!accessor.exhausted()) {
      SyllableId syllable_id = accessor.syllable_id();
      SpellingType type = accessor.properties().type;
      accessor.Next();
      if (type > kNormalSpelling)
        continue;
      string remaining_code;
      if (match.length > code_length) {
        string syllable = primary_table()->GetSyllableById(syllable_id);
        if (syllable.length() > code_length)
          remaining_code = syllable.substr(code_length);
      }
      for (const auto& table : tables_) {
        if (!table->IsOpen())
          continue;
        TableAccessor a = table->QueryWords(syllable_id);
        if (!a.exhausted()) {
          DLOG(INFO) << "remaining code: " << remaining_code;
          result->AddChunk({table.get(), a, remaining_code});
        }
      }
    }
  }
  if (blacklist && !blacklist->empty()) {
    result->AddFilter([blacklist](an<DictEntry> entry) {
      return entry && !blacklist->count(entry->text);
    });
  }
  return keys.size();
}

bool Dictionary::Decode(const Code& code, vector<string>* result) {
  if (!result || tables_.empty())
    return false;
  result->clear();
  for (SyllableId c : code) {
    string s = primary_table()->GetSyllableById(c);
    if (s.empty())
      return false;
    result->push_back(s);
  }
  return true;
}

bool Dictionary::Exists() const {
  return std::filesystem::exists(prism_->file_path()) && !tables_.empty() &&
         std::filesystem::exists(tables_[0]->file_path());
}

bool Dictionary::Remove() {
  if (loaded())
    return false;
  prism_->Remove();
  for (const auto& table : tables_) {
    table->Remove();
  }
  return true;
}

bool Dictionary::Load() {
  LOG(INFO) << "loading dictionary '" << name_ << "'.";
  if (tables_.empty()) {
    LOG(ERROR) << "Cannot load dictionary '" << name_
               << "'; it contains no tables.";
    return false;
  }
  auto& primary_table = tables_[0];
  if (!primary_table || (!primary_table->IsOpen() && !primary_table->Load())) {
    LOG(ERROR) << "Error loading table for dictionary '" << name_ << "'.";
    return false;
  }
  if (!prism_ || (!prism_->IsOpen() && !prism_->Load())) {
    LOG(ERROR) << "Error loading prism for dictionary '" << name_ << "'.";
    return false;
  }
  // packs are optional
  for (int i = 1; i < tables_.size(); ++i) {
    const auto& table = tables_[i];
    if (!table->IsOpen() && table->Exists() && table->Load()) {
      LOG(INFO) << "loaded pack: " << packs_[i - 1];
    }
  }
  return true;
}

bool Dictionary::loaded() const {
  return !tables_.empty() && tables_[0]->IsOpen() && prism_ && prism_->IsOpen();
}

// DictionaryComponent members

static const ResourceType kPrismResourceType = {"prism", "", ".prism.bin"};

static const ResourceType kTableResourceType = {"table", "", ".table.bin"};

DictionaryComponent::DictionaryComponent()
    : prism_resource_resolver_(
          Service::instance().CreateDeployedResourceResolver(
              kPrismResourceType)),
      table_resource_resolver_(
          Service::instance().CreateDeployedResourceResolver(
              kTableResourceType)) {}

DictionaryComponent::~DictionaryComponent() {}

Dictionary* DictionaryComponent::Create(const Ticket& ticket) {
  if (!ticket.schema)
    return nullptr;
  Config* config = ticket.schema->config();
  string dict_name;
  if (!config->GetString(ticket.name_space + "/dictionary", &dict_name) &&
      !config->GetString("speller/dictionary", &dict_name)) {
    LOG(ERROR) << "dictionary not specified for " << ticket.name_space
               << " in schema '" << ticket.schema->schema_id() << "'.";
    return nullptr;
  }
  if (dict_name.empty()) {
    return nullptr;  // not requiring static dictionary
  }
  string prism_name;
  auto prism_list = config->GetList(ticket.name_space + "/prisms");
  if (!config->GetString(ticket.name_space + "/prism", &prism_name) &&
      prism_list) {
    for (const auto& item : *prism_list) {
      auto map = As<ConfigMap>(item);
      if (map && map->GetValue("name") &&
          map->GetValue("name")->GetString(&prism_name)) {
        break;
      }
    }
  }
  if (prism_name.empty()) {
    prism_name = dict_name;
  }
  vector<string> packs;
  if (auto pack_list = config->GetList(ticket.name_space + "/packs")) {
    for (const auto& item : *pack_list) {
      if (auto value = As<ConfigValue>(item)) {
        packs.push_back(value->str());
      }
    }
  }
  const string primary_prism_name = prism_name;
  auto dictionary =
      Create(std::move(dict_name), std::move(prism_name), std::move(packs));
  if (!dictionary || !prism_list)
    return dictionary;
  for (const auto& item : *prism_list) {
    auto map = As<ConfigMap>(item);
    string name;
    if (!map || !map->GetValue("name") ||
        !map->GetValue("name")->GetString(&name) || name.empty() ||
        name == primary_prism_name) {
      continue;
    }
    auto prism = prism_map_[name].lock();
    if (!prism) {
      auto file_path = prism_resource_resolver_->ResolvePath(name);
      prism_map_[name] = prism = New<Prism>(file_path);
    }
    dictionary->AddPrism(name, prism);
  }
  return dictionary;
}

Dictionary* DictionaryComponent::Create(string dict_name,
                                        string prism_name,
                                        vector<string> packs) {
  // obtain prism and primary table objects
  auto primary_table = table_map_[dict_name].lock();
  if (!primary_table) {
    auto file_path = table_resource_resolver_->ResolvePath(dict_name);
    table_map_[dict_name] = primary_table = New<Table>(file_path);
  }
  auto prism = prism_map_[prism_name].lock();
  if (!prism) {
    auto file_path = prism_resource_resolver_->ResolvePath(prism_name);
    prism_map_[prism_name] = prism = New<Prism>(file_path);
  }
  vector<of<Table>> tables = {std::move(primary_table)};
  for (const auto& pack : packs) {
    auto table = table_map_[pack].lock();
    if (!table) {
      auto file_path = table_resource_resolver_->ResolvePath(pack);
      table_map_[pack] = table = New<Table>(file_path);
    }
    tables.push_back(std::move(table));
  }
  auto dictionary = new Dictionary(std::move(dict_name), std::move(packs),
                                   std::move(tables), std::move(prism));
  dictionary->AddPrism(prism_name, dictionary->prism());
  return dictionary;
}

}  // namespace rime
