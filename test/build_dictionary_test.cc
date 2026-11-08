// Copyright RIME Developers
// Distributed under the BSD License
//
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <rime_api.h>
#include <rime/config.h>
#include <rime/context.h>
#include <rime/dict/dict_compiler.h>
#include <rime/dict/dict_settings.h>
#include <rime/dict/dictionary.h>
#include <rime/dict/reverse_lookup_dictionary.h>
#include <rime/engine.h>
#include <rime/gear/table_translator.h>
#include <rime/gear/unity_table_encoder.h>
#include <rime/segmentation.h>
#include <rime/lever/deployment_tasks.h>
#include <rime/service.h>
#include <rime/schema.h>
#include <rime/ticket.h>

using namespace rime;

class RimeBuildDictionaryTest : public ::testing::Test {
 protected:
  void SetUp() override {
    schema_id_ =
        string("build_dictionary_") +
        ::testing::UnitTest::GetInstance()->current_test_info()->name();
    schema_path_ = path{schema_id_ + ".schema.yaml"};
    std::ofstream("build_dict_test.dict.yaml") << R"YAML(---
name: build_dict_test
version: "1.0"
sort: original
use_preset_vocabulary: false
import_tables: [build_dict_source]
encoder:
  rules:
    - length_equal: 2
      formula: AaBa
...
)YAML";
    std::ofstream("build_dict_source.dict.yaml") << R"YAML(---
name: build_dict_source
version: "1.0"
columns: [code, text, weight, stem]
...
ni	拟	1	n
ni	你	100	nr
hao	好	100	h
ni hao	你好	100
)YAML";
    std::ofstream("build_dict_other.dict.yaml") << R"YAML(---
name: build_dict_other
version: "1.0"
...
号	hao	100
)YAML";
  }

  void TearDown() override {
    std::filesystem::remove(schema_path_);
    std::remove((schema_id_ + ".custom.yaml").c_str());
    for (const char* name : {"build_dict_test", "build_dict_other",
                             "build_dict_pack_a", "build_dict_pack_b"}) {
      for (const char* suffix :
           {".dict.yaml", ".table.bin", ".reverse.bin", ".prism.bin",
            ".packs.table.bin", ".packs.reverse.bin", ".pinyin.prism.bin",
            ".zrm.prism.bin", ".dictionary.prism.bin", ".prism.prism.bin",
            ".prisms.prism.bin"}) {
        std::remove((string(name) + suffix).c_str());
      }
    }
    std::remove("build_dict_source.dict.yaml");
    std::remove("build_external.prism.bin");
    std::remove("build_external_full.prism.bin");
    std::remove("build_saved.prism.bin");
    std::remove("zrm.prism.bin");
    std::remove("build_dict_rules.yaml");
    std::remove("build_dict_encoder.userdb.tsv");
    std::filesystem::remove(path{"build_dict_用户.dict.yaml"});
  }

  void WriteSchema(const string& definitions, bool translator = false) {
    std::ofstream file(schema_path_.c_str());
    file << "schema:\n  schema_id: " << schema_id_
         << "\n  name: build dictionary test\n  version: '1.0'\n";
    if (translator) {
      file << R"YAML(engine:
  processors: [speller, selector, express_editor]
  segmentors: [abc_segmentor]
  translators: [script_translator]
translator:
  dictionary: build_dict_test
  user_dict_mode: off
  prisms:
    - {name: pinyin, option: pinyin}
    - {name: zrm, option: zrm}
)YAML";
    }
    file << definitions;
  }

  static string Lookup(Schema& schema, const string& code) {
    the<Dictionary> dict(
        Dictionary::Require("dictionary")->Create({&schema, "translator"}));
    if (!dict || !dict->Load())
      return {};
    DictEntryIterator entries;
    dict->LookupWords(&entries, code, false);
    return entries.exhausted() ? string() : entries.Peek()->text;
  }

  static string Lookup(const string& dictionary,
                       const string& prism,
                       const string& code) {
    Schema schema("build_lookup", new Config);
    schema.config()->SetString("translator/dictionary", dictionary.c_str());
    schema.config()->SetString("translator/prism", prism.c_str());
    return Lookup(schema, code);
  }

  static string Candidates(RimeSessionId session, const char* input) {
    auto api = rime_get_api();
    api->clear_composition(session);
    for (const char* key = input; *key; ++key)
      api->process_key(session, static_cast<unsigned char>(*key), 0);
    RIME_STRUCT(RimeContext, context);
    string result;
    if (api->get_context(session, &context)) {
      for (int i = 0; i < context.menu.num_candidates; ++i) {
        if (i)
          result += '|';
        result += context.menu.candidates[i].text;
      }
      api->free_context(&context);
    }
    return result;
  }

  the<Engine> OpenEngine() {
    the<Config> config(new Config);
    EXPECT_TRUE(config->LoadFromFile(schema_path_));
    the<Engine> engine(Engine::Create());
    engine->ApplySchema(new Schema(schema_id_, config.release()));
    return engine;
  }

  static string Query(Translator* translator,
                      const string& input,
                      const string& tag = "abc") {
    Segment segment(0, input.size());
    segment.tags.insert(tag);
    auto translation = translator->Query(input, segment);
    string result;
    while (translation && !translation->exhausted()) {
      if (!result.empty())
        result += '|';
      result += translation->Peek()->text();
      translation->Next();
    }
    return result;
  }

  static constexpr const char* kDefinitions = R"YAML(build:
  dictionaries:
    build_dict_test:
      algebra:
        pinyin: []
        zrm:
          - xform/^hao$/hk/
    build_dict_other:
      algebra:
        pinyin: []
        zrm:
          - xform/^hao$/hx/
)YAML";

  string schema_id_;
  path schema_path_;
};

