//
// Copyright RIME Developers
//
#include <cmath>
#include <fstream>
#include <cstdlib>
#include <iostream>
#include <utf8.h>
#include <rime/common.h>
#include "gram_db.h"
#include "gram_encoding.h"
#include "octagram.h"

using namespace rime;

namespace {

struct Input {
  path file;
  std::optional<double> constant_weight;
  string section = "default";
};

struct Options {
  vector<Input> inputs;
  string name = "wanxiang-lts";
  string language = "zh-hans";
};

void Usage() {
  std::cout
      << "Usage: build_grammar -t FILE [-w WEIGHT] [-t FILE ...] [options]\n"
      << "  --section NAME     region name for subsequent -t files\n"
      << "  -t FILE            add an input file, in priority order\n"
      << "  -w WEIGHT          common frequency for the preceding -t file\n"
      << "                     with -w: key, optionally followed by ignored "
         "frequency\n"
      << "                     without -w: key and frequency per line\n"
      << "  --name NAME        output prefix (default: wanxiang-lts)\n"
      << "  --language LANG    language suffix (default: zh-hans)\n"
      << "Adjacent files with the same region name and weight share a "
         "section.\n"
      << "Output: NAME-LANG.dlm; later sections override duplicate keys.\n";
}

bool ParseWeight(const string& text, double* weight) {
  try {
    size_t consumed;
    *weight = std::stod(text, &consumed);
    return consumed == text.size() && std::isfinite(*weight) && *weight > 0;
  } catch (const std::exception&) {
    return false;
  }
}

bool ParseOptions(int argc, char** argv, Options* options) {
  std::optional<string> name, language;
  string section = "default";
  for (int i = 1; i < argc; ++i) {
    const string flag = argv[i];
    if (i + 1 == argc)
      return false;
    const string value = argv[++i];
    if (flag == "-t") {
      options->inputs.push_back(
          {std::filesystem::u8path(value), std::nullopt, section});
    } else if (flag == "-w") {
      if (options->inputs.empty() ||
          options->inputs.back().constant_weight.has_value())
        return false;
      double weight;
      if (!ParseWeight(value, &weight))
        return false;
      options->inputs.back().constant_weight = weight;
    } else if (flag == "--section") {
      section = value;
    } else if (flag == "--name") {
      name = value;
    } else if (flag == "--language") {
      language = value;
    } else {
      return false;
    }
  }
  if (name)
    options->name = *name;
  if (language)
    options->language = *language;
  if (options->inputs.empty() || options->name.empty() ||
      options->language.empty())
    return false;
  for (const auto& input : options->inputs)
    if (input.file.empty() || input.section.empty() ||
        !utf8::is_valid(input.section.begin(), input.section.end()))
      return false;
  return true;
}

bool ReadInput(const Input& source, GramDb::Source* data) {
  std::ifstream input(source.file);
  if (!input) {
    LOG(ERROR) << "cannot open grammar input: " << source.file;
    return false;
  }
  data->constant_weight = source.constant_weight;
  string line;
  size_t line_number = 0;
  while (std::getline(input, line)) {
    ++line_number;
    const size_t begin = line.find_first_not_of(" \t\r");
    if (begin == string::npos)
      continue;
    const size_t end = line.find_first_of(" \t\r", begin);
    const string key = line.substr(begin, end - begin);
    if (!utf8::is_valid(key.begin(), key.end())) {
      LOG(ERROR) << source.file << ':' << line_number << ": invalid UTF-8 key";
      return false;
    }
    double weight = 0;
    const bool has_frequency =
        end != string::npos &&
        line.find_first_not_of(" \t\r", end) != string::npos;
    if (has_frequency) {
      const char* begin_frequency = line.c_str() + end;
      char* finish = nullptr;
      weight = std::strtod(begin_frequency, &finish);
      if (finish == begin_frequency || !std::isfinite(weight) || weight <= 0 ||
          line.find_first_not_of(" \t\r", size_t(finish - line.c_str())) !=
              string::npos) {
        LOG(ERROR) << source.file << ':' << line_number
                   << ": invalid frequency or additional fields";
        return false;
      }
    }
    if (source.constant_weight) {
      data->constant_keys.push_back(grammar::encode(key));
    } else {
      if (!has_frequency) {
        LOG(ERROR) << source.file << ':' << line_number
                   << ": missing per-key frequency";
        return false;
      }
      data->weighted_entries.emplace_back(grammar::encode(key), weight);
    }
  }
  if (!input.eof()) {
    LOG(ERROR) << "cannot read grammar input: " << source.file;
    return false;
  }
  return true;
}

}  // namespace

int main(int argc, char* argv[]) {
  std::ios::sync_with_stdio(false);
  if (argc == 2 && string(argv[1]) == "--help") {
    Usage();
    return 0;
  }
  try {
    Options options;
    if (!ParseOptions(argc, argv, &options)) {
      Usage();
      return 2;
    }
    const path output = std::filesystem::absolute(std::filesystem::u8path(
        options.name + "-" + options.language + kGramDbType.suffix));
    for (const auto& input : options.inputs) {
      if (std::filesystem::weakly_canonical(input.file) ==
              std::filesystem::weakly_canonical(output) ||
          (std::filesystem::exists(output) &&
           std::filesystem::exists(input.file) &&
           std::filesystem::equivalent(input.file, output))) {
        LOG(ERROR) << "output must not overwrite an input file: " << input.file;
        return 2;
      }
    }
    GramDb::Builder builder;
    for (size_t begin = 0; begin < options.inputs.size();) {
      size_t end = begin + 1;
      while (end < options.inputs.size() &&
             options.inputs[end].constant_weight ==
                 options.inputs[begin].constant_weight &&
             options.inputs[end].section == options.inputs[begin].section)
        ++end;
      GramDb::Source source;
      source.constant_weight = options.inputs[begin].constant_weight;
      source.name = options.inputs[begin].section;
      for (size_t i = begin; i < end; ++i)
        if (!ReadInput(options.inputs[i], &source))
          return 1;
      LOG(INFO) << "compiling grammar section " << source.name
                << ": files=" << end - begin << ", keys="
                << source.constant_keys.size() + source.weighted_entries.size();
      if (!builder.Add(std::move(source)))
        return 1;
      begin = end;
    }
    GramDb db(output);
    if (!db.Build(builder) || !db.Save()) {
      LOG(ERROR) << "cannot build grammar: " << output;
      return 1;
    }
    LOG(INFO) << "created: " << output;
    std::cout << output.u8string() << '\n';
    return 0;
  } catch (const std::exception& e) {
    LOG(ERROR) << "cannot build grammar: " << e.what();
    return 1;
  }
}
