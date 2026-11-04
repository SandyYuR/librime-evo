//
// Copyright RIME Developers
// Distributed under the BSD License
//
#include <rime/config.h>
#include <rime/context.h>
#include <rime/engine.h>
#include <rime/key_event.h>
#include <rime/schema.h>
#include <rime/gear/keypad_processor.h>
namespace rime {
namespace {
bool IsKeypadDigit(int keycode) {
  return keycode >= XK_KP_0 && keycode <= XK_KP_9;
}
bool IsRawCharacter(int keycode) {
  return (keycode >= XK_0 && keycode <= XK_9) ||
         (keycode >= XK_a && keycode <= XK_z) ||
         (keycode >= XK_A && keycode <= XK_Z);
}
char DigitFromKeycode(int keycode) {
  if (keycode >= XK_KP_0 && keycode <= XK_KP_9)
    return static_cast<char>('0' + keycode - XK_KP_0);
  return static_cast<char>('0' + keycode - XK_0);
}
char RawCharacterFromKeyEvent(const KeyEvent& key_event) {
  int keycode = key_event.keycode();
  char ch = static_cast<char>(keycode);
  if (keycode >= XK_a && keycode <= XK_z &&
      (key_event.shift() || key_event.caps())) {
    ch = static_cast<char>(ch - 'a' + 'A');
  } else if (keycode >= XK_A && keycode <= XK_Z && !key_event.shift() &&
             !key_event.caps()) {
    ch = static_cast<char>(ch - 'A' + 'a');
  }
  return ch;
}
void MarkInputAsRaw(Context* context) {
  Composition& composition = context->composition();
  composition.clear();
  Segment raw(0, context->input().length());
  raw.status = Segment::kSelected;
  raw.tags.insert("raw");
  composition.push_back(raw);
  composition.Forward();
}
}  // namespace
const char* KeypadProcessor::ModeName(Mode mode) {
  switch (mode) {
    case kAuto:
      return "auto";
    case kCompose:
      return "compose";
    case kSelect:
      return "select";
  }
  return "select";
}
KeypadProcessor::KeypadProcessor(const Ticket& ticket) : Processor(ticket) {
  if (!ticket.schema || !ticket.schema->config())
    return;
  string mode;
  if (!ticket.schema->config()->GetString("keypad_processor/mode", &mode))
    return;
  if (mode == "auto")
    mode_ = kAuto;
  else if (mode == "compose")
    mode_ = kCompose;
  else if (mode == "select")
    mode_ = kSelect;
  else
    LOG(WARNING) << "invalid keypad_processor/mode: " << mode
                 << "; using select.";
}
ProcessResult KeypadProcessor::ProcessKeyEvent(const KeyEvent& key_event) {
  Context* context = engine_ ? engine_->context() : nullptr;
  if (!context)
    return kNoop;
  if (!context->IsComposing())
    raw_mode_ = false;
  if (raw_mode_ && key_event.keycode() == XK_BackSpace &&
      !key_event.release() && !key_event.shift() && !key_event.ctrl() &&
      !key_event.alt() && !key_event.super()) {
    if (!context->PopInput())
      return kAccepted;
    if (context->input().empty())
      raw_mode_ = false;
    else
      MarkInputAsRaw(context);
    LOG(INFO) << "[keypad-processor] raw backspace action=delete input="
              << context->input();
    return kAccepted;
  }
  if (!IsKeypadDigit(key_event.keycode())) {
    if (raw_mode_ && !key_event.release() && !key_event.ctrl() &&
        !key_event.alt() && !key_event.super() &&
        IsRawCharacter(key_event.keycode())) {
      char ch = RawCharacterFromKeyEvent(key_event);
      if (!context->PushInput(ch))
        return kNoop;
      MarkInputAsRaw(context);
      LOG(INFO) << "[keypad-processor] raw character=" << ch
                << " action=append";
      return kAccepted;
    }
    return kNoop;
  }
  if (key_event.release() || key_event.shift() || key_event.ctrl() ||
      key_event.alt() || key_event.super())
    return kNoop;
  const char digit = DigitFromKeycode(key_event.keycode());
  const bool composing = context->IsComposing();
  const bool compose = mode_ == kCompose || (mode_ == kAuto && composing);
  if (!compose) {
    LOG(INFO) << "[keypad-processor] key=" << key_event.repr()
              << " mode=" << ModeName(mode_) << " composing=" << composing
              << " action=pass";
    return kNoop;
  }
  if (!context->PushInput(digit))
    return kNoop;
  raw_mode_ = true;
  MarkInputAsRaw(context);
  LOG(INFO) << "[keypad-processor] key=" << key_event.repr()
            << " mode=" << ModeName(mode_) << " composing=" << composing
            << " action=compose digit=" << digit;
  return kAccepted;
}
}  // namespace rime
