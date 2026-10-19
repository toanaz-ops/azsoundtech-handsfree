// AfcSim -- closed-loop simulation of lane A1's canceller (spec 2026-10-19
// section 7). Not shipped. Sweeps step size and loop gain, prints the maximum
// stable gain with and without AFC. Usage:
//   AfcSim [mu=0.1] [pitch=1] [seed=1]

#include "AfcCanceller.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <random>
#include <vector>

namespace
{
constexpr double kFs = 48000.0;
constexpr double kPi = 3.14159265358979323846;   // M_PI is not standard; MSVC lacks it
bool gPem = true;
double gShiftHz = 0.0;

// Frequency shifter: allpass-pair Hilbert (Niemitalo), no added latency.
struct Shifter
{
    struct AP { double a2; double x1 = 0, x2 = 0, y1 = 0, y2 = 0;
        double run (double x) { const double y = a2 * (x + y2) - x2; x2 = x1; x1 = x; y2 = y1; y1 = y; return y; } };
    AP i[4] { {0.6923878*0.6923878}, {0.9360654322959*0.9360654322959}, {0.9882295226860*0.9882295226860}, {0.9987488452737*0.9987488452737} };
    AP q[4] { {0.4021921162426*0.4021921162426}, {0.8561710882420*0.8561710882420}, {0.9722909545651*0.9722909545651}, {0.9952884791278*0.9952884791278} };
    double qd = 0, ph = 0;
    double run (double x, double hz)
    {
        double a = x, b = x;
        for (auto& s : i) a = s.run (a);
        for (auto& s : q) b = s.run (b);
        const double bq = qd; qd = b;            // one-sample delay on one branch
        ph += 2 * kPi * hz / kFs; if (ph > 2 * kPi) ph -= 2 * kPi;
        return a * std::cos (ph) - bq * std::sin (ph);
    }
};

struct Biquad2 { double a1, a2, z1 = 0, z2 = 0;
    double run (double x) { const double y = x - a1 * z1 - a2 * z2; z2 = z1; z1 = y; return y; } };

std::vector<float> makeSource (int n, int mode, unsigned seed)
{
    std::mt19937 rng (seed);
    std::normal_distribution<double> g (0.0, 1.0);
    std::uniform_real_distribution<double> uni (0.0, 6.28);

    std::vector<Biquad2> formants;
    for (auto [f, bw] : { std::pair { 500.0, 80.0 }, { 1500.0, 120.0 }, { 2500.0, 200.0 } })
    {
        const double r = std::exp (-kPi * bw / kFs), th = 2 * kPi * f / kFs;
        formants.push_back ({ -2 * r * std::cos (th), r * r });
    }

    std::vector<float> s ((size_t) n);
    double ph = 0, phase0 = uni (rng);
    double sumsq = 0;
    for (int i = 0; i < n; ++i)
    {
        const double t = i / kFs;
        double v = g (rng);
        if (mode != 2 && mode != 4) { for (auto& f : formants) v = f.run (v); v *= 0.02; }
        const double env = (mode == 3) ? 1.0 : 0.6 + 0.4 * std::pow (std::sin (2 * kPi * 3.7 * t), 2.0);
        double p = 0;
        if (mode == 5)
        {
            p = 0;
            for (double f : { 261.6, 329.6, 392.0, 523.3 })
                for (int k = 1; k < 5; ++k) p += std::sin (2 * kPi * f * k * t) / (k * k);
            v *= 0.05;
        }
        if (mode == 1)
        {
            const double f0 = 140 + 30 * std::sin (2 * kPi * 0.3 * t + phase0) + 10 * std::sin (2 * kPi * 5.5 * t);
            ph += 2 * kPi * f0 / kFs;
            for (int k = 1; k < 8; ++k) p += std::sin (k * ph) / k;
        }
        s[(size_t) i] = (float) (mode == 5 ? (v + 0.3 * p) * env : (0.7 * v + 0.3 * p * 0.3) * env);
        sumsq += (double) s[(size_t) i] * s[(size_t) i];
    }
    const double scale = 0.05 / std::sqrt (sumsq / n);
    for (auto& x : s) x = (float) (x * scale);
    return s;
}

std::vector<float> makeRoom (int len, int delay, double t60, unsigned seed)
{
    std::mt19937 rng (seed + 99);
    std::normal_distribution<double> g (0.0, 1.0);
    std::vector<float> h ((size_t) len, 0.0f);
    double e = 0;
    for (int i = delay; i < len; ++i)
    {
        h[(size_t) i] = (float) (g (rng) * std::exp (-6.9 * (i - delay) / (t60 * kFs)));
        e += (double) h[(size_t) i] * h[(size_t) i];
    }
    for (auto& x : h) x = (float) (x / std::sqrt (e));
    return h;
}

double maxMagnitude (const std::vector<float>& h)
{
    double best = 0;
    for (int b = 1; b < 4096; ++b)
    {
        const double w = kPi * b / 4096.0;
        double re = 0, im = 0;
        for (size_t i = 0; i < h.size(); ++i) { re += h[i] * std::cos (w * i); im -= h[i] * std::sin (w * i); }
        best = std::max (best, std::sqrt (re * re + im * im));
    }
    return best;
}

// One closed-loop run. Gain schedule: learn at -3 dB rel MSG for learnSec,
// ramp to targetDb over 5 s, hold holdSec. Returns true if stable at the end.
bool runLoop (const std::vector<float>& s, const std::vector<float>& h, double msg,
              bool afcOn, float mu, double targetDb, double learnSec, double holdSec,
              double* misalignDb, double* eSnrDb = nullptr, double* learnSnrDb = nullptr)
{
    AfcCanceller afc;
    AfcCanceller::Params p; p.mu = mu; p.pem = gPem;
    afc.setParams (p);
    afc.prepare (kFs);

    const int hl = (int) h.size();
    int first = 0; while (first < hl && h[(size_t) first] == 0.0f) ++first;

    const int n = (int) s.size();
    std::vector<float> u ((size_t) n + (size_t) hl, 0.0f);
    const int off = hl;

    const int learnN = (int) (learnSec * kFs), rampN = (int) (5.0 * kFs), holdN = (int) (holdSec * kFs);
    const int total = std::min (n, learnN + rampN + holdN);
    double tailSq = 0; int tailN = 0, clipped = 0; double sErr = 0, sPow = 0;
    Shifter shifter;
    double lErr = 0, lPow = 0;

    for (int i = 0; i < total; ++i)
    {
        double gDb = -3.0;
        if (i >= learnN) gDb = (i < learnN + rampN) ? -3.0 + (targetDb + 3.0) * (i - learnN) / (double) rampN : targetDb;
        const double G = msg * std::pow (10.0, gDb / 20.0);

        double fb = 0;
        for (int k = first; k < hl; ++k) fb += (double) h[(size_t) k] * u[(size_t) (off + i - k)];
        const float y = (float) (s[(size_t) i] + fb);
        const float e = afcOn ? afc.processSample (y) : y;
        float out = (float) (G * (gShiftHz > 0 ? shifter.run (e, gShiftHz) : (double) e));
        if (std::abs (out) >= 1.0f) { out = std::copysign (1.0f, out); ++clipped; }
        u[(size_t) (off + i)] = out;
        if (afcOn) afc.pushReference (out);
        if (i >= (int) (2 * kFs) && i < (int) (12 * kFs)) { lPow += (double) s[(size_t) i] * s[(size_t) i]; lErr += (double) (e - s[(size_t) i]) * (e - s[(size_t) i]); }
        if (i >= total - (int) (2 * kFs)) { tailSq += (double) out * out; ++tailN;
            sPow += (double) s[(size_t) i] * s[(size_t) i]; sErr += (double) (e - s[(size_t) i]) * (e - s[(size_t) i]); }
    }

    if (misalignDb && afcOn)
    {
        std::vector<float> w; afc.copyImpulseResponse (w);
        double num = 0, den = 0;
        for (int k = 0; k < AfcCanceller::L; ++k)
        {
            const int tap = AfcCanceller::P + k;
            const double ht = tap < hl ? h[(size_t) tap] : 0.0;
            num += (w[(size_t) k] - ht) * (w[(size_t) k] - ht); den += ht * ht;
        }
        *misalignDb = 10 * std::log10 (num / den);

        // Per-band error: |F^ - F|^2 / |F|^2 in octave-ish bands, and source power.
        auto dft = [] (const std::vector<double>& x, double f) {
            double re = 0, im = 0; const double w = 2 * kPi * f / kFs;
            for (size_t i = 0; i < x.size(); ++i) { re += x[i] * std::cos (w * i); im -= x[i] * std::sin (w * i); }
            return re * re + im * im; };
        std::vector<double> diff ((size_t) (AfcCanceller::L + 64), 0.0), tru ((size_t) (AfcCanceller::L + 64), 0.0);
        for (int k = 0; k < AfcCanceller::L; ++k) { const int tap = 64 + k; const double ht = tap < hl ? h[(size_t) tap] : 0.0;
            diff[(size_t) tap] = w[(size_t) k] - ht; tru[(size_t) tap] = ht; }
        for (double f : { 150.0, 300.0, 600.0, 1200.0, 2400.0, 4800.0, 9600.0, 16000.0 })
        {
            double en = 0, ed = 0;
            for (double df = -0.1; df <= 0.1; df += 0.05) { en += dft (diff, f * (1 + df)); ed += dft (tru, f * (1 + df)); }
            std::vector<double> sv (s.begin() + (long) (total - 48000), s.begin() + (long) total);
            std::printf ("      band %6.0f Hz: err %+6.1f dB   src %6.1f dB\n", f, 10 * std::log10 (en / ed), 10 * std::log10 (dft (sv, f) + 1e-30));
        }
    }

    if (eSnrDb) *eSnrDb = 10 * std::log10 (sPow / std::max (1e-30, sErr));
    if (learnSnrDb) *learnSnrDb = 10 * std::log10 (lPow / std::max (1e-30, lErr));
    const double rms = std::sqrt (tailSq / std::max (1, tailN));
    return rms < 0.3 && clipped < (int) (0.001 * total);
}
}

