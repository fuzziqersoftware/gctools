#pragma once

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <unistd.h>

#include <samplerate.h>

#include <algorithm>
#include <unordered_map>
#include <vector>

using namespace std;

vector<float> resample(
    const vector<float>& input_samples,
    size_t num_channels,
    double src_ratio,
    int resample_method);

template <typename SampleKey>
class SampleCache {
public:
  explicit SampleCache(int resample_method)
      : resample_method(resample_method) {}
  ~SampleCache() = default;

  const vector<float>& at(SampleKey k, float src_ratio) const {
    return this->cache.at(k).at(src_ratio);
  }

  const vector<float>& add(SampleKey k, float src_ratio, vector<float>&& data) {
    return this->cache[k].emplace(src_ratio, std::move(data)).first->second;
  }

  const vector<float>& resample_add(
      SampleKey k,
      const vector<float>& input_samples,
      size_t num_channels,
      double src_ratio) {
    try {
      return this->at(k, src_ratio);
    } catch (const std::out_of_range&) {
      auto data = ::resample(
          input_samples,
          num_channels,
          src_ratio,
          this->resample_method);
      return this->add(k, src_ratio, std::move(data));
    }
  }

  vector<float> resample(
      const vector<float>& input_samples,
      size_t num_channels,
      double src_ratio) const {
    return ::resample(
        input_samples,
        num_channels,
        src_ratio,
        this->resample_method);
  }

private:
  int resample_method;
  unordered_map<SampleKey, unordered_map<float, vector<float>>> cache;
};
