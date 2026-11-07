// Copyright RIME Developers
// Distributed under the BSD License
//
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <gtest/gtest.h>
#include <rime/algo/syllabifier.h>
#include <rime/dict/auxiliary_db.h>
#include <rime/dict/dict_compiler.h>
#include <rime/dict/dictionary.h>
#include <rime/dict/text_db.h>
#include <rime/dict/user_dictionary.h>
#include <rime/gear/auxiliary_matcher.h>

using namespace rime;

class RimeAuxiliaryLookupTest : public ::testing::Test {
 protected:
  void SetUp() override {
    std::ofstream("aux_path_test.dict.yaml") << R"YAML(---
name: aux_path_test
version: "1.0"
sort: by_weight
...
你	ni	100
拟	ni	80
泥	ni	50
逆	ni	20
好	hao	100
蒿	hao	10
在	zai	100
哪	na	100
里	li	100
鳞	lin	100
𤆄	lin	100
增	zeng	100
锃	zeng	100000000
加	jia	100
哀	ai	100
爱	ai	10
你好	ni hao	100
拟好	ni hao	10
泥蒿	ni hao	5
你在	ni zai	100
逆鳞	ni lin	100
你𤆄	ni lin	1000
增加	zeng jia	1000
增价	zeng jia	1
你好在哪	ni hao zai na	100
你在哪里	ni zai na li	100
拟在哪里	ni zai na li	10
你好在哪里	ni hao zai na li	100
拟好在哪里	ni hao zai na li	10
)YAML";
    std::ofstream("aux_path_test_aux.yaml") << R"YAML(你	rx
你	abcdef
你	weak
拟	norm
逆	zn
锃	jw
哀	wy
里	zt
)YAML";
    std::ofstream("aux_path_test.schema.yaml") << R"YAML(speller:
  aux_dictionary: aux_path_test_aux
  algebra:
    pinyin: []
    zrm:
      - xform/^hao$/hk/
      - xform/^zai$/zl/
      - xform/^lin$/ln/
      - xform/^zeng$/zg/
      - xform/^jia$/jw/
  aux_algebra:
    aux_path_test:
      - "aux|^([a-z])[a-z]*$|$1|"
      - "aux|^([a-z]{2,})$|$1|abbrev"
      - "aux|^([a-z]{2,})$|$1/|"
      - "aux|^weak$|xy|abbrev"
      - "aux|^norm$|xy|"
    aux_path_other:
      - "aux|^([a-z]{2,})$|$1|"
)YAML";
    for (const string type : {"pinyin", "zrm"}) {
      auto dict = OpenDictionary(type, false);
      DictCompiler compiler(dict.get());
      compiler.set_input_type(type);
      compiler.set_auxiliary_type("aux_path_test");
      ASSERT_TRUE(compiler.Compile(path{"aux_path_test.schema.yaml"}));
    }
    database_.reset(new AuxiliaryDb(path{"aux_path_test.aux.bin"}));
    ASSERT_TRUE(database_->Load());
  }

  void TearDown() override {
    database_.reset();
    for (const char* file :
         {"aux_path_test.dict.yaml", "aux_path_test_aux.yaml",
          "aux_path_test.schema.yaml", "aux_path_test.table.bin",
          "aux_path_test.reverse.bin", "aux_path_test_pinyin.prism.bin",
          "aux_path_test_zrm.prism.bin", "aux_path_test.aux.bin",
          "aux_path_other.aux.bin", "aux_path_user.txt"})
      std::remove(file);
  }

  an<Dictionary> OpenDictionary(const string& type, bool load = true) {
    auto dict = New<Dictionary>(
        "aux_path_test", vector<string>{},
        vector<of<Table>>{New<Table>(path{"aux_path_test.table.bin"})},
        New<Prism>(path{"aux_path_test_" + type + ".prism.bin"}));
    if (load)
      EXPECT_TRUE(dict->Load());
    return dict;
  }

  vector<an<DictEntry>> Lookup(const string& input,
                               const string& type = "zrm",
                               bool enable_aux = true,
                               bool completion = true,
                               bool predict_word = false) {
    auto dict = OpenDictionary(type);
    AuxiliaryMatcher matcher(dict.get(), database_.get());
    Syllabifier syllabifier(" '", completion);
    if (enable_aux) {
      syllabifier.SetAuxiliaryLookup(
          [&matcher](const string& code, vector<AuxGraphMatch>* matches) {
            matcher.Lookup(code, matches);
          });
    }
    SyllableGraph graph;
    syllabifier.BuildSyllableGraph(input, *dict->prism(), &graph);
    auto result = dict->Lookup(graph, 0, nullptr, predict_word);
    vector<an<DictEntry>> entries;
    if (!result)
      return entries;
    for (auto bucket = result->rbegin(); bucket != result->rend(); ++bucket) {
      auto& it = bucket->second;
      while (!it.exhausted()) {
        entries.push_back(it.Peek());
        it.Next();
      }
    }
    return entries;
  }

  static size_t Position(const vector<an<DictEntry>>& entries,
                         const string& text) {
    for (size_t i = 0; i < entries.size(); ++i) {
      if (entries[i]->text == text)
        return i;
    }
    return entries.size();
  }

  the<AuxiliaryDb> database_;
};