TEST_F(RimeBuildDictionaryTest,
       BuildsWithoutConsumersAndKeepsNamespacesSeparate) {
  WriteSchema(kDefinitions);
  SchemaUpdate update(schema_path_);
  ASSERT_TRUE(update.Run(&Service::instance().deployer()));
  for (const char* name : {"build_dict_test", "build_dict_other"}) {
    for (const char* suffix : {".table.bin", ".reverse.bin",
                               ".pinyin.prism.bin", ".zrm.prism.bin"}) {
      EXPECT_TRUE(std::filesystem::exists(string(name) + suffix));
    }
    EXPECT_FALSE(std::filesystem::exists(string(name) + ".prism.bin"));
  }
  EXPECT_EQ("好", Lookup("build_dict_test", "zrm", "hk"));
  EXPECT_EQ("号", Lookup("build_dict_other", "zrm", "hx"));
  EXPECT_TRUE(Lookup("build_dict_other", "zrm", "hk").empty());
}

TEST_F(RimeBuildDictionaryTest,
       ResolvesAlgebraPatchesThroughSchemaCompilation) {
  std::ofstream("build_dict_rules.yaml") << R"YAML(natural:
  __append:
    - xform/^hao$/hk/
)YAML";
  WriteSchema(R"YAML(build:
  dictionaries:
    build_dict_test:
      algebra:
        zrm:
          __patch:
            - build_dict_rules:/natural
)YAML");
  SchemaUpdate update(schema_path_);
  ASSERT_TRUE(update.Run(&Service::instance().deployer()));
  EXPECT_EQ("好", Lookup("build_dict_test", "zrm", "hk"));
}

TEST_F(RimeBuildDictionaryTest, CustomPatchesNamedAlgebraAndAddsDictionary) {
  std::ofstream("build_dict_rules.yaml") << R"YAML(natural:
  __append:
    - xform/^hao$/hk/
)YAML";
  WriteSchema(R"YAML(build:
  dictionaries:
    build_dict_test:
      algebra:
        zrm: []
)YAML");
  std::ofstream(schema_id_ + ".custom.yaml") << R"YAML(patch:
  build/dictionaries/build_dict_test/algebra/zrm:
    __patch:
      - build_dict_rules:/natural
  build/dictionaries/build_dict_other: {}
)YAML";
  SchemaUpdate update(schema_path_);
  ASSERT_TRUE(update.Run(&Service::instance().deployer()));
  EXPECT_EQ("好", Lookup("build_dict_test", "zrm", "hk"));
  EXPECT_TRUE(Lookup("build_dict_test", "zrm", "hao").empty());
  EXPECT_EQ("号", Lookup("build_dict_other", "build_dict_other", "hao"));
}

