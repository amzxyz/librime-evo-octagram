#include <gtest/gtest.h>
#include <cmath>
#include <fstream>
#include <limits>
#include "gram_db.h"
#include "gram_encoding.h"
#include "octagram.h"
#include <rime/config.h>
#include <rime/service.h>

using namespace rime;

class GramDbTest : public testing::Test {
 protected:
  const path model{"octagram-sections-test.dlm"};
  void TearDown() override { std::filesystem::remove(model); }
  static int Score(double weight) {
    return (std::max)(0, int(std::log(weight) * GramDb::kValueScale));
  }
  static GramDb::Source Fixed(vector<string> keys,
                              double weight,
                              string name = "default") {
    return {std::move(keys), {}, weight, std::move(name)};
  }
  static GramDb::Source Weighted(vector<pair<string, double>> entries,
                                 string name = "default") {
    return {{}, std::move(entries), std::nullopt, std::move(name)};
  }
};

TEST_F(GramDbTest, MergesMultipleWeightsAndExplicitScoresInPrefixOrder) {
  GramDb db(model);
  GramDb::Builder builder;
  ASSERT_TRUE(builder.Add(Fixed({"abcd"}, 500000)));
  ASSERT_TRUE(
      builder.Add(Weighted({{"abcdefg", 50}, {"abc", 20}, {"abcde", 30}})));
  ASSERT_TRUE(builder.Add(Fixed({"abcdef"}, 5000000000)));
  ASSERT_TRUE(db.Build(builder));
  ASSERT_TRUE(db.Save());
  for (bool reload : {false, true}) {
    if (reload)
      ASSERT_TRUE(db.Load());
    GramDb::Match result[GramDb::kMaxResults];
    ASSERT_EQ(5, db.Lookup("a", "bcdefg", result));
    const int expected[] = {Score(20), Score(500000), Score(30),
                            Score(5000000000), Score(50)};
    for (int i = 0; i < 5; ++i) {
      EXPECT_EQ(size_t(i + 2), result[i].length);
      EXPECT_EQ(expected[i], result[i].value);
    }
    EXPECT_EQ(0, db.Lookup("az", "bcdefg", result));
    EXPECT_EQ(0, db.Lookup("a", "", result));
  }
}

TEST_F(GramDbTest, LaterSectionOverridesEvenWithALowerWeight) {
  GramDb db(model);
  GramDb::Builder builder;
  ASSERT_TRUE(builder.Add(Fixed({"ab", "abc", "ab", "abcd"}, 500000)));
  ASSERT_TRUE(builder.Add(
      Weighted({{"ab", 2}, {"abc", 10}, {"abc", 20}, {"abc", 15}})));
  ASSERT_TRUE(builder.Add(Fixed({"ab"}, 1)));
  ASSERT_TRUE(db.Build(builder));
  ASSERT_TRUE(db.Save());
  ASSERT_TRUE(db.Load());
  GramDb::Match result[GramDb::kMaxResults];
  ASSERT_EQ(3, db.Lookup("a", "bcd", result));
  EXPECT_EQ(Score(1), result[0].value);
  EXPECT_EQ(Score(20), result[1].value);
  EXPECT_EQ(Score(500000), result[2].value);
}

