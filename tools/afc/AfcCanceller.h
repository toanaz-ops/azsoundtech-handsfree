// AfcCanceller: adaptive feedback cancellation for ONE lane (lane A1, spec
// docs/superpowers/specs/2026-10-19-afc-design.md section 3).
//
// Partitioned-block frequency-domain adaptive filter (PBFDAF / MDF), overlap-
// save, with prediction-error-method (PEM) prewhitening in the learning branch
// only. Models the loudspeaker->room->mic path F at taps k in [P, P + L):
//
//     e(n) = y(n) - yhat(n),   yhat = F^ * u,   F^ has no taps below P.
//
// Per-sample protocol, on the audio thread:
//
//     const float e = afc.processSample (y);   // y already NaN-guarded
//     ... e -> notch chain -> v, u = clamp (duck * v) ...
//     afc.pushReference (u);                   // exactly once per processSample
//
// The partition counter lives in the object, NOT in the callback, so the
// result is identical whatever the host's block size (spec invariant 3). The
// whole yhat of a partition is computed when its first sample is due, from
// reference samples that already exist -- that is why no latency is added
// (spec 3.2).
//
// Real-time contract: every buffer is allocated in the constructor; process
// and push never allocate, lock or throw. juce::dsp::FFT's fallback engine
// works in place for N = 128 (alloca, no heap).
//
// The watchdog (spec 3.5, conditions 1-3) lives here because it needs the
// spectra this object already computes. What to DO about a trip -- crossfade,
// duck, rest, lockout -- is AfcSupervisor's job, not this class's.

#pragma once

#include <juce_dsp/juce_dsp.h>

#include <array>
#include <complex>
#include <vector>

class AfcCanceller
{
public:
    static constexpr int P = 64;            // partition length = minimum modelled lag
    static constexpr int N = 2 * P;         // FFT size
    static constexpr int Bins = N / 2 + 1;  // 65
    static constexpr int K = 128;           // partitions
    static constexpr int L = P * K;         // 8192 modelled taps

    static constexpr int kPemOrder = 32;
    static constexpr int kPemBlock = 1024;

    enum class Trip { None, NonFinite, Inflation, Coefficients };

    struct Params
    {
        float mu = 0.1f;                    // NLMS step, normalised over all K partitions
        float relativeFloor = 1.0e-4f;      // delta_k as a fraction of long-term bin power (I3)
        float excitationGate = 1.0e-4f;     // skip a bin 40 dB below the loudest bin's long-term level
        float frameGateRms = 3.16e-4f;      // skip a frame whose y RMS is below -70 dBFS
        bool  pem = true;
    };

    AfcCanceller();

    // Sample rate only sets the watchdog's time constants; the filter itself
    // is defined in samples. Message thread, before the stream (or after a
    // reset on the audio thread -- it does not allocate).
    void prepare (double sampleRate);
    void setParams (const Params& p) { params_ = p; }

    // Forget everything: filter, histories, PEM model, watchdog state.
    void reset() noexcept;

    // Learning on/off without losing the filter (lane M freeze uses reset;
    // this is for the excitation gates and tests).
    void setLearning (bool on) noexcept { learning_ = on; }

    float processSample (float y) noexcept;
    void  pushReference (float u) noexcept;

    // The latest hard-trip condition, cleared by the read. A tripped canceller
    // keeps running until the supervisor resets it; it does not police itself.
    Trip takeTrip() noexcept { const auto t = trip_; trip_ = Trip::None; return t; }

    // ---- inspection (tests, the simulation tool) ------------------------
    // The modelled impulse response, taps [P, P + L) in order (size L).
    void copyImpulseResponse (std::vector<float>& out) const;
    // Parseval energy of F^ (spec 3.5 condition 3).
    [[nodiscard]] double coefficientEnergy() const noexcept;
    [[nodiscard]] const std::array<float, kPemOrder + 1>& pemModel() const noexcept { return pemA_; }

private:
    using Cpx = std::complex<float>;

    void endPartition() noexcept;
    void forward (const float* timeN, Cpx* spectrum) noexcept;   // N reals -> Bins
    void inverse (const Cpx* spectrum, float* timeN) noexcept;   // Bins -> N reals (scaled)
    void whiten (const float* in, float* out, std::array<float, kPemOrder>& state) const noexcept;
    void updatePem() noexcept;
    void updateWatchdog() noexcept;
    [[nodiscard]] int ring (int partitionsAgo) const noexcept;  // index of X_{j - partitionsAgo}

    Params params_;
    bool   learning_ = true;
    double sampleRate_ = 48000.0;

    juce::dsp::FFT fft_ { 7 };
    std::vector<float> fftWork_;            // 2N floats

    // Filter and histories. Ring buffers hold K + 1 spectra so that the update
    // (lags 1..K) and the next prediction (lags 0..K-1) both find theirs.
    std::vector<Cpx> W_;                    // K * Bins
    std::vector<Cpx> X_;                    // (K + 1) * Bins, reference
    std::vector<Cpx> Xw_;                   // (K + 1) * Bins, whitened reference
    std::vector<double> xwPowerSum_;        // Bins: sum over the K spectra used by the update
    std::vector<double> longTermPower_;     // Bins: smoothed xwPowerSum_ / K
    std::vector<double> xPowerSum_;         // Bins: UNWHITENED reference power, same window
    std::vector<double> xLongTerm_;         // Bins: its smoothed per-partition mean
    double xLongTermMax_ = 0.0;             // loudest bin of xLongTerm_
    int head_ = 0;                          // ring index of the NEWEST spectrum
    int constrainNext_ = 0;

    // Time-domain partition buffers.
    std::array<float, N> uPair_ {};         // [u_{j-1}, u_j]
    std::array<float, N> uwPair_ {};        // whitened
    std::array<float, P> yBlock_ {}, eBlock_ {}, yhatBlock_ {};
    int pos_ = 0;
    bool primed_ = false;                   // a full partition of reference exists

    // PEM.
    std::array<float, kPemOrder + 1> pemA_ {};
    std::array<float, kPemOrder> uWhitenState_ {}, eWhitenState_ {};
    std::array<float, kPemBlock> pemBuffer_ {};
    std::array<float, kPemBlock> pemWindow_ {};   // Hann, built once in the constructor
    int pemFill_ = 0;

    // Watchdog (spec 3.5).
    std::vector<double> yPow_, ePow_;       // smoothed |Y_k|^2, |E_k|^2
    double watchAlpha_ = 1.0 / 75.0;
    int    inflationRun_ = 0;               // partitions in a row any bin was inflated
    int    inflationTrigger_ = 112;         // 150 ms at 48 kHz
    int    binLo_ = 1, binHi_ = 21;         // 100 Hz .. 8 kHz
    Trip   trip_ = Trip::None;

    JUCE_DECLARE_NON_COPYABLE (AfcCanceller)
};
