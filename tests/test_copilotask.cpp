// Lane L2b: ask-request plumbing (sanitiser, validators, argv) and the runner
// delivering exit 3 / 4 results with their stdout. No process is started.

#include <gtest/gtest.h>

#include "app/SidecarRunner.h"

#include <condition_variable>
#include <deque>
#include <mutex>

namespace
{
CopilotRequest askRequest()
{
    CopilotRequest r;
    r.executable = juce::File ("C:/x/HandsFreeCopilot.exe");
    r.sessionLog = juce::File ("C:/x/session.jsonl");
    r.question = "Why does slot 2 ring?";
    r.provider = "anthropic";
    return r;
}

juce::String utf8 (const char* s) { return juce::String::fromUTF8 (s); }
}

TEST (CopilotQuestion, ControlCharsAndQuotesAreNeutralised)
{
    EXPECT_EQ (sanitiseCopilotQuestion ("  a\tb\r\nc  \"d\"  "), "a b c 'd'");
    EXPECT_EQ (sanitiseCopilotQuestion (juce::String ("x") + juce::String::charToString (1) + "y"), "x y");
    EXPECT_EQ (sanitiseCopilotQuestion ("a    b"), "a b");
}

TEST (CopilotQuestion, TrailingBackslashesAreStripped)
{
    EXPECT_EQ (sanitiseCopilotQuestion ("path C:\\dir\\"), "path C:\\dir");
    EXPECT_EQ (sanitiseCopilotQuestion ("a \\ \\\\"), "a");
    EXPECT_TRUE (sanitiseCopilotQuestion ("\\\\\\").isEmpty());
}

TEST (CopilotQuestion, CappedAtTwoThousandCharacters)
{
    const auto out = sanitiseCopilotQuestion (juce::String::repeatedString ("a", 5000));
    EXPECT_EQ (out.length(), 2000);
}

TEST (CopilotQuestion, VietnameseSurvives)
{
    const auto q = utf8 ("t\xe1\xba\xa1i sao k\xc3\xaanh 2 h\xc3\xba?");
    EXPECT_EQ (sanitiseCopilotQuestion (q), q);
}

TEST (CopilotModel, Validator)
{
    EXPECT_TRUE (isValidCopilotModel ("claude-opus-5-5"));
    EXPECT_TRUE (isValidCopilotModel ("meta/llama3.1:8b"));
    EXPECT_TRUE (isValidCopilotModel (juce::String::repeatedString ("a", 128)));
    EXPECT_FALSE (isValidCopilotModel (juce::String::repeatedString ("a", 129)));
    EXPECT_FALSE (isValidCopilotModel (""));
    EXPECT_FALSE (isValidCopilotModel ("a b"));
    EXPECT_FALSE (isValidCopilotModel ("a\"b"));
    EXPECT_FALSE (isValidCopilotModel ("a;b"));
    EXPECT_FALSE (isValidCopilotModel (utf8 ("m\xc3\xb4 h\xc3\xacnh")));
}

TEST (CopilotBaseUrl, Validator)
{
    EXPECT_TRUE (isValidCopilotBaseUrl ("https://api.openai.com/v1"));
    EXPECT_TRUE (isValidCopilotBaseUrl ("http://localhost:11434/v1"));
    EXPECT_FALSE (isValidCopilotBaseUrl (""));
    EXPECT_FALSE (isValidCopilotBaseUrl ("ftp://host/v1"));
    EXPECT_FALSE (isValidCopilotBaseUrl ("https://a b"));
    EXPECT_FALSE (isValidCopilotBaseUrl ("https://a\"b"));
    EXPECT_FALSE (isValidCopilotBaseUrl ("https://a'b"));
    EXPECT_FALSE (isValidCopilotBaseUrl ("https://a\nb"));
    EXPECT_FALSE (isValidCopilotBaseUrl ("https://" + juce::String::repeatedString ("a", 505)));
    EXPECT_TRUE (isValidCopilotBaseUrl ("https://" + juce::String::repeatedString ("a", 504)));
}

TEST (CopilotArguments, FindingsModeIsUnchanged)
{
    CopilotRequest r;
    r.executable = juce::File ("C:/x/HandsFreeCopilot.exe");
    r.sessionLog = juce::File ("C:/x/session.jsonl");
    // Ask-only fields are ignored when there is no question.
    r.provider = "openai_compatible";
    r.model = "m";
    r.baseUrl = "https://h";

    const auto a = buildCopilotArguments (r);
    ASSERT_EQ (a.size(), 4);
    EXPECT_EQ (a[0], r.executable.getFullPathName());
    EXPECT_EQ (a[1], r.sessionLog.getFullPathName());
    EXPECT_EQ (a[2], "--json");
    EXPECT_EQ (a[3], "--low-priority");
}

