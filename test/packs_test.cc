// Copyright RIME Developers
// Distributed under the BSD License
//
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <rime/algo/syllabifier.h>
#include <rime/algo/encoder.h>
#include <rime/segmentation.h>
#include <rime/dict/dict_compiler.h>
#include <rime/dict/auxiliary_db.h>
#include <rime/gear/auxiliary_matcher.h>
#include <rime/dict/dictionary.h>
#include <rime/dict/level_db.h>
#include <rime/dict/reverse_lookup_dictionary.h>
#include <rime/dict/text_db.h>
#include <rime/dict/user_dictionary.h>
#include <rime/engine.h>
#include <rime/deployer.h>
#include <rime/service.h>
#include <rime/schema.h>
#include <rime/ticket.h>
#include <rime/translation.h>
#include <rime/translator.h>

using namespace rime;

class RimePacksTest : public ::testing::Test {
 protected:
  void SetUp() override {
    original_prebuilt_ = Service::instance().deployer().prebuilt_data_dir;
    WriteBase();
    WritePack();
    WriteSchema();
  }

  void TearDown() override {
    Service::instance().deployer().prebuilt_data_dir = original_prebuilt_;
    std::filesystem::remove_all("packs_test_prebuilt");
    for (const char* suffix :
         {".dict.yaml", ".schema.yaml", ".table.bin", ".reverse.bin",
          ".packs.table.bin", ".packs.reverse.bin", ".pinyin.prism.bin",
          ".zrm.prism.bin", ".pack.dict.yaml", ".user.txt"})
      std::remove((string("packs_test") + suffix).c_str());
    std::filesystem::remove_all("packs_test.userdb");
    std::remove("packs_test_essay.txt");
    std::remove("packs_test_aux.yaml");
    std::remove("packs_test_aux.aux.bin");
  }

  void WriteBase(const string& reading = "hao3") {
    std::ofstream file("packs_test.dict.yaml");
    file << "---\nname: packs_test\nversion: '1.0'\nsort: by_weight\n...\n"
         << "好\t" << reading << "\t100\n"
         << "的\tde1\t100\n你\tni3\t100\n"
         << "好的\thao3 de1\t100\n"
         << "你好\tni3 hao3\t100\n";
  }

  void WritePack(const string& weight = "50") {
    // de occurs only inside a phrase, not in any single-character entry.
    std::ofstream("packs_test.pack.dict.yaml")
        << "好的\thao3 de\t" << weight << "\n"
        << "阿\ta\t10\n"
        << "你好的好\tni3 hao3 de hao3\t20\n"
        << "自定义\txcode\t20\n"
        << "好好\t\t20\n";
  }

  void WriteSchema(bool packs = true) {
    std::ofstream file("packs_test.schema.yaml");
    file << "build:\n  dictionaries:\n    packs_test:\n";
    if (packs)
      file << "      packs: [packs_test.pack]\n";
    file << R"YAML(      algebra:
        pinyin: ['xform/[0-9]$//']
        zrm:
          - xform/[0-9]$//
          - xform/^hao$/hk/
          - xform/^de$/dd/
          - xform/^xcode$/zz/
translator:
  dictionary: packs_test
  prism: pinyin
  user_dict_mode: off
  enable_completion: false
  enable_sentence: false
)YAML";
  }

  bool Compile() {
    return DictCompiler::CompileDictionaries(path{"packs_test.schema.yaml"});
  }

  an<Dictionary> Open(const string& type = "pinyin") {
    Schema schema("packs_test", new Config);
    schema.config()->SetString("translator/dictionary", "packs_test");
    schema.config()->SetString("translator/prism", type);
    an<Dictionary> result(
        Dictionary::Require("dictionary")->Create({&schema, "translator"}));
    if (result) {
      EXPECT_TRUE(result->Load());
    }
    return result;
  }

  vector<an<DictEntry>> Entries(Dictionary* dictionary,
                                const string& input,
                                bool prediction = false) {
    SyllableGraph graph;
    Syllabifier syllabifier(" '", false);
    EXPECT_EQ(input.size(), syllabifier.BuildSyllableGraph(
                                input, *dictionary->prism(), &graph));
    vector<an<DictEntry>> result;
    auto collector = dictionary->Lookup(graph, 0, nullptr, prediction);
    if (!collector)
      return result;
    auto full = collector->find(input.size());
    if (full == collector->end())
      return result;
    auto& entries = full->second;
    while (!entries.exhausted()) {
      result.push_back(entries.Peek());
      entries.Next();
    }
    return result;
  }

  set<string> Codes(Dictionary* dictionary,
                    const string& input,
                    const string& text) {
    set<string> codes;
    for (const auto& entry : Entries(dictionary, input)) {
      if (entry->text != text)
        continue;
      RawCode decoded;
      EXPECT_TRUE(dictionary->Decode(entry->code, &decoded));
      codes.insert(decoded.ToString());
    }
    return codes;
  }

  using Time = std::filesystem::file_time_type;
  path original_prebuilt_;
  static Time Mark(const char* suffix) {
    const path file{string("packs_test") + suffix};
    const auto marker = Time::clock::now() - std::chrono::hours(24);
    std::filesystem::last_write_time(file, marker);
    return std::filesystem::last_write_time(file);
  }
};