TEST_F(RimeBuildDictionaryTest, RuntimeSwitchesDictionaryQualifiedPrisms) {
  WriteSchema(kDefinitions, true);
  ASSERT_TRUE(DictCompiler::CompileDictionaries(schema_path_));
  auto api = rime_get_api();
  const auto session = api->create_session();
  ASSERT_NE(0, session);
  EXPECT_TRUE(api->select_schema(session, schema_id_.c_str()));
  api->set_option(session, "pinyin", true);
  EXPECT_NE(string::npos, Candidates(session, "hao").find("好"));
  api->set_option(session, "pinyin", false);
  api->set_option(session, "zrm", true);
  EXPECT_NE(string::npos, Candidates(session, "hk").find("好"));
  api->destroy_session(session);
}

TEST_F(RimeBuildDictionaryTest, DoesNotInferBuildsFromTranslatorsOrSpeller) {
  WriteSchema(R"YAML(speller:
  dictionary: nonexistent_speller_dictionary
  aux_dictionary: nonexistent_auxiliary
  aux_algebra:
    - "aux|^(.*)$|$1|"
)YAML",
              true);
  SchemaUpdate update(schema_path_);
  ASSERT_TRUE(update.Run(&Service::instance().deployer()));
  EXPECT_FALSE(std::filesystem::exists("build_dict_test.table.bin"));
  EXPECT_FALSE(std::filesystem::exists("build_dict_test.pinyin.prism.bin"));
}

TEST_F(RimeBuildDictionaryTest,
       PreservesSourceColumnsSortingAndEncoderMetadata) {
  WriteSchema(kDefinitions);
  ASSERT_TRUE(DictCompiler::CompileDictionaries(schema_path_));
  EXPECT_EQ("拟", Lookup("build_dict_test", "pinyin", "ni"));
  ReverseLookupDictionary reverse(
      New<ReverseDb>(path{"build_dict_test.reverse.bin"}));
  ASSERT_TRUE(reverse.Load());
  string code;
  ASSERT_TRUE(reverse.ReverseLookup("你", &code));
  EXPECT_EQ("ni", code);
  ASSERT_TRUE(reverse.LookupStems("你", &code));
  EXPECT_EQ("nr", code);
  auto settings = reverse.GetDictSettings();
  ASSERT_TRUE(settings);
  EXPECT_TRUE(settings->use_rule_based_encoder());
  ASSERT_TRUE(settings->GetString("encoder/rules/@0/formula", &code));
  EXPECT_EQ("AaBa", code);
}

TEST_F(RimeBuildDictionaryTest, RebuildsOnlyTheChangedPrism) {
  WriteSchema(kDefinitions);
  ASSERT_TRUE(DictCompiler::CompileDictionaries(schema_path_));
  const auto table_time =
      std::filesystem::last_write_time("build_dict_test.table.bin");
  const auto pinyin_time =
      std::filesystem::last_write_time("build_dict_test.pinyin.prism.bin");
  Config schema;
  ASSERT_TRUE(schema.LoadFromFile(schema_path_));
  auto rules = schema.GetList("build/dictionaries/build_dict_test/algebra/zrm");
  ASSERT_TRUE(rules);
  ASSERT_TRUE(rules->Append(New<ConfigValue>("derive/^hk$/hkx/")));
  ASSERT_TRUE(schema.SaveToFile(schema_path_));
  ASSERT_TRUE(DictCompiler::CompileDictionaries(schema_path_));
  EXPECT_EQ(table_time,
            std::filesystem::last_write_time("build_dict_test.table.bin"));
  EXPECT_EQ(pinyin_time, std::filesystem::last_write_time(
                             "build_dict_test.pinyin.prism.bin"));
  EXPECT_EQ("好", Lookup("build_dict_test", "zrm", "hkx"));
}

TEST_F(RimeBuildDictionaryTest, FlatAndMissingAlgebraUseDictionaryPrismName) {
  WriteSchema(R"YAML(build:
  dictionaries:
    build_dict_test:
      algebra: [xform/^hao$/hk/]
    build_dict_other: {}
)YAML");
  ASSERT_TRUE(DictCompiler::CompileDictionaries(schema_path_));
  EXPECT_EQ("好", Lookup("build_dict_test", "build_dict_test", "hk"));
  EXPECT_EQ("号", Lookup("build_dict_other", "build_dict_other", "hao"));
}

