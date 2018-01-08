/**
 * @file timely.h
 * @brief TIMELY congestion control
 * From: http://people.eecs.berkeley.edu/~radhika/timely-code-snippet.cc
 * Units: Microseconds for time, bytes/sec for throughput
 */

#ifndef ERPC_TIMELY_H
#define ERPC_TIMELY_H

#include "common.h"

namespace erpc {
static constexpr double kTimelyEwmaAlpha = 0.02;

static constexpr double kTimelyMinRTT = 2.0;
static constexpr double kTimelyTLow = 50;
static constexpr double kTimelyTHigh = 1000;

static constexpr double kTimelyDecreaseFactor = 0.8;
static constexpr size_t kTimelyHaiThresh = 5;

/// Max = 5 GB/s (40 Gbps), min = 5 MB/s (arbitrary)
static constexpr double kTimelyMaxRate = 5.0 * 1000 * 1000 * 1000;
static constexpr double kTimelyMinRate = 5.0 * 1000 * 1000;
static constexpr double kTimelyAddRate = 5.0 * 1000 * 1000;

class Timely {
 private:
  size_t neg_gradient_count = 0;
  double prev_rtt = 0.0;
  double avg_rtt_diff = 0.0;
  double last_update_time = 0.0;

 public:
  double rate = kTimelyMaxRate;
  void update_rate(double sample_rtt, double cur_time);
};
}  // End erpc

#endif  // ERPC_TIMELY_H