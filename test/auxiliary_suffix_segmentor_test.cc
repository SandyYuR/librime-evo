//
// Copyright RIME Developers
// Distributed under the BSD License
//

#include <gtest/gtest.h>
#include <rime/config.h>
#include <rime/schema.h>
#include <rime/segmentation.h>
#include <rime/ticket.h>
#include <rime/gear/auxiliary_suffix_segmentor.h>
#include <rime/gear/affix_segmentor.h>
#include <rime/gear/abc_segmentor.h>

using namespace rime;

namespace {

Schema MakeSchema(bool with_affix = false) {
  auto* config = new Config;
  config->SetString("translator/aux_suffix", "`");
  if (with_affix) {
    config->SetString("add_user_dict/prefix", "``");
    config->SetString("speller/alphabet", "abcdefghijklmnopqrstuvwxyz`");
    auto segmentors = New<ConfigList>();
    segmentors->Append(New<ConfigValue>("auxiliary_suffix_segmentor"));
    segmentors->Append(New<ConfigValue>("abc_segmentor"));
    segmentors->Append(New<ConfigValue>("affix_segmentor@add_user_dict"));
    config->SetItem("engine/segmentors", segmentors);
  }
  return Schema("auxiliary_suffix_test", config);
}

}  // namespace

TEST(AuxiliarySuffixSegmentorTest, KeepsSuffixOutOfMainSegment) {
  Schema schema = MakeSchema();
  AuxiliarySuffixSegmentor segmentor(
      Ticket(&schema, "auxiliary_suffix_segmentor"));
  Segmentation segmentation;
  segmentation.Reset("ni`z");

  EXPECT_FALSE(segmentor.Proceed(&segmentation));
  ASSERT_EQ(1U, segmentation.size());
  EXPECT_EQ(0U, segmentation[0].start);
  EXPECT_EQ(2U, segmentation[0].end);
  EXPECT_TRUE(segmentation[0].HasTag("abc"));
  EXPECT_TRUE(segmentation[0].HasTag("auxiliary_suffix"));

  EXPECT_TRUE(segmentation.Forward());
  EXPECT_FALSE(segmentor.Proceed(&segmentation));
  ASSERT_EQ(1U, segmentation.size());
  EXPECT_EQ(0U, segmentation[0].start);
  EXPECT_EQ(2U, segmentation[0].end);
  EXPECT_TRUE(segmentation[0].HasTag("auxiliary_suffix"));
}

TEST(AuxiliarySuffixSegmentorTest, ReopensConfirmedMainSegment) {
  Schema schema = MakeSchema();
  AuxiliarySuffixSegmentor segmentor(
      Ticket(&schema, "auxiliary_suffix_segmentor"));
  Segmentation segmentation;
  segmentation.Reset("ni");
  Segment main(0, 2);
  main.status = Segment::kConfirmed;
  main.tags.insert("abc");
  segmentation.AddSegment(main);
  segmentation.Forward();

  segmentation.Reset("ni`z");
  EXPECT_FALSE(segmentor.Proceed(&segmentation));
  ASSERT_EQ(1U, segmentation.size());
  EXPECT_EQ(Segment::kVoid, segmentation[0].status);
  EXPECT_TRUE(segmentation[0].HasTag("auxiliary_suffix"));
  EXPECT_TRUE(segmentation[0].menu == nullptr);
}

TEST(AuxiliarySuffixSegmentorTest, ClearsSuffixStateAfterDeletion) {
  Schema schema = MakeSchema();
  AuxiliarySuffixSegmentor segmentor(
      Ticket(&schema, "auxiliary_suffix_segmentor"));
  Segmentation segmentation;
  segmentation.Reset("ni`z");
  EXPECT_FALSE(segmentor.Proceed(&segmentation));
  ASSERT_EQ(1U, segmentation.size());
  EXPECT_TRUE(segmentation[0].HasTag("auxiliary_suffix"));

  segmentation.Reset("ni");
  EXPECT_TRUE(segmentor.Proceed(&segmentation));
  ASSERT_EQ(1U, segmentation.size());
  EXPECT_TRUE(segmentation[0].HasTag("abc"));
  EXPECT_FALSE(segmentation[0].HasTag("auxiliary_suffix"));
}

TEST(AuxiliarySuffixSegmentorTest, AffixPrefixIsNotConsumedAsAuxiliary) {
  Schema schema = MakeSchema(true);
  AuxiliarySuffixSegmentor suffix(
      Ticket(&schema, "auxiliary_suffix_segmentor"));

  // 单反引号是双反引号前缀的输入中间态。
  for (const char* input : {"`", "``", "``ni", "``ni`z"}) {
    Segmentation segmentation;
    segmentation.Reset(input);
    EXPECT_TRUE(suffix.Proceed(&segmentation)) << input;
    EXPECT_TRUE(segmentation.empty()) << input;
  }

  // 验证完整的后续流程：ABC 标记输入，再由 affix 拆掉前缀。
  AbcSegmentor abc(Ticket(&schema, "abc_segmentor"));
  AffixSegmentor affix(Ticket(&schema, "add_user_dict"));
  Segmentation segmentation;
  segmentation.Reset("``ni");
  EXPECT_TRUE(suffix.Proceed(&segmentation));
  EXPECT_TRUE(abc.Proceed(&segmentation));
  EXPECT_FALSE(affix.Proceed(&segmentation));
  ASSERT_EQ(2U, segmentation.size());
  EXPECT_TRUE(segmentation[0].HasTag("abc_prefix"));
  EXPECT_TRUE(segmentation[0].HasTag("phony"));
  EXPECT_EQ(2U, segmentation[0].end);
  EXPECT_TRUE(segmentation[1].HasTag("abc"));
  EXPECT_EQ(2U, segmentation[1].start);
  EXPECT_EQ(4U, segmentation[1].end);
}

TEST(AuxiliarySuffixSegmentorTest, RequiresAlphabeticSpellingBeforeSuffix) {
  // 不需要 affix 配置，也不会抢占以反引号开头的前缀模式。
  Schema schema = MakeSchema();
  AuxiliarySuffixSegmentor suffix(
      Ticket(&schema, "auxiliary_suffix_segmentor"));
  for (const char* input :
       {"`", "``", "``ni", "``ni`z", "`ni", "1ni`z", "_ni`z", "'ni`z"}) {
    Segmentation segmentation;
    segmentation.Reset(input);
    EXPECT_TRUE(suffix.Proceed(&segmentation)) << input;
    EXPECT_TRUE(segmentation.empty()) << input;
  }

  // 有前置编码的单独触发符仍需等待输入 AUX，不能提前让后续分段器消费。
  Segmentation segmentation;
  segmentation.Reset("ni`");
  EXPECT_FALSE(suffix.Proceed(&segmentation));
  ASSERT_EQ(1U, segmentation.size());
  EXPECT_EQ(2U, segmentation[0].end);
  EXPECT_TRUE(segmentation[0].HasTag("auxiliary_suffix"));
}

TEST(AuxiliarySuffixSegmentorTest, NormalAuxiliaryStillWorksWithAffixEnabled) {
  Schema schema = MakeSchema(true);
  AuxiliarySuffixSegmentor suffix(
      Ticket(&schema, "auxiliary_suffix_segmentor"));
  Segmentation segmentation;
  segmentation.Reset("ni`z");
  EXPECT_FALSE(suffix.Proceed(&segmentation));
  ASSERT_EQ(1U, segmentation.size());
  EXPECT_TRUE(segmentation[0].HasTag("auxiliary_suffix"));
  EXPECT_EQ(2U, segmentation[0].end);
}