TEST_F(RimePacksTest, PhraseOnlyNeutralToneSurvivesBothAlgebras) {
  ASSERT_TRUE(Compile());
  for (const auto& type : {"pinyin", "zrm"}) {
    SCOPED_TRACE(type);
    auto dictionary = Open(type);
    ASSERT_TRUE(dictionary && dictionary->loaded());
    const string input = string(type) == "pinyin" ? "haode" : "hkdd";
    EXPECT_EQ((set<string>{"hao3 de", "hao3 de1"}),
              Codes(dictionary.get(), input, "好的"));
    EXPECT_EQ((set<string>{"ni3 hao3 de hao3"}),
              Codes(dictionary.get(),
                    string(type) == "pinyin" ? "nihaodehao" : "nihkddhk",
                    "你好的好"));
    EXPECT_EQ((set<string>{"ni3 hao3"}),
              Codes(dictionary.get(),
                    string(type) == "pinyin" ? "nihao" : "nihk", "你好"));
  }
}

TEST_F(RimePacksTest,
       ScriptAndTableTranslatorsFindSupplementWithoutLocalBuild) {
  ASSERT_TRUE(Compile());
  for (const char* prism : {"pinyin", "zrm"}) {
    SCOPED_TRACE(prism);
    const string input = string(prism) == "pinyin" ? "xcode" : "zz";
    for (const char* type : {"script_translator", "table_translator",
                             "reverse_lookup_translator"}) {
      SCOPED_TRACE(type);
      the<Config> config(new Config);
      ASSERT_TRUE(config->LoadFromFile(path{"packs_test.schema.yaml"}));
      ASSERT_TRUE(config->SetItem("build", nullptr));
      ASSERT_TRUE(config->SetString("translator/prism", prism));
      ASSERT_TRUE(config->SetItem("lookup", config->GetMap("translator")));
      ASSERT_TRUE(config->SetString("lookup/tag", "abc"));
      the<Engine> engine(Engine::Create());
      engine->ApplySchema(new Schema("packs_external", config.release()));
      the<Translator> translator(
          Translator::Require(type)->Create(Ticket(engine.get(), "lookup")));
      ASSERT_TRUE(translator);
      Segment segment(0, input.size());
      segment.tags.insert("abc");
      auto translation = translator->Query(input, segment);
      ASSERT_TRUE(translation && !translation->exhausted());
      EXPECT_EQ("自定义", translation->Peek()->text());
    }
  }
}