TEST_F(GramDbTest, ConstantTerminalsContainNoIndividualScores) {
  GramDb db(model);
  GramDb::Builder builder;
  ASSERT_TRUE(builder.Add(Fixed({"ab", "ac", "ad"}, 500000)));
  ASSERT_TRUE(db.Build(builder));
  ASSERT_TRUE(db.Save());
  db.Close();
  std::ifstream input(model, std::ios::binary);
  grammar::Metadata metadata;
  grammar::SectionMetadata section;
  ASSERT_TRUE(input.read(reinterpret_cast<char*>(&metadata), sizeof(metadata)));
  ASSERT_TRUE(input.read(reinterpret_cast<char*>(&section), sizeof(section)));
  EXPECT_EQ(1u, metadata.section_count);
  EXPECT_EQ(Score(500000), section.constant_value);
  EXPECT_DOUBLE_EQ(500000, section.constant_weight);
  Darts::DoubleArray trie;
  const size_t offset = sizeof(metadata) + sizeof(section) +
                        ((metadata.text_bytes + 3) & ~uint64_t(3));
  ASSERT_EQ(
      0, trie.open(model.string().c_str(), "rb", offset, section.array_bytes));
  EXPECT_EQ(0, trie.exactMatchSearch<int>("ab"));
  EXPECT_EQ(0, trie.exactMatchSearch<int>("ac"));
  EXPECT_EQ(0, trie.exactMatchSearch<int>("ad"));
  ASSERT_TRUE(db.Load());
  GramDb::Match result[GramDb::kMaxResults];
  ASSERT_EQ(1, db.Lookup("a", "b", result));
  EXPECT_EQ(Score(500000), result[0].value);
}

TEST_F(GramDbTest, EmptySourcesAreSkippedAndBuilderCanBeReused) {
  GramDb db(model);
  GramDb::Builder builder;
  ASSERT_TRUE(builder.Add(Fixed({}, 500000)));
  ASSERT_TRUE(builder.Add(Weighted({{"ab", 0.5}, {"abc", 10}})));
  ASSERT_TRUE(db.Build(builder));
  ASSERT_TRUE(db.Save());
  ASSERT_TRUE(db.Load());
  GramDb::Match result[GramDb::kMaxResults];
  ASSERT_EQ(2, db.Lookup("a", "bc", result));
  EXPECT_EQ(0, result[0].value);
  EXPECT_EQ(Score(10), result[1].value);
  EXPECT_FALSE(db.Build(builder));
  ASSERT_TRUE(builder.Add(Fixed({"xy"}, 900000)));
  ASSERT_TRUE(db.Build(builder));
  ASSERT_TRUE(db.Save());
  ASSERT_TRUE(db.Load());
  ASSERT_EQ(1, db.Lookup("x", "y", result));
  EXPECT_EQ(Score(900000), result[0].value);
}

TEST_F(GramDbTest, SupportsMoreThanTwoSectionsAndLimitsMergedResults) {
  GramDb::Builder builder;
  for (int i = 20; i >= 1; --i) {
    if (i % 2)
      ASSERT_TRUE(builder.Add(Fixed({string(i, 'a')}, 500000 + i)));
    else
      ASSERT_TRUE(builder.Add(Weighted({{string(i, 'a'), double(10 + i)}})));
  }
  GramDb db(model);
  ASSERT_TRUE(db.Build(builder));
  ASSERT_TRUE(db.Save());
  ASSERT_TRUE(db.Load());
  struct {
    GramDb::Match results[GramDb::kMaxResults];
    int guard = 1234567;
  } output;
  ASSERT_EQ(GramDb::kMaxResults,
            db.Lookup("", string(20, 'a'), output.results));
  for (int i = 0; i < GramDb::kMaxResults; ++i)
    EXPECT_EQ(size_t(i + 1), output.results[i].length);
  EXPECT_EQ(1234567, output.guard);
}

TEST_F(GramDbTest, LongUnicodeKeysCrossEncodingChunksWithoutTruncation) {
  string text, encoded;
  for (int i = 0; i < 1024; ++i) {
    text += "你𠀀αA";
    encoded += grammar::encode("你𠀀αA");
  }
  ASSERT_EQ(encoded, grammar::encode(text));
  GramDb::Builder builder;
  ASSERT_TRUE(builder.Add(Fixed({encoded}, 500000)));
  ASSERT_TRUE(builder.Add(Weighted({{"ascii", 20}})));
  GramDb db(model);
  ASSERT_TRUE(db.Build(builder));
  ASSERT_TRUE(db.Save());
  ASSERT_TRUE(db.Load());
  GramDb::Match result[GramDb::kMaxResults];
  ASSERT_EQ(1, db.Lookup("", encoded, result));
  EXPECT_EQ(encoded.size(), result[0].length);
  EXPECT_EQ(Score(500000), result[0].value);
}

