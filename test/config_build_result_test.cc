//
// Copyright RIME Developers
// Distributed under the BSD License
//

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <gtest/gtest.h>
#include <rime_api.h>
#include <rime/config.h>
#include <rime/deployer.h>
#include <rime/lever/deployment_tasks.h>
#include <rime/service.h>
#include <rime/setup.h>

namespace fs = std::filesystem;
using namespace rime;

class RimeConfigBuildResultTest : public ::testing::Test {
 protected:
  void SetUp() override {
    auto& deployer = Service::instance().deployer();
    old_shared_ = deployer.shared_data_dir;
    old_user_ = deployer.user_data_dir;
    old_prebuilt_ = deployer.prebuilt_data_dir;
    old_staging_ = deployer.staging_dir;
    old_sync_ = deployer.sync_dir;
    old_user_id_ = deployer.user_id;
    old_backup_config_files_ = deployer.backup_config_files;

    std::random_device random;
    for (int attempt = 0; attempt < 100 && !owned_; ++attempt) {
      root_ = fs::temp_directory_path() /
              ("rime-config-build-" + std::to_string(random()) + "-" +
               std::to_string(random()));
      owned_ = fs::create_directory(root_);
    }
    ASSERT_TRUE(owned_);
    ASSERT_TRUE(fs::create_directory(root_ / "user"));
    ASSERT_TRUE(fs::create_directory(root_ / "shared"));
    ASSERT_TRUE(fs::create_directory(root_ / "build"));

    // Components capture their resolvers at construction. Reinitialize them
    // against this test's private directories, and restore the global fixture.
    rime_get_api()->finalize();
    Initialize(root_ / "shared", root_ / "user", root_ / "shared",
               root_ / "build");
    initialized_ = true;
  }

  void TearDown() override {
    if (initialized_) {
      rime_get_api()->set_notification_handler(nullptr, nullptr);
      rime_get_api()->finalize();
      Initialize(old_shared_, old_user_, old_prebuilt_, old_staging_);
      auto& deployer = Service::instance().deployer();
      deployer.sync_dir = old_sync_;
      deployer.user_id = old_user_id_;
      deployer.backup_config_files = old_backup_config_files_;
    }
    if (owned_)
      fs::remove_all(root_);
  }

  static void Initialize(const path& shared,
                         const path& user,
                         const path& prebuilt,
                         const path& staging) {
    const auto shared_string = shared.to_utf8_string();
    const auto user_string = user.to_utf8_string();
    const auto prebuilt_string = prebuilt.to_utf8_string();
    const auto staging_string = staging.to_utf8_string();
    RIME_STRUCT(RimeTraits, traits);
    traits.shared_data_dir = shared_string.c_str();
    traits.user_data_dir = user_string.c_str();
    traits.prebuilt_data_dir = prebuilt_string.c_str();
    traits.staging_dir = staging_string.c_str();
    rime_get_api()->initialize(&traits);
    LoadModules(kDeployerModules);
  }

  void Write(const string& name, const string& content) {
    const auto file = root_ / "user" / name;
    // Build timestamps have one-second resolution. Advance source time without
    // sleeps so rewriting an existing source always requests a fresh build.
    const auto timestamp =
        fs::exists(file) ? fs::last_write_time(file) + std::chrono::seconds(2)
                         : fs::file_time_type::clock::now();
    {
      std::ofstream output(file);
      ASSERT_TRUE(output.good());
      output << content;
      ASSERT_TRUE(output.good());
    }
    fs::last_write_time(file, timestamp);
  }

  string ReadBuilt(const string& name) {
    std::ifstream input(root_ / "build" / name);
    return {std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>()};
  }

  bool Update(const string& name) {
    return rime_get_api()->deploy_config_file(name.c_str(), "schema/version");
  }

  bool UpdateWorkspace() {
    WorkspaceUpdate update;
    return update.Run(&Service::instance().deployer());
  }

  static constexpr const char* valid_schema =
      "schema:\n  schema_id: example\n  name: Example\n  version: '1'\n";
  static constexpr const char* invalid_schema =
      "schema:\n  schema_id: example\n  name: Broken\n  version: '2'\n"
      "algebra:\n  __include: missing:/rules\n";

