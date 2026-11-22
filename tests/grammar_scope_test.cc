#include <gtest/gtest.h>
#include <algorithm>
#include <rime/config.h>
#include <rime/dict/vocabulary.h>
#include <rime/gear/grammar.h>
#include <rime/gear/poet.h>
#include <rime/language.h>
#include <rime/registry.h>

using namespace rime;

struct QueryCall {
  bool contextual;
  string context;
  string word;
};

struct ScopeState {
  bool enable_contextual = true;
  vector<QueryCall> calls;
};

class RecordingGrammar : public Grammar {
 public:
  RecordingGrammar(ScopeState* state, bool contextual)
      : state_(state), contextual_(contextual) {}
  double Query(const string& context, const string& word, bool) override {
    state_->calls.push_back({contextual_, context, word});
    if (contextual_)
      return context == "秦" && word == "始皇" ? 100 : 0;
    return (context == "中" && (word == "考" || word == "考后")) ||
                   (context == "有" && word == "手癣")
               ? 100
               : 0;
  }

 private:
  ScopeState* state_;
  bool contextual_;
};

class RecordingComponent : public Grammar::Component {
 public:
  explicit RecordingComponent(ScopeState* state) : state_(state) {}
  Grammar* Create(Config*) override {
    return new RecordingGrammar(state_, false);
  }
  Grammar* CreateContextual(Config*) override {
    return state_->enable_contextual ? new RecordingGrammar(state_, true)
                                     : nullptr;
  }

 private:
  ScopeState* state_;
};

class GrammarScopeTest : public testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(nullptr, Registry::instance().Find("grammar"));
    Registry::instance().Register("grammar", new RecordingComponent(&state));
    registered = true;
  }
  void TearDown() override {
    if (registered)
      Registry::instance().Unregister("grammar");
  }
  static an<DictEntry> Entry(const string& text, double weight = 0) {
    auto entry = New<DictEntry>();
    entry->text = text;
    entry->weight = weight;
    return entry;
  }
  an<Translation> Candidates(const vector<pair<string, double>>& entries) {
    auto result = New<FifoTranslation>();
    for (const auto& [text, weight] : entries)
      result->Append(
          New<Phrase>(&language, "phrase", 0, 2, Entry(text, weight)));
    return result;
  }
  ScopeState state;
  Config config;
  Language language{"test"};
  bool registered = false;
};

struct ContextTranslator {
  string preceding;
  bool contextual_suggestions() const { return true; }
  string GetPrecedingText(size_t) const { return preceding; }
};

TEST_F(GrammarScopeTest, ContextualCandidatesUseOnlyTheContextualView) {
  Poet poet(&language, &config);
  ContextTranslator translator{"中"};
  auto candidates = Candidates({{"考后", 0}, {"靠后", 1}});
  auto result = poet.ContextualWeighted(candidates, "xx", 0, &translator);
  ASSERT_TRUE(result->Peek());
  EXPECT_EQ("靠后", result->Peek()->text());
  for (const auto& call : state.calls)
    EXPECT_TRUE(call.contextual);

  state.calls.clear();
  translator.preceding = "有";
  candidates = Candidates({{"手癣", 0}, {"首选", 1}});
  result = poet.ContextualWeighted(candidates, "xx", 0, &translator);
  ASSERT_TRUE(result->Peek());
  EXPECT_EQ("首选", result->Peek()->text());
}

TEST_F(GrammarScopeTest, ASelectedNameCanUseOneCharacterOfContext) {
  Poet poet(&language, &config);
  ContextTranslator translator{"秦"};
  auto result = poet.ContextualWeighted(Candidates({{"什么", 1}, {"始皇", 0}}),
                                        "xx", 0, &translator);
  ASSERT_TRUE(result->Peek());
  EXPECT_EQ("始皇", result->Peek()->text());
}

TEST_F(GrammarScopeTest, SentenceBoundaryCannotBypassTheContextualView) {
  Poet poet(&language, &config);
  WordGraph graph;
  graph[0][1].push_back(Entry("考"));
  graph[0][1].push_back(Entry("靠", 1));
  graph[1][2].push_back(Entry("后"));
  auto sentence = poet.MakeSentence(graph, 2, "中");
  ASSERT_TRUE(sentence);
  EXPECT_EQ("靠后", sentence->text());
  EXPECT_TRUE(std::any_of(state.calls.begin(), state.calls.end(),
                          [](const QueryCall& call) {
                            return call.contextual && call.context == "中";
                          }));
  state.calls.clear();
  auto sentences = poet.MakeSentences(graph, 2, "中", 3, 0);
  ASSERT_FALSE(sentences.empty());
  EXPECT_EQ("靠后", sentences.front()->text());
  for (const auto& call : state.calls)
    if (call.context == "中")
      EXPECT_TRUE(call.contextual);
}

TEST_F(GrammarScopeTest, InternalSentenceEdgesStillUseTheFullModel) {
  Poet poet(&language, &config);
  WordGraph graph;
  graph[0][1].push_back(Entry("中"));
  graph[1][3].push_back(Entry("考后"));
  graph[1][3].push_back(Entry("靠后", 1));
  auto sentence = poet.MakeSentence(graph, 3, "");
  ASSERT_TRUE(sentence);
  EXPECT_EQ("中考后", sentence->text());
  EXPECT_TRUE(std::any_of(
      state.calls.begin(), state.calls.end(), [](const QueryCall& call) {
        return !call.contextual && call.context == "中" && call.word == "考后";
      }));
  auto sentences = poet.MakeSentences(graph, 3, "", 3, 0);
  ASSERT_FALSE(sentences.empty());
  EXPECT_EQ("中考后", sentences.front()->text());
}

TEST_F(GrammarScopeTest,
       DisabledContextPreservesCandidateOrderAndStartPenalty) {
  state.enable_contextual = false;
  Poet poet(&language, &config);
  ContextTranslator translator{"中"};
  auto candidates = Candidates({{"考后", 0}, {"靠后", 1}});
  EXPECT_EQ(candidates,
            poet.ContextualWeighted(candidates, "xx", 0, &translator));
  EXPECT_TRUE(state.calls.empty());
  WordGraph graph;
  graph[0][1].push_back(Entry("考"));
  graph[0][1].push_back(Entry("靠", 1));
  graph[1][2].push_back(Entry("后"));
  auto sentence = poet.MakeSentence(graph, 2, "中");
  ASSERT_TRUE(sentence);
  EXPECT_EQ("靠后", sentence->text());
  EXPECT_DOUBLE_EQ(1, sentence->weight());
  for (const auto& call : state.calls)
    EXPECT_NE("中", call.context);
}

int main(int argc, char** argv) {
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
