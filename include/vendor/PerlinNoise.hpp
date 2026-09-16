// Minimal 1D Perlin noise — public-domain implementation inlined here
// so we don't take a vcpkg dependency on the full siv::PerlinNoise.
// Output range is approximately [-1, 1] but not strictly bounded. Sufficient
// for camera-noise synthesis where exact bounds don't matter.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <random>

namespace dietdrcamera
{
    class Perlin1D
    {
    public:
        explicit Perlin1D(std::uint32_t seed = 0)
        {
            std::mt19937 rng(seed);
            for (int i = 0; i < 256; ++i) perm[i] = static_cast<std::uint8_t>(i);
            std::shuffle(perm, perm + 256, rng);
            for (int i = 0; i < 256; ++i) perm[i + 256] = perm[i];
        }

        double Sample(double x) const
        {
            const int    xi = static_cast<int>(std::floor(x)) & 255;
            const double xf = x - std::floor(x);
            const double u  = Fade(xf);
            const double g1 = Grad(perm[xi],     xf);
            const double g2 = Grad(perm[xi + 1], xf - 1.0);
            return Lerp(u, g1, g2) * 2.0;
        }

        // Fractal / multi-octave 1D. Useful for richer handheld-camera feel.
        // 3 octaves with persistence 0.5 gives slow-drift + gait + tremor.
        double SampleFractal(double x, int octaves = 3, double persistence = 0.5, double lacunarity = 2.0) const
        {
            double result = 0.0;
            double amp    = 1.0;
            double freq   = 1.0;
            double total  = 0.0;
            for (int i = 0; i < octaves; ++i) {
                result += Sample(x * freq) * amp;
                total  += amp;
                amp    *= persistence;
                freq   *= lacunarity;
            }
            return total > 0.0 ? result / total : 0.0;
        }

    private:
        static double Fade(double t) { return t * t * t * (t * (t * 6.0 - 15.0) + 10.0); }
        static double Lerp(double t, double a, double b) { return a + t * (b - a); }
        static double Grad(int hash, double x) { return (hash & 1) ? x : -x; }

        std::uint8_t perm[512]{};
    };
}
