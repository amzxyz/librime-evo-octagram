#include "gram_db.h"
#include "gram_encoding.h"
#include "octagram.h"
#include <algorithm>
#include <rime/config.h>
#include <rime/resource.h>
#include <rime/service.h>
#include <utf8.h>

namespace rime {

struct GrammarConfig {
  int collocation_max_length = 4;
  int collocation_min_length = 3;
  double collocation_penalty = -12;
  double non_collocation_penalty = -12;
  double weak_collocation_penalty = -24;
  double rear_penalty = -18;
};

namespace {
const ResourceType kModelResource = {"gram_db", "", grammar::kModelSuffix};
}  // namespace

Octagram::Octagram(Config* config,
                   OctagramComponent* component,
                   const vector<string>* sections)
    : config_(std::make_unique<GrammarConfig>()) {
  string language;
  if (config) {
    if (!config->GetString("grammar/language", &language))
      return;
    config->GetInt("grammar/collocation_max_length",
                   &config_->collocation_max_length);
    config->GetInt("grammar/collocation_min_length",
                   &config_->collocation_min_length);
    config->GetDouble("grammar/collocation_penalty",
                      &config_->collocation_penalty);
    config->GetDouble("grammar/non_collocation_penalty",
                      &config_->non_collocation_penalty);
    config->GetDouble("grammar/weak_collocation_penalty",
                      &config_->weak_collocation_penalty);
    config->GetDouble("grammar/rear_penalty", &config_->rear_penalty);
  }
  if (!language.empty()) {
    db_ = component->GetDb(language);
    if (db_) {
      if (sections) {
        if (!db_->SelectSections(*sections, &sections_))
          db_.reset();
      } else {
        sections_ = db_->all_sections();
      }
    }
  }
}

Octagram::~Octagram() {}

inline static double scale_value(int value) {
  return value >= 0 ? double(value) / GramDb::kValueScale : -1;
}

inline static const char* str_begin(const string& str) {
  return str.c_str();
}

inline static const char* str_end(const string& str) {
  return str.c_str() + str.length();
}

inline static const char* last_n_unicode(const string& str,
                                         int max,
                                         int& out_count) {
  const char* begin = str_begin(str);
  const char* p = str_end(str);
  out_count = 0;
  while (p != begin && out_count < max) {
    utf8::unchecked::prior(p);
    ++out_count;
  }
  return p;
}

inline static const char* first_n_unicode(const string& str,
                                          int max,
                                          int& out_count) {
  const char* p = str_begin(str);
  const char* end = str_end(str);
  out_count = 0;
  while (p != end && out_count < max) {
    utf8::unchecked::next(p);
    ++out_count;
  }
  return p;
}

double Octagram::Query(const string& context,
                       const string& word,
                       bool is_rear) {
  if (!db_ || context.empty() || word.empty() ||
      config_->collocation_max_length <= 1) {
    return config_->non_collocation_penalty;
  }
  double result = config_->non_collocation_penalty;
  GramDb::Match matches[GramDb::kMaxResults];
  const int n = (std::min)(grammar::kMaxEncodedUnicode,
                           config_->collocation_max_length - 1);
  int context_len = 0;
  const string context_query = grammar::encode(
      last_n_unicode(context, n, context_len), str_end(context));
  int word_query_len = 0;
  const char* word_end = first_n_unicode(word, n, word_query_len);
  const string word_query = grammar::encode(str_begin(word), word_end);
  const char* context_end = str_end(context_query);
  for (const char* context_ptr = str_begin(context_query); context_len > 0;
       --context_len, context_ptr = grammar::next_unicode(context_ptr)) {
    const std::string_view suffix(context_ptr, context_end - context_ptr);
    const int count = db_->Lookup(suffix, word_query, matches, sections_);
    for (int i = 0; i < count; ++i) {
      const auto& match = matches[i];
      const int match_len = grammar::unicode_length(word_query, match.length);
      const bool whole = context_ptr == str_begin(context_query) &&
                         match.length == word_query.size();
      const double penalty =
          context_len + match_len >= config_->collocation_min_length || whole
              ? config_->collocation_penalty
              : config_->weak_collocation_penalty;
      result = (std::max)(result, scale_value(match.value) + penalty);
    }
  }
  if (is_rear && word_end == str_end(word) &&
      db_->Lookup(word_query, "$", matches, sections_) > 0) {
    result = (std::max)(result,
                        scale_value(matches[0].value) + config_->rear_penalty);
  }
  return result;
}

OctagramComponent::OctagramComponent() {}

OctagramComponent::~OctagramComponent() {}

Octagram* OctagramComponent::Create(Config* config) {
  return new Octagram(config, this);
}

Octagram* OctagramComponent::CreateContextual(Config* config) {
  if (!config)
    return nullptr;
  auto list = config->GetList("grammar/contextual_sections");
  if (!list) {
    if (!config->IsNull("grammar/contextual_sections"))
      LOG(ERROR)
          << "grammar/contextual_sections must be a list of section names";
    return nullptr;
  }
  if (!list->size())
    return nullptr;
  vector<string> names;
  names.reserve(list->size());
  for (size_t i = 0; i < list->size(); ++i) {
    auto item = list->GetValueAt(i);
    string name;
    if (!item || !item->GetString(&name) || name.empty()) {
      LOG(ERROR) << "grammar/contextual_sections must contain nonempty names";
      return nullptr;
    }
    names.push_back(std::move(name));
  }
  auto grammar = std::make_unique<Octagram>(config, this, &names);
  return grammar->db_ ? grammar.release() : nullptr;
}

an<const GramDb> OctagramComponent::GetDb(const string& language) {
  std::lock_guard<std::mutex> lock(db_mutex_);
  if (auto found = db_by_language_.find(language);
      found != db_by_language_.end())
    if (auto db = found->second.lock())
      return db;

  for (auto it = db_by_language_.begin(); it != db_by_language_.end();) {
    if (it->second.expired())
      it = db_by_language_.erase(it);
    else
      ++it;
  }
  the<ResourceResolver> resolver(
      Service::instance().CreateResourceResolver(kModelResource));
  auto db = New<GramDb>(resolver->ResolvePath(language));
  if (!db->Load())
    return nullptr;
  db_by_language_.emplace(language, db);
  return db;
}

}  // namespace rime