TEST_F(RimeAuxiliaryLookupTest, KeepsOrdinaryPrefixCandidates) {
  const auto entries = Lookup("nihk");
  EXPECT_LT(Position(entries, "你好"), entries.size());
  EXPECT_LT(Position(entries, "拟好"), entries.size());
  EXPECT_LT(Position(entries, "泥蒿"), entries.size());
  EXPECT_LT(Position(entries, "你"), entries.size());
  EXPECT_LT(Position(entries, "泥"), entries.size());
}

TEST_F(RimeAuxiliaryLookupTest, AuxiliaryFiltersOnlyItsOwnPath) {
  const auto entries = Lookup("niz");
  EXPECT_LT(Position(entries, "逆"), entries.size());
  EXPECT_EQ(Position(entries, "你"), entries.size());
  EXPECT_EQ(Position(entries, "泥"), entries.size());
  const auto phrase = Lookup("nizln");
  EXPECT_LT(Position(phrase, "逆鳞"), phrase.size());
  EXPECT_EQ(Position(phrase, "你𤆄"), phrase.size());
}

TEST_F(RimeAuxiliaryLookupTest, KeepsOrdinaryPhraseAndAuxiliaryContinuation) {
  const auto short_phrase = Lookup("nizl");
  EXPECT_LT(Position(short_phrase, "你在"), short_phrase.size());
  const auto entries = Lookup("nizlnali");
  EXPECT_LT(Position(entries, "你在哪里"), entries.size());
}

TEST_F(RimeAuxiliaryLookupTest, UnrelatedCharactersDoNotCreateAuxiliaryEdges) {
  auto dict = OpenDictionary("pinyin");
  AuxiliaryMatcher matcher(dict.get(), database_.get());
  vector<AuxGraphMatch> matches;
  matcher.Lookup("haoz", &matches);
  EXPECT_TRUE(matches.empty());
  matcher.Lookup("nixy", &matches);
  ASSERT_EQ(2u, matches.size());
  EXPECT_EQ(kNormalSpelling, matches[0].type);
  EXPECT_EQ(kAbbreviation, matches[1].type);
  EXPECT_LT(matches[1].credibility, 0.0);
}

TEST_F(RimeAuxiliaryLookupTest,
       SuffixUsesCompiledCodesWithoutRetransformation) {
  auto dict = OpenDictionary("pinyin");
  AuxiliaryMatcher matcher(dict.get(), database_.get());
  EXPECT_TRUE(matcher.Match("你", "rx"));
  EXPECT_TRUE(matcher.Match("你在", "rx"));
  EXPECT_TRUE(matcher.Match("你", "rx/"));
  EXPECT_FALSE(matcher.Match("你", "rz"));
}

TEST_F(RimeAuxiliaryLookupTest, PinyinUsesMatchedSpellingLength) {
  const auto ordinary = Lookup("nizai", "pinyin");
  EXPECT_LT(Position(ordinary, "你在"), ordinary.size());
  const auto auxiliary = Lookup("nirzai", "pinyin");
  EXPECT_LT(Position(auxiliary, "你在"), auxiliary.size());
  EXPECT_EQ(Position(auxiliary, "拟好"), auxiliary.size());
}

TEST_F(RimeAuxiliaryLookupTest, AuxiliaryCodeHasNoFixedLength) {
  const auto entries = Lookup("niabcdef/zai", "pinyin");
  EXPECT_LT(Position(entries, "你在"), entries.size());
  EXPECT_EQ(Position(entries, "拟在哪里"), entries.size());
  const auto exact = Lookup("aiwy/", "pinyin");
  ASSERT_FALSE(exact.empty());
  EXPECT_EQ("哀", exact.front()->text);
}

TEST_F(RimeAuxiliaryLookupTest, AbbreviationKeepsPenaltyAndCharacterOrdering) {
  const auto entries = Lookup("zgjw");
  ASSERT_LT(Position(entries, "增加"), entries.size());
  ASSERT_LT(Position(entries, "增价"), entries.size());
  ASSERT_LT(Position(entries, "锃"), entries.size());
  EXPECT_LT(Position(entries, "增加"), Position(entries, "锃"));
  EXPECT_LT(Position(entries, "增价"), Position(entries, "锃"));
  EXPECT_TRUE(entries[Position(entries, "锃")]->abbreviated_auxiliary);
}

