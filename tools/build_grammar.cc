//
// Copyright RIME Developers
//
#include <iostream>
#include <utf8.h>
#include <rime/common.h>
#include "gram_db.h"
#include "gram_encoding.h"
#include "octagram.h"

using namespace rime;

int main(int argc, char* argv[]) {
  std::ios::sync_with_stdio(false);
  std::cin.tie(nullptr);
  string language = argc > 1 ? string(argv[1]) : kGrammarDefaultLanguage;
  LOG(INFO) << "building grammar for language: " << language;

  try {
    GramDb::Builder builder;
    string key;
    while (std::cin >> key) {
      double value;
      if (!(std::cin >> value)) {
        LOG(ERROR) << "missing or invalid weight for: " << key;
        return 1;
      }
      if (!utf8::is_valid(key.begin(), key.end())) {
        LOG(ERROR) << "invalid UTF-8 grammar key: " << key;
        return 1;
      }
      if (!builder.Add(grammar::encode(key), value))
        return 1;
    }
    if (!std::cin.eof()) {
      LOG(ERROR) << "cannot read grammar input";
      return 1;
    }
    GramDb db(path{language + kGramDbType.suffix});
    LOG(INFO) << "creating " << db.file_path();
    if (!db.Build(builder) || !db.Save()) {
      LOG(ERROR) << "failed to build " << db.file_path();
      return 1;
    }
    LOG(INFO) << "created: " << db.file_path();
  } catch (const std::exception& e) {
    LOG(ERROR) << "cannot build grammar database: " << e.what();
    return 1;
  }
  return 0;
}
