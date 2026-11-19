// Host regression for histogram rank, overflow, and cross-worker aggregation.
#include "../latency_stats.h"
#include <cassert>

int main()
{
  Bench::Latency_stats one;
  one.record(644112);
  for (unsigned p : {500, 900, 990, 999}) {
    auto q = one.percentile(p);
    assert(!q.lower_bound && q.ns == 644112);
  }

  Bench::Latency_stats tail;
  for (int i = 0; i < 96; ++i) tail.record(1000);
  tail.record(1000000);
  tail.record(2000000);
  tail.record(3000000);
  tail.record(6500000);
  auto median = tail.percentile(500);
  assert(!median.lower_bound && median.ns >= 1000 && median.ns < 2000);
  auto p99 = tail.percentile(990);
  assert(p99.lower_bound && p99.ns == 999000);
  assert(tail.percentile(999).ns == 6500000);
  assert(!tail.percentile(999).lower_bound);

  Bench::Latency_stats merged;
  merged.merge(one);
  merged.merge(tail);
  assert(merged.count == 101 && merged.hist[999] == 4);
  assert(merged.min_ns == 1000 && merged.max_ns == 6500000);
  // The 100th sample is in the overflow bucket, the 101st is the exact max.
  assert(merged.percentile(990).lower_bound);
  assert(merged.percentile(999).ns == 6500000);
}
