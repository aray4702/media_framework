#include "metrics.h"

#include <cmath>
#include <cstdio>

namespace mf {

void Histogram::add(double ms) {
  size_t bin = ms < 0 ? 0 : std::min<size_t>(static_cast<size_t>(ms), bins_.size() - 1);
  ++bins_[bin];
  ++count_;
  sum_ += ms;
}

double Histogram::percentile(double p) const {
  if (count_ == 0) return 0;
  int64_t rank = static_cast<int64_t>(std::ceil(p * double(count_)));
  int64_t seen = 0;
  for (size_t i = 0; i < bins_.size(); ++i) {
    seen += bins_[i];
    if (seen >= rank) return double(i + 1);  // upper edge of the bin
  }
  return double(bins_.size());
}

void Metrics::startTtff(int64_t nowNs) {
  std::lock_guard<std::mutex> lock(mu_);
  ttffStartNs_ = nowNs;
}

void Metrics::countLate() {
  std::lock_guard<std::mutex> lock(mu_);
  ++r_.lateDrops;
}

void Metrics::countRateCap() {
  std::lock_guard<std::mutex> lock(mu_);
  ++r_.rateCapDrops;
}

void Metrics::countHidden() {
  std::lock_guard<std::mutex> lock(mu_);
  ++r_.hiddenDrops;
}

void Metrics::countDecodeOnly() {
  std::lock_guard<std::mutex> lock(mu_);
  ++r_.decodeOnly;
}

void Metrics::countCorrupt() {
  std::lock_guard<std::mutex> lock(mu_);
  ++r_.corruptSkips;
}

void Metrics::seekLatency(int64_t ns) {
  std::lock_guard<std::mutex> lock(mu_);
  seek_.add(double(ns) / 1e6);
}

void Metrics::planned(int64_t ptsUs, int64_t slot, int64_t vsyncNs) {
  std::lock_guard<std::mutex> lock(mu_);
  plans_[nextPlan_] = {ptsUs, slot, vsyncNs};
  nextPlan_ = (nextPlan_ + 1) % plans_.size();
}

void Metrics::discontinuity() {
  std::lock_guard<std::mutex> lock(mu_);
  havePrev_ = false;
}

void Metrics::presented(int64_t ptsUs, int64_t presentedNs, std::optional<int64_t> avOffsetUs) {
  std::lock_guard<std::mutex> lock(mu_);
  if (presentedNs > 0 && ttffStartNs_ >= 0) {
    r_.ttffMs = double(presentedNs - ttffStartNs_) / 1e6;
    ttffStartNs_ = -1;
  }
  Plan* plan = nullptr;
  for (Plan& p : plans_) {
    if (p.ptsUs == ptsUs) plan = &p;
  }
  if (!plan) return;  // a seek or preroll frame, not playback
  Plan pl = *plan;
  plan->ptsUs = -1;
  if (presentedNs <= 0) {  // replaced before drawing, or dropped by the compositor
    ++r_.lateDrops;
    return;
  }
  ++r_.presented;
  if (havePrev_) {
    ++r_.intervals;
    int64_t actualSlots = std::llround(double(presentedNs - prevNs_) / double(pl.vsyncNs));
    if (actualSlots > pl.slot - prevSlot_) ++r_.janks;
  }
  havePrev_ = true;
  prevNs_ = presentedNs;
  prevSlot_ = pl.slot;
  if (avOffsetUs) {
    av_.add(std::abs(double(*avOffsetUs)) / 1000.0);
    avSignedSumMs_ += double(*avOffsetUs) / 1000.0;
  }
}

MetricsReport Metrics::report() const {
  std::lock_guard<std::mutex> lock(mu_);
  MetricsReport r = r_;
  int64_t shownOrLate = r.presented + r.lateDrops;
  r.droppedRate = shownOrLate ? double(r.lateDrops) / double(shownOrLate) : 0;
  r.jankRate = r.intervals ? double(r.janks) / double(r.intervals) : 0;
  r.avSamples = av_.count();
  r.avMeanAbsMs = av_.mean();
  r.avMeanMs = av_.count() ? avSignedSumMs_ / double(av_.count()) : 0;
  r.avP95AbsMs = av_.percentile(0.95);
  r.seeks = seek_.count();
  r.seekP50Ms = seek_.percentile(0.50);
  r.seekP95Ms = seek_.percentile(0.95);
  return r;
}

std::string MetricsReport::toString() const {
  char buf[512];
  std::snprintf(buf, sizeof(buf),
                "presented=%lld dropped=%.2f%% (late=%lld) rateCapped=%lld hidden=%lld decodeOnly=%lld "
                "corruptSkips=%lld jank=%.2f%% (%lld/%lld) av mean=%+.1fms |mean|=%.1fms |p95|<=%.0fms (n=%lld) "
                "ttff=%.0fms seeks=%lld p50<=%.0fms p95<=%.0fms",
                (long long)presented, droppedRate * 100, (long long)lateDrops, (long long)rateCapDrops,
                (long long)hiddenDrops, (long long)decodeOnly, (long long)corruptSkips, jankRate * 100,
                (long long)janks, (long long)intervals, avMeanMs, avMeanAbsMs, avP95AbsMs, (long long)avSamples, ttffMs,
                (long long)seeks, seekP50Ms, seekP95Ms);
  return buf;
}

}  // namespace mf
