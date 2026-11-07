//
// Copyright RIME Developers
// Distributed under the BSD License
//
// 2011-08-07 GONG Chen <chen.sst@gmail.com>
//
#ifndef RIME_SCRIPT_TRANSLATOR_H_
#define RIME_SCRIPT_TRANSLATOR_H_

#include <rime/common.h>
#include <rime/translation.h>
#include <rime/translator.h>
#include <rime/algo/algebra.h>
#include <rime/gear/memory.h>
#include <rime/gear/translator_commons.h>

namespace rime {

class Code;
class Corrector;
class AuxiliaryMatcher;
class AuxiliaryDb;
struct DictEntry;
class Dictionary;
class Poet;
class UserDictionary;
struct SyllableGraph;

class ScriptTranslator : public Translator,
                         public Memory,
                         public TranslatorOptions {
 public:
  ScriptTranslator(const Ticket& ticket);
  ~ScriptTranslator() override;

  virtual an<Translation> Query(const string& input,
                                const Segment& segment) override;
  virtual bool Memorize(const CommitEntry& commit_entry) override;
  virtual bool ProcessSegmentOnCommit(CommitEntry& commit_entry,
                                      const Segment& seg) override;
  bool FlushPendingCommit(CommitEntry& commit_entry) override;

  string FormatPreedit(const string& preedit);
  string Spell(const Code& code);
  string GetPrecedingText(size_t start) const;
  bool UpdateElements(const CommitEntry& commit_entry);

  bool ConcatenatePhrases(CommitEntry& commit_entry,
                          const vector<an<Phrase>>& phrases);
  bool SaveCommitEntry(CommitEntry& commit_entry);
  bool SelectPrism();

  // options
  int max_homophones() const { return max_homophones_; }
  int spelling_hints() const { return spelling_hints_; }
  bool always_show_comments() const { return always_show_comments_; }
  bool enable_word_completion() const { return enable_word_completion_; }
  std::shared_ptr<AuxiliaryMatcher> auxiliary_matcher() const {
    return auxiliary_matcher_;
  }
  bool auxiliary_enabled() const;
  int max_word_length() const { return max_word_length_; }
  int core_word_length() const;
  Projection* canonicalizer() const { return canonicalizer_.get(); }

 protected:
  int max_homophones_ = 1;
  int spelling_hints_ = 0;
  int max_word_length_ = 0;
  int core_word_length_ = 0;
  bool always_show_comments_ = false;
  bool enable_correction_ = false;
  bool enable_word_completion_ = false;
  the<Projection> canonicalizer_;
  the<Corrector> corrector_;
  std::shared_ptr<AuxiliaryMatcher> auxiliary_matcher_;
  the<AuxiliaryDb> auxiliary_db_;
  string auxiliary_suffix_trigger_ = "`";
  string auxiliary_type_;
  bool auxiliary_mode_ = false;
  string auxiliary_option_;
  struct PrismSelection {
    string name;
    string option;
  };
  vector<PrismSelection> prism_selections_;
  string active_prism_;
  the<Poet> poet_;
  vector<an<Phrase>> queue_;
};

}  // namespace rime

#endif  // RIME_SCRIPT_TRANSLATOR_H_
