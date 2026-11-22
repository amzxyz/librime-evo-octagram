#ifndef RIME_GRAM_DB_H_
#define RIME_GRAM_DB_H_

#include <darts.h>
#include <optional>
#include <string_view>
#include <rime/resource.h>
#include <rime/dict/mapped_file.h>

namespace rime {

namespace grammar {

inline constexpr char kModelSuffix[] = ".dlm";

struct Metadata {
  char format[32];
  uint64_t section_count;
  uint64_t text_bytes;
};

struct SectionMetadata {
  uint64_t array_bytes;
  int64_t constant_value;
  uint64_t name_bytes;
  uint64_t description_bytes;
  double constant_weight;
};

}  // namespace grammar

class GramDb : public MappedFile {
 private:
  struct Section {
    Darts::DoubleArray trie;
    int constant_value = -1;
    double constant_weight = 0;
    string name;
    string description;
  };

 public:
  using Match = Darts::DoubleArray::result_pair_type;
  static constexpr int kMaxResults = 8;
  static constexpr double kValueScale = 10000;
  using SectionList = vector<size_t>;

  struct Source {
    vector<string> constant_keys;
    vector<pair<string, double>> weighted_entries;
    std::optional<double> constant_weight;
    string name = "default";
    string description = {};
  };

  class Builder {
   public:
    // Compile a group so its input buffers can be released before the next.
    bool Add(Source source);

   private:
    friend class GramDb;
    vector<the<Section>> sections_;
  };

  explicit GramDb(const path& file_path) : MappedFile(file_path) {}
  ~GramDb() override { Close(); }

  bool Load();
  bool Save();
  // Consume compiled sections and replace their buffers with the mapped image.
  bool Build(Builder& builder);
  void Close();
  int Lookup(std::string_view context,
             std::string_view word,
             Match results[kMaxResults]) const;
  int Lookup(std::string_view context,
             std::string_view word,
             Match results[kMaxResults],
             const SectionList& sections) const;
  bool SelectSections(const vector<string>& names, SectionList* sections) const;
  const SectionList& all_sections() const { return all_sections_; }

 private:
  void IndexSections();
  void LogSections() const;
  vector<the<Section>> sections_;
  SectionList all_sections_;
  map<string, SectionList> sections_by_name_;
};

}  // namespace rime

#endif  // RIME_GRAM_DB_H_