TEST_F(RimePacksTest, WeightEditsReuseBaseAndPrismsButNewReadingRebuildsPrism) {
  ASSERT_TRUE(Compile());
  const auto table = Mark(".table.bin");
  const auto reverse = Mark(".reverse.bin");
  const auto prism = Mark(".pinyin.prism.bin");
  const auto compact = Mark(".zrm.prism.bin");
  const auto pack = Mark(".packs.table.bin");
  WritePack("500");
  ASSERT_TRUE(Compile());
  EXPECT_EQ(table, std::filesystem::last_write_time("packs_test.table.bin"));
  EXPECT_EQ(reverse,
            std::filesystem::last_write_time("packs_test.reverse.bin"));
  EXPECT_EQ(prism,
            std::filesystem::last_write_time("packs_test.pinyin.prism.bin"));
  EXPECT_EQ(compact,
            std::filesystem::last_write_time("packs_test.zrm.prism.bin"));
  EXPECT_NE(pack,
            std::filesystem::last_write_time("packs_test.packs.table.bin"));
  EXPECT_FALSE(std::filesystem::exists("packs_test.packs.reverse.bin"));
  {
    std::ofstream file("packs_test.pack.dict.yaml", std::ios::app);
    file << "的好\tde2 hao3\t20\n";
  }
  ASSERT_TRUE(Compile());
  EXPECT_EQ(table, std::filesystem::last_write_time("packs_test.table.bin"));
  EXPECT_NE(prism,
            std::filesystem::last_write_time("packs_test.pinyin.prism.bin"));
  auto dictionary = Open();
  EXPECT_EQ((set<string>{"de2 hao3"}),
            Codes(dictionary.get(), "dehao", "的好"));
}

TEST_F(RimePacksTest, BaseEditsRefreshAutomaticEncodingReadings) {
  ASSERT_TRUE(Compile());
  const auto pack = Mark(".packs.table.bin");
  const auto prism = Mark(".pinyin.prism.bin");
  {
    std::ofstream file("packs_test.dict.yaml", std::ios::app);
    file << "号\thao3\t200\n";
  }
  ASSERT_TRUE(Compile());
  EXPECT_NE(pack,
            std::filesystem::last_write_time("packs_test.packs.table.bin"));
  EXPECT_EQ(prism,
            std::filesystem::last_write_time("packs_test.pinyin.prism.bin"));
  WriteBase("hao4");
  ASSERT_TRUE(Compile());
  EXPECT_NE(pack,
            std::filesystem::last_write_time("packs_test.packs.table.bin"));
  auto dictionary = Open();
  EXPECT_EQ((set<string>{"hao4 hao4"}),
            Codes(dictionary.get(), "haohao", "好好"));
}

TEST_F(RimePacksTest, AutomaticEncodingUsesTheRebuiltPrimaryInsteadOfPrebuilt) {
  ASSERT_TRUE(Compile());
  std::filesystem::create_directory("packs_test_prebuilt");
  for (const char* suffix : {".table.bin", ".reverse.bin"}) {
    const string name = string("packs_test") + suffix;
    std::filesystem::rename(name, path{"packs_test_prebuilt"} / name);
  }
  Service::instance().deployer().prebuilt_data_dir =
      path{"packs_test_prebuilt"};
  WriteBase("hao4");
  ASSERT_TRUE(Compile());
  auto dictionary = Open();
  ASSERT_TRUE(dictionary && dictionary->loaded());
  EXPECT_EQ((set<string>{"hao4 hao4"}),
            Codes(dictionary.get(), "haohao", "好好"));
}

TEST_F(RimePacksTest, SupplementRemovalDropsWordsWithoutChangingReverse) {
  ASSERT_TRUE(Compile());
  WriteSchema(false);
  ASSERT_TRUE(Compile());
  EXPECT_FALSE(std::filesystem::exists("packs_test.packs.table.bin"));
  auto dictionary = Open();
  ASSERT_TRUE(dictionary && dictionary->loaded());
  EXPECT_FALSE(dictionary->prism()->has_packs());
  EXPECT_EQ((set<string>{"hao3 de1"}),
            Codes(dictionary.get(), "haode", "好的"));
  DictEntryIterator entries;
  dictionary->LookupWords(&entries, "xcode", false);
  EXPECT_TRUE(entries.exhausted());
  ReverseLookupDictionaryComponent reverse_component;
  the<ReverseLookupDictionary> reverse(reverse_component.Create("packs_test"));
  ASSERT_TRUE(reverse->Load());
  string codes;
  EXPECT_FALSE(reverse->ReverseLookup("阿", &codes));
  EXPECT_TRUE(reverse->ReverseLookup("好", &codes));
  EXPECT_EQ("hao3", codes);
}

