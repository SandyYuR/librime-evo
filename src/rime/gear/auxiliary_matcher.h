//
// Copyright RIME Developers
// Distributed under the BSD License
//
#ifndef RIME_AUXILIARY_MATCHER_H_
#define RIME_AUXILIARY_MATCHER_H_

#include <rime/common.h>
#include <rime/algo/syllabifier.h>

namespace rime {

class Dictionary;
class AuxiliaryDb;

// Shared AUX lookup and matching logic for direct and indirect AUX input.
class AuxiliaryMatcher {
 public:
  AuxiliaryMatcher(Dictionary* primary, AuxiliaryDb* database = nullptr);

  void Lookup(const string& input, vector<AuxGraphMatch>* result);
  bool LookupAuxiliary(const string& text, vector<string>* result);
  bool Match(const string& text, const string& input);

 private:
  bool MatchSequence(const vector<vector<string>>& codes,
                     size_t index,
                     const string& input,
                     size_t input_index,
                     map<pair<size_t, size_t>, bool>* memo) const;

  Dictionary* primary_ = nullptr;
  AuxiliaryDb* database_ = nullptr;
  map<SyllableId, std::set<string>> characters_by_syllable_;
  map<string, vector<string>> auxiliary_cache_;
};

}  // namespace rime

#endif  // RIME_AUXILIARY_MATCHER_H_
