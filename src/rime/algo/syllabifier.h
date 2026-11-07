//
// Copyright RIME Developers
// Distributed under the BSD License
//
// 2011-7-12 Zou xu <zouivex@gmail.com>
//

#ifndef RIME_SYLLABIFIER_H_
#define RIME_SYLLABIFIER_H_

#include <stdint.h>
#include <functional>
#include <set>
#include <rime_api.h>
#include <rime/common.h>
#include "spelling.h"

namespace rime {

class Corrector;
class Prism;
class Projection;

using SyllableId = int32_t;

struct EdgeProperties : SpellingProperties {
  EdgeProperties(SpellingProperties sup) : SpellingProperties(sup) {};
  EdgeProperties() = default;
  // 切分歧義編碼段的起始位置
  set<size_t> ambiguous_source_positions;
  std::shared_ptr<const std::set<string>> auxiliary_chars;
  // The same spelling can have ordinary and conditional AUX interpretations.
  vector<EdgeProperties> alternatives;
};

using SpellingMap = map<SyllableId, EdgeProperties>;
using VertexMap = map<size_t, SpellingType>;
using EndVertexMap = map<size_t, SpellingMap>;
using EdgeMap = map<size_t, EndVertexMap>;

using SpellingPropertiesList = vector<const EdgeProperties*>;
using SpellingIndex = map<SyllableId, SpellingPropertiesList>;
using SpellingIndices = map<size_t, SpellingIndex>;

struct AuxGraphMatch {
  size_t length = 0;
  SyllableId syllable_id = -1;
  std::shared_ptr<const std::set<string>> allowed_chars;
  SpellingType type = kNormalSpelling;
  double credibility = 0.0;
};

// Only AUX edges allocate a path node; ordinary dictionary queries share null.
struct AuxiliaryPath {
  std::shared_ptr<const AuxiliaryPath> previous;
  size_t character_index = 0;
  std::shared_ptr<const std::set<string>> allowed_chars;

  bool Matches(const string& text) const;
};

using AuxMatchProvider =
    std::function<void(const string&, vector<AuxGraphMatch>*)>;

struct SyllableGraph {
  bool has_auxiliary = false;
  size_t input_length = 0;
  size_t interpreted_length = 0;
  VertexMap vertices;
  EdgeMap edges;
  SpellingIndices indices;
};

class Syllabifier {
 public:
  Syllabifier() = default;
  explicit Syllabifier(const string& delimiters,
                       bool enable_completion = false,
                       bool strict_spelling = false,
                       Projection* canonicalizer = nullptr)
      : delimiters_(delimiters),
        enable_completion_(enable_completion),
        strict_spelling_(strict_spelling),
        canonicalizer_(canonicalizer) {}

  RIME_DLL int BuildSyllableGraph(const string& input,
                                  Prism& prism,
                                  SyllableGraph* graph);
  RIME_DLL void EnableCorrection(Corrector* corrector);
  void SetAuxiliaryLookup(AuxMatchProvider lookup) {
    auxiliary_lookup_ = std::move(lookup);
  }

 protected:
  void CheckOverlappedSpellings(SyllableGraph* graph, size_t start, size_t end);
  void Transpose(SyllableGraph* graph);

  string delimiters_;
  bool enable_completion_ = false;
  bool strict_spelling_ = false;
  Projection* canonicalizer_ = nullptr;
  Corrector* corrector_ = nullptr;
  AuxMatchProvider auxiliary_lookup_;
};

}  // namespace rime

#endif  // RIME_SYLLABIFIER_H_
