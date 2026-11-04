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

static void ClearAuxiliarySuffixState(Segmentation* segmentation) {
  if (!segmentation)
    return;
  for (auto& segment : *segmentation) {
    if (segment.HasTag("auxiliary_suffix")) {
      segment.tags.erase("auxiliary_suffix");
      segment.status = Segment::kVoid;
      segment.menu.reset();
      segment.selected_index = 0;
    }
  }
}

static void InvalidateMainSegment(Segmentation* segmentation, size_t marker) {
  if (!segmentation)
    return;
  for (auto& segment : *segmentation) {
    if (segment.start < marker && segment.end == marker) {
      segment.tags.insert("auxiliary_suffix");
      segment.status = Segment::kVoid;
      segment.menu.reset();
      segment.selected_index = 0;
    }
  }
}

}  // namespace

AuxiliarySuffixSegmentor::AuxiliarySuffixSegmentor(const Ticket& ticket)
    : Segmentor(ticket) {
  if (!ticket.schema)
    return;
  if (Config* config = ticket.schema->config()) {
    config->GetString(name_space_ + "/trigger", &trigger_);
    if (name_space_ == "auxiliary_suffix_segmentor")
      config->GetString("translator/aux_suffix", &trigger_);
  }
}

bool AuxiliarySuffixSegmentor::Proceed(Segmentation* segmentation) {
  if (!segmentation || trigger_.empty())
    return true;
  const string& input = segmentation->input();
  const size_t start = segmentation->GetCurrentStartPosition();
  const size_t marker = input.find(trigger_, start);
  // 只有以小写字母开头、且触发符位于编码之后的输入才可能有 AUX 后缀。
  // 使用完整输入判断：分段推进后 start 可能已经等于 marker。
  if (marker == string::npos || marker == 0 || input.empty() ||
      input.front() < 'a' || input.front() > 'z') {
    ClearAuxiliarySuffixState(segmentation);
    return true;
  }

  // A confirmed segment leaves an empty placeholder at its end.  The suffix
  // belongs to the preceding segment, so remove that placeholder first; this
  // lets the preceding segment become the active segment again and receive a
  // fresh filtered menu.
  if (marker == start && !segmentation->empty() &&
      segmentation->back().start == segmentation->back().end &&
      segmentation->back().start == marker) {
    segmentation->Trim();
  }
  InvalidateMainSegment(segmentation, marker);
  if (marker > start) {
    Segment segment(start, marker);
    segment.tags.insert("abc");
    segment.tags.insert("auxiliary_suffix");
    segmentation->AddSegment(segment);
  }
  if (marker + trigger_.size() >= input.size()) {
    // Keep the trigger out of abc_segmentor until an AUX character follows.
    return false;
  }
  // The suffix is an input constraint, not a separate translatable segment.
  return false;
}

}  // namespace rime
