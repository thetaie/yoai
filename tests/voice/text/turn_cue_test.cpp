// The turn cues, checked against what they were made from.
//
//     build/yoai-turn-cue-test
//
// The unfinished and answerable sets are the ones Qwen3.5-2B and 4B were asked
// to tell apart with a <wait> marker (voice/text/turn_cue.hpp has the numbers);
// the rest are the corrections and stops the second judgment is for, and the
// sentences each rule was narrowed by -- a cue that also ends or starts a
// finished sentence is the mistake that costs a reply nobody asked to wait for.
//
// Also checks that the marker the live-turn prompt asks for is the one yoai.h
// tells a host to look for: two copies of one string, which is what this is
// here to keep equal.

#include "yoai.h"

#include "llm/prompt/prompts.hpp"
#include "voice/text/turn_cue.hpp"

#include <cstdio>
#include <cstring>
#include <string>

using yoai::voice::TurnCue;
using yoai::voice::turn_cue;

namespace {

int g_failures = 0;

const char* name(TurnCue c) {
    switch (c) {
        case TurnCue::Unfinished: return "unfinished";
        case TurnCue::Correct:    return "correct";
        case TurnCue::Stop:       return "stop";
        default:                  return "answer";
    }
}

void expect(const char* text, TurnCue want) {
    const TurnCue got = turn_cue(text);
    if (got != want) {
        std::printf("  FAIL %s: %s, wanted %s\n", text, name(got), name(want));
        ++g_failures;
    }
}

void check(bool ok, const char* what) {
    if (!ok) {
        std::printf("  FAIL %s\n", what);
        ++g_failures;
    }
}

}  // namespace

int main() {
    std::printf("unfinished: nothing to answer yet\n");
    const char* const unfinished[] = {
        "我想问一下", "我想问一下，", "我想问一下……", "那个", "那个。", "嗯，我觉得", "你帮我看看",
        "就是说", "帮我查一下", "我想知道", "对了", "我刚才在想", "嗯", "呃，那个", "明天下雨的话",
        "还有", "我想去北京，然后", "你知道", "So I was wondering", "and",
    };
    for (std::size_t i = 0; i < sizeof(unfinished) / sizeof(unfinished[0]); ++i) {
        expect(unfinished[i], TurnCue::Unfinished);
    }

    std::printf("answer: something to answer\n");
    const char* const answer[] = {
        "明天上海天气怎么样", "帮我订去北京的票", "你好", "还有杭州", "讲个笑话", "我今天有点累",
        "我想问一下明天上海天气怎么样", "谢谢", "今天几号", "继续", "好的",
        // What the rules were narrowed by: each ends or starts with a cue's text.
        "停车场在哪", "不过明天呢", "退款金额", "我想要这个", "我要那个", "对，就是", "说对了",
        "这样可不可以", "今天挺暖和", "I like it also", "That's all",
        // "明天" alone has no question yet, but no rule says so; the model may.
        "明天",
    };
    for (std::size_t i = 0; i < sizeof(answer) / sizeof(answer[0]); ++i) {
        expect(answer[i], TurnCue::Answer);
    }

    std::printf("correct: takes back the request and says what instead\n");
    const char* const correct[] = {
        "不，我说错了，应该是杭州", "不对，是杭州", "我是说杭州", "应该是后天", "换成北京",
        "不，杭州", "算了，换个话题", "等一下，我想问别的", "No, Hangzhou", "Actually, make it two",
    };
    for (std::size_t i = 0; i < sizeof(correct) / sizeof(correct[0]); ++i) {
        expect(correct[i], TurnCue::Correct);
    }

    std::printf("stop: takes it back and says nothing else\n");
    const char* const stop[] = {
        "停", "停！", "算了", "别说了", "不用了", "等一下", "不对", "不", "不，那个", "Stop", "Never mind",
    };
    for (std::size_t i = 0; i < sizeof(stop) / sizeof(stop[0]); ++i) {
        expect(stop[i], TurnCue::Stop);
    }

    std::printf("C ABI\n");
    check(yoai_turn_cue(nullptr) == YOAI_CUE_ANSWER, "NULL is ANSWER");
    check(yoai_turn_cue("我想问一下") == YOAI_CUE_UNFINISHED, "UNFINISHED");
    check(yoai_turn_cue("不对，是杭州") == YOAI_CUE_CORRECT, "CORRECT");
    check(yoai_turn_cue("算了") == YOAI_CUE_STOP, "STOP");
    check(yoai_turn_cue("还有杭州") == YOAI_CUE_ANSWER, "ANSWER");

    std::printf("the wait marker\n");
    check(std::strcmp(yoai::llm::kWaitMarker, YOAI_WAIT_MARKER) == 0,
          "prompts.cpp and yoai.h name the same marker");
    yoai::llm::Prompt live;
    check(yoai::llm::Prompts::instance().get(yoai::llm::kPromptLiveTurn, &live) &&
              live.text.find(YOAI_WAIT_MARKER) != std::string::npos,
          "the live-turn prompt asks for that marker");
    const std::string live_turn = yoai_prompt_turn_system(nullptr, YOAI_TURN_LIVE);
    const std::string read_turn = yoai_prompt_turn_system(nullptr, YOAI_TURN_READ);
    check(live_turn.find(live.text) != std::string::npos, "a live turn carries it");
    check(read_turn.find(live.text) == std::string::npos, "a read turn does not");

    std::printf(g_failures == 0 ? "PASS\n" : "%d failure(s)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
