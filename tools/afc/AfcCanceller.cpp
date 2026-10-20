#include "AfcCanceller.h"

#include <algorithm>
#include <cmath>

namespace
{
    constexpr int kRing = AfcCanceller::K + 1;

    bool finite (float v) noexcept { return std::isfinite (v); }

    // Long-term bin power: ~2 s at 48 kHz (1500 partitions of 64).
    constexpr double kLongTermAlpha = 1.0 / 1500.0;

    // An inflated bin must also be above this smoothed |E_k|^2: a -80 dBFS
    // sine in a half-zero-padded 128-point frame. Below it, a +6 dB "rise" is
    // two quiet numbers disagreeing.
    constexpr double kInflationFloor = 1.0e-5;

    // Spec 3.5 condition 3: ||F^||_2 > 2.
    constexpr double kMaxCoefficientEnergy = 4.0;
}

AfcCanceller::AfcCanceller()
    : fftWork_ ((std::size_t) (2 * N), 0.0f),
      W_ ((std::size_t) (K * Bins)),
      X_ ((std::size_t) (kRing * Bins)),
      Xw_ ((std::size_t) (kRing * Bins)),
      xwPowerSum_ ((std::size_t) Bins, 0.0),
      longTermPower_ ((std::size_t) Bins, 0.0),
      xPowerSum_ ((std::size_t) Bins, 0.0),
      xLongTerm_ ((std::size_t) Bins, 0.0),
      yPow_ ((std::size_t) Bins, 0.0),
      ePow_ ((std::size_t) Bins, 0.0)
{
    for (int n = 0; n < kPemBlock; ++n)
        pemWindow_[(std::size_t) n] = (float) (0.5 - 0.5 * std::cos (juce::MathConstants<double>::twoPi
                                                                      * n / (kPemBlock - 1)));
    prepare (48000.0);
    reset();
}

void AfcCanceller::prepare (const double sampleRate)
{
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
    const double partitionsPerSecond = sampleRate_ / (double) P;

    watchAlpha_       = 1.0 / std::max (1.0, 0.100 * partitionsPerSecond);   // tau 100 ms
    inflationTrigger_ = std::max (1, (int) std::ceil (0.150 * partitionsPerSecond));

    const double binHz = sampleRate_ / (double) N;
    binLo_ = std::max (1, (int) std::floor (100.0 / binHz));
    binHi_ = std::min (Bins - 1, (int) std::ceil (8000.0 / binHz));
}

void AfcCanceller::reset() noexcept
{
    std::fill (W_.begin(), W_.end(), Cpx {});
    std::fill (X_.begin(), X_.end(), Cpx {});
    std::fill (Xw_.begin(), Xw_.end(), Cpx {});
    std::fill (xwPowerSum_.begin(), xwPowerSum_.end(), 0.0);
    std::fill (longTermPower_.begin(), longTermPower_.end(), 0.0);
    std::fill (xPowerSum_.begin(), xPowerSum_.end(), 0.0);
    std::fill (xLongTerm_.begin(), xLongTerm_.end(), 0.0);
    std::fill (yPow_.begin(), yPow_.end(), 0.0);
    std::fill (ePow_.begin(), ePow_.end(), 0.0);

    uPair_.fill (0.0f);
    uwPair_.fill (0.0f);
    yBlock_.fill (0.0f);
    eBlock_.fill (0.0f);
    yhatBlock_.fill (0.0f);

    pemA_.fill (0.0f);
    pemA_[0] = 1.0f;
    uWhitenState_.fill (0.0f);
    eWhitenState_.fill (0.0f);
    pemBuffer_.fill (0.0f);
    pemFill_ = 0;

    head_ = 0;
    constrainNext_ = 0;
    pos_ = 0;
    primed_ = false;
    inflationRun_ = 0;
    trip_ = Trip::None;
}

int AfcCanceller::ring (const int partitionsAgo) const noexcept
{
    return (head_ - partitionsAgo + kRing) % kRing;
}

float AfcCanceller::processSample (const float y) noexcept
{
    const float yhat = yhatBlock_[(std::size_t) pos_];
    float e = y - yhat;

    if (! finite (e))
    {
        // Spec 3.5 condition 1: back to y on THIS sample, never a crossfade
        // through a NaN.
        trip_ = Trip::NonFinite;
        e = finite (y) ? y : 0.0f;
    }

    yBlock_[(std::size_t) pos_] = y;
    eBlock_[(std::size_t) pos_] = e;
    return e;
}

