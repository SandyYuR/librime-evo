// Copyright RIME Developers
// Distributed under the BSD License
#ifndef RIME_GEAR_USER_RANK_H_
#define RIME_GEAR_USER_RANK_H_
#include <cstddef>
#include <rime/dict/dictionary.h>
#include <rime/dict/user_dictionary.h>
namespace rime {
namespace user_rank {
// Applies transient manual weight anchors to existing UserDB query results.
// This is NOT a filter or a translation window. Scanning a distant anchor
// traverses dictionary chunks (not Candidate objects) and only if g > 0.
bool HasManualPin(const UserDictEntryIterator& users);
void FilterPinnedStatic(DictEntryIterator* system,
                        const UserDictEntryIterator& users,
                        bool isolate_code = false);
void Apply(DictEntryIterator* system,
           UserDictEntryIterator* users,
           bool isolate_code = false,
           const DictEntryIterator* full_anchor = nullptr);
// Called only when both candidates have otherwise equal eligibility/span.
// observed_position is 1-based; ties are resolved without artificial
// unbounded weight bonuses or a fixed candidate buffer.
bool PreferUser(const DictEntry& user,
                const DictEntry& system,
                size_t observed_position);
}  // namespace user_rank
}  // namespace rime
#endif
