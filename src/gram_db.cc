#include "gram_db.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <utf8.h>

namespace rime {
namespace {

// Older readers must reject a layout with a variable section table.
constexpr char kGrammarFormat[] = "Rime::DartsGrammar/3";
static_assert(sizeof(grammar::Metadata) == 48);
static_assert(sizeof(grammar::SectionMetadata) == 24);

bool ValidWeight(double weight) {
  return std::isfinite(weight) && weight > 0;
}

bool ValidKey(const string& key) {
  return !key.empty() && key.find('\0') == string::npos;
}

int Score(double weight) {
  return (std::max)(0, int(std::log(weight) * GramDb::kValueScale));
}

int LookupTrie(const Darts::DoubleArray& trie,
               const string& context,
               const string& word,
               GramDb::Match* results) {
  size_t node = 0, consumed = 0;
  if (trie.traverse(context.c_str(), node, consumed) == -2)
    return 0;
  const size_t count = trie.commonPrefixSearch(word.c_str(), results,
                                               GramDb::kMaxResults, 0, node);
  return int((std::min)(count, size_t(GramDb::kMaxResults)));
}

}  // namespace

bool GramDb::Builder::Add(Source source) {
  if (source.name.empty() || source.name.find('\0') != string::npos ||
      !utf8::is_valid(source.name.begin(), source.name.end()))
    return false;
  const bool constant = source.constant_weight.has_value();
  if (constant) {
    if (!ValidWeight(*source.constant_weight) ||
        !source.weighted_entries.empty())
      return false;
    for (const auto& key : source.constant_keys)
      if (!ValidKey(key))
        return false;
  } else {
    if (!source.constant_keys.empty())
      return false;
    for (const auto& [key, weight] : source.weighted_entries)
      if (!ValidKey(key) || !ValidWeight(weight))
        return false;
  }
  try {
    auto section = std::make_unique<Section>();
    section->name = std::move(source.name);
    vector<const char*> keys;
    vector<int> values;
    if (constant) {
      auto& data = source.constant_keys;
      std::sort(data.begin(), data.end());
      data.erase(std::unique(data.begin(), data.end()), data.end());
      if (data.empty())
        return true;
      keys.reserve(data.size());
      for (const auto& key : data)
        keys.push_back(key.c_str());
      // Zero terminals allow suffix sharing; the common score is stored once.
      values.resize(keys.size(), 0);
      section->constant_value = Score(*source.constant_weight);
    } else {
      auto& data = source.weighted_entries;
      std::sort(data.begin(), data.end());
      size_t count = 0;
      for (size_t i = 0; i < data.size(); ++i) {
        if (count && data[count - 1].first == data[i].first) {
          data[count - 1].second = data[i].second;
        } else {
          if (count != i)
            data[count] = std::move(data[i]);
          ++count;
        }
      }
      data.resize(count);
      if (data.empty())
        return true;
      keys.reserve(data.size());
      values.reserve(data.size());
      for (const auto& [key, weight] : data) {
        keys.push_back(key.c_str());
        values.push_back(Score(weight));
      }
    }
    section->trie.build(keys.size(), keys.data(), nullptr, values.data());
    sections_.push_back(std::move(section));
    return true;
  } catch (const std::exception& e) {
    LOG(ERROR) << "cannot compile Darts grammar section: " << e.what();
    return false;
  }
}

void GramDb::Close() {
  sections_by_name_.clear();
  all_sections_.clear();
  sections_.clear();
  MappedFile::Close();
}

bool GramDb::Load() {
  Close();
  if (!OpenReadOnly())
    return false;
  auto invalid = [this]() {
    LOG(ERROR) << "invalid multi-section Darts grammar: " << file_path();
    Close();
    return false;
  };
  if (file_size() < sizeof(grammar::Metadata))
    return invalid();
  const auto* metadata = Find<grammar::Metadata>(0);
  const size_t payload = file_size() - sizeof(*metadata);
  if (std::memcmp(metadata->format, kGrammarFormat, sizeof(kGrammarFormat)) ||
      !metadata->section_count ||
      metadata->section_count > payload / sizeof(grammar::SectionMetadata))
    return invalid();
  const size_t count = size_t(metadata->section_count);
  const auto* table = Find<grammar::SectionMetadata>(sizeof(*metadata));
  const size_t table_end = sizeof(*metadata) + count * sizeof(*table);
  if (metadata->names_bytes > SIZE_MAX - 3 ||
      metadata->names_bytes > file_size() - table_end)
    return invalid();
  const size_t padded_names = (size_t(metadata->names_bytes) + 3) & ~size_t(3);
  if (padded_names > file_size() - table_end)
    return invalid();
  size_t remaining = file_size() - table_end - padded_names;
  size_t names_remaining = size_t(metadata->names_bytes);
  const char* names = Find<char>(table_end);
  const Darts::DoubleArray empty;
  for (size_t i = 0; i < count; ++i) {
    if (!table[i].array_bytes || table[i].array_bytes > remaining ||
        table[i].array_bytes % empty.unit_size() ||
        table[i].constant_value < -1 ||
        table[i].constant_value > std::numeric_limits<int>::max() ||
        !table[i].name_bytes || table[i].name_bytes > names_remaining ||
        std::memchr(names, '\0', size_t(table[i].name_bytes)) ||
        !utf8::is_valid(names, names + table[i].name_bytes))
      return invalid();
    names += table[i].name_bytes;
    names_remaining -= size_t(table[i].name_bytes);
    remaining -= size_t(table[i].array_bytes);
  }
  if (remaining || names_remaining)
    return invalid();
  try {
    const char* image = Find<char>(table_end + padded_names);
    names = Find<char>(table_end);
    sections_.reserve(count);
    for (size_t i = 0; i < count; ++i) {
      auto section = std::make_unique<Section>();
      section->name.assign(names, size_t(table[i].name_bytes));
      names += table[i].name_bytes;
      section->constant_value = int(table[i].constant_value);
      section->trie.set_array(image,
                              table[i].array_bytes / section->trie.unit_size());
      image += table[i].array_bytes;
      sections_.push_back(std::move(section));
    }
    IndexSections();
    return true;
  } catch (const std::exception& e) {
    LOG(ERROR) << "cannot load Darts grammar sections: " << e.what();
    Close();
    return false;
  }
}

bool GramDb::Save() {
  return IsOpen() && !sections_.empty() && Flush();
}

bool GramDb::Build(Builder& builder) {
  Close();
  const size_t count = builder.sections_.size();
  if (!count || count > (SIZE_MAX - sizeof(grammar::Metadata)) /
                            sizeof(grammar::SectionMetadata))
    return false;
  size_t bytes =
      sizeof(grammar::Metadata) + count * sizeof(grammar::SectionMetadata);
  size_t names_bytes = 0;
  for (const auto& section : builder.sections_) {
    if (section->name.size() > SIZE_MAX - names_bytes)
      return false;
    names_bytes += section->name.size();
  }
  if (names_bytes > SIZE_MAX - 3)
    return false;
  const size_t padded_names = (names_bytes + 3) & ~size_t(3);
  if (padded_names > SIZE_MAX - bytes)
    return false;
  bytes += padded_names;
  for (const auto& section : builder.sections_) {
    if (section->trie.total_size() > SIZE_MAX - bytes)
      return false;
    bytes += section->trie.total_size();
  }
  sections_.swap(builder.sections_);
  if (!Create(bytes)) {
    Close();
    return false;
  }
  auto* metadata = Allocate<grammar::Metadata>();
  std::memcpy(metadata->format, kGrammarFormat, sizeof(kGrammarFormat));
  metadata->section_count = count;
  metadata->names_bytes = names_bytes;
  auto* table = Allocate<grammar::SectionMetadata>(count);
  char* names = Allocate<char>(padded_names);
  for (size_t i = 0; i < count; ++i) {
    auto& section = *sections_[i];
    const size_t size = section.trie.total_size();
    table[i] = {size, section.constant_value, section.name.size()};
    std::memcpy(names, section.name.data(), section.name.size());
    names += section.name.size();
    char* image = Allocate<char>(size);
    std::memcpy(image, section.trie.array(), size);
    section.trie.set_array(image, section.trie.size());
  }
  IndexSections();
  return true;
}

void GramDb::IndexSections() {
  all_sections_.resize(sections_.size());
  std::iota(all_sections_.begin(), all_sections_.end(), size_t(0));
  sections_by_name_.clear();
  for (size_t i = 0; i < sections_.size(); ++i)
    sections_by_name_[sections_[i]->name].push_back(i);
}

bool GramDb::SelectSections(const vector<string>& names,
                            SectionList* selected) const {
  if (!selected)
    return false;
  selected->clear();
  if (!IsOpen())
    return false;
  for (const auto& name : names) {
    auto found = sections_by_name_.find(name);
    if (found == sections_by_name_.end()) {
      LOG(ERROR) << "unknown grammar section '" << name << "' in "
                 << file_path();
      selected->clear();
      return false;
    }
    selected->insert(selected->end(), found->second.begin(),
                     found->second.end());
  }
  // Selection order must not change the priority stored in the model.
  std::sort(selected->begin(), selected->end());
  selected->erase(std::unique(selected->begin(), selected->end()),
                  selected->end());
  return true;
}

int GramDb::Lookup(const string& context,
                   const string& word,
                   Match results[kMaxResults]) const {
  return Lookup(context, word, results, all_sections_);
}

int GramDb::Lookup(const string& context,
                   const string& word,
                   Match results[kMaxResults],
                   const SectionList& selected) const {
  if (!IsOpen() || selected.empty() || word.empty() || !results)
    return 0;
  if (selected.size() == 1) {
    if (selected[0] >= sections_.size())
      return 0;
    const auto& section = *sections_[selected[0]];
    const int count = LookupTrie(section.trie, context, word, results);
    if (section.constant_value >= 0)
      for (int i = 0; i < count; ++i)
        results[i].value = section.constant_value;
    return count;
  }
  int count = 0;
  Match current[kMaxResults];
  for (size_t index : selected) {
    if (index >= sections_.size())
      return 0;
    const auto& section = sections_[index];
    const int matches = LookupTrie(section->trie, context, word, current);
    for (int i = 0; i < matches; ++i) {
      Match match = current[i];
      if (section->constant_value >= 0)
        match.value = section->constant_value;
      int position = 0;
      while (position < count && results[position].length < match.length)
        ++position;
      // The last selected section wins for the same key, regardless of weight.
      if (position < count && results[position].length == match.length) {
        results[position] = match;
      } else if (position < kMaxResults) {
        for (int j = (std::min)(count, kMaxResults - 1); j > position; --j)
          results[j] = results[j - 1];
        results[position] = match;
        count = (std::min)(count + 1, kMaxResults);
      }
    }
  }
  return count;
}

}  // namespace rime
