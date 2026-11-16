//
// Copyright RIME Developers
// Distributed under the BSD License
//
#include <rime/config.h>
#include <rime/schema.h>
#include <rime/segmentation.h>
#include <rime/gear/auxiliary_suffix_segmentor.h>

namespace rime {

namespace {

static bool CanTakeAuxiliarySuffix(const Segment& segment, size_t marker) {
  return segment.start < marker && segment.end == marker &&
         (segment.HasTag("abc") || segment.auxiliary_suffix);
}

}  // namespace

AuxiliarySuffixSegmentor::AuxiliarySuffixSegmentor(const Ticket& ticket)
    : Segmentor(ticket) {
  if (name_space_ == "segmentor")
    name_space_ = "auxiliary_suffix_segmentor";
  if (!ticket.schema)
    return;
  if (Config* config = ticket.schema->config())
    config->GetString(name_space_ + "/trigger", &trigger_);
}

bool AuxiliarySuffixSegmentor::Proceed(Segmentation* segmentation) {
  if (!segmentation || trigger_.empty())
    return true;
  const string& input = segmentation->input();
  const size_t start = segmentation->GetCurrentStartPosition();
  const size_t marker = input.find(trigger_, start);
  // 只有以小写字母开头、且触发符位于编码之后的输入才可能有 AUX 后缀。
  // 使用完整输入判断：分段推进后 start 可能已经等于 marker。
  if (marker == string::npos || marker == 0 || input.front() < 'a' ||
      input.front() > 'z') {
    return true;
  }

  // A confirmed segment leaves an empty placeholder at its end.  The suffix
  // belongs to the preceding segment, so remove that placeholder first; this
  // lets the preceding segment become the active segment again and receive a
  // fresh filtered menu.
  if (marker == start) {
    if (segmentation->size() < 2 || segmentation->back().end != marker ||
        !CanTakeAuxiliarySuffix((*segmentation)[segmentation->size() - 2],
                                marker))
      return true;
    segmentation->Trim();
  } else {
    const size_t end = segmentation->GetCurrentEndPosition();
    if (end > start && !segmentation->back().HasTag("abc") &&
        !segmentation->back().auxiliary_suffix)
      return true;
    if (end > marker)
      return true;
    if (end < marker)
      segmentation->AddSegment(Segment(start, marker));
  }

  auto& segment = segmentation->back();
  segment.auxiliary_suffix =
      Segment::AuxiliarySuffix{marker, marker + trigger_.size(), input.size()};
  segment.tags.insert("abc");
  segment.tags.insert("auxiliary_suffix");
  segment.status = Segment::kVoid;
  segment.menu.reset();
  segment.selected_index = 0;
  return false;
}

}  // namespace rime