TEST_F(RimeBuildDictionaryTest, DefaultsToFirstGroupInsteadOfUnqualifiedPrism) {
  WriteSchema(kDefinitions);
  ASSERT_TRUE(DictCompiler::CompileDictionaries(schema_path_));
  EXPECT_FALSE(std::filesystem::exists("build_dict_test.prism.bin"));
  Schema schema("build_lookup", new Config);
  ASSERT_TRUE(schema.config()->LoadFromFile(schema_path_));
  ASSERT_TRUE(
      schema.config()->SetString("translator/dictionary", "build_dict_test"));
  EXPECT_EQ("好", Lookup(schema, "hao"));
  EXPECT_TRUE(Lookup(schema, "hk").empty());

  std::filesystem::copy_file("build_dict_test.zrm.prism.bin",
                             "build_dict_test.prism.bin");
  EXPECT_EQ("好", Lookup(schema, "hao"));
  EXPECT_TRUE(Lookup(schema, "hk").empty());

  ASSERT_TRUE(schema.config()->SetString("translator/prism", "zrm"));
  EXPECT_EQ("好", Lookup(schema, "hk"));
  EXPECT_TRUE(Lookup(schema, "hao").empty());
}

TEST_F(RimeBuildDictionaryTest,
       UngroupedDefaultsAndExternalDictionaryStayUsable) {
  WriteSchema(R"YAML(build:
  dictionaries:
    build_dict_test:
      algebra: [xform/^hao$/hk/]
    build_dict_other: {}
)YAML");
  ASSERT_TRUE(DictCompiler::CompileDictionaries(schema_path_));
  Schema schema("build_lookup", new Config);
  ASSERT_TRUE(schema.config()->LoadFromFile(schema_path_));
  ASSERT_TRUE(
      schema.config()->SetString("translator/dictionary", "build_dict_test"));
  EXPECT_EQ("好", Lookup(schema, "hk"));
  ASSERT_TRUE(
      schema.config()->SetString("translator/dictionary", "build_dict_other"));
  EXPECT_EQ("号", Lookup(schema, "hao"));
  ASSERT_TRUE(schema.config()->SetItem("build", nullptr));
  EXPECT_EQ("号", Lookup(schema, "hao"));
}

TEST_F(RimeBuildDictionaryTest, RejectsDuplicateTargetsAndMalformedAlgebra) {
  for (const char* invalid :
       {"build:\n  dictionaries: [build_dict_test]\n",
        "build:\n  dictionaries:\n    build_dict_test: invalid\n",
        "build:\n  dictionaries:\n    build_dict_test:\n"
        "      algebra: {zrm: invalid}\n",
        "build:\n  dictionaries:\n    build_dict_test:\n"
        "      algebra: {zrm: []}\n"
        "    build_dict_test.zrm: {}\n",
        "build:\n  dictionaries:\n    build_dict_test:\n"
        "      packs: [build_dict_test]\n",
        "build:\n  dictionaries:\n    build_dict_test:\n"
        "      packs: [build_dict_other]\n"
        "    build_dict_test.packs: {}\n"}) {
    SCOPED_TRACE(invalid);
    WriteSchema(invalid);
    EXPECT_FALSE(DictCompiler::CompileDictionaries(schema_path_));
    EXPECT_FALSE(std::filesystem::exists("build_dict_test.table.bin"));
  }
}

TEST_F(RimeBuildDictionaryTest,
       LoadsExplicitExternalPrismWithoutBuildDefinition) {
  WriteSchema(kDefinitions);
  ASSERT_TRUE(DictCompiler::CompileDictionaries(schema_path_));
  std::filesystem::copy_file("build_dict_test.zrm.prism.bin",
                             "build_external.prism.bin");
  EXPECT_EQ("好", Lookup("build_dict_test", "build_external", "hk"));
}

TEST_F(RimeBuildDictionaryTest,
       TableTranslatorUsesGroupedDefaultAndCompletion) {
  WriteSchema(string(kDefinitions) + R"YAML(translator:
  dictionary: build_dict_test
  user_dict_mode: off
  enable_completion: true
  enable_sentence: false
)YAML");
  ASSERT_TRUE(DictCompiler::CompileDictionaries(schema_path_));
  auto engine = OpenEngine();
  the<Translator> translator(Translator::Require("table_translator")
                                 ->Create(Ticket(engine.get(), "translator")));
  ASSERT_TRUE(translator);
  EXPECT_NE(string::npos, Query(translator.get(), "hao").find("好"));
  EXPECT_NE(string::npos, Query(translator.get(), "ha").find("好"));
  EXPECT_TRUE(Query(translator.get(), "hk").empty());
}

