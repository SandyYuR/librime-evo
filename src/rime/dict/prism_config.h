// Copyright RIME Developers
// Distributed under the BSD License
//
#ifndef RIME_PRISM_CONFIG_H_
#define RIME_PRISM_CONFIG_H_

#include <rime/common.h>

namespace rime {

class Config;
class ConfigMap;

struct PrismConfig {
  struct Selection {
    string name;
    string option;
  };

  RIME_DLL bool Load(const an<ConfigMap>& settings, const string& name_space);
  RIME_DLL static bool ValidateReferences(Config* config);

  string name;
  vector<Selection> selections;
};

}  // namespace rime

#endif  // RIME_PRISM_CONFIG_H_
