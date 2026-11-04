// Copyright RIME Developers
// Distributed under the BSD License
//
#include <rime/path_resolver.h>

#ifdef _WIN32
#include <cctype>
#endif
#include <filesystem>
#include <system_error>

namespace rime {
namespace safe_path {
namespace {
namespace fs = std::filesystem;

bool Within(const fs::path& root, const fs::path& candidate) {
  auto a = root.begin();
  auto b = candidate.begin();
  for (; a != root.end(); ++a, ++b) {
    if (b == candidate.end() || *a != *b)
      return false;
  }
  return true;
}

path CanonicalDirectory(const path& root) {
  if (root.empty())
    return {};
  std::error_code ec;
  path result(fs::canonical(root, ec));
  if (ec || !fs::is_directory(result, ec) || ec)
    return {};
  return result;
}

// A configured root may itself be a symlink (e.g. a synced volume).
// Reject traversal in the caller's spelling, then map the relative portion
// onto the canonical root before inspecting components underneath it.
path UnderRootRelative(const path& declared_root,
                       const path& canonical_root,
                       const path& candidate) {
  if (candidate.empty())
    return {};
  for (const auto& part : candidate)
    if (part == "..")
      return {};
  std::error_code ec;
  const path declared(fs::absolute(declared_root, ec).lexically_normal());
  if (ec)
    return {};
  const path actual(fs::absolute(candidate, ec).lexically_normal());
  if (ec)
    return {};
  fs::path relative;
  if (Within(declared, actual))
    relative = actual.lexically_relative(declared);
  else if (Within(canonical_root, actual))
    relative = actual.lexically_relative(canonical_root);
  else
    return {};
  if (relative.empty() || relative == ".")
    return {};
  const path resource(relative);
  return ValidRelativeName(resource.generic_to_utf8_string()) ? resource
                                                              : path();
}

bool NoSymlinksBelow(const path& root,
                     const fs::path& relative,
                     bool create_parents) {
  std::error_code ec;
  auto current = fs::path(root);
  for (const auto& part : relative) {
    current /= part;
    const auto status = fs::symlink_status(current, ec);
    if (ec == std::errc::no_such_file_or_directory ||
        (!ec && status.type() == fs::file_type::not_found)) {
      ec.clear();
      if (!create_parents)
        return false;
      fs::create_directory(current, ec);
      if (ec)
        return false;
    } else if (ec || !fs::is_directory(status) || fs::is_symlink(status)) {
      return false;
    }
  }
  return true;
}

}  // namespace

bool ValidRelativeName(const std::string& name, bool allow_subdirs) {
  if (name.empty() || name.size() > 4096 || name.back() == '/')
    return false;
  if (name.front() == '/' || name.find('\\') != std::string::npos ||
      name.find('\0') != std::string::npos)
    return false;
  for (unsigned char c : name) {
    // Colon blocks Windows drive paths and alternate data streams; controls
    // and backslashes are invalid on all hosts to keep shared sync portable.
    if (c < 0x20 || c == 0x7f || c == ':')
      return false;
  }
  if (!allow_subdirs && name.find('/') != std::string::npos)
    return false;
  size_t start = 0;
  while (start < name.size()) {
    const auto end = name.find('/', start);
    const auto length = (end == std::string::npos ? name.size() : end) - start;
    if (length == 0)
      return false;
    const auto part = name.substr(start, length);
    if (part == "." || part == ".." || part.back() == '.' || part.back() == ' ')
      return false;
#ifdef _WIN32
    // 设备名限制仅适用于 Windows；Linux 的 aux.txt 是正常文件。
    std::string stem = part.substr(0, part.find('.'));
    for (auto& ch : stem)
      ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    const bool dos_device =
        stem == "CON" || stem == "PRN" || stem == "AUX" || stem == "NUL" ||
        (stem.size() == 4 &&
         (stem.substr(0, 3) == "COM" || stem.substr(0, 3) == "LPT") &&
         stem[3] >= '1' && stem[3] <= '9');
    if (dos_device)
      return false;
#endif
    if (end == std::string::npos)
      break;
    start = end + 1;
  }
  return true;
}

path ExistingFile(const path& root, const std::string& relative_name) {
  if (!ValidRelativeName(relative_name))
    return {};
  const path base = CanonicalDirectory(root);
  if (base.empty())
    return {};
  std::error_code ec;
  const path target(fs::canonical(base / path(relative_name), ec));
  if (ec || !Within(base, target) || !fs::is_regular_file(target, ec) || ec)
    return {};
  return target;
}

path WritableFile(const path& root,
                  const std::string& relative_name,
                  bool create_parents) {
  if (!ValidRelativeName(relative_name))
    return {};
  const path base = CanonicalDirectory(root);
  if (base.empty())
    return {};
  const auto relative = fs::path(path(relative_name));
  if (!NoSymlinksBelow(base, relative.parent_path(), create_parents))
    return {};
  const path target(base / relative);
  std::error_code ec;
  // Check the resolved parent too: Windows junctions/reparse points are not
  // universally reported as symlinks by every std::filesystem implementation.
  const path actual_parent(fs::canonical(target.parent_path(), ec));
  if (ec || actual_parent != target.parent_path())
    return {};
  const auto status = fs::symlink_status(target, ec);
  if (ec == std::errc::no_such_file_or_directory ||
      (!ec && status.type() == fs::file_type::not_found)) {
    ec.clear();
    // A new regular file may be created here.
  } else if (ec || fs::is_symlink(status) || !fs::is_regular_file(status)) {
    return {};
  } else if (fs::hard_link_count(target, ec) != 1 || ec) {
    // A hardlink to a file outside root could otherwise be overwritten.
    return {};
  }
  return target;
}

bool SafeDatabaseLocation(const path& root, const path& actual_path) {
  const path base = CanonicalDirectory(root);
  if (base.empty())
    return false;
  const path relative = UnderRootRelative(root, base, actual_path);
  if (relative.empty() || !NoSymlinksBelow(base, relative.parent_path(), false))
    return false;
  std::error_code ec;
  const path target(base / relative);
  const path actual_parent(fs::canonical(target.parent_path(), ec));
  if (ec || actual_parent != target.parent_path())
    return false;
  const auto status = fs::symlink_status(target, ec);
  if (ec == std::errc::no_such_file_or_directory)
    return true;
  if (ec)
    return false;
  if (fs::is_regular_file(status)) {
    const path actual(fs::canonical(target, ec));
    return !ec && actual == target && fs::hard_link_count(target, ec) == 1 &&
           !ec;
  }
  if (fs::is_directory(status)) {
    const path actual(fs::canonical(target, ec));
    return !ec && actual == target;
  }
  return status.type() == fs::file_type::not_found;
}

path ExistingFileNoSymlinks(const path& root, const path& candidate) {
  const path base = CanonicalDirectory(root);
  if (base.empty())
    return {};
  const path relative = UnderRootRelative(root, base, candidate);
  if (relative.empty() || !NoSymlinksBelow(base, relative.parent_path(), false))
    return {};
  std::error_code ec;
  const path target(base / relative);
  const path actual_parent(fs::canonical(target.parent_path(), ec));
  if (ec || actual_parent != target.parent_path())
    return {};
  const auto status = fs::symlink_status(target, ec);
  if (ec || fs::is_symlink(status) || !fs::is_regular_file(status))
    return {};
  const path canonical(fs::canonical(target, ec));
  if (ec || canonical != target || !Within(base, canonical))
    return {};
  return canonical;
}

}  // namespace safe_path
}  // namespace rime