TEST_F(RimeBuildDictionaryTest,
       TableTranslatorUsesExplicitPrismForExactLookup) {
  WriteSchema(string(kDefinitions) + R"YAML(translator:
  dictionary: build_dict_test
  prism: zrm
  user_dict_mode: off
  enable_completion: false
  enable_sentence: false
)YAML");
  ASSERT_TRUE(DictCompiler::CompileDictionaries(schema_path_));
  auto engine = OpenEngine();
  the<Translator> translator(Translator::Require("table_translator")
                                 ->Create(Ticket(engine.get(), "translator")));
  ASSERT_TRUE(translator);
  EXPECT_NE(string::npos, Query(translator.get(), "hk").find("好"));
  EXPECT_TRUE(Query(translator.get(), "h").empty());
  EXPECT_TRUE(Query(translator.get(), "hao").empty());
}

TEST_F(RimeBuildDictionaryTest, TableEncoderLoadsStemsAndCreatesUserPhrase) {
  WriteSchema(string(kDefinitions) + R"YAML(translator:
  dictionary: build_dict_test
  prism: zrm
  user_dict: build_dict_encoder
  db_class: plain_userdb
  user_dict_mode: auto
  enable_encoder: true
  enable_completion: false
  enable_sentence: false
)YAML");
  ASSERT_TRUE(DictCompiler::CompileDictionaries(schema_path_));
  auto engine = OpenEngine();
  the<Translator> translator(Translator::Require("table_translator")
                                 ->Create(Ticket(engine.get(), "translator")));
  ASSERT_TRUE(translator);
  auto table = static_cast<TableTranslator*>(translator.get());
  ASSERT_TRUE(table->encoder());
  ASSERT_TRUE(table->encoder()->loaded());
  vector<string> codes;
  ASSERT_TRUE(table->encoder()->TranslateWord("你", &codes));
  ASSERT_EQ(1u, codes.size());
  EXPECT_EQ("nr", codes.front());
  ASSERT_TRUE(table->encoder()->EncodePhrase("你好", "1"));
  EXPECT_NE(string::npos, Query(translator.get(), "nh").find("你好"));
}

TEST_F(RimeBuildDictionaryTest, MemoryEstimateReadsUnicodeHeaderAndImports) {
  const path unicode_source{"build_dict_用户.dict.yaml"};
  {
    std::ofstream header(
        static_cast<const std::filesystem::path&>(unicode_source));
    header << "---\nname: build_dict_用户\nversion: '1.0'\n"
              "import_tables: [build_dict_source]\n...\n";
  }
  const auto original = DictCompiler::EstimateMemory("build_dict_用户");
  {
    std::ofstream source("build_dict_source.dict.yaml", std::ios::app);
    source << "extra\t额外\t1\n";
  }
  EXPECT_GT(DictCompiler::EstimateMemory("build_dict_用户"), original);
}

TEST_F(RimeBuildDictionaryTest, PacksSourcesShareOneIndependentTable) {
  for (const char* name : {"build_dict_pack_a", "build_dict_pack_b"}) {
    std::ofstream file(string(name) + ".dict.yaml");
    file << "---\nname: " << name << "\nversion: '1.0'\n...\n";
    file << (string(name) == "build_dict_pack_a" ? "妳\tni\t50\n"
                                                 : "豪\thao\t200\n");
  }
  WriteSchema(R"YAML(build:
  dictionaries:
    build_dict_test:
      packs: [build_dict_pack_a, build_dict_pack_b]
      algebra:
        zrm: [xform/^hao$/hk/]
translator:
  dictionary: build_dict_test
  prism: zrm
  user_dict_mode: off
  enable_completion: false
  enable_sentence: false
)YAML");
  ASSERT_TRUE(DictCompiler::CompileDictionaries(schema_path_));
  EXPECT_TRUE(std::filesystem::exists("build_dict_test.packs.table.bin"));
  EXPECT_FALSE(std::filesystem::exists("build_dict_test.packs.reverse.bin"));
  EXPECT_FALSE(std::filesystem::exists("build_dict_pack_a.table.bin"));
  EXPECT_FALSE(std::filesystem::exists("build_dict_pack_b.table.bin"));
  std::ofstream("build_dict_pack_b.dict.yaml", std::ios::app)
      << "毫\thao\t150\n";
  ASSERT_TRUE(DictCompiler::CompileDictionaries(schema_path_));
  auto engine = OpenEngine();
  the<Translator> translator(Translator::Require("table_translator")
                                 ->Create(Ticket(engine.get(), "translator")));
  ASSERT_TRUE(translator);
  const auto candidates = Query(translator.get(), "hk");
  EXPECT_NE(string::npos, candidates.find("好"));
  EXPECT_NE(string::npos, candidates.find("豪"));
  EXPECT_NE(string::npos, candidates.find("毫"));
  EXPECT_NE(string::npos, Query(translator.get(), "ni").find("妳"));
}

