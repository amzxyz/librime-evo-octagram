#include <gtest/gtest.h>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <thread>
#include "gram_db.h"
#include "gram_encoding.h"

using namespace rime;

class GramDbTest : public testing::Test {
 protected:
  const path model{"octagram-test.gramx"};
  const path other{"octagram-other-test.gramx"};
  void TearDown() override {
    std::filesystem::remove(model);
    std::filesystem::remove(other);
  }
};

TEST_F(GramDbTest, RoundTripPreservesPrefixesAndMaximumDuplicateWeight) {
  GramDb db(model);
  ASSERT_TRUE(db.Build({{"abc", 20}, {"a", 5}, {"ab", 10}, {"abc", 50}}));
  ASSERT_TRUE(db.Save());
  GramDb::Match result[GramDb::kMaxResults];
  for (bool reload : {false, true}) {
    if (reload)
      ASSERT_TRUE(db.Load());
    ASSERT_EQ(2, db.Lookup("a", "bcdef", result));
    EXPECT_EQ(1u, result[0].length);
    EXPECT_EQ(int(std::log(10) * GramDb::kValueScale), result[0].value);
    EXPECT_EQ(2u, result[1].length);
    EXPECT_EQ(int(std::log(50) * GramDb::kValueScale), result[1].value);
    EXPECT_EQ(0, db.Lookup("ax", "bc", result));
    EXPECT_EQ(0, db.Lookup("abc", "d", result));
    EXPECT_EQ(0, db.Lookup("a", "", result));
  }
}

TEST_F(GramDbTest, EncodedUnicodeAndBinaryKeysUseByteLengths) {
  GramDb db(model);
  const auto context = grammar::encode("你好");
  const auto word = grammar::encode("世界");
  ASSERT_TRUE(db.Build({{context + word, 9}, {string("a\0b", 3), 0.5}}));
  ASSERT_TRUE(db.Save());
  ASSERT_TRUE(db.Load());
  GramDb::Match result[GramDb::kMaxResults];
  ASSERT_EQ(1, db.Lookup(context, word, result));
  EXPECT_EQ(word.size(), result[0].length);
  EXPECT_EQ(int(std::log(9) * GramDb::kValueScale), result[0].value);
  ASSERT_EQ(1, db.Lookup(string("a\0", 2), "b", result));
  EXPECT_EQ(1u, result[0].length);
  EXPECT_EQ(0, result[0].value);
}

TEST_F(GramDbTest, LimitsResultsAndHandlesQueriesBeyondTheStackBuffer) {
  vector<pair<string, double>> data;
  for (size_t i = 1; i <= 12; ++i)
    data.emplace_back(string(i, 'a'), 2);
  data.emplace_back(string(100, 'x'), 3);
  GramDb db(model);
  ASSERT_TRUE(db.Build(data));
  ASSERT_TRUE(db.Save());
  GramDb::Match result[GramDb::kMaxResults];
  ASSERT_EQ(GramDb::kMaxResults, db.Lookup("", string(12, 'a'), result));
  EXPECT_EQ(8u, result[7].length);
  ASSERT_EQ(1, db.Lookup(string(50, 'x'), string(50, 'x'), result));
  EXPECT_EQ(50u, result[0].length);
}

TEST_F(GramDbTest, ExtendedUnicodeKeysRemainDistinct) {
  const vector<string> contexts{"😀", "😁", "𠀀", "𠀁", "α", "β"};
  vector<pair<string, double>> data;
  for (size_t i = 0; i < contexts.size(); ++i)
    data.emplace_back(grammar::encode(contexts[i] + "字"), double(i + 2));
  for (size_t i = 0; i < data.size(); ++i)
    for (size_t j = i + 1; j < data.size(); ++j)
      EXPECT_NE(data[i].first, data[j].first);
  GramDb db(model);
  ASSERT_TRUE(db.Build(data));
  ASSERT_TRUE(db.Save());
  ASSERT_TRUE(db.Load());
  GramDb::Match result[GramDb::kMaxResults];
  for (size_t i = 0; i < contexts.size(); ++i) {
    ASSERT_EQ(1, db.Lookup(grammar::encode(contexts[i]), grammar::encode("字"),
                           result));
    EXPECT_EQ(int(std::log(double(i + 2)) * GramDb::kValueScale),
              result[0].value);
  }
}

