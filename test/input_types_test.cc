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

}  // namespace
}  // namespace rime
