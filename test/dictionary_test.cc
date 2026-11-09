//
// Copyright RIME Developers
// Distributed under the BSD License
//
// 2011-07-05 GONG Chen <chen.sst@gmail.com>
//
#include <gtest/gtest.h>
#include <limits>
#include <sstream>
#include <rime/dict/dict_settings.h>
#include <rime/dict/entry_collector.h>
#include <rime/common.h>
#include <rime/algo/encoder.h>
#include <rime/algo/syllabifier.h>
#include <rime/dict/dictionary.h>
#include <rime/dict/dict_compiler.h>

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

TEST_F(RimeDictionaryTest, ScriptLookupWithExclude) {
  ASSERT_TRUE(dict_->loaded());
  rime::SyllableGraph g;
  rime::Syllabifier s;
  rime::string input("gei");
  rime::hash_set<rime::string> blacklist{"\xe7\xbb\x99"};  // 给
  ASSERT_TRUE(s.BuildSyllableGraph(input, *dict_->prism(), &g) > 0);
  EXPECT_EQ(g.interpreted_length, g.input_length);
  auto c = dict_->Lookup(g, 0, &blacklist);
  EXPECT_TRUE(!c);
}

TEST_F(RimeDictionaryTest, TableLookupWithExclude) {
  ASSERT_TRUE(dict_->loaded());
  rime::string input("gei");
  rime::hash_set<rime::string> blacklist{"\xe7\xbb\x99"};  // 给
  rime::DictEntryIterator iter;
  size_t keys = dict_->LookupWords(&iter, input, true, 100, &blacklist);
  EXPECT_EQ(keys, 1);
  EXPECT_TRUE(iter.exhausted());
}

namespace {

std::vector<std::string> DrainEntries(rime::Dictionary* dictionary,
                                      rime::DictEntryIterator& iterator) {
  std::vector<std::string> entries;
  while (!iterator.exhausted()) {
    auto entry = iterator.Peek();
    rime::RawCode code;
    if (!entry || !dictionary->Decode(entry->code, &code)) {
      ADD_FAILURE() << "invalid dictionary entry";
      break;
    }
    entries.push_back(entry->text + "\t" + code.ToString());
    iterator.Next();
  }
  return entries;
}

void ExpectBestFirst(rime::Dictionary* dictionary,
                     const rime::string& code,
                     size_t minimum) {
  rime::DictEntryIterator iterator;
  dictionary->LookupWords(&iterator, code, true);
  const size_t total = iterator.entry_count();
  ASSERT_GE(total, minimum);
  rime::an<rime::DictEntry> previous;
  rime::set<std::pair<rime::string, rime::string>> seen;
  size_t count = 0;
  while (!iterator.exhausted()) {
    auto entry = iterator.Peek();
    ASSERT_TRUE(entry);
    rime::RawCode spelling;
    ASSERT_TRUE(dictionary->Decode(entry->code, &spelling));
    EXPECT_TRUE(seen.emplace(entry->text, spelling.ToString()).second);
    // LookupWords preserves the first matching key before heap ordering begins.
    if (count > 1) {
      if (previous->remaining_code_length == entry->remaining_code_length)
        EXPECT_GE(previous->weight, entry->weight);
      else
        EXPECT_LT(previous->remaining_code_length,
                  entry->remaining_code_length);
    }
    previous = entry;
    ++count;
    iterator.Next();
  }
  EXPECT_EQ(total, count);
}

}  // namespace

TEST_F(RimeDictionaryTest, PredictiveLookupOrder) {
  ExpectBestFirst(dict_.get(), "z", 50);
  ExpectBestFirst(dict_.get(), "", 1000);
}

TEST_F(RimeDictionaryTest, CopiedIteratorsAreIndependent) {
  for (bool heap : {false, true}) {
    SCOPED_TRACE(heap);
    rime::DictEntryIterator original;
    dict_->LookupWords(&original, "z", true);
    if (heap) {
      ASSERT_TRUE(original.Next());
    }
    const auto cached = original.Peek();
    ASSERT_TRUE(cached);
    rime::DictEntryIterator copy(original);
    rime::DictEntryIterator assigned;
    assigned = original;
    auto fork = original.Fork();
    const auto expected = DrainEntries(dict_.get(), copy);
    EXPECT_EQ(cached, original.Peek());
    EXPECT_EQ(expected, DrainEntries(dict_.get(), assigned));
    EXPECT_EQ(expected, DrainEntries(dict_.get(), fork));
    EXPECT_EQ(expected, DrainEntries(dict_.get(), original));
  }
}

TEST_F(RimeDictionaryTest, SkipDropsCachedEntryAndRebuildsHeap) {
  for (bool heap : {false, true}) {
    SCOPED_TRACE(heap);
    rime::DictEntryIterator cached;
    rime::DictEntryIterator fresh;
    dict_->LookupWords(&cached, "z", true);
    dict_->LookupWords(&fresh, "z", true);
    if (heap) {
      ASSERT_TRUE(cached.Next());
      ASSERT_TRUE(fresh.Next());
    }
    const auto first = cached.Peek();
    ASSERT_TRUE(first);
    ASSERT_TRUE(cached.Skip(0));
    EXPECT_EQ(first, cached.Peek());
    ASSERT_TRUE(cached.Skip(3));
    ASSERT_TRUE(fresh.Skip(3));
    EXPECT_EQ(DrainEntries(dict_.get(), fresh),
              DrainEntries(dict_.get(), cached));
  }
  rime::DictEntryIterator oversized;
  dict_->LookupWords(&oversized, "z", true);
  ASSERT_TRUE(oversized.Skip(1));
  EXPECT_FALSE(oversized.Skip(std::numeric_limits<size_t>::max()));
  EXPECT_TRUE(oversized.exhausted());
}
