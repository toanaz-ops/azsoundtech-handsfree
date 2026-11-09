// Lane L2a: parseCopilotFindings (spec 2026-11-09 section 4.1).

#include <gtest/gtest.h>

#include "app/CopilotFindings.h"

// NOTE: no non-ASCII bytes in this source. MSVC reads it in the ANSI code page
// (no /utf-8 here), so Vietnamese is written as JSON \uXXXX escapes or \x bytes.

TEST (CopilotFindings, ParsesRealSidecarSample)
{
    // Shape copied from `copilot.py tests/fixtures/session-sample.jsonl --json`.
    const juce::String json (R"json([
  {"code":"ceiling","severity":"alert","title":"1008 Hz ch\u1EA1m tr\u1EA7n -18 dB (slot 01 l\u00E0n R)",
   "advice":"H\u1EA1 gain monitor; \u0111\u1EEBng t\u0103ng tr\u1EA7n.",
   "evidence":{"hz":1007.8,"slot":0,"lane":"R","deepest_db":-18,"ceiling_db":-18.0}},
  {"code":"false_rate","severity":"warn","title":"1/3 notch b\u1ECB \u0111\u00E1nh d\u1EA5u oan","advice":"Th\u1EED preset Safe.",
   "evidence":{"false":1,"judged":3}},
  {"code":"recurrence","severity":"info","title":"1008 Hz h\u00FA l\u1EA1i 2 l\u1EA7n","advice":"Ki\u1EC3m tra h\u01B0\u1EDBng mic.",
   "evidence":{"hz":1007.8,"count":2}}
])json");

    const auto parsed = parseCopilotFindings (json);
    ASSERT_TRUE (parsed.has_value());
    ASSERT_EQ (parsed->size(), 3u);
    EXPECT_EQ ((*parsed)[0].code, "ceiling");
    EXPECT_EQ ((*parsed)[0].severity, "alert");
    EXPECT_EQ ((*parsed)[1].severity, "warn");
    EXPECT_EQ ((*parsed)[2].severity, "info");
    EXPECT_EQ ((*parsed)[2].title, juce::String (L"1008 Hz h\u00FA l\u1EA1i 2 l\u1EA7n"));
}

TEST (CopilotFindings, EmptyArrayIsEmptyVectorNotNullopt)
{
    const auto parsed = parseCopilotFindings ("[]");
    ASSERT_TRUE (parsed.has_value());
    EXPECT_TRUE (parsed->empty());
}

TEST (CopilotFindings, InvalidJsonIsNullopt)
{
    EXPECT_FALSE (parseCopilotFindings ("not json at all").has_value());
    EXPECT_FALSE (parseCopilotFindings ("[{\"code\":").has_value());
    EXPECT_FALSE (parseCopilotFindings ("").has_value());
}

TEST (CopilotFindings, ObjectInsteadOfArrayIsNullopt)
{
    EXPECT_FALSE (parseCopilotFindings (R"({"code":"a","title":"t","advice":"x"})").has_value());
}

TEST (CopilotFindings, ElementsMissingFieldsOrNotObjectsAreSkipped)
{
    const auto parsed = parseCopilotFindings (R"([
      {"code":"a","title":"t"},
      {"title":"t","advice":"x"},
      {"code":"a","advice":"x"},
      42, "text", null,
      {"code":"ok","severity":"warn","title":"T","advice":"A"}
    ])");

    ASSERT_TRUE (parsed.has_value());
    ASSERT_EQ (parsed->size(), 1u);
    EXPECT_EQ ((*parsed)[0].code, "ok");
}

TEST (CopilotFindings, UnknownOrMissingSeverityBecomesInfo)
{
    const auto parsed = parseCopilotFindings (R"([
      {"code":"a","severity":"critical","title":"T","advice":"A"},
      {"code":"b","title":"T","advice":"A"},
      {"code":"c","severity":7,"title":"T","advice":"A"}
    ])");

    ASSERT_TRUE (parsed.has_value());
    ASSERT_EQ (parsed->size(), 3u);
    for (const auto& f : *parsed)
        EXPECT_EQ (f.severity, "info");
}

TEST (CopilotFindings, VietnameseTextRoundTripsByteExact)
{
    // Raw UTF-8 bytes in the JSON text: U+1EA1 (E1 BA A1) and U+1ED5 (E1 BB 95).
    const std::string text = "[{\"code\":\"x\",\"title\":\"H\xE1\xBA\xA1 g\",\"advice\":\"\xE1\xBB\x95n\"}]";
    const auto parsed = parseCopilotFindings (juce::String::fromUTF8 (text.c_str()));

    ASSERT_TRUE (parsed.has_value());
    ASSERT_EQ (parsed->size(), 1u);
    EXPECT_STREQ ((*parsed)[0].title.toRawUTF8(), "H\xE1\xBA\xA1 g");
    EXPECT_STREQ ((*parsed)[0].advice.toRawUTF8(), "\xE1\xBB\x95n");
}

TEST (CopilotFindings, OverCapListIsTruncated)
{
    juce::String json ("[");
    for (int i = 0; i < kMaxCopilotFindings + 20; ++i)
    {
        if (i > 0)
            json << ",";
        json << "{\"code\":\"c" << i << "\",\"title\":\"T\",\"advice\":\"A\"}";
    }
    json << "]";

    const auto parsed = parseCopilotFindings (json);
    ASSERT_TRUE (parsed.has_value());
    EXPECT_EQ (static_cast<int> (parsed->size()), kMaxCopilotFindings);
    EXPECT_EQ (parsed->front().code, "c0");
}
