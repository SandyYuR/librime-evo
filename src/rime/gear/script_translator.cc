//
// Copyright RIME Developers
// Distributed under the BSD License
//
// Script translator
//
// 2011-07-10 GONG Chen <chen.sst@gmail.com>
//
#include <algorithm>
#include <stack>
#include <cmath>
#include <boost/algorithm/string/join.hpp>
#include <boost/range/adaptor/reversed.hpp>
#include <rime/common.h>
#include <rime/composition.h>
#include <rime/candidate.h>
#include <rime/config.h>
#include <rime/context.h>
#include <rime/engine.h>
#include <rime/language.h>
#include <rime/schema.h>
#include <rime/translation.h>
#include <rime/algo/syllabifier.h>
#include <utf8.h>
#include <rime/dict/corrector.h>
#include <rime/dict/dictionary.h>
#include <rime/dict/reverse_lookup_dictionary.h>
#include <rime/dict/user_dictionary.h>
#include <rime/gear/poet.h>
#include <rime/gear/auxiliary_matcher.h>
#include <rime/gear/script_translator.h>
#include <rime/gear/translator_commons.h>
#include <rime/gear/user_rank.h>

namespace rime {

// static const char* quote_left = "\xef\xbc\x88";
// static const char* quote_right = "\xef\xbc\x89";

namespace {

struct SyllabifyTask {
  const Code& code;
  const SyllableGraph& graph;
  size_t target_pos;
  function<void(SyllabifyTask* task,
                size_t depth,
                size_t current_pos,
                size_t next_pos)>
      push;
  function<void(SyllabifyTask* task, size_t depth)> pop;
  vector<string> characters;
};

static vector<string> SplitCharacters(const string& text) {
  vector<string> result;
  for (auto p = text.begin(); p != text.end();) {
    const auto begin = p;
    utf8::next(p, text.end());
    result.emplace_back(begin, p);
  }
  return result;
}

static bool syllabify_dfs(SyllabifyTask* task,
                          size_t depth,
                          size_t current_pos) {
  if (depth == task->code.size()) {
    return current_pos == task->target_pos;
  }
  SyllableId syllable_id = task->code.at(depth);
  auto z = task->graph.edges.find(current_pos);
  if (z == task->graph.edges.end())
    return false;
  // favor longer spellings
  for (const auto& y : boost::adaptors::reverse(z->second)) {
    size_t end_vertex_pos = y.first;
    if (end_vertex_pos > task->target_pos)
      continue;
    auto x = y.second.find(syllable_id);
    if (x != y.second.end()) {
      const auto& props = x->second;
      if (!props.has_normal &&
          (depth >= task->characters.size() ||
           (!props.auxiliary_chars ||
            !props.auxiliary_chars->count(task->characters[depth]))))
        continue;
      task->push(task, depth, current_pos, end_vertex_pos);
      if (syllabify_dfs(task, depth + 1, end_vertex_pos))
        return true;
      task->pop(task, depth);
    }
  }
  return false;
}

}  // anonymous namespace

class ScriptSyllabifier : public PhraseSyllabifier {
 public:
  ScriptSyllabifier(ScriptTranslator* translator,
                    Corrector* corrector,
                    const string& input,
                    size_t start,
                    std::shared_ptr<AuxiliaryMatcher> auxiliary)
      : translator_(translator),
        input_(input),
        start_(start),
        syllabifier_(translator->delimiters(),
                     translator->enable_completion(),
                     translator->strict_spelling(),
                     translator->canonicalizer()),
        auxiliary_active_(bool(auxiliary)) {
    if (corrector) {
      syllabifier_.EnableCorrection(corrector);
    }
    if (auxiliary) {
      syllabifier_.SetAuxiliaryLookup(
          [auxiliary](const string& input, vector<AuxGraphMatch>* out) {
            auxiliary->Lookup(input, out);
          });
    }
  }

  virtual Spans Syllabify(const Phrase* phrase);
  size_t BuildSyllableGraph(Prism& prism);
  string GetPreeditString(const Phrase& cand) const;
  string GetOriginalSpelling(const Phrase& cand) const;
  bool IsCorrection(const Code& code, size_t code_length) const;

  const SyllableGraph& syllable_graph() const { return syllable_graph_; }
  bool auxiliary_active() const { return auxiliary_active_; }

