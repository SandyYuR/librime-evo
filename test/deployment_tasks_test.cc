//
// Copyright RIME Developers
// Distributed under the BSD License
//

#include <gtest/gtest.h>
#include <rime/component.h>
#include <rime/deployer.h>
#include <rime/dict/prism.h>
#include <rime/context.h>
#include <rime/engine.h>
#include <rime/gear/rewriter.h>
#include <rime/lever/deployment_tasks.h>
#include <rime/service.h>
#include <rime_api.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <system_error>
#include "rime/config/config_component.h"
#include "rime/registry.h"

namespace fs = std::filesystem;
using namespace rime;

class RimeDeploymentTasksTest : public ::testing::Test {
 protected:
  RimeDeploymentTasksTest() = default;

  void CleanUp() {
    std::error_code ec;
    fs::remove("installation.yaml", ec);
  }
  virtual void SetUp() { CleanUp(); }
  virtual void TearDown() { CleanUp(); }
};

TEST_F(RimeDeploymentTasksTest, InstallationUpdate) {
  Registry& r = Registry::instance();
  r.Register("installation_update", new Component<InstallationUpdate>);
  // initialization
  string sync_dir;
  {
    rime::Deployer deployer;
    deployer.distribution_code_name = "rime-test";
    deployer.distribution_name = "rime-test";
    deployer.distribution_version = "1.0.0";
    EXPECT_TRUE(
        deployer.RunTask("installation_update", rime::TaskInitializer()));
    Config config;
    EXPECT_TRUE(config.LoadFromFile(path("installation.yaml")));
    EXPECT_EQ(config["distribution_code_name"].ToString(), "rime-test");
    EXPECT_EQ(config["distribution_name"].ToString(), "rime-test");
    EXPECT_EQ(config["distribution_version"].ToString(), "1.0.0");
    EXPECT_TRUE(config.HasKey("backup_config_files"));
    EXPECT_TRUE(config.HasKey("install_time"));
    EXPECT_TRUE(config.HasKey("installation_id"));
    EXPECT_TRUE(config.HasKey("rime_version"));
    EXPECT_TRUE(config.HasKey("sync_dir"));
    sync_dir = config["sync_dir"].ToString();
    EXPECT_EQ(path(sync_dir), path(".") / "sync");
    EXPECT_FALSE(config.HasKey("update_time"));
  }
  // update
  string update_time;
  {
    rime::Deployer deployer;
    deployer.distribution_code_name = "rime-test";
    deployer.distribution_name = "rime-test";
    deployer.distribution_version = "2.0.0";
    EXPECT_TRUE(
        deployer.RunTask("installation_update", rime::TaskInitializer()));
    Config config;
    EXPECT_TRUE(config.LoadFromFile(path("installation.yaml")));
    EXPECT_EQ(config["distribution_code_name"].ToString(), "rime-test");
    EXPECT_EQ(config["distribution_name"].ToString(), "rime-test");
    EXPECT_EQ(config["distribution_version"].ToString(), "2.0.0");
    EXPECT_TRUE(config.HasKey("backup_config_files"));
    EXPECT_TRUE(config.HasKey("install_time"));
    EXPECT_TRUE(config.HasKey("installation_id"));
    EXPECT_TRUE(config.HasKey("rime_version"));
    EXPECT_TRUE(config.HasKey("sync_dir"));
    EXPECT_EQ(config["sync_dir"].ToString(), sync_dir);
    EXPECT_TRUE(config.HasKey("update_time"));
    update_time = config["update_time"].ToString();
    EXPECT_TRUE(!update_time.empty());
  }
  // no update
  {
    rime::Deployer deployer;
    deployer.distribution_code_name = "rime-test";
    deployer.distribution_name = "rime-test";
    deployer.distribution_version = "2.0.0";
    EXPECT_TRUE(
        deployer.RunTask("installation_update", rime::TaskInitializer()));
    Config config;
    EXPECT_TRUE(config.LoadFromFile(path("installation.yaml")));
    EXPECT_EQ(config["update_time"].ToString(), update_time);
    EXPECT_TRUE(config.HasKey("sync_dir"));
    EXPECT_EQ(config["sync_dir"].ToString(), sync_dir);
  }
}