  fs::path root_;
  bool owned_ = false;
  bool initialized_ = false;
  path old_shared_, old_user_, old_prebuilt_, old_staging_, old_sync_;
  string old_user_id_;
  bool old_backup_config_files_ = true;
};

TEST_F(RimeConfigBuildResultTest,
       FailedBuildPreservesPartialDataAndReportsFailure) {
  Write("example.schema.yaml", invalid_schema);
  the<Config> config(
      Config::Require("config_builder")->Create("example.schema"));
  ASSERT_TRUE(config);
  EXPECT_FALSE(config->IsBuildSuccessful());
  EXPECT_EQ("example", (*config)["schema"]["schema_id"].ToString());
  EXPECT_FALSE(fs::exists(root_ / "build" / "example.schema.yaml"));
  config.reset();
  EXPECT_FALSE(Update("example.schema.yaml"));

  the<Config> missing(Config::Require("config_builder")->Create("missing"));
  ASSERT_TRUE(missing);
  EXPECT_FALSE(missing->IsBuildSuccessful());
}

TEST_F(RimeConfigBuildResultTest, FailedSchemaReportsDeploymentFailure) {
  Write("default.yaml",
        "config_version: '1'\nschema_list:\n  - schema: example\n");
  Write("example.schema.yaml", invalid_schema);
  vector<string> notifications;
  rime_get_api()->set_notification_handler(
      [](void* context, RimeSessionId, const char* type, const char* value) {
        if (string(type) == "deploy")
          static_cast<vector<string>*>(context)->emplace_back(value);
      },
      &notifications);
  ASSERT_TRUE(rime_get_api()->start_maintenance(True));
  rime_get_api()->join_maintenance_thread();
  rime_get_api()->set_notification_handler(nullptr, nullptr);
  ASSERT_FALSE(notifications.empty());
  EXPECT_EQ("failure", notifications.back());
}

TEST_F(RimeConfigBuildResultTest,
       OldSchemaCannotMaskFailedRebuildAndRetrySucceeds) {
  Write("default.yaml",
        "config_version: '1'\nschema_list:\n  - schema: example\n");
  Write("example.schema.yaml", valid_schema);
  ASSERT_TRUE(UpdateWorkspace());
  const auto previous = ReadBuilt("example.schema.yaml");
  ASSERT_FALSE(previous.empty());

  Write("example.schema.yaml", invalid_schema);
  EXPECT_FALSE(UpdateWorkspace());
  EXPECT_EQ(previous, ReadBuilt("example.schema.yaml"));

  Write("example.schema.yaml", valid_schema);
  EXPECT_TRUE(UpdateWorkspace());
  the<Config> rebuilt(
      Config::Require("config_builder")->Create("example.schema"));
  ASSERT_TRUE(rebuilt);
  EXPECT_TRUE(rebuilt->IsBuildSuccessful());
}

TEST_F(RimeConfigBuildResultTest, OldDefaultCannotMaskFailedRebuild) {
  Write("default.yaml", "config_version: '1'\nschema_list: []\n");
  ASSERT_TRUE(UpdateWorkspace());
  const auto previous = ReadBuilt("default.yaml");
  ASSERT_FALSE(previous.empty());
  Write("default.yaml",
        "config_version: '2'\nschema_list:\n  __include: missing:/schemas\n");
  EXPECT_FALSE(UpdateWorkspace());
  EXPECT_EQ(previous, ReadBuilt("default.yaml"));
}

TEST_F(RimeConfigBuildResultTest, OutputWriteFailureIsNotASuccessfulBuild) {
  Write("example.schema.yaml", valid_schema);
  // A directory at the output file path fails on both Windows and POSIX,
  // including when the test user could override ordinary file permissions.
  ASSERT_TRUE(fs::create_directory(root_ / "build" / "example.schema.yaml"));
  the<Config> config(
      Config::Require("config_builder")->Create("example.schema"));
  ASSERT_TRUE(config);
  EXPECT_FALSE(config->IsBuildSuccessful());
}