TEST_F(RimeBuildDictionaryTest, ReverseTranslatorUsesDictionaryQualifiedPrism) {
  WriteSchema(string(kDefinitions) + R"YAML(translator:
  dictionary: build_dict_test
lookup:
  dictionary: build_dict_test
  prism: zrm
  prefix: "`"
  tag: reverse_lookup
)YAML");
  ASSERT_TRUE(DictCompiler::CompileDictionaries(schema_path_));
  auto engine = OpenEngine();
  the<Translator> translator(Translator::Require("reverse_lookup_translator")
                                 ->Create(Ticket(engine.get(), "lookup")));
  ASSERT_TRUE(translator);
  EXPECT_NE(string::npos,
            Query(translator.get(), "`hk", "reverse_lookup").find("好"));
  EXPECT_TRUE(Query(translator.get(), "`hao", "reverse_lookup").empty());
}

TEST_F(RimeBuildDictionaryTest, AllDictionaryTranslatorsSwitchPrisms) {
  for (bool completion : {false, true}) {
    WriteSchema(string(kDefinitions) + R"YAML(lookup:
  dictionary: build_dict_test
  user_dict_mode: off
  enable_sentence: false
  prefix: "`"
  tag: abc
  target: lookup
  prisms:
    - {name: pinyin, option: full_input}
    - {name: zrm, option: compact_input}
)YAML" +
                (completion ? "  enable_completion: true\n"
                            : "  enable_completion: false\n"));
    ASSERT_TRUE(DictCompiler::CompileDictionaries(schema_path_));
    for (const char* type : {"table_translator", "script_translator",
                             "reverse_lookup_translator"}) {
      SCOPED_TRACE(type);
      SCOPED_TRACE(completion);
      auto engine = OpenEngine();
      auto* context = engine->context();
      the<Translator> translator(
          Translator::Require(type)->Create(Ticket(engine.get(), "lookup")));
      ASSERT_TRUE(translator);
      const string prefix =
          string(type) == "reverse_lookup_translator" ? "`" : "";
      auto query = [&](const string& code) {
        context->set_input(prefix + code);
        return Query(translator.get(), prefix + code);
      };
      EXPECT_EQ("好", query("hao"));
      EXPECT_TRUE(query("hk").empty());
      context->set_option("compact_input", true);
      EXPECT_EQ("好", query("hk"));
      EXPECT_TRUE(query("hao").empty());
      context->set_option("compact_input", false);
      context->set_option("full_input", true);
      EXPECT_EQ("好", query("hao"));
      EXPECT_TRUE(query("hk").empty());
    }
  }
}

TEST_F(RimeBuildDictionaryTest, TableOptionRefreshesExistingComposition) {
  WriteSchema(string(kDefinitions) + R"YAML(engine:
  processors: [speller, selector, express_editor]
  segmentors: [abc_segmentor]
  translators: [table_translator]
translator:
  dictionary: build_dict_test
  user_dict_mode: off
  enable_completion: false
  enable_sentence: false
  prisms:
    - {name: pinyin, option: full_input}
    - {name: zrm, option: compact_input}
)YAML");
  ASSERT_TRUE(DictCompiler::CompileDictionaries(schema_path_));
  auto api = rime_get_api();
  auto session = api->create_session();
  ASSERT_NE(0, session);
  ASSERT_TRUE(api->select_schema(session, schema_id_.c_str()));
  EXPECT_TRUE(Candidates(session, "hk").empty());
  api->set_option(session, "compact_input", true);
  RIME_STRUCT(RimeContext, context);
  ASSERT_TRUE(api->get_context(session, &context));
  ASSERT_GT(context.menu.num_candidates, 0);
  EXPECT_STREQ("好", context.menu.candidates[0].text);
  api->free_context(&context);
  api->set_option(session, "compact_input", false);
  ASSERT_TRUE(api->get_context(session, &context));
  EXPECT_EQ(0, context.menu.num_candidates);
  api->free_context(&context);
  api->destroy_session(session);
}