TEST_F(RimeAuxiliaryLookupTest, SameSpellingKeepsSeparateAuxiliaryMetadata) {
  const auto entries = Lookup("nixy", "pinyin");
  ASSERT_LT(Position(entries, "你"), entries.size());
  ASSERT_LT(Position(entries, "拟"), entries.size());
  const auto weak = entries[Position(entries, "你")];
  const auto normal = entries[Position(entries, "拟")];
  EXPECT_TRUE(weak->abbreviated_auxiliary);
  EXPECT_FALSE(normal->abbreviated_auxiliary);
  EXPECT_LT(weak->weight, normal->weight);
}

TEST_F(RimeAuxiliaryLookupTest, AuxiliaryWorksInLongEntryTail) {
  const auto entries = Lookup("nizlnaliz", "zrm", true, false);
  EXPECT_LT(Position(entries, "你在哪里"), entries.size());
  const auto first_aux = Lookup("nirhkzlnali", "zrm", true, false);
  EXPECT_LT(Position(first_aux, "你好在哪里"), first_aux.size());
  EXPECT_EQ(Position(first_aux, "拟好在哪里"), first_aux.size());
}

TEST_F(RimeAuxiliaryLookupTest, ExactPhrasePrecedesLongWordPrediction) {
  const auto entries = Lookup("nirhkzlna", "zrm", true, false, true);
  const auto exact = Position(entries, "你好在哪");
  const auto prediction = Position(entries, "你好在哪里");
  ASSERT_LT(exact, entries.size());
  ASSERT_LT(prediction, entries.size());
  EXPECT_LT(exact, prediction);
  EXPECT_TRUE(entries[exact]->IsExactMatch());
  EXPECT_TRUE(entries[prediction]->IsPredictiveMatch());
  EXPECT_EQ(Position(entries, "拟好在哪里"), entries.size());
}

TEST_F(RimeAuxiliaryLookupTest, WithoutAuxiliaryKeepsNativeResults) {
  const auto plain = Lookup("nihk", "zrm", false);
  const auto enabled = Lookup("nihk");
  ASSERT_EQ(plain.size(), enabled.size());
  for (size_t i = 0; i < plain.size(); ++i) {
    EXPECT_EQ(plain[i]->text, enabled[i]->text);
    EXPECT_EQ(plain[i]->code, enabled[i]->code);
    EXPECT_DOUBLE_EQ(plain[i]->weight, enabled[i]->weight);
    EXPECT_DOUBLE_EQ(plain[i]->quality_len, enabled[i]->quality_len);
  }
  for (const string input : {"nihk", "nizl", "nizlnali", "zgjw"}) {
    SCOPED_TRACE(input);
    const auto native = Lookup(input, "zrm", false);
    const auto auxiliary = Lookup(input);
    for (const auto& entry : native)
      EXPECT_LT(Position(auxiliary, entry->text), auxiliary.size());
  }
}

TEST_F(RimeAuxiliaryLookupTest, UserDictionaryFiltersTheChosenAuxiliaryPath) {
  auto dict = OpenDictionary("zrm");
  auto db =
      New<UserDbWrapper<TextDb>>(path{"aux_path_user.txt"}, "aux_path_user");
  ASSERT_TRUE(db->Open());
  ASSERT_TRUE(db->Update("ni lin \t逆鳞", "1,1,1,0,0"));
  ASSERT_TRUE(db->Update("ni lin \t你𤆄", "1,1,1,0,0"));
  ASSERT_TRUE(db->Update("ni \t你", "1,1,1,0,0"));
  UserDictionary user("aux_path_user", db);
  user.Attach(dict->primary_table(), dict->prism());
  user.SetUserDictMode("auto");
  AuxiliaryMatcher matcher(dict.get(), database_.get());
  Syllabifier syllabifier;
  syllabifier.SetAuxiliaryLookup(
      [&matcher](const string& code, vector<AuxGraphMatch>* matches) {
        matcher.Lookup(code, matches);
      });
  SyllableGraph graph;
  ASSERT_EQ(5, syllabifier.BuildSyllableGraph("nizln", *dict->prism(), &graph));
  auto result = user.Lookup(graph, 0);
  ASSERT_TRUE(result);
  auto full = result->find(5);
  ASSERT_NE(result->end(), full);
  ASSERT_FALSE(full->second.exhausted());
  EXPECT_EQ("逆鳞", full->second.Peek()->text);
  EXPECT_FALSE(full->second.Next());
  SyllableGraph ordinary_graph;
  ASSERT_EQ(4, syllabifier.BuildSyllableGraph("nihk", *dict->prism(),
                                              &ordinary_graph));
  auto ordinary_result = user.Lookup(ordinary_graph, 0);
  ASSERT_TRUE(ordinary_result);
  auto prefix = ordinary_result->find(2);
  ASSERT_NE(ordinary_result->end(), prefix);
  ASSERT_FALSE(prefix->second.exhausted());
  EXPECT_EQ("你", prefix->second.Peek()->text);
}