TEST_F(GramDbTest, RejectsInvalidWeightsKeysAndTruncatedSectionTable) {
  GramDb db(model);
  GramDb::Builder builder;
  EXPECT_FALSE(db.Build(builder));
  EXPECT_FALSE(builder.Add(Fixed({""}, 500000)));
  EXPECT_FALSE(builder.Add(Fixed({"ab"}, 0)));
  EXPECT_FALSE(
      builder.Add(Fixed({"ab"}, std::numeric_limits<double>::infinity())));
  EXPECT_FALSE(builder.Add(Weighted({{"ab", -1}})));
  EXPECT_FALSE(builder.Add(
      Weighted({{"ab", std::numeric_limits<double>::quiet_NaN()}})));
  ASSERT_TRUE(builder.Add(Fixed({"ab"}, 500000)));
  ASSERT_TRUE(builder.Add(Weighted({{"abc", 20}})));
  ASSERT_TRUE(db.Build(builder));
  ASSERT_TRUE(db.Save());
  db.Close();
  {
    std::fstream file(model, std::ios::binary | std::ios::in | std::ios::out);
    grammar::Metadata metadata;
    ASSERT_TRUE(
        file.read(reinterpret_cast<char*>(&metadata), sizeof(metadata)));
    metadata.section_count = UINT64_MAX;
    file.seekp(0);
    file.write(reinterpret_cast<const char*>(&metadata), sizeof(metadata));
  }
  EXPECT_FALSE(db.Load());
  EXPECT_FALSE(db.IsOpen());
  GramDb::Match result[GramDb::kMaxResults];
  EXPECT_EQ(0, db.Lookup("a", "bc", result));
}

TEST_F(GramDbTest, SelectsNamedRegionsBeforeMergingAndKeepsModelPriority) {
  GramDb::Builder builder;
  ASSERT_TRUE(builder.Add(Fixed({"ab"}, 5000, "常用搭配")));
  ASSERT_TRUE(builder.Add(Weighted({{"abc", 20}}, "精确")));
  ASSERT_TRUE(builder.Add(Fixed({"ab"}, 500000, "片段")));
  GramDb db(model);
  ASSERT_TRUE(db.Build(builder));
  ASSERT_TRUE(db.Save());
  ASSERT_TRUE(db.Load());
  GramDb::SectionList selected;
  GramDb::Match matches[GramDb::kMaxResults];
  ASSERT_TRUE(db.SelectSections({"常用搭配", "精确"}, &selected));
  ASSERT_EQ(2, db.Lookup("a", "bc", matches, selected));
  EXPECT_EQ(Score(5000), matches[0].value);
  EXPECT_EQ(Score(20), matches[1].value);
  ASSERT_TRUE(db.SelectSections({"片段", "常用搭配"}, &selected));
  ASSERT_EQ(1, db.Lookup("a", "bc", matches, selected));
  EXPECT_EQ(Score(500000), matches[0].value);
  ASSERT_TRUE(db.SelectSections({}, &selected));
  EXPECT_EQ(0, db.Lookup("a", "bc", matches, selected));
  EXPECT_FALSE(db.SelectSections({"不存在"}, &selected));
  EXPECT_TRUE(selected.empty());
}

