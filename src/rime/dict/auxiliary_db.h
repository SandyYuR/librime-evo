// Copyright RIME Developers
// Distributed under the BSD License
//
#ifndef RIME_AUXILIARY_DB_H_
#define RIME_AUXILIARY_DB_H_

#include <rime/algo/spelling.h>
#include <rime/common.h>
#include <rime/dict/mapped_file.h>
#include <rime/dict/string_table.h>

namespace rime {

struct AuxiliaryCode {
  string code;
  SpellingType type = kNormalSpelling;
  double credibility = 0.0;
};

struct AuxiliaryCharacter {
  string text;
  SpellingType type = kNormalSpelling;
  double credibility = 0.0;
};

class AuxiliaryDb : public MappedFile {
 public:
  explicit AuxiliaryDb(const path& file_path);

  bool Load();
  bool Build(const map<string, vector<AuxiliaryCode>>& by_character,
             const map<string, vector<AuxiliaryCharacter>>& by_code,
             uint32_t checksum);
  bool Save();

  bool Lookup(const string& code, vector<AuxiliaryCharacter>* result) const;
  void CommonPrefixLookup(
      const string& input,
      vector<pair<size_t, vector<AuxiliaryCharacter>>>* result) const;
  bool ReverseLookup(const string& text, vector<AuxiliaryCode>* result) const;
  uint32_t checksum() const;

 private:
  struct Metadata;

  Metadata* metadata_ = nullptr;
  the<StringTable> character_keys_;
  the<StringTable> character_values_;
  the<StringTable> code_keys_;
  the<StringTable> code_values_;
};

}  // namespace rime

#endif  // RIME_AUXILIARY_DB_H_
