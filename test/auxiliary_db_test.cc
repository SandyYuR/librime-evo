// Copyright RIME Developers
// Distributed under the BSD License
//
#include <cstdio>
#include <map>
#include <gtest/gtest.h>
#include <rime/dict/auxiliary_db.h>

namespace {

using rime::AuxiliaryCharacter;
using rime::AuxiliaryCode;
using rime::AuxiliaryDb;
using rime::kAbbreviation;
using rime::kNormalSpelling;
using rime::path;
using rime::string;

constexpr const char* kAuxiliaryFile = "auxiliary_db_test.aux.bin";

TEST(AuxiliaryDbTest, SavesForwardAndReverseIndexes) {
  std::map<string, std::vector<AuxiliaryCode>> by_character;
  by_character["你"] = {{"r", kNormalSpelling, 0.0},
                        {"rx", kAbbreviation, -1.0},
                        {"rx/", kNormalSpelling, 0.0}};
  std::map<string, std::vector<AuxiliaryCharacter>> by_code;
  by_code["r"] = {{"你", kNormalSpelling, 0.0}};
  by_code["rx"] = {{"你", kAbbreviation, -1.0}};
  by_code["rx/"] = {{"你", kNormalSpelling, 0.0}};

  AuxiliaryDb database(path{kAuxiliaryFile});
  database.Remove();
  ASSERT_TRUE(database.Build(by_character, by_code, 1234));
  ASSERT_TRUE(database.Save());
  database.Close();

  AuxiliaryDb loaded(path{kAuxiliaryFile});
  ASSERT_TRUE(loaded.Load());
  EXPECT_EQ(1234u, loaded.checksum());

  std::vector<AuxiliaryCode> codes;
  ASSERT_TRUE(loaded.ReverseLookup("你", &codes));
  ASSERT_EQ(3u, codes.size());
  EXPECT_EQ("r", codes[0].code);
  EXPECT_EQ(kNormalSpelling, codes[0].type);
  EXPECT_EQ("rx", codes[1].code);
  EXPECT_EQ(kAbbreviation, codes[1].type);
  EXPECT_EQ("rx/", codes[2].code);

  std::vector<std::pair<size_t, std::vector<AuxiliaryCharacter>>> prefixes;
  loaded.CommonPrefixLookup("rx/", &prefixes);
  ASSERT_EQ(3u, prefixes.size());
  EXPECT_EQ(1u, prefixes[0].first);
  EXPECT_EQ(2u, prefixes[1].first);
  EXPECT_EQ(3u, prefixes[2].first);
  ASSERT_EQ(1u, prefixes[1].second.size());
  EXPECT_EQ("你", prefixes[1].second[0].text);
  EXPECT_EQ(kAbbreviation, prefixes[1].second[0].type);

  loaded.Close();
  EXPECT_TRUE(loaded.Remove());
}

}  // namespace