TEST_F(GramDbTest, ANameSelectsAllItsWeightGroupsAndRejectsInvalidNameData) {
  GramDb::Builder builder;
  ASSERT_TRUE(builder.Add(Fixed({"ab"}, 5000, "shared")));
  ASSERT_TRUE(builder.Add(Fixed({"abc"}, 500000, "shared")));
  ASSERT_TRUE(builder.Add(Fixed({"abcd"}, 500000, "other")));
  EXPECT_FALSE(builder.Add(Fixed({"ab"}, 5, string("bad\0name", 8))));
  GramDb db(model);
  ASSERT_TRUE(db.Build(builder));
  ASSERT_TRUE(db.Save());
  ASSERT_TRUE(db.Load());
  GramDb::SectionList selected;
  ASSERT_TRUE(db.SelectSections({"shared", "shared"}, &selected));
  EXPECT_EQ(2u, selected.size());
  GramDb::Match result[GramDb::kMaxResults];
  ASSERT_EQ(2, db.Lookup("a", "bcd", result, selected));
  EXPECT_EQ(Score(5000), result[0].value);
  EXPECT_EQ(Score(500000), result[1].value);
  db.Close();
  std::fstream file(model, std::ios::binary | std::ios::in | std::ios::out);
  grammar::Metadata metadata;
  ASSERT_TRUE(file.read(reinterpret_cast<char*>(&metadata), sizeof(metadata)));
  file.seekp(sizeof(metadata) +
             metadata.section_count * sizeof(grammar::SectionMetadata));
  file.put('\0');
  file.close();
  EXPECT_FALSE(db.Load());
}

TEST_F(GramDbTest, RejectsModelsWithAnotherFormat) {
  GramDb db(model);
  GramDb::Builder builder;
  ASSERT_TRUE(builder.Add(Fixed({"ab"}, 5000)));
  ASSERT_TRUE(db.Build(builder));
  ASSERT_TRUE(db.Save());
  db.Close();
  {
    std::fstream file(model, std::ios::binary | std::ios::in | std::ios::out);
    constexpr char format[32] = "Rime::DartsGrammar/4";
    ASSERT_TRUE(file.write(format, sizeof(format)));
  }
  EXPECT_FALSE(db.Load());
  EXPECT_FALSE(db.IsOpen());
}

TEST_F(GramDbTest, LastGrammarViewReleasesTheModelMapping) {
  auto& deployer = Service::instance().deployer();
  deployer.user_data_dir = std::filesystem::current_path();
  deployer.shared_data_dir = deployer.user_data_dir;
  const string language = model.stem().to_utf8_string();
  Config config;
  config.SetString("grammar/language", language);
  auto names = New<ConfigList>();
  names->Append(New<ConfigValue>("default"));
  config.SetItem("grammar/contextual_sections", names);

  OctagramComponent component;
  for (int i = 0; i < 30; ++i) {
    GramDb::Builder builder;
    ASSERT_TRUE(builder.Add(Fixed({"ab"}, i == 0 ? 5000 : 500000)));
    GramDb writer(model);
    ASSERT_TRUE(writer.Build(builder));
    ASSERT_TRUE(writer.Save());
    writer.Close();
    the<Octagram> sentence(component.Create(&config));
    the<Octagram> contextual(component.CreateContextual(&config));
    ASSERT_TRUE(contextual);
    weak<const GramDb> opening = component.GetDb(language);
    ASSERT_FALSE(opening.expired());
    sentence.reset();
    EXPECT_FALSE(opening.expired());
    EXPECT_DOUBLE_EQ(
        contextual->Query("a", "b", false),
        double(Score(i == 0 ? 5000 : 500000)) / GramDb::kValueScale - 12);
    contextual.reset();
    ASSERT_TRUE(opening.expired());
    EXPECT_TRUE(std::filesystem::remove(model));
  }
}

