// Tracks per-baud PONG_ACK counts across a sweep;
// picks the index with the highest score subject to a
// minimum-hit floor.
#pragma once
#include <stdint.h>
#include <stddef.h>

#ifndef UTIL_BAUD_SWEEP_MAX_BAUDS
#    define UTIL_BAUD_SWEEP_MAX_BAUDS 16
#endif

namespace autolink
{
class UtilBaudSweep
{
public:
    struct Config {
        int pingSamplesPerBaud = 4;
        float minAcceptRate = 0.5f;
        int expectedSamples = -1;
    };

    explicit UtilBaudSweep(int numBauds) : numBauds_(0)
    {
        if (numBauds < 0)
            numBauds = 0;
        if (numBauds > UTIL_BAUD_SWEEP_MAX_BAUDS)
            numBauds = UTIL_BAUD_SWEEP_MAX_BAUDS;
        numBauds_ = numBauds;
        for (int i = 0; i < UTIL_BAUD_SWEEP_MAX_BAUDS;
             i++)
            scores_[i] = 0;
    }

    void configure(const Config &c);

    void score(int idx)
    {
        if (idx >= 0 && idx < numBauds_)
            scores_[idx]++;
    }
    void resetIndex(int idx)
    {
        if (idx >= 0 && idx < numBauds_)
            scores_[idx] = 0;
    }
    void resetAll();


    int pickBest() const;

    int samplesPerBaud() const
    {
        return cfg_.pingSamplesPerBaud;
    }

    int minHitsForReliable() const
    {
        int m = (int)(cfg_.minAcceptRate *
                      (float)cfg_.expectedSamples);
        return m < 1 ? 1 : m;
    }

    int scoreAt(int idx) const
    {
        return (idx >= 0 && idx < numBauds_)
            ? scores_[idx]
            : 0;
    }
    int numBauds() const { return numBauds_; }

private:
    int scores_[UTIL_BAUD_SWEEP_MAX_BAUDS];
    int numBauds_;
    Config cfg_;
};

} // namespace autolink