TEST_F(RimeBuildDictionaryTest,
       RejectsConflictingPrismsInDeploymentAndQueries) {
  const string definitions = string(kDefinitions) + R"YAML(nested:
  lookup:
    dictionary: build_dict_test
    user_dict_mode: off
    enable_sentence: false
    tag: abc
    prism: zrm
    prisms:
      - {name: pinyin, option: full_input}
)YAML";
  WriteSchema(definitions);
  SchemaUpdate update(schema_path_);
  EXPECT_FALSE(update.Run(&Service::instance().deployer()));
  EXPECT_FALSE(std::filesystem::exists("build_dict_test.table.bin"));
  WriteSchema(kDefinitions);
  ASSERT_TRUE(DictCompiler::CompileDictionaries(schema_path_));
  WriteSchema(definitions);
  Schema schema("conflicting_prisms", new Config);
  ASSERT_TRUE(schema.config()->LoadFromFile(schema_path_));
  the<Dictionary> dict(
      Dictionary::Require("dictionary")->Create({&schema, "nested/lookup"}));
  EXPECT_FALSE(dict);
  for (const char* type :
       {"table_translator", "script_translator", "reverse_lookup_translator"}) {
    SCOPED_TRACE(type);
    auto engine = OpenEngine();
    engine->context()->set_input("hk");
    the<Translator> translator(Translator::Require(type)->Create(
        Ticket(engine.get(), "nested/lookup")));
    ASSERT_TRUE(translator);
    EXPECT_TRUE(Query(translator.get(), "hk").empty());
  }
}

TEST_F(RimeBuildDictionaryTest, RejectsEmptyOrMalformedPrismSelections) {
  for (const char* invalid :
       {"prisms: []", "prisms: zrm", "prism: []", "prisms: [{name: zrm}]",
        "prisms: [{name: zrm, option: ''}]",
        "prism: ''\n  prisms: [{name: zrm, option: zrm}]"}) {
    SCOPED_TRACE(invalid);
    WriteSchema(string(kDefinitions) +
                "lookup:\n  dictionary: build_dict_test\n  " + invalid + "\n");
    EXPECT_FALSE(DictCompiler::CompileDictionaries(schema_path_));
    EXPECT_FALSE(std::filesystem::exists("build_dict_test.table.bin"));
  }
}

TEST_F(RimeBuildDictionaryTest,
       MissingGroupNeverLoadsUnrelatedPrismAndCanRecover) {
  WriteSchema(kDefinitions);
  ASSERT_TRUE(DictCompiler::CompileDictionaries(schema_path_));
  std::filesystem::copy_file("build_dict_other.zrm.prism.bin", "zrm.prism.bin");
  std::filesystem::copy_file("build_dict_test.zrm.prism.bin",
                             "build_saved.prism.bin");
  for (bool dynamic : {false, true}) {
    WriteSchema(string(kDefinitions) + R"YAML(lookup:
  dictionary: build_dict_test
  user_dict_mode: off
  enable_sentence: false
  enable_completion: false
  tag: abc
  target: lookup
)YAML" +
                (dynamic ? R"YAML(  prisms:
    - {name: pinyin, option: full_input}
    - {name: zrm, option: compact_input}
)YAML"
                         : "  prism: zrm\n"));
    for (const char* type : {"table_translator", "script_translator",
                             "reverse_lookup_translator"}) {
      SCOPED_TRACE(type);
      SCOPED_TRACE(dynamic);
      std::remove("build_dict_test.zrm.prism.bin");
      auto engine = OpenEngine();
      engine->context()->set_option("compact_input", true);
      engine->context()->set_input("hx");
      the<Translator> translator(
          Translator::Require(type)->Create(Ticket(engine.get(), "lookup")));
      ASSERT_TRUE(translator);
      EXPECT_TRUE(Query(translator.get(), "hx").empty());
      EXPECT_TRUE(Query(translator.get(), "hk").empty());
      std::filesystem::copy_file("build_saved.prism.bin",
                                 "build_dict_test.zrm.prism.bin");
      engine->context()->set_input("hk");
      EXPECT_EQ("好", Query(translator.get(), "hk"));
    }
  }
}

