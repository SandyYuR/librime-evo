// Copyright RIME Developers
// Distributed under the BSD License
//
#include <algorithm>
#include <rime/config.h>
#include <rime/context.h>
#include <rime/dict/prism_config.h>

namespace rime {

bool InputTypesConfig::Contains(const string& option) const {
  return std::any_of(options.begin(), options.end(),
                     [&](const auto& item) { return item.option == option; });
}

bool InputTypesConfig::IsEnabled(const string& option) const {
  return std::any_of(enabled.begin(), enabled.end(),
                     [&](const auto& item) { return item.option == option; });
}

string InputTypesConfig::InitialOption(const Context* context) const {
  if (!reset.empty())
    return reset;
  if (context) {
    for (const auto& item : enabled) {
      if (context->get_option(item.option))
        return item.option;
    }
  }
  return enabled.empty() ? string() : enabled.front().option;
}

bool InputTypesConfig::Load(Config* config) {
  options.clear();
  enabled.clear();
  reset.clear();
  if (!config || config->IsNull("input_types"))
    return true;
  auto settings = config->GetMap("input_types");
  auto list = settings ? As<ConfigList>(settings->Get("options")) : nullptr;
  if (!list) {
    LOG(ERROR) << "input_types/options must be a list";
    return false;
  }
  set<string> names;
  for (const auto& item : *list) {
    auto entry = As<ConfigMap>(item);
    auto value = entry ? entry->GetValue("option") : nullptr;
    PrismConfig::Selection selection;
    if (!value || !value->GetString(&selection.option) ||
        selection.option.empty() || !names.insert(selection.option).second) {
      LOG(ERROR) << "input_types/options require unique nonempty option names";
      return false;
    }
    selection.name = selection.option;
    if (entry->HasKey("name")) {
      auto name = entry->GetValue("name");
      if (!name || !name->GetString(&selection.name) ||
          selection.name.empty()) {
        LOG(ERROR) << "input_types/options name must be a nonempty prism name";
        return false;
      }
    }
    options.push_back(std::move(selection));
  }
  if (settings->HasKey("enabled")) {
    auto selected = As<ConfigList>(settings->Get("enabled"));
    if (!selected) {
      LOG(ERROR) << "input_types/enabled must be a list";
      return false;
    }
    set<string> unique;
    for (const auto& item : *selected) {
      auto value = As<ConfigValue>(item);
      const string option = value ? value->str() : string();
      if (!Contains(option) || !unique.insert(option).second) {
        LOG(ERROR) << "unknown or repeated input_types/enabled option: "
                   << option;
        return false;
      }
      for (const auto& candidate : options) {
        if (candidate.option == option) {
          enabled.push_back(candidate);
          break;
        }
      }
    }
  } else {
    enabled = options;
  }
  if (settings->HasKey("reset")) {
    auto value = settings->GetValue("reset");
    if (!value || !value->GetString(&reset) || !IsEnabled(reset)) {
      LOG(ERROR) << "input_types/reset must name an enabled option";
      return false;
    }
  }
  return true;
}

bool PrismConfig::Load(Config* schema, const string& name_space) {
  name.clear();
  selections.clear();
  follows_input_types = false;
  auto settings = schema ? schema->GetMap(name_space) : nullptr;
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
  if (auto reference = settings->GetValue("prisms")) {
    InputTypesConfig types;
    if (reference->str() != "input_types" || !types.Load(schema) ||
        types.enabled.empty()) {
      LOG(ERROR) << name_space
                 << "/prisms requires a nonempty input_types group";
      return false;
    }
    selections = std::move(types.enabled);
    follows_input_types = true;
    return true;
  }
  auto list = As<ConfigList>(settings->Get("prisms"));
  if (!list || list->empty()) {
    LOG(ERROR) << name_space
               << "/prisms must be input_types or a nonempty list";
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
bool ValidatePrismReferences(Config* schema,
                             const an<ConfigItem>& item,
                             const string& path) {
  if (auto map = As<ConfigMap>(item)) {
    if (map->GetValue("dictionary") || map->GetValue("user_dict")) {
      PrismConfig config;
      if (!config.Load(schema, path))
        return false;
    }
    for (const auto& entry : *map) {
      const string child =
          path.empty() ? entry.first : path + "/" + entry.first;
      if (!ValidatePrismReferences(schema, entry.second, child))
        return false;
    }
  } else if (auto list = As<ConfigList>(item)) {
    for (const auto& entry : *list) {
      if (!ValidatePrismReferences(schema, entry, path))
        return false;
    }
  }
  return true;
}
}  // namespace

bool PrismConfig::ValidateReferences(Config* config) {
  InputTypesConfig types;
  return config && types.Load(config) &&
         ValidatePrismReferences(config, config->GetItem(""), "");
}

}  // namespace rime