TEST_F(GramDbTest, InvalidDartsTransitionsAreTreatedAsMisses) {
  GramDb db(model);
  GramDb::Builder builder;
  ASSERT_TRUE(builder.Add(Fixed({"ab"}, 5000)));
  ASSERT_TRUE(db.Build(builder));
  ASSERT_TRUE(db.Save());
  db.Close();
  {
    std::fstream file(model, std::ios::binary | std::ios::in | std::ios::out);
    grammar::Metadata metadata;
    ASSERT_TRUE(
        file.read(reinterpret_cast<char*>(&metadata), sizeof(metadata)));
    file.seekp(sizeof(metadata) + sizeof(grammar::SectionMetadata) +
               ((metadata.text_bytes + 3) & ~uint64_t(3)));
    const uint32_t invalid_root = 0x7ffffc00;
    ASSERT_TRUE(file.write(reinterpret_cast<const char*>(&invalid_root),
                           sizeof(invalid_root)));
  }
  ASSERT_TRUE(db.Load());
  GramDb::Match result[GramDb::kMaxResults];
  EXPECT_EQ(0, db.Lookup("a", "b", result));
  EXPECT_EQ(0, db.Lookup("", "ab", result));
  db.Close();
  std::ofstream(model, std::ios::binary | std::ios::trunc).close();
  EXPECT_FALSE(db.Load());
  EXPECT_FALSE(db.IsOpen());
}

TEST_F(GramDbTest, DistinguishesSentenceAndContextWithoutACharacterLengthRule) {
  GramDb::Builder builder;
  ASSERT_TRUE(
      builder.Add(Fixed({grammar::encode("有道理")}, 500000, "collocations")));
  ASSERT_TRUE(
      builder.Add(Weighted({{grammar::encode("秦始皇"), 5000000}}, "names")));
  ASSERT_TRUE(builder.Add(Weighted({{grammar::encode("中考后"), 5000000000000},
                                    {grammar::encode("有手癣"), 5000000000000},
                                    {grammar::encode("考后$"), 5000000000000},
                                    {grammar::encode("秦始皇"), 5000000000000}},
                                   "fragments")));
  GramDb db(model);
  ASSERT_TRUE(db.Build(builder));
  ASSERT_TRUE(db.Save());
  db.Close();
  auto& deployer = Service::instance().deployer();
  deployer.user_data_dir = std::filesystem::current_path();
  deployer.shared_data_dir = deployer.user_data_dir;
  Config config;
  config.SetString("grammar/language", "octagram-sections-test");
  config.SetInt("grammar/collocation_max_length", 6);
  config.SetInt("grammar/collocation_min_length", 2);
  config.SetString("grammar/collocation_penalty", "-14");
  config.SetString("grammar/non_collocation_penalty", "-6");
  config.SetString("grammar/weak_collocation_penalty", "-100");
  config.SetString("grammar/rear_penalty", "-18");
  auto names = New<ConfigList>();
  names->Append(New<ConfigValue>("collocations"));
  names->Append(New<ConfigValue>("names"));
  config.SetItem("grammar/contextual_sections", names);
  OctagramComponent component;
  std::unique_ptr<Octagram> sentence(component.Create(&config));
  std::unique_ptr<Octagram> contextual(component.CreateContextual(&config));
  ASSERT_TRUE(contextual);
  EXPECT_GT(sentence->Query("中", "考后", true), -6);
  EXPECT_DOUBLE_EQ(-6, contextual->Query("中", "考后", true));
  EXPECT_GT(sentence->Query("有", "手癣", true), -6);
  EXPECT_DOUBLE_EQ(-6, contextual->Query("有", "手癣", true));
  EXPECT_DOUBLE_EQ(double(Score(5000000)) / GramDb::kValueScale - 14,
                   contextual->Query("秦", "始皇", true));
  EXPECT_GT(contextual->Query("有", "道理", true), -6);
  config.SetItem("grammar/contextual_sections", New<ConfigList>());
  EXPECT_EQ(nullptr, component.CreateContextual(&config));
  auto missing = New<ConfigList>();
  missing->Append(New<ConfigValue>("unknown"));
  config.SetItem("grammar/contextual_sections", missing);
  EXPECT_EQ(nullptr, component.CreateContextual(&config));
  config.SetString("grammar/contextual_sections", "collocations");
  EXPECT_EQ(nullptr, component.CreateContextual(&config));
}

int main(int argc, char** argv) {
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
