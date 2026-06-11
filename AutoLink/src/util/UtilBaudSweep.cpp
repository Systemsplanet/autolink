// UtilBaudSweep.cpp — auto-baud reliability scoring implementation.
// See UtilBaudSweep.h for the public interface.
#include "UtilBaudSweep.h"

namespace autolink {

void UtilBaudSweep::configure(const Config& c) {
    cfg_ = c;
    if (cfg_.expectedSamples < 0) cfg_.expectedSamples = cfg_.pingSamplesPerBaud;
    if (cfg_.expectedSamples < 1) cfg_.expectedSamples = 1;  // never divide by zero
}

void UtilBaudSweep::resetAll() {
    for (size_t i = 0; i < scores_.size(); i++) scores_[i] = 0;
}

int UtilBaudSweep::pickBest() const {
    // The reliability sweep is "send N PINGs at each baud, pick the highest
    // baud whose decode rate meets the threshold." Without this, a single
    // missed PING would drop the slave to a slower baud permanently.
    //
    // The pick is conservative: a baud is only "reliable" if at least
    // minAcceptRate * expectedSamples PINGs decoded. If the top baud
    // doesn't meet the bar, we fall back to lower bauds -- preferring a
    // working slow link over a flaky fast one.
    int minHits = (int)(cfg_.minAcceptRate * (float)cfg_.expectedSamples);
    if (minHits < 1) minHits = 1;   // at least one decode required
    for (int j = (int)scores_.size() - 1; j >= 0; j--) {
        if (scores_[j] >= minHits) return j;
    }
    // Nothing met the threshold: fall back to whatever scored anything at
    // all. Returning -1 would force the caller to use baud[0] blindly,
    // which is worse than picking a low-but-real decode rate.
    for (int j = (int)scores_.size() - 1; j >= 0; j--) {
        if (scores_[j] > 0) return j;
    }
    return -1;
}

} // namespace autolink
