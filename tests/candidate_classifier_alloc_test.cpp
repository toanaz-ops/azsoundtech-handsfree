// Lane C1a, condition 1 of Q5: CandidateClassifier::score() makes ZERO global
// operator new / delete calls once the model is loaded.
//
// This is its OWN executable on purpose. It replaces every global allocation
// function; doing that inside HandsFreeTests would override the allocator the
// ASan / TSan nightly jobs install and make those jobs meaningless.
//
// The counter counts only while a thread_local probe is armed, so gtest, JUCE
// and the loader (which allocate freely) never move it. The positive control
// proves the counter can see an allocation at all: a gate that cannot fail
// proves nothing.

#include <gtest/gtest.h>

#include "app/CandidateClassifier.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <new>
#include <vector>

namespace
{
struct Probe
{
    bool armed = false;
    std::size_t news = 0;
    std::size_t deletes = 0;
};
thread_local Probe gProbe;

void noteNew() noexcept { if (gProbe.armed) ++gProbe.news; }
void noteDelete() noexcept { if (gProbe.armed) ++gProbe.deletes; }

void* allocPlain (std::size_t n) noexcept
{
    noteNew();
    return std::malloc (n == 0 ? 1 : n);
}

void* allocAligned (std::size_t n, std::size_t align) noexcept
{
    noteNew();
    if (n == 0) n = 1;
#if defined (_MSC_VER)
    return _aligned_malloc (n, align);          // MSVC has no std::aligned_alloc
#else
    const std::size_t rounded = (n + align - 1) / align * align;   // aligned_alloc wants a multiple
    return std::aligned_alloc (align, rounded);
#endif
}

void freePlain (void* p) noexcept { noteDelete(); std::free (p); }

void freeAligned (void* p) noexcept
{
    noteDelete();
#if defined (_MSC_VER)
    _aligned_free (p);                          // must pair with _aligned_malloc
#else
    std::free (p);
#endif
}
} // namespace

// ---- every global allocation function (C++17), so no form slips past ----
void* operator new (std::size_t n) { if (void* p = allocPlain (n)) return p; throw std::bad_alloc(); }
void* operator new[] (std::size_t n) { if (void* p = allocPlain (n)) return p; throw std::bad_alloc(); }
void* operator new (std::size_t n, const std::nothrow_t&) noexcept { return allocPlain (n); }
void* operator new[] (std::size_t n, const std::nothrow_t&) noexcept { return allocPlain (n); }
void* operator new (std::size_t n, std::align_val_t a)
{ if (void* p = allocAligned (n, (std::size_t) a)) return p; throw std::bad_alloc(); }
void* operator new[] (std::size_t n, std::align_val_t a)
{ if (void* p = allocAligned (n, (std::size_t) a)) return p; throw std::bad_alloc(); }
void* operator new (std::size_t n, std::align_val_t a, const std::nothrow_t&) noexcept { return allocAligned (n, (std::size_t) a); }
void* operator new[] (std::size_t n, std::align_val_t a, const std::nothrow_t&) noexcept { return allocAligned (n, (std::size_t) a); }

void operator delete (void* p) noexcept { freePlain (p); }
void operator delete[] (void* p) noexcept { freePlain (p); }
void operator delete (void* p, std::size_t) noexcept { freePlain (p); }
void operator delete[] (void* p, std::size_t) noexcept { freePlain (p); }
void operator delete (void* p, const std::nothrow_t&) noexcept { freePlain (p); }
void operator delete[] (void* p, const std::nothrow_t&) noexcept { freePlain (p); }
void operator delete (void* p, std::align_val_t) noexcept { freeAligned (p); }
void operator delete[] (void* p, std::align_val_t) noexcept { freeAligned (p); }
void operator delete (void* p, std::size_t, std::align_val_t) noexcept { freeAligned (p); }
void operator delete[] (void* p, std::size_t, std::align_val_t) noexcept { freeAligned (p); }
void operator delete (void* p, std::align_val_t, const std::nothrow_t&) noexcept { freeAligned (p); }
void operator delete[] (void* p, std::align_val_t, const std::nothrow_t&) noexcept { freeAligned (p); }

namespace
{
using RawInput = CandidateClassifier::RawInput;

RawInput inputFor (int i)
{
    RawInput in;
    in.hz = 100.0f + (float) (i % 97) * 150.0f;
    in.peakiness = 5.0f + (float) (i % 13);
    in.pNorm = (float) (i % 10) / 10.0f;
    in.rNorm = (float) (i % 7) / 7.0f;
    in.riseRatio = 0.5f + (float) (i % 11);
    in.mNorm = (float) (i % 5) / 5.0f;
    in.penalty = (i % 2) ? 1.0f : 0.5f;
    in.hasOtherLane = (i % 3) != 0;
    in.lrRatio = 0.25f + (float) (i % 9);
    for (int k = 0; k < CandidateClassifier::kNearBins; ++k)
        in.nearMags[(std::size_t) k] = 1.0f / (1.0f + (float) std::abs (k - 5) * (float) (1 + i % 4));
    if (i % 50 == 0) in.nearMags[2] = std::numeric_limits<float>::quiet_NaN();   // no-opinion path allocates nothing either
    if (i % 70 == 0) in.nearMags.fill (1.0e-40f);                                  // denormal path
    return in;
}

} // namespace

TEST (CandidateClassifierAlloc, ScoreAllocatesNothingAfterLoad)
{
    CandidateClassifier c;
    juce::String error;
    const auto json = juce::File (juce::String (HANDSFREE_SOURCE_DIR))
                          .getChildFile ("tests/fixtures/classifier-fixture-v1.json").loadFileAsString();
    ASSERT_TRUE (c.loadFromJson (json, error)) << error;

    // Warm-up outside the probe: first-touch effects must not be blamed on score().
    for (int i = 0; i < 16; ++i)
        (void) c.score (inputFor (i));

    int opinions = 0;
    gProbe = Probe {};
    gProbe.armed = true;
    for (int i = 0; i < 1000; ++i)
        opinions += c.score (inputFor (i)).has_value() ? 1 : 0;
    gProbe.armed = false;

    std::printf ("[ C1a ] score() x1000: operator new calls = %zu, operator delete calls = %zu, opinions = %d\n",
                 gProbe.news, gProbe.deletes, opinions);
    EXPECT_EQ (gProbe.news, 0u);
    EXPECT_EQ (gProbe.deletes, 0u);
    EXPECT_GT (opinions, 900);   // the loop really scored; it did not just return early
}

TEST (CandidateClassifierAlloc, CounterSeesAVectorAllocation)
{
    gProbe = Probe {};
    gProbe.armed = true;
    {
        std::vector<int> v (100);
        static volatile int* volatile keep;
        keep = v.data();          // stop the compiler eliding the allocation
        (void) keep;
    }
    gProbe.armed = false;
    std::printf ("[ C1a ] positive control: operator new calls = %zu, operator delete calls = %zu\n",
                 gProbe.news, gProbe.deletes);
    EXPECT_GE (gProbe.news, 1u);
    EXPECT_GE (gProbe.deletes, 1u);
}