void AfcCanceller::pushReference (float u) noexcept
{
    if (! finite (u))
        u = 0.0f;

    uPair_[(std::size_t) (P + pos_)] = u;

    if (++pos_ == P)
    {
        endPartition();
        pos_ = 0;
    }
}

void AfcCanceller::forward (const float* timeN, Cpx* spectrum) noexcept
{
    std::copy (timeN, timeN + N, fftWork_.begin());
    std::fill (fftWork_.begin() + N, fftWork_.end(), 0.0f);
    fft_.performRealOnlyForwardTransform (fftWork_.data(), true);

    for (int k = 0; k < Bins; ++k)
        spectrum[k] = { fftWork_[(std::size_t) (2 * k)], fftWork_[(std::size_t) (2 * k + 1)] };
}

void AfcCanceller::inverse (const Cpx* spectrum, float* timeN) noexcept
{
    std::fill (fftWork_.begin(), fftWork_.end(), 0.0f);
    for (int k = 0; k < Bins; ++k)
    {
        fftWork_[(std::size_t) (2 * k)]     = spectrum[k].real();
        fftWork_[(std::size_t) (2 * k + 1)] = spectrum[k].imag();
    }

    // JUCE's real inverse already scales by 1/N (pinned by a test).
    fft_.performRealOnlyInverseTransform (fftWork_.data());
    std::copy (fftWork_.begin(), fftWork_.begin() + N, timeN);
}

void AfcCanceller::whiten (const float* in, float* out,
                           std::array<float, kPemOrder>& state) const noexcept
{
    // A(z) FIR; state holds the last kPemOrder inputs, newest first.
    for (int n = 0; n < P; ++n)
    {
        float acc = pemA_[0] * in[n];
        for (int i = 1; i <= kPemOrder; ++i)
            acc += pemA_[(std::size_t) i] * state[(std::size_t) (i - 1)];

        for (int i = kPemOrder - 1; i > 0; --i)
            state[(std::size_t) i] = state[(std::size_t) (i - 1)];
        state[0] = in[n];

        out[n] = acc;
    }
}