TEST_F(RimeDeploymentTasksTest, InstallationUpdateRecover) {
  Registry& r = Registry::instance();
  r.Register("installation_update", new Component<InstallationUpdate>);
  // write a corrupted installation.yaml
  {
    std::ofstream fout("installation.yaml");
    fout << "|\n  abc";  // root node is a scalar
    fout.close();
    Config config;
    config.LoadFromFile(path("installation.yaml"));
    EXPECT_TRUE(config.IsValue(""));
  }
  {
    rime::Deployer deployer;
    deployer.distribution_code_name = "rime-test";
    deployer.distribution_name = "rime-test";
    deployer.distribution_version = "2.0.0";
    EXPECT_TRUE(
        deployer.RunTask("installation_update", rime::TaskInitializer()));
    Config config;
    EXPECT_TRUE(config.LoadFromFile(path("installation.yaml")));
    EXPECT_EQ(config["distribution_code_name"].ToString(), "rime-test");
    EXPECT_EQ(config["distribution_name"].ToString(), "rime-test");
    EXPECT_EQ(config["distribution_version"].ToString(), "2.0.0");
    EXPECT_TRUE(config.HasKey("backup_config_files"));
    EXPECT_TRUE(config.HasKey("install_time"));
    EXPECT_TRUE(config.HasKey("installation_id"));
    EXPECT_TRUE(config.HasKey("rime_version"));
    EXPECT_TRUE(config.HasKey("sync_dir"));
    EXPECT_FALSE(config.HasKey("update_time"));  // as if it is newly installed
  }
}

class RimeWorkspacePrismTest : public ::testing::Test {
 protected:
  void SetUp() override {
    fs::remove_all(root_);
    fs::create_directories(root_);
    Initialize(root_, root_ / "build");
    std::ofstream(root_ / "default.yaml") << R"YAML(config_version: "1"
schema_list:
  - schema: workspace_prism_main
)YAML";
    std::ofstream(root_ / "workspace_prism.dict.yaml") << R"YAML(---
name: workspace_prism
version: "1"
...
你	ni	100
)YAML";
    std::ofstream(root_ / "workspace_prism_main.schema.yaml") << R"YAML(schema:
  schema_id: workspace_prism_main
  version: "1"
  dependencies: [workspace_prism_sub]
build:
  dictionaries:
    workspace_prism:
      algebra:
        shared: [xform/^ni$/ab/]
)YAML";
  }

  void TearDown() override {
    Initialize(path{"."}, path{"."});
    fs::remove_all(root_);
  }

  static void Initialize(const path& data, const path& staging) {
    auto api = rime_get_api();
    api->finalize();
    const auto data_dir = data.to_utf8_string();
    const auto staging_dir = staging.to_utf8_string();
    RIME_STRUCT(RimeTraits, traits);
    traits.shared_data_dir = traits.user_data_dir = traits.prebuilt_data_dir =
        data_dir.c_str();
    traits.staging_dir = staging_dir.c_str();
    api->initialize(&traits);
  }

  void WriteSecondSchema(const char* shared_spelling) {
    std::ofstream(root_ / "workspace_prism_sub.schema.yaml") << R"YAML(schema:
  schema_id: workspace_prism_sub
  version: "1"
build:
  dictionaries:
    workspace_prism:
      algebra:
        standalone: [xform/^ni$/jt/]
        shared: [xform/^ni$/)YAML" << shared_spelling << "/]\n";
  }

  Deployer& deployer_ = Service::instance().deployer();
  const fs::path root_ = fs::absolute("workspace_prism_test");
};