int main (int argc, char** argv)
{
    const float mu   = argc > 1 ? (float) std::atof (argv[1]) : 0.1f;
    const int pitch = argc > 2 ? std::atoi (argv[2]) : 1;   // 0 formant noise, 1 + pitch, 2 white
    const bool pemOn = argc > 6 ? std::atoi (argv[6]) != 0 : true;
    gPem = pemOn;
    gShiftHz = argc > 7 ? std::atof (argv[7]) : 0.0;
    const unsigned seed = argc > 3 ? (unsigned) std::atoi (argv[3]) : 1u;
    const double learn = argc > 4 ? std::atof (argv[4]) : 20.0;

    const auto h = makeRoom (300 + (int) (0.06 * kFs), 300, 0.06, seed);

    if (argc > 5 && std::string (argv[5]) == "open")   // open-loop identification check: u white, y = h * u
    {
        for (int pemOn = 0; pemOn < 2; ++pemOn)
        {
        AfcCanceller afc; AfcCanceller::Params p; p.mu = mu; p.pem = pemOn != 0; afc.setParams (p);
        std::mt19937 rng (7); std::normal_distribution<double> g (0, 0.1);
        const int n = (int) (5 * kFs), hl = (int) h.size();
        std::vector<float> u ((size_t) (n + hl), 0.0f);
        for (int i = 0; i < n; ++i)
        {
            const float ui = (float) g (rng);
            double y = 0; for (int k = 300; k < hl; ++k) y += h[(size_t) k] * u[(size_t) (hl + i - k)];
            afc.processSample ((float) y); u[(size_t) (hl + i)] = ui; afc.pushReference (ui);
        }
        std::vector<float> w; afc.copyImpulseResponse (w);
        double num = 0, den = 0;
        for (int k = 0; k < AfcCanceller::L; ++k) { const int tap = 64 + k; const double ht = tap < hl ? h[(size_t) tap] : 0.0;
            num += (w[(size_t) k] - ht) * (w[(size_t) k] - ht); den += ht * ht; }
        std::printf ("open loop, pem=%d: misalignment %.1f dB\n", pemOn, 10 * std::log10 (num / den));
        }
        return 0;
    }
    const double msg = 1.0 / maxMagnitude (h);
    const auto s = makeSource ((int) ((learn + 20.0) * kFs), pitch, seed);

    double mis = 0;
    runLoop (s, h, msg, true, mu, -3.0, learn, 2.0, &mis);
    std::printf ("shift=%.0fHz pem=%d mu=%.3f src=%d seed=%u: misalignment after %.0f s at -3 dB: %.1f dB\n", gShiftHz, (int) gPem, mu, pitch, seed, learn, mis);

    for (bool afc : { false, true })
    {
        double best = -99;
        for (double t = -3; t <= 15; t += 2.0)
        {
            double snr = 0, lsnr = 0;
            const bool ok = runLoop (s, h, msg, afc, mu, t, learn, 10.0, nullptr, &snr, &lsnr);
            std::printf ("    %s %+3.0f dB: %s  e-vs-s SNR %.1f dB (learning 2-12 s: %.1f dB)\n", afc ? "on " : "off", t, ok ? "stable  " : "UNSTABLE", snr, lsnr);
            if (ok) best = t; else break;
        }
        std::printf ("  %s: max stable gain = %+.0f dB rel. MSG\n", afc ? "AFC on " : "AFC off", best);
    }
    return 0;
}