void AfcCanceller::endPartition() noexcept
{
    std::array<Cpx, Bins> spec {};

    //----------------------------------------------------------------------
    // 1. The reference of the partition that just ended becomes X_j.
    if (params_.pem)
        whiten (uPair_.data() + P, uwPair_.data() + P, uWhitenState_);
    else
        std::copy (uPair_.begin() + P, uPair_.end(), uwPair_.begin() + P);

    head_ = (head_ + 1) % kRing;
    Cpx* xNew  = &X_ [(std::size_t) (head_ * Bins)];
    Cpx* xwNew = &Xw_[(std::size_t) (head_ * Bins)];
    forward (uPair_.data(),  xNew);
    forward (uwPair_.data(), xwNew);

    // Prediction-window power: X'_{j-p}, p = 0..K-1. The spectrum that fell
    // out of it is the oldest one, now at ring(K).
    const Cpx* xwOut = &Xw_[(std::size_t) (ring (K) * Bins)];
    const Cpx* xOut  = &X_ [(std::size_t) (ring (K) * Bins)];
    for (int k = 0; k < Bins; ++k)
    {
        xwPowerSum_[(std::size_t) k] = std::max (0.0, xwPowerSum_[(std::size_t) k]
                                                     + (double) std::norm (xwNew[k])
                                                     - (double) std::norm (xwOut[k]));
        xPowerSum_[(std::size_t) k]  = std::max (0.0, xPowerSum_[(std::size_t) k]
                                                     + (double) std::norm (xNew[k])
                                                     - (double) std::norm (xOut[k]));
    }

    //----------------------------------------------------------------------
    // 2. Learn from e_j against the spectra that PREDICTED it: X'_{j-1-p},
    //    i.e. ring(p + 1). Their power sum is the window above shifted by one.
    double frameEnergy = 0.0;
    for (float v : yBlock_)
        frameEnergy += (double) v * v;
    const bool frameExcited = frameEnergy >= (double) P * params_.frameGateRms * params_.frameGateRms;

    if (learning_ && primed_ && frameExcited)
    {
        std::array<float, N> ePad {};
        std::array<float, P> ew {};
        if (params_.pem)
            whiten (eBlock_.data(), ew.data(), eWhitenState_);
        else
            std::copy (eBlock_.begin(), eBlock_.end(), ew.begin());
        std::copy (ew.begin(), ew.end(), ePad.begin() + P);
        forward (ePad.data(), spec.data());

        const Cpx* xwOldest = &Xw_[(std::size_t) (ring (K) * Bins)];
        xLongTermMax_ = 0.0;
        for (int k = 0; k < Bins; ++k)
            xLongTermMax_ = std::max (xLongTermMax_, xLongTerm_[(std::size_t) k]);
        for (int k = 0; k < Bins; ++k)
        {
            // Window for the update: ring(1..K) = prediction window - newest + oldest.
            const double sum = std::max (0.0, xwPowerSum_[(std::size_t) k]
                                                - (double) std::norm (xwNew[k])
                                                + (double) std::norm (xwOldest[k]));
            const double perPartition = sum / (double) K;
            auto& lt = longTermPower_[(std::size_t) k];
            lt += kLongTermAlpha * (perPartition - lt);

            // I3: a bin the reference does not excite (a notch hole, silence,
            // a band the programme never reaches) teaches nothing -- and the
            // whitened power cannot say so, because whitening flattens
            // exactly those bins. The gate is on the UNWHITENED reference:
            // below -40 dB of the loudest bin's long-term level, no update.
            const double xNow = xPowerSum_[(std::size_t) k] / (double) K;
            auto& xlt = xLongTerm_[(std::size_t) k];
            xlt += kLongTermAlpha * (xNow - xlt);
            if (xNow < (double) params_.excitationGate * xLongTermMax_)
                continue;

            const double delta = (double) params_.relativeFloor * lt * (double) K + 1.0e-12;
            const Cpx g = spec[(std::size_t) k] * (float) ((double) params_.mu / (sum + delta));

            for (int p = 0; p < K; ++p)
            {
                const Cpx x = Xw_[(std::size_t) (ring (p + 1) * Bins + k)];
                W_[(std::size_t) (p * Bins + k)] += g * std::conj (x);
            }
        }

        // Rotating gradient constraint: one partition per update.
        std::array<float, N> w {};
        Cpx* wp = &W_[(std::size_t) (constrainNext_ * Bins)];
        inverse (wp, w.data());
        std::fill (w.begin() + P, w.end(), 0.0f);
        forward (w.data(), wp);
        constrainNext_ = (constrainNext_ + 1) % K;
    }

    //----------------------------------------------------------------------
    // 3. Watchdog on the partition that just ended (spec 3.5).
    updateWatchdog();

    //----------------------------------------------------------------------
    // 4. PEM model from e (PEM-AFROW): every kPemBlock samples.
    if (params_.pem)
    {
        std::copy (eBlock_.begin(), eBlock_.end(), pemBuffer_.begin() + pemFill_);
        pemFill_ += P;
        if (pemFill_ >= kPemBlock)
        {
            updatePem();
            pemFill_ = 0;
        }
    }

    //----------------------------------------------------------------------
    // 5. Predict partition j + 1 from X_{j-p} = ring(p), p = 0..K-1.
    std::fill (spec.begin(), spec.end(), Cpx {});
    for (int p = 0; p < K; ++p)
    {
        const Cpx* wp = &W_[(std::size_t) (p * Bins)];
        const Cpx* xp = &X_[(std::size_t) (ring (p) * Bins)];
        for (int k = 0; k < Bins; ++k)
            spec[(std::size_t) k] += wp[k] * xp[k];
    }

    std::array<float, N> yhat {};
    inverse (spec.data(), yhat.data());
    for (int n = 0; n < P; ++n)
    {
        const float v = yhat[(std::size_t) (P + n)];
        if (! finite (v))
        {
            trip_ = Trip::NonFinite;
            yhatBlock_.fill (0.0f);
            break;
        }
        yhatBlock_[(std::size_t) n] = v;
    }

    // Slide the reference pairs: the partition that ended becomes "previous".
    std::copy (uPair_.begin() + P, uPair_.end(), uPair_.begin());
    std::copy (uwPair_.begin() + P, uwPair_.end(), uwPair_.begin());

    primed_ = true;
}

