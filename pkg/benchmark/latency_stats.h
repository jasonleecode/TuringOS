#pragma once

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

namespace Bench {
struct Quantile {
  long long ns;
  bool lower_bound;
};

struct Latency_stats {
  static constexpr unsigned buckets = 1000;
  using Count = unsigned long long;
  Count hist[buckets]{};
  Count count = 0;
  long long min_ns = std::numeric_limits<long long>::max(), max_ns = 0;
  double mean = 0, m2 = 0;

  void record(long long ns)
  {
    ++hist[std::min<long long>(ns / 1000, buckets - 1)];
    min_ns = std::min(min_ns, ns);
    max_ns = std::max(max_ns, ns);
    double delta = ns - mean;
    mean += delta / ++count;
    m2 += delta * (ns - mean);
  }

  void merge(Latency_stats const &other)
  {
    if (!other.count) return;
    double delta = other.mean - mean;
    Count combined = count + other.count;
    m2 += other.m2 + delta * delta * count * other.count / combined;
    mean += delta * other.count / combined;
    count = combined;
    min_ns = std::min(min_ns, other.min_ns);
    max_ns = std::max(max_ns, other.max_ns);
    for (unsigned b = 0; b < buckets; ++b) hist[b] += other.hist[b];
  }

  Quantile percentile(unsigned permille) const
  {
    if (!count) return {0, false};
    Count rank = (count / 1000) * permille + ((count % 1000) * permille + 999) / 1000;
    if (rank <= 1 || min_ns == max_ns) return {min_ns, false};
    if (rank == count) return {max_ns, false};
    Count seen = 0;
    for (unsigned b = 0; b < buckets - 1; ++b) {
      seen += hist[b];
      if (seen >= rank)
        return {std::clamp<long long>(b * 1000LL + 500, min_ns, max_ns), false};
    }
    return {std::max<long long>((buckets - 1) * 1000LL, min_ns), true};
  }

  void print(unsigned divisor = 1) const
  {
    if (!count) return;
    double scale = 1000.0 * divisor;
    printf("    min=%.3f us  avg=%.3f us  max=%.3f us  stddev=%.3f us\n",
           min_ns / scale, mean / scale, max_ns / scale, std::sqrt(m2 / count) / scale);
    for (unsigned p : {500u, 900u, 990u, 999u}) {
      auto q = percentile(p);
      printf("  p%g%s%.3f us", p / 10.0, q.lower_bound ? ">=" : "=", q.ns / scale);
    }
    putchar('\n');
  }
};
}
