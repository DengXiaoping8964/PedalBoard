// Toolchain probe: proves cl.exe, the Windows SDK headers/libs and the static
// runtime all work together before PedalBoard itself is configured.
//
// Deliberately uses a Win32 API (QueryPerformanceCounter) so a missing or
// mismatched SDK shows up here as a link error rather than as a confusing
// failure halfway through the JUCE build.

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

int main()
{
    static_assert (sizeof (void*) == 8, "expected an x64 toolchain");

    LARGE_INTEGER freq{}, t0{}, t1{};
    QueryPerformanceFrequency (&freq);
    QueryPerformanceCounter (&t0);

    // Touch the pieces the audio thread relies on: atomics without locks, a
    // heap container allocated up front, and floating point math.
    std::atomic<float> gain{0.5f};
    std::vector<float> buffer (512, 0.0f);

    for (std::size_t i = 0; i < buffer.size(); ++i)
        buffer[i] = std::sin (static_cast<float> (i) * 0.01f) * gain.load();

    double sum = 0.0;
    QueryPerformanceCounter (&t1);

    for (const auto sample : buffer)
        sum += std::fabs (sample);

    const double us = (t1.QuadPart - t0.QuadPart) * 1e6 / static_cast<double> (freq.QuadPart);

    std::printf ("PedalBoard toolchain probe\n");
    std::printf ("  MSVC _MSC_VER: %d\n", _MSC_VER);
    std::printf ("  std          : C++%ld\n", __cplusplus / 100L);
    std::printf ("  channels     : %zu\n", buffer.size());
    std::printf ("  |sum|        : %.6f\n", sum);
    std::printf ("  QPC tick/us  : %.1f\n", static_cast<double> (freq.QuadPart) / 1e6);
    std::printf ("  work time    : %.3f us\n", us);

    // A non-zero sum means the math actually executed on a real x64 build.
    return sum > 0.0 ? 0 : 1;
}