void AfcCanceller::updateWatchdog() noexcept
{
    if (trip_ != Trip::None)
        return;

    std::array<float, N> pad {};
    std::array<Cpx, Bins> Y {}, E {};

    std::copy (yBlock_.begin(), yBlock_.end(), pad.begin() + P);
    forward (pad.data(), Y.data());
    std::copy (eBlock_.begin(), eBlock_.end(), pad.begin() + P);
    forward (pad.data(), E.data());

    bool inflated = false;
    for (int k = 0; k < Bins; ++k)
    {
        auto& yp = yPow_[(std::size_t) k];
        auto& ep = ePow_[(std::size_t) k];
        yp += watchAlpha_ * ((double) std::norm (Y[(std::size_t) k]) - yp);
        ep += watchAlpha_ * ((double) std::norm (E[(std::size_t) k]) - ep);

        if (k >= binLo_ && k <= binHi_ && ep > kInflationFloor && ep > 4.0 * yp)
            inflated = true;
    }

    inflationRun_ = inflated ? inflationRun_ + 1 : 0;
    if (inflationRun_ >= inflationTrigger_)
    {
        trip_ = Trip::Inflation;
        return;
    }

    const double energy = coefficientEnergy();
    if (! std::isfinite (energy))
        trip_ = Trip::NonFinite;
    else if (energy > kMaxCoefficientEnergy)
        trip_ = Trip::Coefficients;
}

void AfcCanceller::updatePem() noexcept
{
    // Autocorrelation of the Hann-windowed block.
    std::array<float, kPemBlock> xw {};
    for (int n = 0; n < kPemBlock; ++n)
        xw[(std::size_t) n] = pemBuffer_[(std::size_t) n] * pemWindow_[(std::size_t) n];

    std::array<double, kPemOrder + 1> r {};
    for (int lag = 0; lag <= kPemOrder; ++lag)
    {
        double acc = 0.0;
        for (int n = lag; n < kPemBlock; ++n)
            acc += (double) xw[(std::size_t) n] * xw[(std::size_t) (n - lag)];
        r[(std::size_t) lag] = acc;
    }

    if (! (r[0] > 1.0e-12) || ! std::isfinite (r[0]))
        return;

    r[0] *= 1.0001;   // white-noise correction: keeps the recursion well away from |k| = 1

    // Levinson-Durbin. Accept only if every reflection coefficient is inside
    // the unit circle and every coefficient is finite (spec 3.4).
    std::array<double, kPemOrder + 1> a {};
    a[0] = 1.0;
    double err = r[0];

    for (int i = 1; i <= kPemOrder; ++i)
    {
        double acc = r[(std::size_t) i];
        for (int j = 1; j < i; ++j)
            acc += a[(std::size_t) j] * r[(std::size_t) (i - j)];

        const double k = -acc / err;
        if (! std::isfinite (k) || std::abs (k) >= 1.0)
            return;

        std::array<double, kPemOrder + 1> prev = a;
        for (int j = 1; j < i; ++j)
            a[(std::size_t) j] = prev[(std::size_t) j] + k * prev[(std::size_t) (i - j)];
        a[(std::size_t) i] = k;

        err *= (1.0 - k * k);
        if (! (err > 0.0))
            return;
    }

    for (int i = 0; i <= kPemOrder; ++i)
        if (! std::isfinite (a[(std::size_t) i]))
            return;

    for (int i = 0; i <= kPemOrder; ++i)
        pemA_[(std::size_t) i] = (float) a[(std::size_t) i];
}

double AfcCanceller::coefficientEnergy() const noexcept
{
    // Parseval for a real N-point signal from its non-negative bins:
    // sum |w|^2 = (|W_0|^2 + |W_{N/2}|^2 + 2 sum_{0<k<N/2} |W_k|^2) / N.
    double total = 0.0;
    for (int p = 0; p < K; ++p)
    {
        const Cpx* wp = &W_[(std::size_t) (p * Bins)];
        double s = (double) std::norm (wp[0]) + (double) std::norm (wp[Bins - 1]);
        for (int k = 1; k < Bins - 1; ++k)
            s += 2.0 * (double) std::norm (wp[k]);
        total += s / (double) N;
    }
    return total;
}

void AfcCanceller::copyImpulseResponse (std::vector<float>& out) const
{
    out.assign ((std::size_t) L, 0.0f);

    // Needs a scratch FFT; this is an inspection path, never the audio thread.
    juce::dsp::FFT fft (7);
    std::vector<float> work ((std::size_t) (2 * N), 0.0f);

    for (int p = 0; p < K; ++p)
    {
        std::fill (work.begin(), work.end(), 0.0f);
        const Cpx* wp = &W_[(std::size_t) (p * Bins)];
        for (int k = 0; k < Bins; ++k)
        {
            work[(std::size_t) (2 * k)]     = wp[k].real();
            work[(std::size_t) (2 * k + 1)] = wp[k].imag();
        }
        fft.performRealOnlyInverseTransform (work.data());
        std::copy (work.begin(), work.begin() + P, out.begin() + p * P);
    }
}
