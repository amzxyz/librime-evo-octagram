#include "gram_db.h"
#include "gram_encoding.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <ostream>
#include <streambuf>
#include <marisa/iostream.h>

namespace rime {
namespace {

constexpr char kGrammarFormat[] = "Rime::Grammar/MARISA/1";
constexpr int kNumTries = 3;
constexpr int kTrieConfig = kNumTries | MARISA_NORMAL_CACHE;
// A final zero window permits the same unaligned read for every key ID.
constexpr size_t kValuePadding = sizeof(uint64_t) - 1;

uint32_t ReadValue(const char* values,
                   size_t id,
                   uint32_t bits,
                   uint32_t mask) {
  const size_t offset = id * bits;
  uint64_t window;
  std::memcpy(&window, values + offset / 8, sizeof(window));
  return uint32_t(window >> (offset % 8)) & mask;
}

void WriteValue(char* values,
                size_t id,
                uint32_t bits,
                uint32_t mask,
                uint32_t value) {
  const size_t offset = id * bits;
  uint64_t window;
  std::memcpy(&window, values + offset / 8, sizeof(window));
  window = (window & ~(uint64_t(mask) << (offset % 8))) |
           (uint64_t(value) << (offset % 8));
  std::memcpy(values + offset / 8, &window, sizeof(window));
}

// Write MARISA's image directly into the mapped file, without a temporary file
// or a second full-size serialization buffer.
class ImageBuffer : public std::streambuf {
 public:
  ImageBuffer(char* data, size_t size) { setp(data, data + size); }
  size_t written() const { return pptr() - pbase(); }
};

}  // namespace

void GramDb::Close() {
  trie_.clear();
  values_ = nullptr;
  value_table_ = nullptr;
  value_bits_ = 0;
  value_mask_ = 0;
  MappedFile::Close();
}

bool GramDb::Load() {
  Close();
  if (!OpenReadOnly())
    return false;
  auto fail = [this]() {
    LOG(ERROR) << "invalid MARISA grammar database: " << file_path();
    Close();
    return false;
  };
  if (file_size() < sizeof(grammar::Metadata))
    return fail();
  const auto* metadata = Find<grammar::Metadata>(0);
  if (std::memcmp(metadata->format, kGrammarFormat, sizeof(kGrammarFormat)))
    return fail();
  const size_t payload = file_size() - sizeof(*metadata);
  if (metadata->trie_size > payload || !metadata->key_count ||
      !metadata->value_bits || metadata->value_bits > 31 ||
      metadata->trie_size % alignof(int32_t) ||
      (metadata->value_count &&
       metadata->value_count != (uint64_t(1) << metadata->value_bits)) ||
      metadata->value_count > SIZE_MAX / sizeof(int32_t) ||
      metadata->key_count > (SIZE_MAX - 7) / metadata->value_bits)
    return fail();
  const size_t value_size =
      (metadata->key_count * metadata->value_bits + 7) / 8;
  const size_t table_size = metadata->value_count * sizeof(int32_t);
  const size_t remaining = payload - metadata->trie_size;
  if (remaining < table_size || remaining - table_size < kValuePadding ||
      remaining - table_size - kValuePadding != value_size)
    return fail();
  try {
    const char* image = Find<char>(sizeof(*metadata));
    trie_.map(image, metadata->trie_size);
    if (trie_.num_keys() != metadata->key_count)
      return fail();
    value_table_ =
        metadata->value_count
            ? reinterpret_cast<const int32_t*>(image + metadata->trie_size)
            : nullptr;
    values_ = image + metadata->trie_size + table_size;
    value_bits_ = uint32_t(metadata->value_bits);
    value_mask_ = (uint32_t(1) << value_bits_) - 1;
  } catch (const std::exception& e) {
    LOG(ERROR) << "cannot map grammar database: " << e.what();
    return fail();
  }
  return true;
}

bool GramDb::Save() {
  // Build allocates the exact serialized size; shrinking would invalidate the
  // mapped trie and its value array.
  return IsOpen() && values_ && Flush();
}

bool GramDb::Builder::Add(const string& key, double weight) {
  if (key.empty() || !std::isfinite(weight) || weight <= 0) {
    LOG(ERROR)
        << "grammar entries require nonempty keys and positive finite weights";
    return false;
  }
  keys_.push_back(
      key.data(), key.size(),
      static_cast<float>(
          (std::min)(weight, double(std::numeric_limits<float>::max()))));
  values_.push_back(
      uint32_t((std::max)(0, int32_t(std::log(weight) * kValueScale))));
  return true;
}

bool GramDb::Build(const vector<pair<string, double>>& data) {
  Close();
  try {
    Builder builder;
    for (const auto& [key, weight] : data)
      if (!builder.Add(key, weight))
        return false;
    return Build(builder);
  } catch (const std::exception& e) {
    LOG(ERROR) << "cannot build grammar database: " << e.what();
    return false;
  }
}

bool GramDb::Build(Builder& builder) {
  Close();
  if (builder.keys_.empty() || builder.keys_.size() != builder.values_.size())
    return false;
  try {
    marisa::Keyset keys;
    keys.swap(builder.keys_);
    vector<uint32_t> input_values;
    input_values.swap(builder.values_);
    marisa::Trie built;
    built.build(keys, kTrieConfig);
    const size_t trie_size = built.io_size();
    // MARISA assigns IDs back to the input keyset. Duplicate entries share an
    // ID; retain their maximum weight without a second round of trie lookups.
    vector<uint32_t> scores(built.num_keys());
    uint32_t maximum_value = 0;
    for (size_t i = 0; i < input_values.size(); ++i) {
      const uint32_t score = input_values[i];
      scores[keys[i].id()] = (std::max)(scores[keys[i].id()], score);
      maximum_value = (std::max)(maximum_value, score);
    }
    // Key IDs and scores are now associated; release the input buffers before
    // allocating the output image and its shared score table.
    keys.clear();
    vector<uint32_t>().swap(input_values);
    uint32_t bits = 1;
    while (maximum_value >> bits)
      ++bits;
    if (built.num_keys() > (SIZE_MAX - 7) / bits)
      return false;
    const size_t direct_size = (built.num_keys() * bits + 7) / 8;
    // Repeated scores can be represented by a small shared table. Compare
    // complete encoded sizes, including its padding, before choosing it.
    auto distinct = scores;
    std::sort(distinct.begin(), distinct.end());
    distinct.erase(std::unique(distinct.begin(), distinct.end()),
                   distinct.end());
    uint32_t table_bits = 1;
    while ((distinct.size() - 1) >> table_bits)
      ++table_bits;
    size_t value_count = 0;
    if (table_bits < bits && table_bits < sizeof(size_t) * 8 - 2) {
      const size_t count = size_t(1) << table_bits;
      const size_t index_size = (built.num_keys() * table_bits + 7) / 8;
      if (index_size < direct_size &&
          count * sizeof(int32_t) < direct_size - index_size) {
        value_count = count;
        bits = table_bits;
      }
    }
    const uint32_t mask = (uint32_t(1) << bits) - 1;
    const size_t value_size = (built.num_keys() * bits + 7) / 8;
    const size_t table_size = value_count * sizeof(int32_t);
    if (trie_size > SIZE_MAX - sizeof(grammar::Metadata) - kValuePadding ||
        table_size >
            SIZE_MAX - sizeof(grammar::Metadata) - kValuePadding - trie_size ||
        value_size > SIZE_MAX - sizeof(grammar::Metadata) - kValuePadding -
                         trie_size - table_size)
      return false;
    const size_t size = sizeof(grammar::Metadata) + trie_size + table_size +
                        value_size + kValuePadding;
    if (!Create(size))
      return false;
    auto* metadata = Allocate<grammar::Metadata>();
    auto* image = Allocate<char>(trie_size);
    auto* value_table = value_count ? Allocate<int32_t>(value_count) : nullptr;
    auto* values = Allocate<char>(value_size + kValuePadding);
    if (!metadata || !image || !values || (value_count && !value_table)) {
      Close();
      return false;
    }
    if (value_table)
      std::copy(distinct.begin(), distinct.end(), value_table);
    ImageBuffer buffer(image, trie_size);
    std::ostream stream(&buffer);
    marisa::write(stream, built);
    if (!stream || buffer.written() != trie_size) {
      Close();
      return false;
    }
    for (size_t i = 0; i < scores.size(); ++i) {
      const uint32_t value =
          value_table ? uint32_t(std::lower_bound(distinct.begin(),
                                                  distinct.end(), scores[i]) -
                                 distinct.begin())
                      : scores[i];
      WriteValue(values, i, bits, mask, value);
    }
    metadata->trie_size = trie_size;
    metadata->key_count = built.num_keys();
    metadata->value_bits = bits;
    metadata->value_count = value_count;
    std::memcpy(metadata->format, kGrammarFormat, sizeof(kGrammarFormat));
    trie_.map(image, trie_size);
    values_ = values;
    value_table_ = value_table;
    value_bits_ = bits;
    value_mask_ = mask;
  } catch (const std::exception& e) {
    LOG(ERROR) << "cannot build grammar database: " << e.what();
    Close();
    return false;
  }
  return true;
}

int GramDb::Lookup(const string& context,
                   const string& word,
                   Match results[kMaxResults]) const {
  if (!IsOpen() || !values_ || word.empty() || !results)
    return 0;
  char buffer[grammar::kMaxQueryLength * 8];
  string overflow;
  const char* query;
  size_t length;
  if (context.size() <= sizeof(buffer) &&
      word.size() <= sizeof(buffer) - context.size()) {
    std::memcpy(buffer, context.data(), context.size());
    std::memcpy(buffer + context.size(), word.data(), word.size());
    query = buffer;
    length = context.size() + word.size();
  } else {
    overflow = context;
    overflow += word;
    query = overflow.data();
    length = overflow.size();
  }
  // The trie is shared and immutable. Reuse only per-thread search state;
  // set_query resets it even when consecutive calls use different databases.
  thread_local marisa::Agent agent;
  agent.set_query(query, length);
  int count = 0;
  while (count < kMaxResults && trie_.common_prefix_search(agent)) {
    if (agent.key().length() <= context.size())
      continue;
    const uint32_t value =
        ReadValue(values_, agent.key().id(), value_bits_, value_mask_);
    results[count++] = {value_table_ ? value_table_[value] : int(value),
                        agent.key().length() - context.size()};
  }
  return count;
}

}  // namespace rime
