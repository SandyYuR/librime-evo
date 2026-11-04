// Copyright RIME Developers
// Distributed under the BSD License
//
// Root-confined paths for configuration-supplied resource names.
#ifndef RIME_PATH_RESOLVER_H_
#define RIME_PATH_RESOLVER_H_

#include <string>
#include <rime_api.h>
#include <rime/common.h>

namespace rime {
namespace safe_path {

// Names are relative UTF-8 resource identifiers, never filesystem paths.
// Allows nested names such as "lua/sequence" when allow_subdirs is true.
// Invalid names are rejected instead of silently rewritten.
RIME_DLL bool ValidRelativeName(const std::string& name,
                                bool allow_subdirs = true);

// Resolve an existing regular file under an existing root. In-root symlinks
// are permitted; symlinks escaping the root are rejected after
// canonicalization. An empty path indicates failure.
RIME_DLL path ExistingFile(const path& root, const std::string& relative_name);

// Prepare a location for a new/replaceable regular file under an existing root.
// Optionally creates subdirectories. Every subdirectory and an existing target
// must be non-symlink; neither file types nor parent traversal are allowed.
// Returns an empty path on failure. This is best-effort path confinement, NOT
// an atomic protection against a hostile process concurrently swapping dirs.
RIME_DLL path WritableFile(const path& root,
                           const std::string& relative_name,
                           bool create_parents = false);

// Validate the actual file/directory backing a DB, created from a name +
// extension by the backend. Existing locations must not be symlinks; an absent
// final component is allowed for DB creation. No parent is created.
RIME_DLL bool SafeDatabaseLocation(const path& root, const path& actual_path);

// For caller-supplied paths (e.g. Restore), require a regular file contained
// in root and reject symlinks in all components below root. Returns empty on
// failure. Caller may separately check allowed relative file naming/layout.
RIME_DLL path ExistingFileNoSymlinks(const path& root, const path& candidate);

}  // namespace safe_path
}  // namespace rime

#endif  // RIME_PATH_RESOLVER_H_