TEST_F(RimeWorkspacePrismTest, SharesPrismOnlyWhenAlgebraMatches) {
  WriteSecondSchema("ab");
  WorkspaceUpdate update;
  ASSERT_TRUE(update.Run(&deployer_));

  const path shared_path =
      deployer_.staging_dir / "workspace_prism.shared.prism.bin";
  const path standalone_path =
      deployer_.staging_dir / "workspace_prism.standalone.prism.bin";
  ASSERT_TRUE(fs::exists(shared_path));
  EXPECT_FALSE(fs::exists(standalone_path));
  EXPECT_FALSE(
      fs::exists(deployer_.staging_dir / "workspace_prism_sub.schema.yaml"));

  std::ofstream(root_ / "default.custom.yaml") << R"YAML(patch:
  schema_list:
    - schema: workspace_prism_main
    - schema: workspace_prism_sub
)YAML";
  ASSERT_TRUE(update.Run(&deployer_));
  {
    Prism shared(shared_path);
    Prism standalone(standalone_path);
    ASSERT_TRUE(shared.Load());
    ASSERT_TRUE(standalone.Load());
    EXPECT_TRUE(shared.HasKey("ab"));
    EXPECT_TRUE(standalone.HasKey("jt"));
  }
  const auto last_build = fs::last_write_time(shared_path);
  std::ofstream(root_ / "workspace_prism_main.custom.yaml")
      << "patch:\n  menu/page_size: 7\n";
  ASSERT_TRUE(update.Run(&deployer_));
  EXPECT_EQ(last_build, fs::last_write_time(shared_path));

  const auto second_schema = root_ / "workspace_prism_sub.schema.yaml";
  const auto old_time = fs::last_write_time(second_schema);
  WriteSecondSchema("cd");
  fs::last_write_time(second_schema, old_time + std::chrono::seconds(2));
  EXPECT_FALSE(update.Run(&deployer_));
  EXPECT_EQ(last_build, fs::last_write_time(shared_path));
  Prism shared(shared_path);
  ASSERT_TRUE(shared.Load());
  EXPECT_TRUE(shared.HasKey("ab"));
  EXPECT_FALSE(shared.HasKey("cd"));
}

TEST_F(RimeWorkspacePrismTest, DeploysAndSwitchesConditionalRewriter) {
  std::ofstream(root_ / "rewrite.tsv") << "你\t妳\n";
  std::ofstream(root_ / "workspace_prism_main.custom.yaml") << R"YAML(patch:
  engine/processors: [speller, selector, express_editor]
  engine/segmentors: [abc_segmentor]
  engine/translators: [table_translator]
  engine/filters:
    - {name: rewriter@rewrite_test, when: rewrite_on}
  translator:
    dictionary: workspace_prism
    prism: shared
    user_dict_mode: off
  rewrite_test:
    files: [rewrite.tsv]
    option: true
    mode: replace
)YAML";
  WorkspaceUpdate update;
  ASSERT_TRUE(update.Run(&deployer_));

  auto api = rime_get_api();
  auto session = api->create_session();
  ASSERT_NE(0, session);
  ASSERT_TRUE(api->select_schema(session, "workspace_prism_main"));
  ASSERT_TRUE(api->process_key(session, 'a', 0));
  ASSERT_TRUE(api->process_key(session, 'b', 0));
  auto* context = Service::instance().GetSession(session)->context();
  ASSERT_TRUE(context->GetSelectedCandidate());
  EXPECT_EQ("你", context->GetSelectedCandidate()->text());
  api->set_option(session, "rewrite_on", true);
  ASSERT_TRUE(context->GetSelectedCandidate());
  EXPECT_EQ("妳", context->GetSelectedCandidate()->text());
  api->set_option(session, "rewrite_on", false);
  ASSERT_TRUE(context->GetSelectedCandidate());
  EXPECT_EQ("你", context->GetSelectedCandidate()->text());
  api->destroy_session(session);
}
