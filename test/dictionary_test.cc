//
// Copyright RIME Developers
// Distributed under the BSD License
//
// 2011-07-05 GONG Chen <chen.sst@gmail.com>
//
#include <gtest/gtest.h>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <rime/dict/dict_settings.h>
#include <rime/dict/entry_collector.h>
#include <rime/common.h>
#include <rime/algo/encoder.h>
#include <rime/algo/syllabifier.h>
#include <rime/dict/dictionary.h>
#include <rime/dict/dict_compiler.h>
#include <memory>
#include <set>

class RimeDictionaryTest : public ::testing::Test {
 public:
  virtual void SetUp() {
    if (!dict_) {
      dict_.reset(new rime::Dictionary(
          "dictionary_test", {},
          {rime::New<rime::Table>(rime::path{"dictionary_test.table.bin"})},
          rime::New<rime::Prism>(rime::path{"dictionary_test.prism.bin"})));
    }
    if (!rebuilt_) {
      dict_->Remove();
      rime::DictCompiler dict_compiler(dict_.get());
      dict_compiler.Compile(rime::path());  // no schema file
      rebuilt_ = true;
    }
    dict_->Load();
  }
  virtual void TearDown() { dict_.reset(); }

 protected:
  static rime::the<rime::Dictionary> dict_;
  static bool rebuilt_;
};

rime::the<rime::Dictionary> RimeDictionaryTest::dict_;
bool RimeDictionaryTest::rebuilt_ = false;

TEST_F(RimeDictionaryTest, Ready) {
  EXPECT_TRUE(dict_->loaded());
}

TEST_F(RimeDictionaryTest, SimpleLookup) {
  ASSERT_TRUE(dict_->loaded());
  rime::DictEntryIterator it;
  dict_->LookupWords(&it, "zhong", false);
  ASSERT_FALSE(it.exhausted());
  EXPECT_EQ("\xe4\xb8\xad", it.Peek()->text);  // 中
  ASSERT_EQ(1, it.Peek()->code.size());
  rime::RawCode raw_code;
  ASSERT_TRUE(dict_->Decode(it.Peek()->code, &raw_code));
  EXPECT_EQ("zhong", raw_code.ToString());
}

TEST_F(RimeDictionaryTest, PredictiveLookup) {
  ASSERT_TRUE(dict_->loaded());
  rime::DictEntryIterator it;
  dict_->LookupWords(&it, "z", true);
  ASSERT_FALSE(it.exhausted());
  EXPECT_EQ("\xe5\x92\x8b", it.Peek()->text);  // 咋
  ASSERT_EQ(1, it.Peek()->code.size());
  rime::RawCode raw_code;
  ASSERT_TRUE(dict_->Decode(it.Peek()->code, &raw_code));
  EXPECT_EQ("za", raw_code.ToString());
}

TEST_F(RimeDictionaryTest, ScriptLookup) {
  ASSERT_TRUE(dict_->loaded());
  rime::SyllableGraph g;
  rime::Syllabifier s;
  rime::string input("shurufa");
  ASSERT_TRUE(s.BuildSyllableGraph(input, *dict_->prism(), &g) > 0);
  EXPECT_EQ(g.interpreted_length, g.input_length);
  auto c = dict_->Lookup(g, 0);
  ASSERT_TRUE(bool(c));

  ASSERT_TRUE(c->find(3) != c->end());
  rime::DictEntryIterator& d3((*c)[3]);
  EXPECT_FALSE(d3.exhausted());
  auto e1 = d3.Peek();
  ASSERT_TRUE(bool(e1));
  EXPECT_EQ(1, e1->code.size());
  EXPECT_EQ(3, e1->text.length());
  EXPECT_TRUE(d3.Next());

  ASSERT_TRUE(c->find(5) != c->end());
  rime::DictEntryIterator& d5((*c)[5]);
  EXPECT_FALSE(d5.exhausted());
  auto e2 = d5.Peek();
  ASSERT_TRUE(bool(e2));
  EXPECT_EQ(2, e2->code.size());

  ASSERT_TRUE(c->find(7) != c->end());
  rime::DictEntryIterator& d7((*c)[7]);
  EXPECT_FALSE(d7.exhausted());
  auto e3 = d7.Peek();
  ASSERT_TRUE(bool(e3));
  EXPECT_EQ(3, e3->code.size());
  EXPECT_EQ(9, e3->text.length());
  EXPECT_FALSE(d7.Next());
}

