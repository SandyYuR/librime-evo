//
// Copyright RIME Developers
// Distributed under the BSD License
//
#include <rime/gear/auxiliary_matcher.h>
#include <rime/dict/dictionary.h>
#include <rime/dict/reverse_lookup_dictionary.h>
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

static vector<string> SplitCodes(const string& text) {
  vector<string> result;
  size_t start = 0;
  while (start < text.size()) {
    size_t end = text.find(' ', start);
    if (end == string::npos)
      end = text.size();
    if (end > start)
      result.emplace_back(text.substr(start, end - start));
    start = end + 1;
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

AuxiliaryMatcher::AuxiliaryMatcher(Dictionary* primary,
                                   ReverseLookupDictionary* reverse)
    : primary_(primary), reverse_(reverse) {
  if (!primary_ || !primary_->loaded())
    return;
  Syllabary syllables;
  if (primary_->primary_table()->GetSyllabary(&syllables)) {
    SyllableId id = 0;
    for (const auto& syllable : syllables)
      main_ids_[syllable] = id++;
  }
}

void AuxiliaryMatcher::Lookup(const string& input,
                              vector<AuxGraphMatch>* result) {
  if (!primary_ || !result || input.empty())
    return;
  vector<Prism::Match> matches;
  primary_->prism()->CommonPrefixSearch(input, &matches);
  for (const auto& match : matches) {
    for (auto accessor = primary_->prism()->QuerySpelling(match.value);
         !accessor.exhausted(); accessor.Next()) {
      if (!accessor.properties().auxiliary)
        continue;
      auto id = accessor.syllable_id();
      const auto properties = accessor.properties();
      const auto base_code = primary_->primary_table()->GetSyllableById(id);
      auto base = main_ids_.find(base_code);
      if (base == main_ids_.end())
        continue;
      const string& chars = accessor.properties().auxiliary_characters;
      if (chars.empty())
        continue;
      auto table_words = primary_->primary_table()->QueryWords(base->second);
      if (table_words.exhausted())
        continue;
      auto permitted = permitted_chars_.find(chars);
      if (permitted == permitted_chars_.end()) {
        std::set<string> allowed;
        for (auto p = chars.begin(); p != chars.end();) {
          auto begin = p;
          utf8::next(p, chars.end());
          allowed.emplace(begin, p);
        }
        permitted =
            permitted_chars_
                .emplace(chars, std::make_shared<const std::set<string>>(
                                    std::move(allowed)))
                .first;
      }
      if (!permitted->second->empty())
        result->push_back({match.length, base->second, permitted->second,
                           properties.type, properties.credibility});
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
  if (!reverse_)
    return false;
  string encoded;
  const bool found = reverse_->LookupAuxiliary(text, &encoded);
  if (found)
    *result = SplitCodes(encoded);
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
