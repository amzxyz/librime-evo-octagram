#include "gram_db.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <iomanip>
#include <numeric>
#include <sstream>
#include <utf8.h>

namespace rime {
namespace {

// Older readers must reject a layout with a variable section table.
constexpr char kGrammarFormat[] = "Rime::EvoGrammar/2";
static_assert(sizeof(grammar::Metadata) == 48);
static_assert(sizeof(grammar::SectionMetadata) == 40);

bool ValidWeight(double weight) {
  return std::isfinite(weight) && weight > 0;
}

bool ValidKey(const string& key) {
  return !key.empty() && key.find('\0') == string::npos;
}

int Score(double weight) {
  return (std::max)(0, int(std::log(weight) * GramDb::kValueScale));
}

bool ValidSectionText(std::string_view text) {
  return text.size() <= 4096 &&
         std::none_of(text.begin(), text.end(),
                      [](unsigned char c) { return c < 0x20 || c == 0x7f; }) &&
         utf8::is_valid(text.begin(), text.end());
}

int LookupTrie(const Darts::DoubleArray& trie,
               std::string_view context,
               std::string_view word,
               GramDb::Match* results) {
  // Darts ignores the size passed to set_array() during lookup. Check only
  // the visited units so a corrupt model cannot make lookup read past its
  // section.
  using Unit = Darts::Details::DoubleArrayUnit;
  const auto* units = static_cast<const Unit*>(trie.array());
  const size_t size = trie.size();
  size_t base = units[0].offset();
  auto step = [&](unsigned char label, Unit* unit) {
    const size_t next = base ^ label;
    if (next >= size || units[next].label() != label)
      return false;
    *unit = units[next];
    base = next ^ unit->offset();
    return true;
  };
  Unit unit;
  for (unsigned char label : context)
    if (!step(label, &unit))
      return 0;
  int count = 0;
  for (size_t i = 0; i < word.size() && count < GramDb::kMaxResults; ++i) {
    if (!step(static_cast<unsigned char>(word[i]), &unit))
      break;
    if (unit.has_leaf()) {
      if (base >= size || !(units[base].label() >> 31))
        return 0;
      results[count++] = {units[base].value(), i + 1};
    }
  }
  return count;
}

}  // namespace

bool GramDb::Builder::Add(Source source) {
  if (source.name.empty() || !ValidSectionText(source.name) ||
      !ValidSectionText(source.description))
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
    section->description = std::move(source.description);
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
      section->constant_weight = *source.constant_weight;
      section->constant_value = Score(section->constant_weight);
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
    if (section->trie.build(keys.size(), keys.data(), nullptr, values.data()))
      return false;
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
  auto invalid = [this]() {
    LOG(ERROR) << "invalid Darts grammar (expected " << kGrammarFormat
               << "): " << file_path();
    Close();
    return false;
  };
  try {
    if (!OpenReadOnly())
      return false;
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
    if (metadata->text_bytes > SIZE_MAX - 3 ||
        metadata->text_bytes > file_size() - table_end)
      return invalid();
    const size_t padded_text = (size_t(metadata->text_bytes) + 3) & ~size_t(3);
    if (padded_text > file_size() - table_end)
      return invalid();
    size_t remaining = file_size() - table_end - padded_text;
    std::string_view text(Find<char>(table_end), size_t(metadata->text_bytes));
    const size_t unit_size = Darts::DoubleArray().unit_size();
    for (size_t i = 0; i < count; ++i) {
      const auto& entry = table[i];
      if (!entry.array_bytes || entry.array_bytes > remaining ||
          entry.array_bytes % unit_size || entry.constant_value < -1 ||
          entry.constant_value > std::numeric_limits<int>::max() ||
          (entry.constant_value == -1 ? entry.constant_weight != 0
                                      : !ValidWeight(entry.constant_weight)) ||
          !entry.name_bytes || entry.name_bytes > text.size() ||
          !ValidSectionText(text.substr(0, size_t(entry.name_bytes))))
        return invalid();
      text.remove_prefix(size_t(entry.name_bytes));
      if (entry.description_bytes > text.size() ||
          !ValidSectionText(text.substr(0, size_t(entry.description_bytes))))
        return invalid();
      text.remove_prefix(size_t(entry.description_bytes));
      remaining -= size_t(entry.array_bytes);
    }
    if (remaining || !text.empty())
      return invalid();
    const char* image = Find<char>(table_end + padded_text);
    const char* names = Find<char>(table_end);
    sections_.reserve(count);
    for (size_t i = 0; i < count; ++i) {
      const auto& entry = table[i];
      auto section = std::make_unique<Section>();
      section->name.assign(names, size_t(entry.name_bytes));
      names += entry.name_bytes;
      section->description.assign(names, size_t(entry.description_bytes));
      names += entry.description_bytes;
      section->constant_weight = entry.constant_weight;
      section->constant_value = int(entry.constant_value);
      section->trie.set_array(image, entry.array_bytes / unit_size);
      image += entry.array_bytes;
      sections_.push_back(std::move(section));
    }
    IndexSections();
    LogSections();
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
  size_t text_bytes = 0;
  for (const auto& section : builder.sections_) {
    const size_t length = section->name.size() + section->description.size();
    if (length > SIZE_MAX - text_bytes)
      return false;
    text_bytes += length;
  }
  if (text_bytes > SIZE_MAX - 3)
    return false;
  const size_t padded_text = (text_bytes + 3) & ~size_t(3);
  if (padded_text > SIZE_MAX - bytes)
    return false;
  bytes += padded_text;
  for (const auto& section : builder.sections_) {
    if (section->trie.total_size() > SIZE_MAX - bytes)
      return false;
    bytes += section->trie.total_size();
  }
  try {
    // Preallocate the complete file. Retain no pointers across any resize.
    if (!(Exists() ? (Resize(bytes) && OpenReadWrite()) : Create(bytes)) ||
        capacity() != bytes) {
      Close();
      return false;
    }
    sections_.swap(builder.sections_);
    auto* metadata = Allocate<grammar::Metadata>();
    std::memcpy(metadata->format, kGrammarFormat, sizeof(kGrammarFormat));
    metadata->section_count = count;
    metadata->text_bytes = text_bytes;
    auto* table = Allocate<grammar::SectionMetadata>(count);
    char* text = Allocate<char>(padded_text);
    for (size_t i = 0; i < count; ++i) {
      auto& section = *sections_[i];
      const size_t size = section.trie.total_size();
      table[i] = {size, section.constant_value, section.name.size(),
                  section.description.size(), section.constant_weight};
      std::memcpy(text, section.name.data(), section.name.size());
      text += section.name.size();
      std::memcpy(text, section.description.data(), section.description.size());
      text += section.description.size();
      char* image = Allocate<char>(size);
      std::memcpy(image, section.trie.array(), size);
      section.trie.set_array(image, section.trie.size());
    }
    IndexSections();
    return true;
  } catch (const std::exception& e) {
    LOG(ERROR) << "cannot write Darts grammar " << file_path() << ": "
               << e.what();
    Close();
    return false;
  }
}

void GramDb::IndexSections() {
  all_sections_.resize(sections_.size());
  std::iota(all_sections_.begin(), all_sections_.end(), size_t(0));
  sections_by_name_.clear();
  for (size_t i = 0; i < sections_.size(); ++i)
    sections_by_name_[sections_[i]->name].push_back(i);
}

void GramDb::LogSections() const {
  LOG(INFO) << "loaded grammar " << file_path()
            << ", sections=" << sections_.size();
  for (size_t i = 0; i < sections_.size(); ++i) {
    const auto& section = *sections_[i];
    std::ostringstream weight;
    if (section.constant_value < 0)
      weight << "per-key";
    else {
      weight << std::setprecision(std::numeric_limits<double>::max_digits10)
             << section.constant_weight;
      weight << " score=" << section.constant_value;
    }
    LOG(INFO) << "grammar section[" << i
              << "] name=" << std::quoted(section.name)
              << " weight=" << weight.str()
              << " description=" << std::quoted(section.description);
  }
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

int GramDb::Lookup(std::string_view context,
                   std::string_view word,
                   Match results[kMaxResults]) const {
  return Lookup(context, word, results, all_sections_);
}

int GramDb::Lookup(std::string_view context,
                   std::string_view word,
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