TEST_F(RimePacksTest, UserDictionaryStoresAndQueriesCanonicalSupplementCodes) {
  ASSERT_TRUE(Compile());
  auto dictionary = Open();
  auto entries = Entries(dictionary.get(), "haode");
  an<DictEntry> neutral;
  for (const auto& entry : entries) {
    RawCode code;
    ASSERT_TRUE(dictionary->Decode(entry->code, &code));
    if (code.ToString() == "hao3 de")
      neutral = entry;
  }
  ASSERT_TRUE(neutral);
  for (bool cached : {false, true}) {
    SCOPED_TRACE(cached);
    an<Db> db;
    if (cached)
      db = New<UserDbWrapper<LevelDb>>(path{"packs_test.userdb"}, "packs_test");
    else
      db =
          New<UserDbWrapper<TextDb>>(path{"packs_test.user.txt"}, "packs_test");
    ASSERT_TRUE(db->Open());
    UserDictionary user("packs_test", db);
    user.Attach(dictionary->primary_table(), dictionary->prism());
    ASSERT_TRUE(user.Load());
    user.SetUserDictMode("auto");
    ASSERT_TRUE(user.UpdateEntry(*neutral, 1));
    string stored;
    EXPECT_TRUE(db->Fetch("hao3 de \t好的", &stored));
    SyllableGraph graph;
    Syllabifier syllabifier(" '", false);
    ASSERT_EQ(5, syllabifier.BuildSyllableGraph("haode", *dictionary->prism(),
                                                &graph));
    auto found = user.Lookup(graph, 0);
    ASSERT_TRUE(found);
    auto full = found->find(5);
    ASSERT_NE(found->end(), full);
    ASSERT_FALSE(full->second.exhausted());
    EXPECT_EQ(neutral->code, full->second.Peek()->code);
    EXPECT_EQ("好的", full->second.Peek()->text);
    ASSERT_TRUE(db->Close());
  }
}

TEST_F(RimePacksTest, AddingPacksExtendsCodesWithoutRebuildingTheBase) {
  WriteSchema(false);
  ASSERT_TRUE(Compile());
  const auto base = Mark(".table.bin");
  WriteSchema(true);
  ASSERT_TRUE(Compile());
  EXPECT_EQ(base, std::filesystem::last_write_time("packs_test.table.bin"));
  auto dictionary = Open();
  ASSERT_TRUE(dictionary && dictionary->loaded());
  EXPECT_EQ((set<string>{"hao3 de", "hao3 de1"}),
            Codes(dictionary.get(), "haode", "好的"));
}

TEST_F(RimePacksTest, UnchangedDeploymentReusesEveryOutputAndEmptyPackIsValid) {
  ASSERT_TRUE(Compile());
  map<string, Time> outputs;
  for (const char* suffix : {".table.bin", ".reverse.bin", ".packs.table.bin",
                             ".pinyin.prism.bin", ".zrm.prism.bin"})
    outputs[suffix] = Mark(suffix);
  ASSERT_TRUE(Compile());
  for (const auto& output : outputs)
    EXPECT_EQ(output.second,
              std::filesystem::last_write_time("packs_test" + output.first));
  std::ofstream("packs_test.pack.dict.yaml") << "# empty custom dictionary\n";
  ASSERT_TRUE(Compile());
  auto dictionary = Open();
  ASSERT_TRUE(dictionary && dictionary->loaded());
  EXPECT_EQ((set<string>{"hao3 de1"}),
            Codes(dictionary.get(), "haode", "好的"));
}

