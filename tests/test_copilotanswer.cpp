// Lane L2b: parseCopilotAnswer (sidecar `--ask --json` stdout).

#include <gtest/gtest.h>

#include "app/CopilotAnswer.h"

using Kind = CopilotAnswer::Kind;

namespace
{
juce::String utf8 (const char* s) { return juce::String::fromUTF8 (s); }
}

TEST (CopilotAnswer, FullAnswer)
{
    const auto a = parseCopilotAnswer (utf8 (
        R"x({"answer": "H\u1ea1 gain k\u00eanh 2", "blocked": false, "provider": "openai_compatible", "model": "llama3",
            "usage": {"input_tokens": 123, "output_tokens": 45},
            "findings": [{"code": "c1", "severity": "alert", "title": "t", "advice": "a", "evidence": {}},
                         {"code": "c2", "severity": "weird", "title": "t2", "advice": "a2"},
                         {"title": "missing fields"}, 7]})x"));
    ASSERT_EQ (a.kind, Kind::answer);
    EXPECT_EQ (a.text, utf8 ("H\xe1\xba\xa1 gain k\xc3\xaanh 2"));
    EXPECT_EQ (a.provider, "openai_compatible");
    EXPECT_EQ (a.model, "llama3");
    EXPECT_EQ (a.inputTokens, 123);
    EXPECT_EQ (a.outputTokens, 45);
    ASSERT_EQ (a.findings.size(), 2u);
    EXPECT_EQ (a.findings[0].severity, "alert");
    EXPECT_EQ (a.findings[1].severity, "info");
}

TEST (CopilotAnswer, NullAndBadUsageBecomeMinusOne)
{
    auto a = parseCopilotAnswer (R"({"answer": "x", "usage": {"input_tokens": null, "output_tokens": 7}})");
    ASSERT_EQ (a.kind, Kind::answer);
    EXPECT_EQ (a.inputTokens, -1);
    EXPECT_EQ (a.outputTokens, 7);

    a = parseCopilotAnswer (R"({"answer": "x", "usage": {"input_tokens": -3, "output_tokens": 1.5}})");
    EXPECT_EQ (a.inputTokens, -1);
    EXPECT_EQ (a.outputTokens, -1);

    a = parseCopilotAnswer (R"({"answer": "x"})");
    ASSERT_EQ (a.kind, Kind::answer);
    EXPECT_EQ (a.inputTokens, -1);
    EXPECT_TRUE (a.findings.empty());
}

TEST (CopilotAnswer, BadFindingsArrayYieldsEmptyList)
{
    const auto a = parseCopilotAnswer (R"({"answer": "x", "findings": "nope"})");
    ASSERT_EQ (a.kind, Kind::answer);
    EXPECT_TRUE (a.findings.empty());
}

TEST (CopilotAnswer, Blocked)
{
    const auto a = parseCopilotAnswer (R"({"blocked": true, "message": "fixed refusal"})");
    EXPECT_EQ (a.kind, Kind::blocked);
    EXPECT_EQ (a.text, "fixed refusal");
}

TEST (CopilotAnswer, Error)
{
    const auto a = parseCopilotAnswer (R"({"error": "connection failed"})");
    EXPECT_EQ (a.kind, Kind::error);
    EXPECT_EQ (a.text, "connection failed");
}

TEST (CopilotAnswer, InvalidShapes)
{
    for (const char* text : { "", "not json", "[]", "42", "{}", R"({"answer": ""})", R"({"answer": 5})",
                              R"({"answer": null})", R"({"blocked": true})", R"({"error": 3})" })
        EXPECT_EQ (parseCopilotAnswer (text).kind, Kind::invalid) << text;
}

TEST (CopilotAnswer, BlockedWinsOverAnAnswerField)
{
    const auto a = parseCopilotAnswer (R"({"blocked": true, "message": "m", "answer": "secret"})");
    EXPECT_EQ (a.kind, Kind::blocked);
    EXPECT_EQ (a.text, "m");
}

TEST (CopilotAnswer, Caps)
{
    const auto big = juce::String::repeatedString ("a", 20000);
    auto a = parseCopilotAnswer ("{\"answer\": \"" + big + "\"}");
    ASSERT_EQ (a.kind, Kind::answer);
    EXPECT_EQ (a.text.length(), kMaxCopilotAnswerChars);

    a = parseCopilotAnswer ("{\"error\": \"" + big + "\"}");
    EXPECT_EQ (a.text.length(), kMaxCopilotMessageChars);

    a = parseCopilotAnswer ("{\"blocked\": true, \"message\": \"" + big + "\"}");
    EXPECT_EQ (a.text.length(), kMaxCopilotMessageChars);

    juce::String rows;
    for (int i = 0; i < 100; ++i)
        rows += (i ? "," : "") + juce::String (R"({"code":"c","title":"t","advice":"a"})");
    a = parseCopilotAnswer ("{\"answer\": \"x\", \"findings\": [" + rows + "]}");
    EXPECT_EQ ((int) a.findings.size(), kMaxCopilotFindings);
}
