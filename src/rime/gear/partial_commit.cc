// Copyright RIME Developers
// Distributed under the BSD License
#include <algorithm>
#include <utf8.h>
#include <rime/candidate.h>
#include <rime/config.h>
#include <rime/context.h>
#include <rime/engine.h>
#include <rime/gear/partial_commit.h>
#include <rime/gear/translator_commons.h>
#include <rime/key_event.h>
#include <rime/schema.h>
namespace rime {
namespace {
string FirstCharacter(const string& text) {
  if (text.empty())
    return string();
  const char* begin = text.data();
  const char* end = begin;
  utf8::unchecked::next(end);
  return string(begin, end);
}
string LastCharacter(const string& text) {
  if (text.empty())
    return string();
  const char* end = text.data() + text.size();
  const char* begin = end;
  utf8::unchecked::prior(begin);
  return string(begin, end);
}
size_t CharacterCount(const string& text) {
  size_t count = 0;
  const char* current = text.data();
  const char* end = current + text.size();
  while (current < end) {
    utf8::unchecked::next(current);
    ++count;
  }
  return count;
}
string FirstCharacters(const string& text, size_t count) {
  if (text.empty() || count == 0)
    return string();
  const char* current = text.data();
  const char* end = current + text.size();
  while (count-- > 0 && current < end)
    utf8::unchecked::next(current);
  return string(text.data(), current);
}
an<Candidate> CurrentCandidate(const Context* context) {
  if (!context || context->composition().empty())
    return nullptr;
  const Segment& segment = context->composition().back();
  return segment.GetSelectedCandidate();
}
string ConfirmedPrefix(const Context* context) {
  if (!context || context->composition().size() < 2)
    return string();
  const string& input = context->input();
  string prefix;
  const auto& composition = context->composition();
  for (size_t i = 0; i + 1 < composition.size(); ++i) {
    const Segment& segment = composition[i];
    if (auto candidate = segment.GetSelectedCandidate()) {
      prefix += candidate->text();
    } else if (!segment.HasTag("phony")) {
      prefix += input.substr(segment.start, segment.end - segment.start);
    }
  }
  return prefix;
}
bool GetPhysicalStops(const Segment& segment,
                      const an<Candidate>& candidate,
                      vector<size_t>* stops) {
  if (!candidate || !stops)
    return false;
  auto phrase = As<Phrase>(Candidate::GetGenuineCandidate(candidate));
  if (!phrase)
    return false;
  const Spans spans = phrase->spans();
  for (size_t vertex : spans.vertices()) {
    if (vertex >= segment.start && vertex <= segment.end)
      stops->push_back(vertex);
  }
  return stops->size() >= 2;
}
}  // namespace
PartialCommit::PartialCommit(const Ticket& ticket) : Processor(ticket) {
  if (!ticket.schema || !ticket.schema->config())
    return;
  Config* config = ticket.schema->config();
  config->GetBool("partial_commit/control_digits", &control_digits_);
  config->GetString("partial_commit/first_key", &first_key_);
  config->GetString("partial_commit/last_key", &last_key_);
}
bool PartialCommit::Matches(const KeyEvent& key_event,
                            const string& configured_key) const {
  if (configured_key.empty())
    return false;
  if (configured_key.size() == 1 &&
      key_event.keycode() == static_cast<unsigned char>(configured_key[0]))
    return true;
  return key_event.repr() == configured_key;
}
bool PartialCommit::IsFunctionMode(const Context* context) const {
  if (!context || context->composition().empty())
    return false;
  const Segment& segment = context->composition().back();
  return segment.HasAnyTagIn({"number", "unicode", "calculator", "shijian"});
}
bool PartialCommit::ControlDigit(const KeyEvent& key_event,
                                 size_t* count) const {
  if (!count || !key_event.ctrl() || key_event.release() || key_event.alt() ||
      key_event.super())
    return false;
  if (key_event.keycode() >= XK_0 && key_event.keycode() <= XK_9) {
    *count = key_event.keycode() == XK_0
                 ? 10
                 : static_cast<size_t>(key_event.keycode() - XK_0);
    return true;
  }
  if (key_event.keycode() >= XK_KP_0 && key_event.keycode() <= XK_KP_9) {
    *count = key_event.keycode() == XK_KP_0
                 ? 10
                 : static_cast<size_t>(key_event.keycode() - XK_KP_0);
    return true;
  }
  return false;
}
bool PartialCommit::CommitPrefix(Context* context, size_t count) {
  if (!context || count == 0 || context->composition().empty())
    return false;
  const Segment& segment = context->composition().back();
  auto candidate = CurrentCandidate(context);
  if (!candidate || candidate->text().empty())
    return false;
  vector<size_t> stops;
  if (!GetPhysicalStops(segment, candidate, &stops))
    return false;
  const size_t available_syllables = stops.size() - 1;
  const size_t candidate_length = CharacterCount(candidate->text());
  count = (std::min)(count, available_syllables);
  count = (std::min)(count, candidate_length);
  if (count == 0)
    return false;
  const size_t cut = stops[count];
  const string& input = context->input();
  if (cut > input.size())
    return false;
  string rest = input.substr(cut);
  if (!rest.empty() && (rest.front() == '\'' || rest.front() == ' '))
    rest.erase(0, 1);
  const string committed =
      ConfirmedPrefix(context) + FirstCharacters(candidate->text(), count);
  LOG(INFO) << "[partial-commit] action=prefix count=" << count
            << " text=" << candidate->text() << " commit=" << committed
            << " rest=" << rest;
  engine_->CommitText(committed);
  context->Clear();
  if (!rest.empty())
    context->set_input(rest);
  return true;
}
bool PartialCommit::CommitCharacter(Context* context, bool first) {
  if (!context || !context->IsComposing() || IsFunctionMode(context))
    return false;
  string text = context->input();
  if (auto candidate = context->GetSelectedCandidate())
    text = candidate->text();
  if (text.empty())
    return false;
  string character = first ? FirstCharacter(text) : LastCharacter(text);
  if (character.empty() || character == text)
    return false;
  LOG(INFO) << "[partial-commit] action=" << (first ? "first" : "last")
            << " text=" << text << " commit=" << character;
  engine_->CommitText(character);
  context->Clear();
  return true;
}
ProcessResult PartialCommit::ProcessKeyEvent(const KeyEvent& key_event) {
  if (key_event.release() || key_event.alt() || key_event.super())
    return kNoop;
  Context* context = engine_ ? engine_->context() : nullptr;
  if (!context || !context->IsComposing() || IsFunctionMode(context))
    return kNoop;
  if (control_digits_) {
    size_t count = 0;
    if (ControlDigit(key_event, &count))
      return CommitPrefix(context, count) ? kAccepted : kNoop;
  }
  if (key_event.ctrl())
    return kNoop;
  const bool select_first = Matches(key_event, first_key_);
  const bool select_last = Matches(key_event, last_key_);
  if (!select_first && !select_last)
    return kNoop;
  return CommitCharacter(context, select_first) ? kAccepted : kNoop;
}
}  // namespace rime
