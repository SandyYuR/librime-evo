// Copyright RIME Developers
// Distributed under the BSD License
//
#ifndef RIME_PRISM_CONFIG_H_
#define RIME_PRISM_CONFIG_H_

#include <rime/common.h>

namespace rime {

class Config;
class Context;

struct PrismConfig {
  struct Selection {
    string name;
    string option;
  };

  RIME_DLL bool Load(Config* schema, const string& name_space);
  RIME_DLL static bool ValidateReferences(Config* config);

  string name;
  vector<Selection> selections;
  bool follows_input_types = false;
};

struct InputTypesConfig {
  RIME_DLL bool Load(Config* config);
  RIME_DLL bool Contains(const string& option) const;
  RIME_DLL bool IsEnabled(const string& option) const;
  RIME_DLL string InitialOption(const Context* context) const;

  vector<PrismConfig::Selection> options;
  vector<PrismConfig::Selection> enabled;
  string reset;
};

}  // namespace rime

#endif  // RIME_PRISM_CONFIG_H_