TEST_F(RimePacksTest, MissingRequiredSupplementFailsInsteadOfUsingWrongIds) {
  ASSERT_TRUE(Compile());
  std::remove("packs_test.packs.table.bin");
  Schema schema("packs_external", new Config);
  schema.config()->SetString("translator/dictionary", "packs_test");
  schema.config()->SetString("translator/prism", "pinyin");
  the<Dictionary> dictionary(
      Dictionary::Require("dictionary")->Create({&schema, "translator"}));
  ASSERT_TRUE(dictionary);
  EXPECT_FALSE(dictionary->Load());
  EXPECT_FALSE(dictionary->loaded());
}

TEST_F(RimePacksTest, PresetExpansionDoesNotCopyTheBaseVocabularyIntoPacks) {
  std::ofstream("packs_test_essay.txt") << "你好好\t200\n";
  std::ifstream source("packs_test.dict.yaml");
  string text((std::istreambuf_iterator<char>(source)), {});
  source.close();
  text.insert(text.find("..."), "vocabulary: packs_test_essay\n");
  std::ofstream("packs_test.dict.yaml") << text;
  ASSERT_TRUE(Compile());
  Table supplement(path{"packs_test.packs.table.bin"});
  ASSERT_TRUE(supplement.Load());
  EXPECT_EQ(5u, supplement.metadata()->num_entries);
  auto dictionary = Open();
  EXPECT_EQ((set<string>{"ni3 hao3 hao3"}),
            Codes(dictionary.get(), "nihaohao", "你好好"));
}

TEST_F(RimePacksTest, PredictiveLongEntryRetainsCanonicalTailCodes) {
  std::ofstream("packs_test.pack.dict.yaml", std::ios::app)
      << "你好的好啊\tni3 hao3 de hao3 a\t20\n";
  ASSERT_TRUE(Compile());
  auto dictionary = Open();
  bool found = false;
  for (const auto& entry : Entries(dictionary.get(), "nihaodehao", true)) {
    if (entry->text != "你好的好啊")
      continue;
    found = true;
    EXPECT_TRUE(entry->IsPredictiveMatch());
    EXPECT_EQ(4, entry->matching_code_size);
    RawCode code;
    ASSERT_TRUE(dictionary->Decode(entry->code, &code));
    EXPECT_EQ("ni3 hao3 de hao3 a", code.ToString());
  }
  EXPECT_TRUE(found);
}

TEST_F(RimePacksTest,
       AuxiliaryUsesSingleCharacterEntriesWithoutAReverseDatabase) {
  std::ofstream("packs_test_aux.yaml") << "的\tkd\n";
  std::ifstream source("packs_test.schema.yaml");
  string text((std::istreambuf_iterator<char>(source)), {});
  source.close();
  text.insert(text.find("translator:"), R"YAML(  auxiliary:
    dictionary: packs_test_aux
    algebra:
      packs_test_aux: ['aux|^([a-z])[a-z]$|$1|']
)YAML");
  std::ofstream("packs_test.schema.yaml") << text;
  ASSERT_TRUE(Compile());
  ASSERT_TRUE(DictCompiler::CompileAuxiliary(path{"packs_test.schema.yaml"}));
  AuxiliaryDb auxiliary(path{"packs_test_aux.aux.bin"});
  ASSERT_TRUE(auxiliary.Load());
  auto dictionary = Open();
  ASSERT_TRUE(std::filesystem::remove("packs_test.reverse.bin"));
  AuxiliaryMatcher matcher(dictionary.get(), &auxiliary);
  Syllabifier syllabifier(" '", false);
  syllabifier.SetAuxiliaryLookup(
      [&matcher](const string& input, vector<AuxGraphMatch>* matches) {
        matcher.Lookup(input, matches);
      });
  SyllableGraph graph;
  ASSERT_EQ(6, syllabifier.BuildSyllableGraph("haodek", *dictionary->prism(),
                                              &graph));
  auto found = dictionary->Lookup(graph, 0);
  ASSERT_TRUE(found);
  auto full = found->find(6);
  ASSERT_NE(found->end(), full);
  set<string> codes;
  while (!full->second.exhausted()) {
    const auto entry = full->second.Peek();
    EXPECT_EQ("好的", entry->text);
    RawCode code;
    ASSERT_TRUE(dictionary->Decode(entry->code, &code));
    codes.insert(code.ToString());
    full->second.Next();
  }
  EXPECT_EQ((set<string>{"hao3 de1"}), codes);
}