TEST_F(GramDbTest, LongKeysCrossEncodingChunksWithoutTruncation) {
  const string phrase = "瓶身描绘的牡丹一如你初妆";
  string text, encoded;
  for (int i = 0; i < 1024; ++i) {
    text += "你𠀀αA";
    encoded += grammar::encode("你𠀀αA");
  }
  EXPECT_EQ(encoded, grammar::encode(text));
  const string ascii(4096, 'x');
  EXPECT_EQ(ascii, grammar::encode(ascii));
  GramDb db(model);
  GramDb::Builder builder;
  ASSERT_TRUE(builder.Add(grammar::encode(phrase), 50));
  ASSERT_TRUE(builder.Add(encoded, 60));
  ASSERT_TRUE(builder.Add(ascii, 70));
  ASSERT_TRUE(db.Build(builder));
  ASSERT_TRUE(db.Save());
  ASSERT_TRUE(db.Load());
  GramDb::Match result[GramDb::kMaxResults];
  ASSERT_EQ(1, db.Lookup(grammar::encode("瓶身描绘的牡丹"),
                         grammar::encode("一如你初妆"), result));
  EXPECT_EQ(int(std::log(50) * GramDb::kValueScale), result[0].value);
  ASSERT_EQ(1, db.Lookup("", encoded, result));
  EXPECT_EQ(encoded.size(), result[0].length);
  ASSERT_EQ(1, db.Lookup("", ascii, result));
  EXPECT_EQ(ascii.size(), result[0].length);
}

TEST_F(GramDbTest, IncrementalBuilderCanBeConsumedAndReused) {
  GramDb db(model);
  GramDb::Builder builder;
  EXPECT_FALSE(builder.Add("", 10));
  ASSERT_TRUE(builder.Add("ab", 9));
  ASSERT_TRUE(builder.Add("abc", 12));
  ASSERT_TRUE(builder.Add("ab", 20));
  ASSERT_TRUE(db.Build(builder));
  ASSERT_TRUE(db.Save());
  GramDb::Match result[GramDb::kMaxResults];
  ASSERT_EQ(2, db.Lookup("a", "bc", result));
  EXPECT_EQ(int(std::log(20) * GramDb::kValueScale), result[0].value);
  EXPECT_EQ(int(std::log(12) * GramDb::kValueScale), result[1].value);
  EXPECT_FALSE(db.Build(builder));
  ASSERT_TRUE(builder.Add("new", 30));
  ASSERT_TRUE(db.Build(builder));
  ASSERT_TRUE(db.Save());
  ASSERT_TRUE(db.Load());
  ASSERT_EQ(1, db.Lookup("n", "ew", result));
  EXPECT_EQ(int(std::log(30) * GramDb::kValueScale), result[0].value);
}

TEST_F(GramDbTest, RejectsInvalidWeightsAndEmptyData) {
  GramDb db(model);
  EXPECT_FALSE(db.Build({}));
  for (double weight : {0.0, -1.0, std::numeric_limits<double>::infinity(),
                        std::numeric_limits<double>::quiet_NaN()})
    EXPECT_FALSE(db.Build({{"a", weight}}));
  EXPECT_FALSE(db.Build({{"", 1}}));
}

TEST_F(GramDbTest, PackedWeightsPreserveEveryValueAcrossByteBoundaries) {
  GramDb db(model);
  GramDb::Match result[GramDb::kMaxResults];
  for (unsigned bits = 1; bits <= 23; ++bits) {
    const unsigned maximum = (std::min)((1u << bits) - 1, 7000000u);
    vector<pair<string, double>> data;
    for (unsigned i = 0; i < 257; ++i) {
      const auto key = "key/" + std::to_string((i * 131) % 257) + "/";
      data.emplace_back(
          key, std::exp(double(maximum) * i / 256 / GramDb::kValueScale));
    }
    // Values below one are clamped to zero; the largest finite double must
    // still fit without losing any of the original logarithmic precision.
    if (bits == 23) {
      data.emplace_back("tiny/", std::numeric_limits<double>::min());
      data.emplace_back("large/", std::numeric_limits<double>::max());
    }
    ASSERT_TRUE(db.Build(data));
    ASSERT_TRUE(db.Save());
    ASSERT_TRUE(db.Load());
    for (const auto& [key, weight] : data) {
      ASSERT_EQ(1, db.Lookup("", key, result));
      EXPECT_EQ((std::max)(0, int(std::log(weight) * GramDb::kValueScale)),
                result[0].value);
    }
    std::ifstream file(model, std::ios::binary);
    grammar::Metadata header;
    file.read(reinterpret_cast<char*>(&header), sizeof(header));
    EXPECT_GE(header.value_bits, 1u);
    EXPECT_LE(header.value_bits, 23u);
    EXPECT_EQ(0u, header.value_count);
    EXPECT_EQ(sizeof(header) + header.trie_size +
                  header.value_count * sizeof(int32_t) +
                  (header.key_count * header.value_bits + 7) / 8 + 7,
              std::filesystem::file_size(model));
  }
  ASSERT_TRUE(db.Build({{"zero", 0.5}, {"one", 1}}));
  ASSERT_TRUE(db.Save());
  ASSERT_TRUE(db.Load());
  ASSERT_EQ(1, db.Lookup("", "zero", result));
  EXPECT_EQ(0, result[0].value);
}

