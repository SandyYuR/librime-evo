// Copyright RIME Developers
// Distributed under the BSD License
#ifndef RIME_PARTIAL_COMMIT_H_
#define RIME_PARTIAL_COMMIT_H_
#include <rime/processor.h>
namespace rime {
class Context;
class KeyEvent;
class PartialCommit : public Processor {
 public:
  explicit PartialCommit(const Ticket& ticket);
  ProcessResult ProcessKeyEvent(const KeyEvent& key_event) override;

 private:
  bool Matches(const KeyEvent& key_event, const string& configured_key) const;
  bool IsFunctionMode(const Context* context) const;
  bool ControlDigit(const KeyEvent& key_event, size_t* count) const;
  bool CommitPrefix(Context* context, size_t count);
  bool CommitCharacter(Context* context, bool first);
  bool control_digits_ = false;
  string first_key_;
  string last_key_;
};
}  // namespace rime
#endif