TEST (CopilotArguments, AnthropicAsk)
{
    auto r = askRequest();
    r.question = "  why \"ring\"?  ";
    const auto a = buildCopilotArguments (r);
    const juce::StringArray expected { r.executable.getFullPathName(), r.sessionLog.getFullPathName(),
                                       "--ask", "why 'ring'?", "--json", "--low-priority",
                                       "--provider", "anthropic", "--timeout", "100" };
    EXPECT_EQ (a, expected);

    r.model = "claude-opus-5-5";
    const auto b = buildCopilotArguments (r);
    EXPECT_EQ (b.joinIntoString ("|").fromFirstOccurrenceOf ("--provider", true, false),
               "--provider|anthropic|--model|claude-opus-5-5|--timeout|100");
}

TEST (CopilotArguments, OpenAiCompatibleAsk)
{
    auto r = askRequest();
    r.provider = "openai_compatible";
    r.model = "llama3";
    r.baseUrl = "http://localhost:11434/v1";
    const auto a = buildCopilotArguments (r);
    EXPECT_EQ (a.joinIntoString ("|").fromFirstOccurrenceOf ("--provider", true, false),
               "--provider|openai_compatible|--model|llama3|--base-url|http://localhost:11434/v1|--timeout|100");
}

TEST (CopilotArguments, EveryInvalidAskIsEmpty)
{
    auto base = askRequest();

    auto r = base; r.question = "  \t\r\n \\ ";           EXPECT_TRUE (buildCopilotArguments (r).isEmpty()) << "empty question";
    r = base; r.provider = "gemini";                       EXPECT_TRUE (buildCopilotArguments (r).isEmpty()) << "unknown provider";
    r = base; r.provider = "";                             EXPECT_TRUE (buildCopilotArguments (r).isEmpty()) << "no provider";
    r = base; r.model = "bad model";                       EXPECT_TRUE (buildCopilotArguments (r).isEmpty()) << "bad model";
    r = base; r.baseUrl = "https://x/v1";                  EXPECT_TRUE (buildCopilotArguments (r).isEmpty()) << "url for anthropic";

    r = base; r.provider = "openai_compatible";            EXPECT_TRUE (buildCopilotArguments (r).isEmpty()) << "no model, no url";
    r.model = "llama3";                                    EXPECT_TRUE (buildCopilotArguments (r).isEmpty()) << "no url";
    r.baseUrl = "ftp://x";                                 EXPECT_TRUE (buildCopilotArguments (r).isEmpty()) << "bad url";
    r.baseUrl = "http://localhost:11434/v1"; r.model = ""; EXPECT_TRUE (buildCopilotArguments (r).isEmpty()) << "no model";
}

//==============================================================================
namespace
{
class CannedBackend : public CopilotBackend
{
public:
    explicit CannedBackend (CopilotRunResult r) : result_ (std::move (r)) {}

    CopilotRunResult run (const CopilotRequest& req, CopilotCancelToken&) override
    {
        lastQuestion = req.question;
        return result_;
    }

    juce::String lastQuestion;

private:
    CopilotRunResult result_;
};

// Non-zero exit, but stdout still has to reach onDone (exits 3 and 4 carry JSON).
void expectDelivered (int exitCode, const char* json)
{
    CopilotRunResult canned;
    canned.status = CopilotRunResult::Status::failed;
    canned.exitCode = exitCode;
    canned.stdoutText = json;

    std::mutex m;
    std::deque<std::function<void()>> queue;
    SidecarRunner runner (std::make_unique<CannedBackend> (canned),
                          [&] (std::function<void()> fn) { std::lock_guard<std::mutex> l (m); queue.push_back (std::move (fn)); });

    bool delivered = false;
    CopilotRunResult got;
    auto req = askRequest();
    ASSERT_TRUE (runner.request (req, [&] (CopilotRunResult r) { delivered = true; got = std::move (r); }));

    for (int i = 0; i < 2500 && ! delivered; ++i)
    {
        std::deque<std::function<void()>> local;
        { std::lock_guard<std::mutex> l (m); local.swap (queue); }
        for (auto& f : local)
            f();
        if (! delivered)
            juce::Thread::sleep (2);
    }

    ASSERT_TRUE (delivered);
    EXPECT_EQ (got.status, CopilotRunResult::Status::failed);
    EXPECT_EQ (got.exitCode, exitCode);
    EXPECT_EQ (got.stdoutText, juce::String (json));
}
}

TEST (SidecarRunnerAsk, ExitFourStdoutReachesOnDone)
{
    expectDelivered (4, R"({"blocked": true, "message": "chan"})");
}

TEST (SidecarRunnerAsk, ExitThreeStdoutReachesOnDone)
{
    expectDelivered (3, R"({"error": "ket noi that bai"})");
}

TEST (SidecarRunnerAsk, InvalidAskIsRefusedWithoutAProcess)
{
    // The real backend: the executable exists (any file) but the request is
    // invalid, so it must return exit 5 and never launch it.
    auto req = askRequest();
    req.executable = juce::File::getSpecialLocation (juce::File::currentExecutableFile);
    req.provider = "nonsense";

    ChildProcessBackend backend;
    CopilotCancelToken token;
    const auto r = backend.run (req, token);
    EXPECT_EQ (r.status, CopilotRunResult::Status::failed);
    EXPECT_EQ (r.exitCode, kCopilotInvalidRequestExitCode);
    EXPECT_TRUE (r.stdoutText.isEmpty());
}
