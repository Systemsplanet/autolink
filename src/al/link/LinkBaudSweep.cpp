// Per-baud PONG_ACK accumulator.
#include "al/link/LinkBaudSweep.h"

namespace autolink
{
void UtilBaudSweep::configure(const Config &c)
{
    cfg_ = c;
    if (cfg_.expectedSamples < 0)
        cfg_.expectedSamples = cfg_.pingSamplesPerBaud;
    if (cfg_.expectedSamples < 1)
        cfg_.expectedSamples = 1;
}

void UtilBaudSweep::resetAll()
{
    for (int i = 0; i < numBauds_; i++)
        scores_[i] = 0;
}

int UtilBaudSweep::pickBest() const
{
    int minHits = (int)(cfg_.minAcceptRate * (float)cfg_.expectedSamples);
    if (minHits < 1)
        minHits = 1;
    for (int j = 0; j < numBauds_; j++)
        if (scores_[j] >= minHits)
            return j;
    // Fall back to any baud with a hit.
    for (int j = 0; j < numBauds_; j++)
        if (scores_[j] > 0)
            return j;
    return -1;
}

} // namespace autolink