TEST(RimeAuxiliaryCompilationTest, SingleCharacterTableUsesPureCode) {
  rime::DictSettings settings;
  std::istringstream header(
      "name: single_test\nversion: 1.0\n"
      "sort: by_weight\n");
  ASSERT_TRUE(settings.LoadFromStream(header));
  rime::EntryCollector collector;
  collector.Configure(&settings);
  collector.CreateEntry("甲", "ab", "100");
  std::vector<rime::string> codes;
  ASSERT_TRUE(collector.TranslateWord("甲", &codes));
  ASSERT_EQ(1u, codes.size());
  EXPECT_EQ("ab", codes.front());
}

TEST(RimeAuxiliaryCompilationTest, HeaderlessImportedSourceKeepsFirstRow) {
  std::istringstream input("# data only\n甲\tcd\n家\tef\n");
  rime::DictSettings settings;
  ASSERT_TRUE(settings.LoadDictHeader(input, true));
  EXPECT_TRUE(settings.empty());
  std::string first;
  ASSERT_TRUE(bool(std::getline(input, first)));
  EXPECT_EQ("# data only", first);
  ASSERT_TRUE(bool(std::getline(input, first)));
  EXPECT_EQ("甲\tcd", first);
}

TEST(RimeAuxiliaryOrderingTest, AbbreviationPrecedesSameLengthOrdinaryEntry) {
  constexpr const char* kDictName = "aux_order_test";
  constexpr const char* kDictFile = "aux_order_test.dict.yaml";
  constexpr const char* kAuxFile = "aux_order_test_aux.yaml";
  constexpr const char* kSchemaFile = "aux_order_test.schema.yaml";
  constexpr const char* kTableFile = "aux_order_test.table.bin";
  constexpr const char* kPrismFile = "aux_order_test.prism.bin";

  std::ofstream(kDictFile) << R"YAML(---
name: aux_order_test
version: "1.0"
sort: by_weight
...
甲	ab	1
甲乙	ab xy	1000
丙	ab xy	100
)YAML";
  std::ofstream(kAuxFile) << "甲\txy\n";
  std::ofstream(kSchemaFile) << R"YAML(speller:
  aux_dictionary: aux_order_test_aux
  aux_algebra:
    abbrev:
      - "aux|^([a-z])([a-z])$|$1$2|abbrev"
)YAML";

  rime::the<rime::Dictionary> dict(new rime::Dictionary(
      kDictName, {}, {rime::New<rime::Table>(rime::path{kTableFile})},
      rime::New<rime::Prism>(rime::path{kPrismFile})));
  dict->Remove();
  rime::DictCompiler compiler(dict.get());
  compiler.set_auxiliary_type("abbrev");
  ASSERT_TRUE(compiler.Compile(rime::path{kSchemaFile}));
  ASSERT_TRUE(dict->Load());

  int base_value = -1;
  ASSERT_TRUE(dict->prism()->GetValue("ab", &base_value));
  auto base = dict->prism()->QuerySpelling(base_value);
  ASSERT_FALSE(base.exhausted());
  const auto base_id = base.syllable_id();

  rime::Syllabifier syllabifier;
  syllabifier.SetAuxiliaryLookup(
      [base_id](const rime::string& input,
                std::vector<rime::AuxGraphMatch>* matches) {
        if (input == "abxy") {
          matches->push_back({4, base_id,
                              std::make_shared<const std::set<rime::string>>(
                                  std::set<rime::string>{"甲"}),
                              rime::kAbbreviation, 0.0});
        }
      });
  rime::SyllableGraph graph;
  ASSERT_EQ(4u, syllabifier.BuildSyllableGraph("abxy", *dict->prism(), &graph));

  auto result = dict->Lookup(graph, 0);
  ASSERT_TRUE(result);
  auto found = result->find(4);
  ASSERT_NE(result->end(), found);
  auto& entries = found->second;
  ASSERT_FALSE(entries.exhausted());
  EXPECT_EQ("甲乙", entries.Peek()->text);
  EXPECT_FALSE(entries.Peek()->abbreviated_auxiliary);
  ASSERT_TRUE(entries.Next());
  EXPECT_EQ("甲", entries.Peek()->text);
  EXPECT_TRUE(entries.Peek()->abbreviated_auxiliary);
  ASSERT_TRUE(entries.Next());
  EXPECT_EQ("丙", entries.Peek()->text);
  EXPECT_FALSE(entries.Peek()->abbreviated_auxiliary);

  dict.reset();
  std::remove(kDictFile);
  std::remove(kAuxFile);
  std::remove(kSchemaFile);
  std::remove(kTableFile);
  std::remove(kPrismFile);
  std::remove("aux_order_test.reverse.bin");
}