 protected:
  ScriptTranslator* translator_;
  string input_;
  size_t start_;
  Syllabifier syllabifier_;
  SyllableGraph syllable_graph_;
  bool auxiliary_active_ = false;
};

class ScriptTranslation : public Translation {
 public:
  ScriptTranslation(ScriptTranslator* translator,
                    Corrector* corrector,
                    Poet* poet,
                    const string& input,
                    size_t start,
                    size_t end_of_input,
                    int max_sentences,
                    double sentence_cutoff_threshold,
                    const string& auxiliary_suffix)
      : translator_(translator),
        poet_(poet),
        start_(start),
        end_of_input_(end_of_input),
        syllabifier_(New<ScriptSyllabifier>(
            translator,
            corrector,
            input,
            start,
            translator->auxiliary_enabled() ? translator->auxiliary_matcher()
                                            : nullptr)),
        enable_correction_(corrector),
        max_sentences_(max_sentences),
        sentence_cutoff_threshold_(sentence_cutoff_threshold),
        auxiliary_suffix_(auxiliary_suffix) {
    set_exhausted(true);
  }
  bool Evaluate(Dictionary* dict, UserDictionary* user_dict);
  virtual bool Next();
  virtual an<Candidate> Peek();

 protected:
  bool CheckEmpty();
  bool IsNormalSpelling() const;
  bool PrepareCandidate();
  template <class QueryResult>
  void EnrollEntries(map<int, DictEntryList>& entries_by_end_pos,
                     const an<QueryResult>& query_result,
                     size_t start_pos);
  WordGraph PrepareForMakingSentence(Dictionary* dict,
                                     UserDictionary* user_dict);
  an<Sentence> MakeSentence(Dictionary* dict, UserDictionary* user_dict);
  deque<an<Sentence>> MakeSentences(Dictionary* dict,
                                    UserDictionary* user_dict);

  ScriptTranslator* translator_;
  Poet* poet_;
  size_t start_;
  size_t end_of_input_;
  an<ScriptSyllabifier> syllabifier_;

  an<DictEntryCollector> phrase_;
  an<UserDictEntryCollector> user_phrase_;
  deque<an<Sentence>> sentences_;

  an<Phrase> candidate_ = nullptr;
  size_t candidate_index_ = 0;
  enum CandidateSource {
    kUninitialized,
    kUserPhrase,
    kSysPhrase,
    kSentence,
  };
  CandidateSource candidate_source_ = kUninitialized;

  DictEntryCollector::reverse_iterator phrase_iter_;
  UserDictEntryCollector::reverse_iterator user_phrase_iter_;

  const size_t max_corrections_ = 4;
  size_t correction_count_ = 0;
  int max_sentences_ = 1;
  double sentence_cutoff_threshold_ = 0.1;

