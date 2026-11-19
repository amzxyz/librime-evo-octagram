#ifndef RIME_GRAM_DB_H_
#define RIME_GRAM_DB_H_

#include <marisa.h>
#include <rime/dict/mapped_file.h>

namespace rime {

namespace grammar {

struct Metadata {
  char format[32];
  uint64_t trie_size;
  uint64_t key_count;
  uint64_t value_bits;
  uint64_t value_count;
};

}  // namespace grammar

class GramDb : public MappedFile {
 public:
  struct Match {
    int value;
    size_t length;
  };
  static constexpr int kMaxResults = 8;
  static constexpr double kValueScale = 10000;

  class Builder {
   public:
    bool Add(const string& key, double weight);

   private:
    friend class GramDb;
    marisa::Keyset keys_;
    vector<uint32_t> values_;
  };

  explicit GramDb(const path& file_path) : MappedFile(file_path) {}
  ~GramDb() override { Close(); }

  bool Load();
  bool Save();
  bool Build(const vector<pair<string, double>>& data);
  // Consume the entries so input buffers can be freed before serialization.
  bool Build(Builder& builder);
  void Close();
  int Lookup(const string& context,
             const string& word,
             Match results[kMaxResults]) const;

 private:
  marisa::Trie trie_;
  const char* values_ = nullptr;
  const int32_t* value_table_ = nullptr;
  uint32_t value_bits_ = 0;
  uint32_t value_mask_ = 0;
};

}  // namespace rime

#endif  // RIME_GRAM_DB_H_
