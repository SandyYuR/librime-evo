//
// Copyright RIME Developers
// Distributed under the BSD License
//
#include <rime/gear/auxiliary_matcher.h>
#include <rime/dict/auxiliary_db.h>
#include <rime/dict/dictionary.h>
#include <utf8.h>

namespace rime {

namespace {

static vector<string> SplitCharacters(const string& text) {
  vector<string> result;
  for (auto p = text.begin(); p != text.end();) {
    auto begin = p;
    utf8::next(p, text.end());
    result.emplace_back(begin, p);
  }
  return result;
}

static bool MatchPrefix(const vector<string>& codes, const string& input) {
  for (const auto& code : codes) {
    if (code.compare(0, input.size(), input) == 0)
      return true;
  }
  return false;
}

}  // namespace

AuxiliaryMatcher::AuxiliaryMatcher(Dictionary* primary, AuxiliaryDb* database)
    : primary_(primary), database_(database) {}

void AuxiliaryMatcher::Lookup(const string& input,
                              vector<AuxGraphMatch>* result) {
  if (!primary_ || !primary_->loaded() || !database_ || !result ||
      input.empty())
    return;
  vector<Prism::Match> matches;
  primary_->prism()->CommonPrefixSearch(input, &matches);
  for (const auto& match : matches) {
    if (match.length == input.size())
      continue;
    vector<pair<size_t, vector<AuxiliaryCharacter>>> suffixes;
    database_->CommonPrefixLookup(input.substr(match.length), &suffixes);
    if (suffixes.empty())
      continue;
    for (auto accessor = primary_->prism()->QuerySpelling(match.value);
         !accessor.exhausted(); accessor.Next()) {
      const auto base = accessor.properties();
      if (base.type >= kAbbreviation || base.is_correction)
        continue;
      const auto id = accessor.syllable_id();
      auto cached = characters_by_syllable_.find(id);
      if (cached == characters_by_syllable_.end()) {
        std::set<string> characters;
        for (const auto& table : primary_->tables()) {
          if (!table->IsOpen())
            continue;
          for (auto words = table->QueryWords(id); !words.exhausted();
               words.Next()) {
            const string text = table->GetEntryText(*words.entry());
            if (!text.empty() && utf8::distance(text.begin(), text.end()) == 1)
              characters.insert(text);
          }
        }
        cached =
            characters_by_syllable_.emplace(id, std::move(characters)).first;
      }
      for (const auto& suffix : suffixes) {
        // Character membership and spelling metadata belong to the same edge.
        // Combining metadata across groups would promote abbreviated AUX hits.
        map<pair<SpellingType, double>, std::set<string>> groups;
        for (const auto& item : suffix.second) {
          if (!cached->second.count(item.text))
            continue;
          const auto type = (std::max)(base.type, item.type);
          groups[{type, base.credibility + item.credibility}].insert(item.text);
        }
        for (auto& group : groups) {
          result->push_back({match.length + suffix.first, id,
                             std::make_shared<const std::set<string>>(
                                 std::move(group.second)),
                             group.first.first, group.first.second});
        }
      }
    }
  }
}

bool AuxiliaryMatcher::LookupAuxiliary(const string& text,
                                       vector<string>* result) {
  if (!result || text.empty())
    return false;
  result->clear();
  auto cached = auxiliary_cache_.find(text);
  if (cached != auxiliary_cache_.end()) {
    *result = cached->second;
    return !result->empty();
  }
  if (!database_)
    return false;
  vector<AuxiliaryCode> codes;
  const bool found = database_->ReverseLookup(text, &codes);
  for (const auto& code : codes)
    result->push_back(code.code);
  auxiliary_cache_[text] = *result;
  return !result->empty();
}

bool AuxiliaryMatcher::MatchSequence(
    const vector<vector<string>>& codes,
    size_t index,
    const string& input,
    size_t input_index,
    map<pair<size_t, size_t>, bool>* memo) const {
  if (input_index >= input.size())
    return true;
  if (index >= codes.size())
    return false;
  auto key = std::make_pair(index, input_index);
  auto found = memo->find(key);
  if (found != memo->end())
    return found->second;

  for (const auto& code : codes[index]) {
    size_t next_input = input_index;
    size_t code_pos = 0;
    while (next_input < input.size() && code_pos < code.size()) {
      if (input[next_input] == code[code_pos])
        ++next_input;
      ++code_pos;
    }
    if (MatchSequence(codes, index + 1, input, next_input, memo))
      return (*memo)[key] = true;
  }
  // A suffix can target any character in a phrase.
  if (MatchSequence(codes, index + 1, input, input_index, memo))
    return (*memo)[key] = true;
  return (*memo)[key] = false;
}

bool AuxiliaryMatcher::Match(const string& text, const string& input) {
  if (text.empty() || input.empty())
    return false;
  vector<vector<string>> codes;
  for (const auto& character : SplitCharacters(text)) {
    vector<string> character_codes;
    if (LookupAuxiliary(character, &character_codes))
      codes.push_back(std::move(character_codes));
    else
      codes.emplace_back();
  }
  if (codes.size() == 1)
    return MatchPrefix(codes.front(), input);
  map<pair<size_t, size_t>, bool> memo;
  return MatchSequence(codes, 0, input, 0, &memo);
}

}  // namespace rime
