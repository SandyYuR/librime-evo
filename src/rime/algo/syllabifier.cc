//
// Copyright RIME Developers
// Distributed under the BSD License
//
// 2011-07-12 Zou Xu <zouivex@gmail.com>
// 2012-02-11 GONG Chen <chen.sst@gmail.com>
//
#include <algorithm>
#include <queue>
#include <string_view>
#include <utf8.h>
#include <rime/dict/vocabulary.h>
#include <boost/range/adaptor/reversed.hpp>
#include <rime/algo/syllabifier.h>
#include <rime/dict/corrector.h>
#include <rime/dict/prism.h>
#include "syllabifier.h"

namespace rime {
using namespace corrector;

using Vertex = pair<size_t, SpellingType>;
using VertexQueue =
    std::priority_queue<Vertex, vector<Vertex>, std::greater<Vertex>>;

// 權重階梯：
// 1. 全拼：用戶完整輸入了所有編碼。Penalty = 0
// 2. 簡拼：用戶輸入了縮寫，明確希望匹配某個字。Penalty ≈ -2.3
// 3. 補全 (Completion)：用戶還沒打完，算法瞎猜的。Penalty ≈ -3.0
const double kCompletionPenalty = -2.995732273553991;      // log(0.05)
const double kCorrectionCredibility = -4.605170185988091;  // log(0.01)

int Syllabifier::BuildSyllableGraph(const string& input,
                                    Prism& prism,
                                    SyllableGraph* graph) {
  if (input.empty())
    return 0;

  size_t farthest = 0;
  VertexQueue queue;
  queue.push(Vertex{0, kNormalSpelling});  // start

  while (!queue.empty()) {
    Vertex vertex(queue.top());
    queue.pop();
    size_t current_pos = vertex.first;

    // record a visit to the vertex
    if (graph->vertices.find(current_pos) == graph->vertices.end()) {
      graph->vertices.insert(vertex);  // preferred spelling type comes first
    } else {
      //      graph->vertices[current_pos] =
      //          std::min(vertex.second, graph->vertices[current_pos]);
      continue;  // discard worse spelling types
    }

    if (current_pos > farthest)
      farthest = current_pos;

    // consume leading delimiters
    size_t begin_pos = current_pos;
    while (begin_pos < input.length() &&
           delimiters_.find(input[begin_pos]) != string::npos)
      ++begin_pos;
    DLOG(INFO) << "current_pos: " << current_pos
               << ", begin_pos: " << begin_pos;

    // see where we can go by advancing a syllable
    vector<Prism::Match> matches;
    set<SyllableId> exact_match_syllables;
    std::string_view current_input(input.data() + begin_pos,
                                   input.length() - begin_pos);
    if (canonicalizer_) {
      const size_t limit =
          (std::min)(current_input.length(), prism.max_key_length());
      string syllable;
      syllable.reserve(limit);
      for (size_t len = 1; len <= limit; ++len) {
        // TODO: 須重構 Calculus 爲寫入時複製以避免複製子串.
        syllable.assign(current_input.data(), len);
        canonicalizer_->Apply(&syllable);
        // 重排打破了前綴單調性, 故不能用前綴下行搜索, 改爲每次步進精確查詢.
        int value = -1;
        if (prism.GetValue(syllable, &value)) {
          matches.emplace_back(Prism::Match{value, len});
        }
      }
    } else {
      prism.CommonPrefixSearch(current_input, &matches);
    }
    if (corrector_) {
      for (auto& m : matches) {
        exact_match_syllables.insert(m.value);
      }
      Corrections corrections;
      corrector_->ToleranceSearch(prism, string{current_input}, &corrections,
                                  5);
      for (const auto& m : corrections) {
        for (auto accessor = prism.QuerySpelling(m.first);
             !accessor.exhausted(); accessor.Next()) {
          auto props = accessor.properties();
          if (props.type == kNormalSpelling && !props.is_correction) {
            matches.push_back({m.first, m.second.length});
            break;
          }
        }
      }
    }

    size_t leading_gap = begin_pos - current_pos;
    if (!matches.empty()) {
      auto& end_vertices(graph->edges[current_pos]);
      for (const auto& m : matches) {
        if (m.length == 0)
          continue;
        size_t end_pos = current_pos + leading_gap + m.length;
        // consume trailing delimiters
        while (end_pos < input.length() &&
               delimiters_.find(input[end_pos]) != string::npos)
          ++end_pos;
        DLOG(INFO) << "end_pos: " << end_pos;
        bool matches_input = (current_pos == 0 && end_pos == input.length());
        SpellingMap& spellings(end_vertices[end_pos]);
        SpellingType end_vertex_type = kInvalidSpelling;
        // when spelling algebra is enabled,
        // a spelling evaluates to a set of syllables;
        // otherwise, it resembles exactly the syllable itself.
        SpellingAccessor accessor(prism.QuerySpelling(m.value));
        while (!accessor.exhausted()) {
          SyllableId syllable_id = accessor.syllable_id();
          EdgeProperties props(accessor.properties());
          if (props.auxiliary) {
            // AUX is a conditional path, never an ordinary spelling edge.
            accessor.Next();
            continue;
          }
          if (strict_spelling_ && matches_input &&
              props.type != kNormalSpelling) {
            // disqualify fuzzy spelling or abbreviation as single word
          } else {
            props.end_pos = end_pos;
            // add a syllable with properties to the edge's
            // spelling-to-syllable map
            if (corrector_ && exact_match_syllables.find(m.value) ==
                                  exact_match_syllables.end()) {
              props.is_correction = true;
              props.credibility = kCorrectionCredibility;
            }
            auto it = spellings.find(syllable_id);
            if (it == spellings.end()) {
              spellings.insert({syllable_id, props});
            } else {
              it->second.type = (std::min)(it->second.type, props.type);
            }
            // let end_vertex_type be the best (smaller) type of spelling
            // that ends at the vertex
            if (end_vertex_type > props.type && !props.is_correction) {
              end_vertex_type = props.type;
            }
          }
          accessor.Next();
        }
        if (spellings.empty()) {
          DLOG(INFO) << "not spelled.";
          end_vertices.erase(end_pos);
          continue;
        }
        // find the best common type in a path up to the end vertex
        // eg. pinyin "shurfa" has vertex type kNormalSpelling at position 3,
        // kAbbreviation at position 4 and kAbbreviation at position 6
        if (end_vertex_type < vertex.second) {
          end_vertex_type = vertex.second;
        }
        queue.push(Vertex{end_pos, end_vertex_type});
        DLOG(INFO) << "added to syllable graph, edge: [" << current_pos << ", "
                   << end_pos << ")";
      }
    }
    if (auxiliary_lookup_) {
      vector<AuxGraphMatch> aux_matches;
      // Prism keys never contain the user-facing syllable delimiters.  The
      // ordinary matcher consumes them while walking the graph, so AUX must
      // query the same compact spelling (e.g. `hk n` -> `hkn`) and map the
      // result back to the original byte span below.
      string compact_aux_input;
      compact_aux_input.reserve(current_input.size());
      for (size_t p = begin_pos; p < input.size(); ++p) {
        if (delimiters_.find(input[p]) == string::npos)
          compact_aux_input.push_back(input[p]);
      }
      auxiliary_lookup_(compact_aux_input, &aux_matches);
      // An unseparated input uses the longest complete AUX code at this
      // position.  Shorter prefixes remain available only while no longer
      // AUX code has matched, so `aiwy` cannot fall back to `aiw` + `y`.
      size_t longest_aux_length = 0;
      for (const auto& m : aux_matches) {
        if (m.length > 0 && m.allowed_chars && !m.allowed_chars->empty() &&
            m.syllable_id >= 0) {
          longest_aux_length = (std::max)(longest_aux_length, m.length);
        }
      }
      for (const auto& m : aux_matches) {
        if (m.length == 0 || (!m.allowed_chars || m.allowed_chars->empty()) ||
            m.syllable_id < 0 || m.length < longest_aux_length)
          continue;
        graph->has_auxiliary = true;
        // m.length is measured in the compact spelling.  Walk the original
        // input and consume exactly that many non-delimiter bytes so a span
        // such as `hk n` correctly ends at byte 4 rather than byte 3.
        size_t end_pos = begin_pos;
        size_t consumed = 0;
        while (end_pos < input.size() && consumed < m.length) {
          if (delimiters_.find(input[end_pos]) == string::npos) {
            ++consumed;
          }
          ++end_pos;
        }
        if (consumed != m.length)
          continue;
        while (end_pos < input.size() &&
               delimiters_.find(input[end_pos]) != string::npos)
          ++end_pos;
        // An abbreviated AUX hit supplies a low-quality candidate path.  It
        // must not turn the whole span into a hard AUX filter, otherwise an
        // ordinary phrase such as `zgjw` is discarded in favor of one
        // character whose abbreviated AUX happens to be `jw`.
        if (m.type < kAbbreviation) {
          graph->auxiliary_constraints.push_back(
              {current_pos, end_pos, m.syllable_id, m.allowed_chars});
        }
        auto& spellings = graph->edges[current_pos][end_pos];
        auto it = spellings.find(m.syllable_id);
        if (it == spellings.end()) {
          EdgeProperties props;
          props.end_pos = end_pos;
          props.type = m.type;
          props.credibility = m.credibility;
          props.has_normal = false;
          props.auxiliary_chars = m.allowed_chars;
          spellings.emplace(m.syllable_id, std::move(props));
        } else {
          it->second.type = (std::min)(it->second.type, m.type);
          it->second.credibility =
              (std::max)(it->second.credibility, m.credibility);
          auto merged = std::make_shared<std::set<string>>(
              it->second.auxiliary_chars ? *it->second.auxiliary_chars
                                         : std::set<string>{});
          merged->insert(m.allowed_chars->begin(), m.allowed_chars->end());
          it->second.auxiliary_chars = std::move(merged);
        }
        queue.push(Vertex{end_pos, vertex.second});
      }
    }
  }

  DLOG(INFO) << "remove stale vertices and edges";
  set<int> good;
  good.insert(farthest);
  // fuzzy spellings are immune to invalidation by normal spellings
  SpellingType last_type =
      (std::max)(graph->vertices[farthest], kFuzzySpelling);
  for (int i = farthest - 1; i >= 0; --i) {
    if (graph->vertices.find(i) == graph->vertices.end())
      continue;
    // remove stale edges
    for (auto j = graph->edges[i].begin(); j != graph->edges[i].end();) {
      if (good.find(j->first) == good.end()) {
        // not connected
        graph->edges[i].erase(j++);
        continue;
      }
      // remove disqualified syllables (eg. matching abbreviated spellings)
      // when there is a path of more favored type
      SpellingType edge_type = kInvalidSpelling;
      for (auto k = j->second.begin(); k != j->second.end();) {
        if (k->second.is_correction) {
          ++k;
          continue;  // Don't care correction edges
        }
        // AUX edges remain valid even when their optional algebra metadata
        // marks them as abbreviations.  Their type affects candidate quality,
        // while auxiliary_chars keeps the path conditional on the AUX match.
        if (k->second.type > last_type && !k->second.auxiliary_chars) {
          j->second.erase(k++);
        } else {
          if (k->second.type < edge_type)
            edge_type = k->second.type;
          ++k;
        }
      }
      if (j->second.empty()) {
        graph->edges[i].erase(j++);
      } else {
        bool contains_aux = false;
        for (const auto& spelling : j->second)
          contains_aux |= bool(spelling.second.auxiliary_chars);
        if (edge_type < kAbbreviation && !contains_aux)
          CheckOverlappedSpellings(graph, i, j->first);
        ++j;
      }
    }
    if (graph->vertices[i] > last_type || graph->edges[i].empty()) {
      DLOG(INFO) << "remove stale vertex at " << i;
      graph->vertices.erase(i);
      graph->edges.erase(i);
      continue;
    }
    // keep the valid vertex
    good.insert(i);
  }

  if (enable_completion_ && farthest < input.length()) {
    DLOG(INFO) << "completion enabled";
    const size_t kExpandSearchLimit = 512;
    vector<Prism::Match> keys;
    prism.ExpandSearch(input.substr(farthest), &keys, kExpandSearchLimit);
    if (!keys.empty()) {
      size_t current_pos = farthest;
      size_t end_pos = input.length();
      size_t code_length = end_pos - current_pos;
      auto& end_vertices(graph->edges[current_pos]);
      auto& spellings(end_vertices[end_pos]);
      for (const auto& m : keys) {
        if (m.length < code_length)
          continue;
        // when spelling algebra is enabled,
        // a spelling evaluates to a set of syllables;
        // otherwise, it resembles exactly the syllable itself.
        SpellingAccessor accessor(prism.QuerySpelling(m.value));
        while (!accessor.exhausted()) {
          SyllableId syllable_id = accessor.syllable_id();
          SpellingProperties props = accessor.properties();
          if (props.type < kAbbreviation) {
            props.type = kCompletion;
            props.credibility += kCompletionPenalty;
            props.end_pos = end_pos;
            // add a syllable with properties to the edge's
            // spelling-to-syllable map
            spellings.insert({syllable_id, props});
          }
          accessor.Next();
        }
      }
      if (spellings.empty()) {
        DLOG(INFO) << "no completion could be made.";
        end_vertices.erase(end_pos);
      } else {
        DLOG(INFO) << "added to syllable graph, completion: [" << current_pos
                   << ", " << end_pos << ")";
        farthest = end_pos;
      }
    }
  }

  // A detected AUX prefix is only a constraint while its edge is part of a
  // complete graph path.  A prefix that cannot consume the remaining input
  // must not suppress ordinary candidates on the same span.
  if (graph->has_auxiliary && farthest < input.length()) {
    // An unconsumed suffix means no complete interpretation exists.  AUX
    // prefixes must not turn a partial match into a hard candidate filter.
    graph->auxiliary_constraints.clear();
    graph->has_auxiliary = false;
  } else if (graph->has_auxiliary) {
    vector<AuxiliaryConstraint> live_constraints;
    live_constraints.reserve(graph->auxiliary_constraints.size());
    for (const auto& aux : graph->auxiliary_constraints) {
      auto start = graph->edges.find(aux.start);
      if (start == graph->edges.end())
        continue;
      auto end = start->second.find(aux.end);
      if (end == start->second.end())
        continue;
      auto spelling = end->second.find(aux.syllable_id);
      if (spelling == end->second.end() || !spelling->second.auxiliary_chars)
        continue;
      live_constraints.push_back(aux);
    }
    graph->auxiliary_constraints.swap(live_constraints);
    graph->has_auxiliary = !graph->auxiliary_constraints.empty();
  }

  graph->input_length = input.length();
  graph->interpreted_length = farthest;
  DLOG(INFO) << "input length: " << graph->input_length;
  DLOG(INFO) << "syllabified length: " << graph->interpreted_length;

  Transpose(graph);

  return farthest;
}

void Syllabifier::CheckOverlappedSpellings(SyllableGraph* graph,
                                           size_t start,
                                           size_t end) {
  if (!graph || graph->edges.find(start) == graph->edges.end())
    return;
  // if "Z" = "YX", mark the vertex between Y and X an ambiguous syllable joint
  auto& y_end_vertices(graph->edges[start]);
  // enumerate Ys
  for (const auto& y : y_end_vertices) {
    size_t joint = y.first;
    if (joint >= end)
      break;
    // test X
    if (graph->edges.find(joint) == graph->edges.end())
      continue;
    auto& x_end_vertices(graph->edges[joint]);
    for (auto& x : x_end_vertices) {
      if (x.first < end)
        continue;
      if (x.first == end) {
        // discourage syllables at an ambiguous joint
        // bad cases include pinyin syllabification "niju'ede"
        for (auto& spelling : x.second) {
          // 這條邊（X）相對於起點構成歧義
          spelling.second.ambiguous_source_positions.insert(start);
        }
        graph->vertices[joint] = kAmbiguousSpelling;
        DLOG(INFO) << "ambiguous syllable joint at position " << joint << ".";
      }
      break;
    }
  }
}

void Syllabifier::Transpose(SyllableGraph* graph) {
  for (const auto& start : graph->edges) {
    auto& index(graph->indices[start.first]);
    for (const auto& end : boost::adaptors::reverse(start.second)) {
      for (const auto& spelling : end.second) {
        SyllableId syll_id = spelling.first;
        index[syll_id].push_back(&spelling.second);
      }
    }
  }
}

void Syllabifier::EnableCorrection(Corrector* corrector) {
  corrector_ = corrector;
}

namespace {

bool MatchesRecursive(const SyllableGraph& graph,
                      const Code& code,
                      const vector<string>& characters,
                      size_t depth,
                      size_t pos,
                      size_t end,
                      size_t code_size,
                      std::map<std::pair<size_t, size_t>, bool>* memo) {
  if (depth == code_size)
    return pos == end;
  auto state = std::make_pair(depth, pos);
  auto hit = memo->find(state);
  if (hit != memo->end())
    return hit->second;
  bool result = false;
  auto s = graph.edges.find(pos);
  if (s != graph.edges.end()) {
    for (const auto& next : s->second) {
      if (next.first > end)
        continue;
      auto e = next.second.find(code[depth]);
      if (e == next.second.end())
        continue;
      const auto& props = e->second;
      bool allowed = props.has_normal;
      if (!allowed && depth < characters.size())
        allowed = (props.auxiliary_chars &&
                   props.auxiliary_chars->count(characters[depth]) != 0);
      if (allowed && MatchesRecursive(graph, code, characters, depth + 1,
                                      next.first, end, code_size, memo)) {
        result = true;
        break;
      }
    }
  }
  if (depth < characters.size()) {
    for (const auto& aux : graph.auxiliary_constraints) {
      if (aux.start != pos || aux.syllable_id != code[depth] || aux.end > end ||
          !aux.allowed_chars || !aux.allowed_chars->count(characters[depth]))
        continue;
      if (MatchesRecursive(graph, code, characters, depth + 1, aux.end, end,
                           code_size, memo)) {
        result = true;
        break;
      }
    }
  }
  (*memo)[state] = result;
  return result;
}

}  // namespace

bool SyllableGraphMatches(const SyllableGraph& graph,
                          const Code& code,
                          const string& text,
                          size_t start,
                          size_t end,
                          size_t depth_limit) {
  size_t code_size =
      depth_limit ? std::min(depth_limit, code.size()) : code.size();
  vector<string> characters;
  auto cur = text.begin();
  while (cur != text.end()) {
    auto begin = cur;
    utf8::next(cur, text.end());
    characters.emplace_back(begin, cur);
  }
  std::map<std::pair<size_t, size_t>, bool> memo;
  // An AUX prefix is a candidate-path constraint, not a score bonus.  When
  // the candidate pool is being evaluated at the exact AUX span, ordinary
  // abbreviation paths must be removed from that pool.
  bool exact_aux_span = false;
  for (const auto& aux : graph.auxiliary_constraints) {
    if (aux.start != start || aux.end != end)
      continue;
    exact_aux_span = true;
    if (code_size == 1 && characters.size() == 1 &&
        aux.syllable_id == code[0] && aux.allowed_chars &&
        aux.allowed_chars->count(characters[0]))
      return true;
  }
  if (exact_aux_span)
    return false;
  return MatchesRecursive(graph, code, characters, 0, start, end, code_size,
                          &memo);
}

}  // namespace rime
