#ifndef RIME_OCTAGRAM_H_
#define RIME_OCTAGRAM_H_

#include <rime/common.h>
#include <rime/component.h>
#include <rime/resource.h>
#include <rime/gear/grammar.h>

namespace rime {

extern const ResourceType kGramDbType;
extern const string kGrammarDefaultLanguage;

class Config;
class GramDb;
struct GrammarConfig;
class OctagramComponent;

class Octagram : public Grammar {
 public:
  Octagram(Config* config,
           OctagramComponent* component,
           const vector<string>* sections = nullptr);
  virtual ~Octagram();
  double Query(const string& context,
               const string& word,
               bool is_rear) override;

 private:
  friend class OctagramComponent;
  the<GrammarConfig> config_;
  GramDb* db_ = nullptr;
  vector<size_t> sections_;
};

class OctagramComponent : public Grammar::Component {
 public:
  OctagramComponent();
  virtual ~OctagramComponent();

  Octagram* Create(Config* config) override;
  Octagram* CreateContextual(Config* config) override;

  GramDb* GetDb(const string& language);

 private:
  map<string, the<GramDb>> db_by_language_;
};

}  // namespace rime

#endif  // RIME_OCTAGRAM_H_