TEST_F(RimePacksTest, AlgebraEditsRebuildOnlyTheSelectedPrism) {
  ASSERT_TRUE(Compile());
  map<string, Time> outputs;
  for (const char* suffix : {".table.bin", ".reverse.bin", ".packs.table.bin",
                             ".pinyin.prism.bin", ".zrm.prism.bin"})
    outputs[suffix] = Mark(suffix);
  Config schema;
  ASSERT_TRUE(schema.LoadFromFile(path{"packs_test.schema.yaml"}));
  auto algebra = schema.GetList("build/dictionaries/packs_test/algebra/zrm");
  ASSERT_TRUE(algebra);
  algebra->Append(New<ConfigValue>("xform/^dd$/ds/"));
  ASSERT_TRUE(schema.SaveToFile(path{"packs_test.schema.yaml"}));
  ASSERT_TRUE(Compile());
  for (const auto& output : outputs) {
    SCOPED_TRACE(output.first);
    const auto updated =
        std::filesystem::last_write_time("packs_test" + output.first);
    if (output.first == ".zrm.prism.bin") {
      EXPECT_NE(output.second, updated);
    } else {
      EXPECT_EQ(output.second, updated);
    }
  }
  auto dictionary = Open("zrm");
  ASSERT_TRUE(dictionary && dictionary->loaded());
  EXPECT_EQ((set<string>{"hao3 de", "hao3 de1"}),
            Codes(dictionary.get(), "hkds", "好的"));
}

TEST_F(RimePacksTest, PacksNeverChangePrimaryReverseReadingsOrStems) {
  std::ofstream("packs_test.dict.yaml") << R"YAML(---
name: packs_test
version: '1.0'
columns: [text, code, weight, stem]
...
好	hao3	100	h
)YAML";
  auto write_pack = [](const string& reading) {
    std::ofstream("packs_test.pack.dict.yaml")
        << "---\nname: custom\nversion: '1.0'\n"
           "columns: [text, code, weight, stem]\n...\n"
           "号\thao4\t50\th2\n好\t"
        << reading << "\t50\th2\n";
  };
  write_pack("hao4");
  ASSERT_TRUE(Compile());
  const auto stamp = Mark(".reverse.bin");
  auto check_reverse = [&]() {
    ReverseLookupDictionary reverse(
        New<ReverseDb>(path{"packs_test.reverse.bin"}));
    ASSERT_TRUE(reverse.Load());
    string value;
    ASSERT_TRUE(reverse.ReverseLookup("好", &value));
    EXPECT_EQ("hao3", value);
    ASSERT_TRUE(reverse.LookupStems("好", &value));
    EXPECT_EQ("h", value);
    EXPECT_FALSE(reverse.ReverseLookup("号", &value));
    EXPECT_FALSE(reverse.LookupStems("号", &value));
    EXPECT_EQ(stamp,
              std::filesystem::last_write_time("packs_test.reverse.bin"));
    EXPECT_FALSE(std::filesystem::exists("packs_test.packs.reverse.bin"));
  };
  check_reverse();
  write_pack("hao3");
  ASSERT_TRUE(Compile());
  check_reverse();
  WriteSchema(false);
  ASSERT_TRUE(Compile());
  check_reverse();
}

TEST_F(RimePacksTest, ReplacingPackDoesNotReuseItsOwnOldEncodingReadings) {
  std::ofstream("packs_test.pack.dict.yaml") << "好\thao4\t50\n好好\t\t50\n";
  ASSERT_TRUE(Compile());
  {
    auto dictionary = Open();
    EXPECT_EQ((set<string>{"hao4 hao4"}),
              Codes(dictionary.get(), "haohao", "好好"));
  }
  std::ofstream("packs_test.pack.dict.yaml") << "好好\t\t50\n";
  ASSERT_TRUE(Compile());
  auto dictionary = Open();
  EXPECT_EQ((set<string>{"hao3 hao3"}),
            Codes(dictionary.get(), "haohao", "好好"));
}
