// UtilBaudSweep.cpp — auto-baud reliability scoring implementation.
// See UtilBaudSweep.h for the public interface.
#include "al/protocol/UtilBaudSweep.h"

namespace autolink {

void UtilBaudSweep::configure(const Config& c) {
    cfg_ = c;
    if (cfg_.expectedSamples < 0) cfg_.expectedSamples = cfg_.pingSamplesPerBaud;
    if (cfg_.expectedSamples < 1) cfg_.expectedSamples = 1;  // never divide by zero
}

void UtilBaudSweep::resetAll() {
    for (int i = 0; i < numBauds_; i++) scores_[i] = 0;
}

int UtilBaudSweep::pickBest() const {
    // The reliability sweep finds the FASTEST baud whose decode rate meets
    // the threshold. Iterate from lowest index (fastest baud) to highest
    // (slowest), returning the first one that qualifies. The previous code
    // searched in reverse (slowest-first), so when 19200 happened to hit the
    // threshold while 115200 only scored 2/4 (timing jitter, not a physical
    // failure), it incorrectly locked at 19200.
    int minHits = (int)(cfg_.minAcceptRate * (float)cfg_.expectedSamples);
    if (minHits < 1) minHits = 1;
    for (int j = 0; j < numBauds_; j++) {
        if (scores_[j] >= minHits) return j;
    }
    // Nothing met the strict threshold; fall back to the FASTEST baud that
    // received any PINGs at all. Prefer a fast baud with a few hits over a
    // slow baud with more hits: timing jitter can prevent the fastest baud
    // from reaching the threshold even when it's physically reachable, and
    // locking at a lower baud because of that is always the wrong call.
    for (int j = 0; j < numBauds_; j++) {
        if (scores_[j] > 0) return j;
    }
    return -1;
}

} // namespace autolink
