#pragma once
#include <stdint.h>
#include <stddef.h>
#include <vector>

// ----------------------------------------------------------------------------
// UtilBaudSweep — auto-baud reliability scoring.
//
// The auto-baud handshake sends PING frames from the Ping at every
// candidate baud in `allowedBauds`. The Pong scores each decoded PING into
// a per-baud bucket. At REQ time the Pong picks the highest baud whose
// success rate meets a threshold, instead of the old "any PING wins"
// behavior that one missed PING could drop the link to a much slower rate.
//
// Pure value class: no UART, no locks. Owns its own score vector; the link
// layer just hands it a count per baud and asks for a recommendation at
// REQ time. Host-testable on its own (see UtilBaudSweepTest).
// ----------------------------------------------------------------------------
namespace autolink {

class UtilBaudSweep {
public:
    // Config — sweep parameters mirrored from AutoLinkConfig. Wired in via
    // configure() once the baud list size is known; not changed after that.
    struct Config {
        int   pingSamplesPerBaud = 4;   // PINGs the Ping emits per baud
        float minAcceptRate       = 0.5f; // min fraction of PINGs that must decode
                                          // at a baud for it to be considered reliable
        int   expectedSamples     = -1;  // if -1, configure() derives this from
                                          // pingSamplesPerBaud automatically; tests
                                          // can set it explicitly to a fixed value
    };

    explicit UtilBaudSweep(int numBauds) : scores_(numBauds, 0) {}

    // Wire a config in. Must be called before any score() / pickBest() calls.
    void configure(const Config& c);

    // Per-PING tick: record one decode at baud index `idx`. The caller is
    // responsible for tracking the current baud index and which sample of N
    // we're on; this class just counts.
    void score(int idx) { if (idx >= 0 && idx < (int)scores_.size()) scores_[idx]++; }

    // Mark a PING window as "we're done with this baud" -- resets the
    // running score for that index to 0 so a fresh sweep starts clean. Not
    // needed for the simple "send N PINGs, pick at end" model, but useful
    // if the Pong ever wants to re-evaluate mid-sweep.
    void resetIndex(int idx) { if (idx >= 0 && idx < (int)scores_.size()) scores_[idx] = 0; }

    // Reset the whole sweep (call on link drop / start of new negotiation).
    void resetAll();

    // Return the highest baud index whose success rate >= minAcceptRate.
    // Returns -1 if nothing scored (no reliable baud found); the caller
    // is expected to fall back to baud[0] in that case.
    int pickBest() const;

    // How many PINGs the Ping should send at each baud.
    int samplesPerBaud() const { return cfg_.pingSamplesPerBaud; }

    // The minimum number of decodes at a baud before the Pong considers
    // it "reliable" and sends the fast-ack. Mirrors the threshold used
    // by pickBest(); exposed so the SWP handler can decide mid-sweep
    // whether to keep going or commit to the current baud.
    int minHitsForReliable() const {
        int m = (int)(cfg_.minAcceptRate * (float)cfg_.expectedSamples);
        return m < 1 ? 1 : m;
    }

    // How many decoded PINGs landed at index `idx`. Exposed for logging
    // and tests.
    int scoreAt(int idx) const {
        return (idx >= 0 && idx < (int)scores_.size()) ? scores_[idx] : 0;
    }
    int numBauds() const { return (int)scores_.size(); }

private:
    std::vector<int> scores_;
    Config           cfg_;
};

} // namespace autolink
