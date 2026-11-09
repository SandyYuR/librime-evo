//
// Copyright RIME Developers
// Distributed under the BSD License
//

#include <gtest/gtest.h>
#include <sstream>

#include <rime/config.h>
#include <rime/context.h>
#include <rime/engine.h>
#include <rime/schema.h>

namespace rime {
namespace {

class InputTypesTest : public ::testing::Test {
 protected:
  void SetUp() override {
    engine_.reset(Engine::Create());
    auto* config = new Config;
    std::istringstream stream(R"yaml(
input_types:
  reset: zrm
  options:
    - {option: pinyin, label: 全拼}
    - {option: zrm, label: 自然码}
)yaml");
    ASSERT_TRUE(config->LoadFromStream(stream));
    engine_->ApplySchema(new Schema("input_types_test", config));
  }

  the<Engine> engine_;
};

TEST_F(InputTypesTest, ResetsToConfiguredInputType) {
  EXPECT_FALSE(engine_->context()->get_option("pinyin"));
  EXPECT_TRUE(engine_->context()->get_option("zrm"));
}

TEST_F(InputTypesTest, SelectingInputTypeClearsOtherTypes) {
  engine_->context()->set_option("pinyin", true);

  EXPECT_TRUE(engine_->context()->get_option("pinyin"));
  EXPECT_FALSE(engine_->context()->get_option("zrm"));

  engine_->context()->set_option("zrm", true);

  EXPECT_FALSE(engine_->context()->get_option("pinyin"));
  EXPECT_TRUE(engine_->context()->get_option("zrm"));
}

TEST(InputTypesEnabledTest, DisabledOptionCannotBeActivatedOrRestored) {
  the<Engine> engine(Engine::Create());
  auto config = new Config;
  std::istringstream data(R"YAML(input_types:
  enabled: [pinyin, flypy]
  reset: flypy
  options:
    - {option: pinyin, label: 全拼}
    - {option: zrm, label: 自然码}
    - {option: flypy, label: 小鹤}
)YAML");
  ASSERT_TRUE(config->LoadFromStream(data));
  engine->context()->set_option("zrm", true);
  engine->ApplySchema(new Schema("enabled_input_test", config));
  EXPECT_TRUE(engine->context()->get_option("flypy"));
  EXPECT_FALSE(engine->context()->get_option("zrm"));
  engine->context()->set_option("zrm", true);
  EXPECT_FALSE(engine->context()->get_option("zrm"));
  EXPECT_TRUE(engine->context()->get_option("flypy"));
  engine->context()->set_option("pinyin", true);
  EXPECT_TRUE(engine->context()->get_option("pinyin"));
  EXPECT_FALSE(engine->context()->get_option("flypy"));
  engine->context()->set_option("pinyin", false);
  EXPECT_TRUE(engine->context()->get_option("pinyin"));
}

TEST(InputTypesEnabledTest, NoResetPreservesAnEnabledSelection) {
  the<Engine> engine(Engine::Create());
  auto config = new Config;
  std::istringstream data(R"YAML(input_types:
  enabled: [pinyin, flypy]
  options:
    - {option: pinyin}
    - {option: flypy}
)YAML");
  ASSERT_TRUE(config->LoadFromStream(data));
  engine->context()->set_option("flypy", true);
  engine->ApplySchema(new Schema("saved_input_test", config));
  EXPECT_TRUE(engine->context()->get_option("flypy"));
  EXPECT_FALSE(engine->context()->get_option("pinyin"));
}
}  // namespace
}  // namespace rime