TEST_F(RimeBuildDictionaryTest,
       ExternalPrismsSwitchWithoutLocalBuildDefinitions) {
  WriteSchema(kDefinitions);
  ASSERT_TRUE(DictCompiler::CompileDictionaries(schema_path_));
  std::filesystem::copy_file("build_dict_test.zrm.prism.bin",
                             "build_external.prism.bin");
  std::filesystem::copy_file("build_dict_test.pinyin.prism.bin",
                             "build_external_full.prism.bin");
  WriteSchema(R"YAML(lookup:
  dictionary: build_dict_test
  user_dict_mode: off
  enable_completion: false
  enable_sentence: false
  tag: abc
  target: lookup
  prisms:
    - {name: build_external, option: compact_input}
    - {name: build_external_full, option: full_input}
)YAML");
  for (const char* type :
       {"table_translator", "script_translator", "reverse_lookup_translator"}) {
    SCOPED_TRACE(type);
    auto engine = OpenEngine();
    the<Translator> translator(
        Translator::Require(type)->Create(Ticket(engine.get(), "lookup")));
    engine->context()->set_input("hk");
    EXPECT_EQ("好", Query(translator.get(), "hk"));
    engine->context()->set_option("full_input", true);
    engine->context()->set_input("hao");
    EXPECT_EQ("好", Query(translator.get(), "hao"));
  }
}

TEST_F(RimeBuildDictionaryTest, AlgebraGroupNamesAreNotPrismReferences) {
  WriteSchema(R"YAML(build:
  dictionaries:
    build_dict_test:
      algebra:
        dictionary: []
        prism: [xform/^hao$/hk/]
        prisms: []
)YAML");
  ASSERT_TRUE(DictCompiler::CompileDictionaries(schema_path_));
  EXPECT_EQ("好", Lookup("build_dict_test", "prism", "hk"));
  EXPECT_EQ("好", Lookup("build_dict_test", "prisms", "hao"));
}

TEST_F(RimeBuildDictionaryTest, CustomCanReplaceFixedPrismWithSelections) {
  WriteSchema(string(kDefinitions) + R"YAML(lookup:
  dictionary: build_dict_test
  user_dict_mode: off
  enable_completion: false
  enable_sentence: false
  prism: zrm
)YAML");
  std::ofstream(schema_id_ + ".custom.yaml") << R"YAML(patch:
  lookup/prism: null
  lookup/prisms:
    - {name: pinyin, option: full_input}
    - {name: zrm, option: compact_input}
)YAML";
  SchemaUpdate update(schema_path_);
  ASSERT_TRUE(update.Run(&Service::instance().deployer()));
  the<Engine> engine(Engine::Create());
  engine->ApplySchema(new Schema(schema_id_));
  the<Translator> translator(Translator::Require("table_translator")
                                 ->Create(Ticket(engine.get(), "lookup")));
  EXPECT_EQ("好", Query(translator.get(), "hao"));
  engine->context()->set_option("compact_input", true);
  EXPECT_EQ("好", Query(translator.get(), "hk"));
}

TEST_F(RimeBuildDictionaryTest, TableUserEntriesWorkWithoutStaticDictionary) {
  WriteSchema(R"YAML(lookup:
  dictionary: ""
  user_dict: build_dict_encoder
  db_class: plain_userdb
  user_dict_mode: auto
  enable_completion: false
  enable_sentence: false
)YAML");
  auto engine = OpenEngine();
  the<Translator> translator(Translator::Require("table_translator")
                                 ->Create(Ticket(engine.get(), "lookup")));
  ASSERT_TRUE(translator);
  auto table = static_cast<TableTranslator*>(translator.get());
  ASSERT_FALSE(table->dict());
  ASSERT_TRUE(table->user_dict());
  DictEntry entry;
  entry.text = "好";
  entry.custom_code = "hk ";
  ASSERT_TRUE(table->user_dict()->UpdateEntry(entry, 1));
  EXPECT_EQ("好", Query(translator.get(), "hk"));
}
