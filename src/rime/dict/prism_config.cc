// Copyright RIME Developers
// Distributed under the BSD License
//
#include <rime/config.h>
#include <rime/dict/prism_config.h>

namespace rime {

bool PrismConfig::Load(const an<ConfigMap>& settings,
                       const string& name_space) {
  name.clear();
  selections.clear();
  if (!settings)
    return true;
  if (settings->HasKey("prism") && settings->HasKey("prisms")) {
    LOG(ERROR) << name_space
               << ": prism and prisms cannot be configured together";
    return false;
  }
  if (settings->HasKey("prism")) {
    auto value = settings->GetValue("prism");
    if (!value || !value->GetString(&name)) {
      LOG(ERROR) << name_space << "/prism must be a string";
      return false;
    }
  }
  if (!settings->HasKey("prisms"))
    return true;
  auto list = As<ConfigList>(settings->Get("prisms"));
  if (!list || list->empty()) {
    LOG(ERROR) << name_space << "/prisms must be a nonempty list";
    return false;
  }
  for (const auto& item : *list) {
    auto entry = As<ConfigMap>(item);
    auto prism = entry ? entry->GetValue("name") : nullptr;
    auto option = entry ? entry->GetValue("option") : nullptr;
    Selection selection;
    if (!prism || !option || !prism->GetString(&selection.name) ||
        !option->GetString(&selection.option) || selection.name.empty() ||
        selection.option.empty()) {
      LOG(ERROR) << name_space
                 << "/prisms entries require nonempty name and option";
      return false;
    }
    selections.push_back(std::move(selection));
  }
  return true;
}

namespace {

bool ValidatePrismReferences(const an<ConfigItem>& item, const string& path) {
  if (auto map = As<ConfigMap>(item)) {
    if (map->GetValue("dictionary") || map->GetValue("user_dict")) {
      PrismConfig config;
      if (!config.Load(map, path))
        return false;
    }
    for (const auto& entry : *map) {
      const string child =
          path.empty() ? entry.first : path + "/" + entry.first;
      if (!ValidatePrismReferences(entry.second, child))
        return false;
    }
  } else if (auto list = As<ConfigList>(item)) {
    for (const auto& entry : *list) {
      if (!ValidatePrismReferences(entry, path))
        return false;
    }
  }
  return true;
}

}  // namespace

bool PrismConfig::ValidateReferences(Config* config) {
  return config && ValidatePrismReferences(config->GetItem(""), "");
}

}  // namespace rime
