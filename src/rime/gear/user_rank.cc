// Copyright RIME Developers
// Distributed under the BSD License
#include <rime/gear/user_rank.h>
#include <rime/gear/user_rank_math.h>
#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <vector>

namespace rime {
namespace user_rank {
namespace {
struct Pin {
  an<DictEntry> entry;
  int position;
};
using PinnedTexts = std::map<Code, std::set<string>>;

// 不同读音分别处理手排，避免互相影响。
void RankReading(const DictEntryIterator* system,
                 UserDictEntryIterator* users,
                 const Code& reading,
                 bool isolate_code,
                 std::vector<Pin>& pins) {
  std::sort(pins.begin(), pins.end(), [](const Pin& a, const Pin& b) {
    if (a.position != b.position)
      return a.position < b.position;
    if (a.entry->manual_version != b.entry->manual_version)
      return a.entry->manual_version > b.entry->manual_version;
    return a.entry->text < b.entry->text;
  });
  DictEntryIterator scan;
  if (system)
    scan = system->Fork();
  std::vector<an<DictEntry>> ordinary;
  ordinary.reserve(users->cache_size());
  for (auto& e : users->entries()) {
    if (e && users->Allows(e) && e->manual_position == 0 &&
        e->remaining_code_length == 0 && (!isolate_code || e->code == reading))
      ordinary.push_back(e);
  }
  std::stable_sort(ordinary.begin(), ordinary.end(),
                   [](const an<DictEntry>& a, const an<DictEntry>& b) {
                     return a->weight > b->weight;
                   });
  size_t user_index = 0;
  std::set<string> seen_text;
  auto next_weight = [&]() -> double {
    while (true) {
      if (isolate_code) {
        while (!scan.exhausted() && scan.Peek()->code != reading)
          scan.Next();
      }
      const double sys = scan.exhausted()
                             ? -std::numeric_limits<double>::infinity()
                             : scan.Peek()->weight;
      const double usr = user_index == ordinary.size()
                             ? -std::numeric_limits<double>::infinity()
                             : ordinary[user_index]->weight;
      if (!std::isfinite(sys) && !std::isfinite(usr))
        return -std::numeric_limits<double>::infinity();
      string text;
      double weight;
      if (usr >= sys && user_index < ordinary.size()) {
        text = ordinary[user_index]->text;
        weight = usr;
        ++user_index;
      } else {
        text = scan.Peek()->text;
        weight = sys;
        scan.Next();
      }
      // 先按最终去重结果确定位置，避免重复词条挤占序号。
      if (seen_text.insert(text).second)
        return weight;
    }
  };
  // 只为有效手排遍历候选，不创建候选窗口。
  size_t base_consumed = 0, pins_placed = 0;
  double previous = std::numeric_limits<double>::infinity();
  double upcoming = next_weight();
  for (const auto& pin : pins) {
    const size_t wanted = static_cast<size_t>(pin.position);
    while (base_consumed + pins_placed + 1 < wanted &&
           std::isfinite(upcoming)) {
      previous = upcoming;
      ++base_consumed;
      upcoming = next_weight();
    }
    const double anchor = WeightBetween(previous, upcoming);
    pin.entry->weight = anchor;
    previous = anchor;
    ++pins_placed;
  }
}
}  // namespace

bool PreferUser(const DictEntry& user,
                const DictEntry& system,
                size_t observed_position) {
  if (user.weight != system.weight)
    return user.weight > system.weight;
  if (user.manual_position > 0)
    return PreferPinnedOnTie(user.manual_position, observed_position);
  return true;
}

bool HasManualPin(const UserDictEntryIterator& users) {
  for (auto& e : users.entries())
    if (e && users.Allows(e) && e->manual_position > 0 && e->IsExactMatch() &&
        e->remaining_code_length == 0)
      return true;
  return false;
}

void FilterPinnedStatic(DictEntryIterator* system,
                        const UserDictEntryIterator& users,
                        bool isolate_code) {
  if (!system)
    return;
  PinnedTexts texts;
  for (const auto& e : users.entries()) {
    if (!e || !users.Allows(e) || e->manual_position <= 0 ||
        !e->IsExactMatch() || e->remaining_code_length != 0)
      continue;
    texts[isolate_code ? e->code : Code{}].insert(e->text);
  }
  if (texts.empty())
    return;
  system->AddFilter([texts, isolate_code](an<DictEntry> e) {
    if (!e)
      return false;
    const Code reading = isolate_code ? e->code : Code{};
    auto match = texts.find(reading);
    return match == texts.end() || match->second.count(e->text) == 0;
  });
}

// 存在手排时不执行等长自动置顶；否则仅已学习词参与。
void ApplyEqualLengthPriority(DictEntryIterator* system,
                              UserDictEntryIterator* users,
                              bool isolate_code) {
  if (!system)
    return;
  std::set<Code> manually_ranked;
  bool has_learned_user = false;
  for (const auto& entry : users->entries()) {
    if (!entry || !users->Allows(entry) || !entry->IsExactMatch() ||
        entry->remaining_code_length != 0)
      continue;
    if (entry->manual_position > 0) {
      manually_ranked.insert(isolate_code ? entry->code : Code{});
    } else if (entry->commit_count > 0) {
      has_learned_user = true;
    }
  }
  if (!has_learned_user)
    return;
  const auto top = system->TopWeightsByCode(isolate_code);
  for (auto& entry : users->entries()) {
    if (!entry || !users->Allows(entry) || entry->manual_position > 0 ||
        entry->commit_count <= 0 || !entry->IsExactMatch() ||
        entry->remaining_code_length != 0)
      continue;
    const Code code = isolate_code ? entry->code : Code{};
    if (manually_ranked.count(code))
      continue;
    const auto found = top.find(code);
    if (found == top.end())
      continue;
    const double offset = 0.02 + 0.001 * std::atan(entry->weight);
    entry->weight = (std::max)(entry->weight, found->second + offset);
  }
}

void Apply(DictEntryIterator* system,
           UserDictEntryIterator* users,
           bool isolate_code,
           const DictEntryIterator* full_anchor) {
  if (!users)
    return;
  ApplyEqualLengthPriority(system, users, isolate_code);
  std::map<Code, std::vector<Pin>> readings;
  for (auto& e : users->entries()) {
    if (e && users->Allows(e) && e->manual_position > 0 && e->IsExactMatch() &&
        e->remaining_code_length == 0) {
      Code reading = isolate_code ? e->code : Code{};
      readings[reading].push_back({e, e->manual_position});
    }
  }
  if (readings.empty()) {
    users->SortAll();
    return;
  }
  FilterPinnedStatic(system, *users, isolate_code);
  DictEntryIterator anchor_copy;
  if (full_anchor) {
    anchor_copy = full_anchor->Fork();
    FilterPinnedStatic(&anchor_copy, *users, isolate_code);
  }
  for (auto& [reading, pins] : readings)
    RankReading(full_anchor ? &anchor_copy : system, users, reading,
                isolate_code, pins);
  users->SortAll();
}
}  // namespace user_rank
}  // namespace rime