TEST_F(GramDbTest, RejectsInvalidPackedWeightWidthsAndMissingPadding) {
  GramDb db(model);
  for (uint64_t bits : {0u, 32u, 64u}) {
    ASSERT_TRUE(db.Build({{"abc", 3}}));
    ASSERT_TRUE(db.Save());
    db.Close();
    std::fstream file(model, std::ios::in | std::ios::out | std::ios::binary);
    grammar::Metadata header;
    file.read(reinterpret_cast<char*>(&header), sizeof(header));
    header.value_bits = bits;
    file.seekp(0);
    file.write(reinterpret_cast<const char*>(&header), sizeof(header));
    file.close();
    EXPECT_FALSE(db.Load());
  }
  ASSERT_TRUE(db.Build({{"abc", 3}}));
  ASSERT_TRUE(db.Save());
  db.Close();
  std::filesystem::resize_file(model, std::filesystem::file_size(model) - 1);
  EXPECT_FALSE(db.Load());
}

TEST_F(GramDbTest, SharedScoresRoundTripAndDuplicateKeysKeepTheMaximum) {
  vector<pair<string, double>> data;
  for (unsigned i = 0; i < 1000; ++i)
    data.emplace_back("key/" + std::to_string(i) + "/", double(100 + i % 78));
  data.emplace_back(data[0].first, 177);
  GramDb db(model);
  ASSERT_TRUE(db.Build(data));
  ASSERT_TRUE(db.Save());
  ASSERT_TRUE(db.Load());
  GramDb::Match result[GramDb::kMaxResults];
  for (unsigned i = 0; i < 1000; ++i) {
    ASSERT_EQ(1, db.Lookup("", data[i].first, result));
    const double weight = i ? data[i].second : 177;
    EXPECT_EQ(int(std::log(weight) * GramDb::kValueScale), result[0].value);
  }
  db.Close();
  std::fstream file(model, std::ios::in | std::ios::out | std::ios::binary);
  grammar::Metadata header;
  file.read(reinterpret_cast<char*>(&header), sizeof(header));
  EXPECT_EQ(7u, header.value_bits);
  EXPECT_EQ(128u, header.value_count);
  // Each possible code has a table slot, so even a corrupt code cannot index
  // past the mapped score table. Invalid dimensions are rejected on load.
  header.value_count = 127;
  file.seekp(0);
  file.write(reinterpret_cast<const char*>(&header), sizeof(header));
  file.close();
  EXPECT_FALSE(db.Load());
}

TEST_F(GramDbTest, RejectsOldTruncatedAndInconsistentImages) {
  GramDb db(model);
  {
    std::ofstream out(model, std::ios::binary);
    grammar::Metadata header{};
    std::strcpy(header.format, "Rime::Grammar/1.0");
    out.write(reinterpret_cast<const char*>(&header), sizeof(header));
  }
  EXPECT_FALSE(db.Load());
  EXPECT_FALSE(db.IsOpen());
  ASSERT_TRUE(db.Build({{"abc", 3}}));
  ASSERT_TRUE(db.Save());
  db.Close();
  {
    std::fstream file(model, std::ios::in | std::ios::out | std::ios::binary);
    grammar::Metadata header;
    file.read(reinterpret_cast<char*>(&header), sizeof(header));
    ++header.key_count;
    file.seekp(0);
    file.write(reinterpret_cast<const char*>(&header), sizeof(header));
  }
  EXPECT_FALSE(db.Load());
  EXPECT_FALSE(db.IsOpen());
  ASSERT_TRUE(db.Build({{"abc", 3}}));
  ASSERT_TRUE(db.Save());
  db.Close();
  std::filesystem::resize_file(model, sizeof(grammar::Metadata) + 8);
  EXPECT_FALSE(db.Load());
  GramDb::Match result[GramDb::kMaxResults];
  EXPECT_EQ(0, db.Lookup("a", "bc", result));
}

TEST_F(GramDbTest, ReusesSearchStateAcrossModelsAndThreads) {
  GramDb first(model), second(other);
  ASSERT_TRUE(first.Build({{"abc", 3}}));
  ASSERT_TRUE(first.Save());
  ASSERT_TRUE(second.Build({{"abd", 4}}));
  ASSERT_TRUE(second.Save());
  auto run = [&]() {
    GramDb::Match result[GramDb::kMaxResults];
    for (int i = 0; i < 1000; ++i) {
      ASSERT_EQ(1, first.Lookup("a", "bc", result));
      EXPECT_EQ(int(std::log(3) * GramDb::kValueScale), result[0].value);
      EXPECT_EQ(0, second.Lookup("a", "bc", result));
      ASSERT_EQ(1, second.Lookup("a", "bd", result));
      EXPECT_EQ(int(std::log(4) * GramDb::kValueScale), result[0].value);
    }
  };
  std::thread one(run), two(run);
  one.join();
  two.join();
  first.Close();
  EXPECT_FALSE(first.IsOpen());
  ASSERT_TRUE(first.Build({{"abc", 5}}));
  ASSERT_TRUE(first.Save());
}

int main(int argc, char** argv) {
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