  bool enable_correction_;
  string auxiliary_suffix_;
};

// ScriptTranslator implementation

ScriptTranslator::ScriptTranslator(const Ticket& ticket)
    : Translator(ticket), Memory(ticket), TranslatorOptions(ticket) {
  if (!engine_)
    return;
  if (Config* config = engine_->schema()->config()) {
    config->GetInt(name_space_ + "/spelling_hints", &spelling_hints_);
    config->GetInt(name_space_ + "/max_word_length", &max_word_length_);
    config->GetInt(name_space_ + "/core_word_length", &core_word_length_);
    config->GetBool(name_space_ + "/always_show_comments",
                    &always_show_comments_);
    config->GetBool(name_space_ + "/enable_correction", &enable_correction_);
    if (!config->GetBool(name_space_ + "/enable_word_completion",
                         &enable_word_completion_)) {
      enable_word_completion_ = enable_completion_;
    }
    config->GetInt(name_space_ + "/max_homophones", &max_homophones_);
    poet_.reset(new Poet(language(), config));
    if (an<ConfigList> rules = config->GetList(name_space_ + "/canonicalize")) {
      the<Projection> canonicalizer(new Projection());
      if (canonicalizer->Load(rules)) {
        canonicalizer_ = std::move(canonicalizer);
      }
    }
    if (an<ConfigList> rules = config->GetList("speller/aux_algebra")) {
      the<Projection> auxiliary_algebra(new Projection());
      if (auxiliary_algebra->Load(rules)) {
        auxiliary_algebra_ = std::move(auxiliary_algebra);
      }
    }
    auxiliary_mode_ = true;
    config->GetBool(name_space_ + "/auxiliary_mode", &auxiliary_mode_);
    config->GetString(name_space_ + "/auxiliary_option", &auxiliary_option_);
    config->GetString(name_space_ + "/aux_suffix", &auxiliary_suffix_trigger_);
  }
  if (enable_correction_) {
    if (auto* corrector = Corrector::Require("corrector")) {
      corrector_.reset(corrector->Create(ticket));
    }
  }
}

ScriptTranslator::~ScriptTranslator() = default;

an<Translation> ScriptTranslator::Query(const string& input,
                                        const Segment& segment) {
  if (!dict_ || !dict_->loaded())
    return nullptr;
  if (!segment.HasAnyTagIn(tags_))
    return nullptr;

  DLOG(INFO) << "input = '" << input << "', [" << segment.start << ", "
             << segment.end << ")";

  FinishSession();

  if (!auxiliary_matcher_ && dict_->prism()->SupportsAuxiliaryCharacters()) {
    if (auto component =
            ReverseLookupDictionary::Require("reverse_lookup_dictionary")) {
      Ticket ticket(engine_, name_space_);
      auxiliary_reverse_.reset(component->Create(ticket));
      if (auxiliary_reverse_ && !auxiliary_reverse_->Load())
        auxiliary_reverse_.reset();
    }
    auxiliary_matcher_ = std::make_shared<AuxiliaryMatcher>(
        dict_.get(), auxiliary_reverse_.get());
  }

  string auxiliary_suffix;
  const string& full_input = engine_->context()->input();
  if (segment.HasTag("auxiliary_suffix") &&
      full_input.size() > segment.end + auxiliary_suffix_trigger_.size() &&
      full_input.compare(segment.end, auxiliary_suffix_trigger_.size(),
                         auxiliary_suffix_trigger_) == 0) {
    auxiliary_suffix =
        full_input.substr(segment.end + auxiliary_suffix_trigger_.size());
    if (auxiliary_algebra_)
      auxiliary_algebra_->Apply(&auxiliary_suffix);
  }

  bool user_dict_available =
      user_dict_ && user_dict_->loaded() && !IsUserDictDisabledFor(input);

  size_t end_of_input = auxiliary_suffix.empty()
                            ? engine_->context()->input().length()
                            : segment.end;
  // the translator should survive translations it creates
  auto result = New<ScriptTranslation>(
      this, corrector_.get(), poet_.get(), input, segment.start, end_of_input,
      max_sentences_, sentence_cutoff_threshold_, auxiliary_suffix);
  if (!result ||
      !result->Evaluate(dict_.get(),
                        user_dict_available ? user_dict_.get() : NULL)) {
    return nullptr;
  }
  // 手排权重已在词库查询阶段计算。
  an<Translation> deduped = New<DistinctTranslation>(result);
  if (contextual_suggestions_) {
    return poet_->ContextualWeighted(deduped, input, segment.start, this);
  }
  return deduped;
}

bool ScriptTranslator::auxiliary_enabled() const {
  if (!auxiliary_matcher_ || !engine_)
    return false;
  if (!auxiliary_option_.empty())
    return engine_->context()->get_option(auxiliary_option_);
  return auxiliary_mode_;
}

int ScriptTranslator::core_word_length() const {
  if (max_word_length_ <= 0) {
    return core_word_length_;
  }
  return (std::min)(core_word_length_, max_word_length_);
}

static bool exceed_upperlimit(int length, int upper_limit) {
  return upper_limit > 0 && length > upper_limit;
}

bool ScriptTranslator::SaveCommitEntry(CommitEntry& commit_entry) {
  if (commit_entry.empty())
    return true;
  if (!StartSession())
    return false;
  if (exceed_upperlimit(commit_entry.Length(), max_word_length()))
    return UpdateElements(commit_entry);
  return commit_entry.Save();
}

bool ScriptTranslator::ConcatenatePhrases(CommitEntry& commit_entry,
                                          const vector<an<Phrase>>& phrases) {
  const int kCoreWordLength = core_word_length();
  const int n = phrases.size();
  for (int i = 0; i < n; ++i) {
    int cur_len = 0;
    int j = i;
    for (; j < n; ++j) {
      commit_entry.AppendPhrase(phrases.at(j));
      cur_len += phrases.at(j)->code().size();
      if (kCoreWordLength <= 0 || cur_len > kCoreWordLength)
        break;
      if (!SaveCommitEntry(commit_entry))
        return false;
    }
    if (kCoreWordLength > 0) {
      if (j == i && !SaveCommitEntry(commit_entry))
        return false;
      commit_entry.Clear();
    }
  }
  const bool ok = SaveCommitEntry(commit_entry);
  commit_entry.Clear();
  return ok;
}

bool ScriptTranslator::ProcessSegmentOnCommit(CommitEntry& commit_entry,
                                              const Segment& seg) {
  auto phrase =
      As<Phrase>(Candidate::GetGenuineCandidate(seg.GetSelectedCandidate()));
  const bool recognized = OwnsPhrase(phrase);
  if (recognized)
    queue_.push_back(phrase);
  if (!recognized || seg.status >= Segment::kConfirmed) {
    if (!ConcatenatePhrases(commit_entry, queue_))
      return false;
    queue_.clear();
  }
  return true;
}

bool ScriptTranslator::FlushPendingCommit(CommitEntry& commit_entry) {
  const bool ok = ConcatenatePhrases(commit_entry, queue_);
  queue_.clear();
  return ok;
}

string ScriptTranslator::FormatPreedit(const string& preedit) {
  string result = preedit;
  preedit_formatter_.Apply(&result);
  return result;
}

string ScriptTranslator::Spell(const Code& code) {
  string result;
  vector<string> syllables;
  if (!dict_ || !dict_->Decode(code, &syllables) || syllables.empty())
    return result;
  result = boost::algorithm::join(syllables, string(1, delimiters_.at(0)));
  comment_formatter_.Apply(&result);
  return result;
}

string ScriptTranslator::GetPrecedingText(size_t start) const {
  return !contextual_suggestions_ ? string()
         : start > 0 ? engine_->context()->composition().GetTextBefore(start)
                     : engine_->context()->commit_history().latest_text();
}

bool ScriptTranslator::UpdateElements(const CommitEntry& commit_entry) {
  bool update_elements = false;
  // avoid updating single character entries within a phrase which is
  // composed with single characters only
  if (commit_entry.elements.size() > 1) {
    for (const DictEntry* e : commit_entry.elements) {
      if (e->code.size() > 1) {
        update_elements = true;
        break;
      }
    }
  }
  if (update_elements) {
    for (const DictEntry* e : commit_entry.elements) {
      if (!user_dict_->UpdateEntry(*e, 0))
        return false;
    }
  }
  return true;
}

bool ScriptTranslator::Memorize(const CommitEntry& commit_entry) {
  if (!user_dict_)
    return false;
  if (!UpdateElements(commit_entry))
    return false;
  const bool written = user_dict_->UpdateEntry(commit_entry, 1);
  return written;
}

// ScriptSyllabifier implementation

Spans ScriptSyllabifier::Syllabify(const Phrase* phrase) {
  Spans result;
  vector<size_t> vertices;
  vertices.push_back(start_);
  SyllabifyTask task{
      phrase->code(), syllable_graph_, phrase->end() - start_,
      [&](SyllabifyTask* task, size_t depth, size_t current_pos,
          size_t next_pos) { vertices.push_back(start_ + next_pos); },
      [&](SyllabifyTask* task, size_t depth) { vertices.pop_back(); }};
  task.characters = SplitCharacters(phrase->text());
  if (syllabify_dfs(&task, 0, phrase->start() - start_)) {
    result.set_vertices(std::move(vertices));
  }
  return result;
}

size_t ScriptSyllabifier::BuildSyllableGraph(Prism& prism) {
  return (size_t)syllabifier_.BuildSyllableGraph(input_, prism,
                                                 &syllable_graph_);
}

bool ScriptSyllabifier::IsCorrection(const Code& code,
                                     size_t code_length) const {
  vector<bool> path_attributes;
  path_attributes.reserve(8);

  SyllabifyTask task{
      code, syllable_graph_, code_length,
      // push
      [&](SyllabifyTask* task, size_t depth, size_t current_pos,
          size_t next_pos) {
        auto id = task->code[depth];

        auto start_iter = syllable_graph_.edges.find(current_pos);
        if (start_iter != syllable_graph_.edges.end()) {
          auto end_iter = start_iter->second.find(next_pos);
          if (end_iter != start_iter->second.end()) {
            auto prop_iter = end_iter->second.find(id);
            if (prop_iter != end_iter->second.end()) {
              path_attributes.push_back(prop_iter->second.is_correction);
              return;
            }
          }
        }
        // edge not found
        path_attributes.push_back(false);
      },
      // pop
      [&](SyllabifyTask* task, size_t depth) { path_attributes.pop_back(); }};

  if (syllabify_dfs(&task, 0, 0)) {
    for (bool is_correction : path_attributes) {
      if (is_correction)
        return true;
    }
  }
  return false;
}

string ScriptSyllabifier::GetPreeditString(const Phrase& cand) const {
  const auto& delimiters = translator_->delimiters();
  std::stack<size_t> lengths;
  string output;
  SyllabifyTask task{cand.matching_code(), syllable_graph_, cand.end() - start_,
                     [&](SyllabifyTask* task, size_t depth, size_t current_pos,
                         size_t next_pos) {
                       size_t len = output.length();
                       if (depth > 0 && len > 0 &&
                           delimiters.find(output[len - 1]) == string::npos) {
                         output += delimiters.at(0);
                       }
                       output +=
                           input_.substr(current_pos, next_pos - current_pos);
                       lengths.push(len);
                     },
                     [&](SyllabifyTask* task, size_t depth) {
                       output.resize(lengths.top());
                       lengths.pop();
                     }};
  task.characters = SplitCharacters(cand.text());
  if (syllabify_dfs(&task, 0, cand.start() - start_)) {
    return translator_->FormatPreedit(output);
  } else {
    return string();
  }
}

string ScriptSyllabifier::GetOriginalSpelling(const Phrase& cand) const {
  if (translator_ &&
      static_cast<int>(cand.code().size()) <= translator_->spelling_hints()) {
    return translator_->Spell(cand.code());
  }
  return string();
}

template <class Ptr, class Iter>
static bool has_exact_match_phrase(Ptr ptr, Iter iter, size_t consumed) {
  // AUX 过滤后候选可能为空，不能直接取 rbegin()。
  if (!ptr || iter == ptr->rend())
    return false;
  if (iter->first != consumed || iter->second.exhausted())
    return false;
  const auto entry = iter->second.Peek();
  return entry && entry->IsExactMatch();
}

// 同音截断前先应用 AUX 过滤。
template <class Collector>
static void FilterAuxiliaryEntries(const an<Collector>& collector,
                                   const SyllableGraph& graph,
                                   size_t start) {
  if (!collector)
    return;
  for (auto it = collector->begin(); it != collector->end();) {
    const size_t end = it->first;
    it->second.AddFilter([&graph, start, end](an<DictEntry> entry) {
      bool keep =
          entry && SyllableGraphMatches(graph, entry->code, entry->text, start,
                                        end, entry->matching_code_size);
      return keep;
    });
    if (it->second.exhausted()) {
      it = collector->erase(it);
    } else
      ++it;
  }
}

template <class Collector>
static void FilterAuxiliarySuffixEntries(
    const an<Collector>& collector,
    const std::shared_ptr<AuxiliaryMatcher>& matcher,
    const string& suffix) {
  if (!collector || !matcher || suffix.empty())
    return;
  for (auto it = collector->begin(); it != collector->end();) {
    it->second.AddFilter([matcher, suffix](an<DictEntry> entry) {
      const bool keep = entry && matcher->Match(entry->text, suffix);
      return keep;
    });
    if (it->second.exhausted())
      it = collector->erase(it);
    else
      ++it;
  }
}

template <class Collector>
static void RemoveExhaustedBuckets(const an<Collector>& collector) {
  if (!collector)
    return;
  for (auto it = collector->begin(); it != collector->end();) {
    if (it->second.exhausted())
      it = collector->erase(it);
    else
      ++it;
  }
}

// ScriptTranslation implementation

bool ScriptTranslation::Evaluate(Dictionary* dict, UserDictionary* user_dict) {
  size_t consumed = syllabifier_->BuildSyllableGraph(*dict->prism());
  const auto& syllable_graph = syllabifier_->syllable_graph();
  bool predict_word = translator_->enable_word_completion() &&
                      start_ + consumed == end_of_input_;

  phrase_ =
      dict->Lookup(syllable_graph, 0, &translator_->blacklist(), predict_word);
  if (user_dict) {
    const size_t kUnlimitedDepth = 0;
    const size_t kNumSyllablesToPredictWord = 4;
    user_phrase_ =
        user_dict->Lookup(syllable_graph, 0, kUnlimitedDepth,
                          predict_word ? kNumSyllablesToPredictWord : 0);
  }
  if (syllabifier_->auxiliary_active()) {
    FilterAuxiliaryEntries(phrase_, syllable_graph, 0);
    FilterAuxiliaryEntries(user_phrase_, syllable_graph, 0);
  }
  if (!auxiliary_suffix_.empty()) {
    FilterAuxiliarySuffixEntries(phrase_, translator_->auxiliary_matcher(),
                                 auxiliary_suffix_);
    FilterAuxiliarySuffixEntries(user_phrase_, translator_->auxiliary_matcher(),
                                 auxiliary_suffix_);
  }
  // AUX 过滤后再计算手排位置。
  if (user_phrase_) {
    for (auto& by_end : *user_phrase_) {
      DictEntryIterator* sys = nullptr;
      if (phrase_) {
        auto found = phrase_->find(by_end.first);
        if (found != phrase_->end())
          sys = &found->second;
      }
      // 同一输入区间的不同读音共用可见候选绝对位置。
      user_rank::Apply(sys, &by_end.second);
    }
  }
  // 手排过滤只会耗尽桶内迭代器，不会删除外层编码桶；这里统一清理。
  RemoveExhaustedBuckets(phrase_);
  RemoveExhaustedBuckets(user_phrase_);
  if ((!phrase_ || phrase_->empty()) &&
      (!user_phrase_ || user_phrase_->empty()))
    return false;

  if (phrase_)
    phrase_iter_ = phrase_->rbegin();
  if (user_phrase_)
    user_phrase_iter_ = user_phrase_->rbegin();

  auto is_correction_match = [&](auto& iter_pair, size_t len) {
    if (iter_pair.first != len || iter_pair.second.exhausted())
      return false;
    const auto entry = iter_pair.second.Peek();
    return entry && syllabifier_->IsCorrection(entry->code, len);
  };

  bool has_reliable_phrase =
      has_exact_match_phrase(phrase_, phrase_iter_, consumed) &&
      !is_correction_match(*phrase_iter_, consumed);

  bool has_reliable_user_phrase =
      has_exact_match_phrase(user_phrase_, user_phrase_iter_, consumed) &&
      !is_correction_match(*user_phrase_iter_, consumed);

  bool has_at_least_two_syllables = syllable_graph.edges.size() >= 2;
  DLOG(INFO) << "consumed: " << consumed
             << ", has_reliable_phrase: " << has_reliable_phrase
             << ", has_reliable_user_phrase: " << has_reliable_user_phrase
             << ", has_at_least_two_syllables: " << has_at_least_two_syllables;
  // make sentences when there is no exact-matching phrase candidate
  if (has_at_least_two_syllables && !has_reliable_phrase &&
      !has_reliable_user_phrase) {
    if (max_sentences_ > 1)
      sentences_ = MakeSentences(dict, user_dict);
    else if (max_sentences_) {
      auto sentence = MakeSentence(dict, user_dict);
      if (sentence)
        sentences_ = {sentence};
      else
        sentences_.clear();
    }
  }
  if (!auxiliary_suffix_.empty() && translator_->auxiliary_matcher()) {
    for (auto it = sentences_.begin(); it != sentences_.end();) {
      if (!translator_->auxiliary_matcher()->Match((*it)->text(),
                                                   auxiliary_suffix_))
        it = sentences_.erase(it);
      else
        ++it;
    }
  }

  return !CheckEmpty();
}

bool ScriptTranslation::Next() {
  do {
    if (exhausted())
      return false;
    if (candidate_source_ == kUninitialized) {
      PrepareCandidate();  // to determine candidate_source_
    }
    switch (candidate_source_) {
      case kUninitialized:
        break;
      case kSentence:
        if (!sentences_.empty())
          sentences_.pop_front();
        break;
      case kUserPhrase: {
        UserDictEntryIterator& uter(user_phrase_iter_->second);
        if (!uter.Next()) {
          ++user_phrase_iter_;
        }
      } break;
      case kSysPhrase: {
        DictEntryIterator& iter(phrase_iter_->second);
        if (!iter.Next()) {
          ++phrase_iter_;
        }
      } break;
    }
    candidate_.reset();
    candidate_source_ = kUninitialized;
    if (enable_correction_) {
      // populate next candidate and skip it if it's a correction beyond max
      // numbers.
      if (!PrepareCandidate()) {
        break;
      }
    }
  } while (enable_correction_ &&
           syllabifier_->IsCorrection(candidate_->code(),
                                      candidate_->end() - start_) &&
           // limit the number of correction candidates
           ++correction_count_ > max_corrections_);
  if (!CheckEmpty()) {
    ++candidate_index_;
    return true;
  }
  return false;
}

an<Candidate> ScriptTranslation::Peek() {
  if (candidate_source_ == kUninitialized && !PrepareCandidate()) {
    return nullptr;
  }
  if (candidate_->preedit().empty()) {
    candidate_->set_preedit(syllabifier_->GetPreeditString(*candidate_));
  }
  if (candidate_->comment().empty()) {
    auto spelling = syllabifier_->GetOriginalSpelling(*candidate_);
    if (!spelling.empty() && (translator_->always_show_comments() ||
                              spelling != candidate_->preedit())) {
      candidate_->set_comment(/*quote_left + */ spelling /* + quote_right*/);
    }
  }
  candidate_->set_syllabifier(syllabifier_);
  return candidate_;
}

static bool always_true() {
  return true;
}

template <typename T>
inline static bool prefer_user_phrase(
    T user_phrase_weight,
    T sys_phrase_weight,
    function<bool()> compare_on_tie = always_true) {
  return user_phrase_weight > sys_phrase_weight ||
         (user_phrase_weight == sys_phrase_weight && compare_on_tie());
}

bool ScriptTranslation::PrepareCandidate() {
iter_incremented:
  if (exhausted()) {
    candidate_source_ = kUninitialized;
    candidate_ = nullptr;
    return false;
  }
  if (!sentences_.empty()) {
    candidate_source_ = kSentence;
    candidate_ = sentences_[0];
    return true;
  }
  if (CheckEmpty()) {
    candidate_source_ = kUninitialized;
    candidate_ = nullptr;
    return false;
  }
  const size_t full_code_length = end_of_input_ - start_;
  size_t user_phrase_code_length = 0;
  if (user_phrase_ && user_phrase_iter_ != user_phrase_->rend()) {
    user_phrase_code_length = user_phrase_iter_->first;
  }
  size_t phrase_code_length = 0;
  if (phrase_ && phrase_iter_ != phrase_->rend()) {
    phrase_code_length = phrase_iter_->first;
  }
  if (user_phrase_code_length > 0 &&
      prefer_user_phrase(
          user_phrase_code_length, phrase_code_length,
          // 編碼長度相同時, 用戶詞優先
          [this, full_code_length, phrase_code_length]() {
            UserDictEntryIterator& uter = user_phrase_iter_->second;
            DictEntryIterator& iter = phrase_iter_->second;
            const auto user_entry = uter.Peek();
            const auto system_entry = iter.Peek();
            if (!user_entry)
              return false;
            if (!system_entry)
              return true;
            // 但若用戶詞爲糾錯結果, 則免去優先權, 回歸權重比較
            bool user_is_correction = syllabifier_->IsCorrection(
                user_entry->code, phrase_code_length);
            if (user_is_correction) {
              // 因對糾錯候選施加了較大懲罰, 通常結果爲 系統原文 > 用戶糾錯
              // 兩者皆糾錯時, 用戶詞的動態權重有助達成 用戶糾錯 > 系統糾錯
              return user_rank::PreferUser(*user_entry, *system_entry,
                                           candidate_index_ + 1);
            }
            // 長詞聯想之前須至少出一個嚴格匹配的候選
            // 故確定首選之際, 系統嚴格匹配 > 用戶長詞聯想
            const int kNumExactMatchOnTop = 1;
            if (candidate_index_ < kNumExactMatchOnTop &&
                !has_exact_match_phrase(user_phrase_, user_phrase_iter_,
                                        full_code_length) &&
                has_exact_match_phrase(phrase_, phrase_iter_, full_code_length))
              return false;
            return user_rank::PreferUser(*user_entry, *system_entry,
                                         candidate_index_ + 1);
          })) {
    UserDictEntryIterator& uter = user_phrase_iter_->second;
    const auto entry = uter.Peek();
    if (!entry) {
      ++user_phrase_iter_;
      goto iter_incremented;
    }
    DLOG(INFO) << "user phrase '" << entry->text
               << "', code length: " << user_phrase_code_length;
    candidate_source_ = kUserPhrase;
    candidate_ =
        New<Phrase>(translator_->language(),
                    entry->IsPredictiveMatch() ? "completion" : "user_phrase",
                    start_, start_ + user_phrase_code_length, entry);
    candidate_->set_quality(std::exp(entry->weight) +
                            translator_->initial_quality() +
                            (entry->quality_len / full_code_length));
    return true;
  } else if (phrase_code_length > 0) {
    DictEntryIterator& iter = phrase_iter_->second;
    const auto entry = iter.Peek();
    if (!entry) {
      ++phrase_iter_;
      goto iter_incremented;
    }
    DLOG(INFO) << "phrase '" << entry->text
               << "', code length: " << phrase_code_length;
    candidate_source_ = kSysPhrase;
    candidate_ =
        New<Phrase>(translator_->language(),
                    entry->IsPredictiveMatch() ? "completion" : "phrase",
                    start_, start_ + phrase_code_length, entry);
    candidate_->set_quality(std::exp(entry->weight) +
                            translator_->initial_quality() +
                            (entry->quality_len / full_code_length));
    return true;
  } else {
    candidate_source_ = kUninitialized;
    candidate_ = nullptr;
    return false;
  }
}

bool ScriptTranslation::CheckEmpty() {
  set_exhausted((!phrase_ || phrase_iter_ == phrase_->rend()) &&
                (!user_phrase_ || user_phrase_iter_ == user_phrase_->rend()));
  return exhausted();
}

template <class QueryResult>
void ScriptTranslation::EnrollEntries(
    map<int, DictEntryList>& entries_by_end_pos,
    const an<QueryResult>& query_result,
    size_t start_pos) {
  if (query_result) {
    if (syllabifier_->auxiliary_active()) {
      FilterAuxiliaryEntries(query_result, syllabifier_->syllable_graph(),
                             start_pos);
    }
    for (auto& y : *query_result) {
      DictEntryList& homophones = entries_by_end_pos[y.first];
      while (homophones.size() < translator_->max_homophones() &&
             !y.second.exhausted()) {
        homophones.push_back(y.second.Peek());
        if (!y.second.Next())
          break;
      }
    }
  }
}

WordGraph ScriptTranslation::PrepareForMakingSentence(
    Dictionary* dict,
    UserDictionary* user_dict) {
  const int kMaxSyllablesForUserPhraseQuery = 5;
  const auto& syllable_graph = syllabifier_->syllable_graph();
  WordGraph graph;
  for (const auto& x : syllable_graph.edges) {
    auto& same_start_pos = graph[x.first];
    auto user_results = user_dict
                            ? user_dict->Lookup(syllable_graph, x.first,
                                                kMaxSyllablesForUserPhraseQuery)
                            : nullptr;
    auto system_results =
        dict->Lookup(syllable_graph, x.first, &translator_->blacklist());
    // AUX 可能删除候选节点，必须先过滤再遍历。
    if (syllabifier_->auxiliary_active()) {
      FilterAuxiliaryEntries(user_results, syllable_graph, x.first);
      FilterAuxiliaryEntries(system_results, syllable_graph, x.first);
    }
    if (user_results) {
      for (auto& y : *user_results) {
        DictEntryIterator* sys = nullptr;
        if (system_results) {
          auto found = system_results->find(y.first);
          if (found != system_results->end())
            sys = &found->second;
        }
        // 句子组合仍按读音隔离，避免手排跨读音影响 Poet 的路径评分。
        user_rank::Apply(sys, &y.second, true);
      }
    }
    EnrollEntries(same_start_pos, user_results, x.first);
    EnrollEntries(same_start_pos, system_results, x.first);
  }
  return graph;
}

deque<an<Sentence>> ScriptTranslation::MakeSentences(
    Dictionary* dict,
    UserDictionary* user_dict) {
  const auto& syllable_graph = syllabifier_->syllable_graph();
  WordGraph graph = PrepareForMakingSentence(dict, user_dict);
  auto sentences =
      poet_->MakeSentences(graph, syllable_graph.interpreted_length,
                           translator_->GetPrecedingText(start_),
                           max_sentences_, sentence_cutoff_threshold_);
  for (auto& sentence : sentences) {
    sentence->Offset(start_);
    sentence->set_syllabifier(syllabifier_);
  }
  return sentences;
}

an<Sentence> ScriptTranslation::MakeSentence(Dictionary* dict,
                                             UserDictionary* user_dict) {
  const auto& syllable_graph = syllabifier_->syllable_graph();
  WordGraph graph = PrepareForMakingSentence(dict, user_dict);
  if (auto sentence =
          poet_->MakeSentence(graph, syllable_graph.interpreted_length,
                              translator_->GetPrecedingText(start_))) {
    sentence->Offset(start_);
    sentence->set_syllabifier(syllabifier_);
    return sentence;
  }
  return nullptr;
}

}  // namespace rime
