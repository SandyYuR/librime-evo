// Copyright RIME Developers
// Distributed under the BSD License
//
#include <gtest/gtest.h>
#include <rime/path_resolver.h>

#include <filesystem>
#include <fstream>
#include <string>

namespace rime {
namespace {
namespace fs = std::filesystem;

class SafePathTest : public ::testing::Test {
 protected:
  void SetUp() override {
    auto seed = fs::temp_directory_path() / fs::path("rime-path-test");
    for (int n = 0; n < 500; ++n) {
      root_ = path(seed.string() + "-" + std::to_string(n));
      std::error_code ec;
      if (fs::create_directory(root_, ec))
        break;
    }
    ASSERT_TRUE(fs::is_directory(root_));
    fs::create_directory(root_ / "data");
    std::ofstream(static_cast<const std::filesystem::path&>(root_ / "data" /
                                                            "source.txt"))
        << "text";
    outside_ = root_;
    outside_ += "-outside";
    fs::create_directories(outside_);
    std::ofstream(
        static_cast<const std::filesystem::path&>(outside_ / "secret.txt"))
        << "secret";
  }
  void TearDown() override {
    std::error_code ec;
    fs::remove_all(root_, ec);
    fs::remove_all(outside_, ec);
  }
  path root_, outside_;
};

TEST_F(SafePathTest, RejectsUnsafeNames) {
  for (const std::string& s :
       {"", "../secret", "data/../source", "./data", "/etc/passwd", "C:/secret",
        "\\\\server\\path", "data//source", "data/", "data\\file", "data/."}) {
    EXPECT_FALSE(safe_path::ValidRelativeName(s)) << s;
  }
  EXPECT_TRUE(safe_path::ValidRelativeName("lua/sequence"));
  EXPECT_TRUE(safe_path::ValidRelativeName("data/source.txt"));
  EXPECT_FALSE(safe_path::ValidRelativeName("lua/sequence", false));
  EXPECT_TRUE(safe_path::ValidRelativeName("中文/词表.txt"));
#ifdef _WIN32
  EXPECT_FALSE(safe_path::ValidRelativeName("con.txt"));
  EXPECT_FALSE(safe_path::ValidRelativeName("aux.txt"));
  EXPECT_FALSE(safe_path::ValidRelativeName("aux.dict.yaml"));
#else
  EXPECT_TRUE(safe_path::ValidRelativeName("con.txt"));
  EXPECT_TRUE(safe_path::ValidRelativeName("aux.txt"));
  EXPECT_TRUE(safe_path::ValidRelativeName("aux.dict.yaml"));
  const path backup = root_ / "backup";
  fs::create_directory(backup);
  EXPECT_FALSE(safe_path::WritableFile(backup, "aux.txt").empty());
  EXPECT_FALSE(safe_path::WritableFile(backup, "aux.dict.yaml").empty());
#endif
}

TEST_F(SafePathTest, ExistingFileIsRootConfined) {
  EXPECT_FALSE(safe_path::ExistingFile(root_, "data/source.txt").empty());
  EXPECT_TRUE(safe_path::ExistingFile(root_, "../secret.txt").empty());
  std::error_code ec;
  fs::create_directory_symlink(outside_, root_ / "link_out", ec);
  if (!ec) {
    EXPECT_TRUE(safe_path::ExistingFile(root_, "link_out/secret.txt").empty());
    EXPECT_TRUE(
        safe_path::WritableFile(root_, "link_out/target.tsv", true).empty());
  }
  fs::create_directory_symlink(root_ / "data", root_ / "link_in", ec);
  if (!ec) {
    EXPECT_FALSE(safe_path::ExistingFile(root_, "link_in/source.txt").empty());
    EXPECT_TRUE(
        safe_path::WritableFile(root_, "link_in/target.tsv", false).empty());
  }
}

TEST_F(SafePathTest, WritableRejectsSymlinkTarget) {
  const path target =
      safe_path::WritableFile(root_, "export/lua/stats.tsv", true);
  ASSERT_FALSE(target.empty());
  EXPECT_TRUE(fs::is_directory(root_ / "export" / "lua"));
  std::error_code ec;
  fs::create_symlink(outside_ / "secret.txt",
                     root_ / "export" / "lua" / "escape.tsv", ec);
  if (!ec)
    EXPECT_TRUE(
        safe_path::WritableFile(root_, "export/lua/escape.tsv").empty());
  EXPECT_TRUE(safe_path::SafeDatabaseLocation(root_, root_ / "lua.userdb"));
  fs::create_directory(root_ / "lua.userdb");
  EXPECT_TRUE(safe_path::SafeDatabaseLocation(root_, root_ / "lua.userdb"));
}

TEST_F(SafePathTest, ATrustedRootMayItselfBeASymlink) {
  path alias = root_;
  alias += "-alias";
  std::error_code ec;
  fs::create_directory_symlink(root_, alias, ec);
  if (ec)
    return;
  EXPECT_FALSE(
      safe_path::ExistingFileNoSymlinks(alias, alias / "data/source.txt")
          .empty());
  EXPECT_TRUE(safe_path::SafeDatabaseLocation(alias, alias / "new.userdb"));
  EXPECT_FALSE(
      safe_path::WritableFile(alias, "export/result.tsv", true).empty());
  fs::remove(alias, ec);
}

TEST_F(SafePathTest, RestoreNeedsNonSymlinkPath) {
  EXPECT_FALSE(
      safe_path::ExistingFileNoSymlinks(root_, root_ / "data/source.txt")
          .empty());
  EXPECT_TRUE(safe_path::ExistingFileNoSymlinks(root_, outside_ / "secret.txt")
                  .empty());
  std::error_code ec;
  fs::create_symlink(outside_ / "secret.txt", root_ / "redirect.txt", ec);
  if (!ec)
    EXPECT_TRUE(safe_path::ExistingFileNoSymlinks(root_, root_ / "redirect.txt")
                    .empty());
}

}  // namespace
}  // namespace rime
