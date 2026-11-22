#ifndef RIME_GRAM_DB_H_
#define RIME_GRAM_DB_H_

#include <darts.h>
#include <optional>
#include <rime/resource.h>
#include <rime/dict/mapped_file.h>

namespace rime {

namespace grammar {

struct Metadata {
  char format[32];
  uint64_t section_count;
  uint64_t names_bytes;
};

struct SectionMetadata {
  uint64_t array_bytes;
  int64_t constant_value;
  uint64_t name_bytes;
};

}  // namespace grammar

class GramDb : public MappedFile {
 private:
  struct Section {
    Darts::DoubleArray trie;
    int constant_value = -1;
    string name;
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
  int Lookup(const string& context,
             const string& word,
             Match results[kMaxResults]) const;
  int Lookup(const string& context,
             const string& word,
             Match results[kMaxResults],
             const SectionList& sections) const;
  bool SelectSections(const vector<string>& names, SectionList* sections) const;
  const SectionList& all_sections() const { return all_sections_; }

 private:
  void IndexSections();
  vector<the<Section>> sections_;
  SectionList all_sections_;
  map<string, SectionList> sections_by_name_;
};

}  // namespace rime

#endif  // RIME_GRAM_DB_H_
