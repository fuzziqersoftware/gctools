#include "SampleCache.hh"

#include <format>

using namespace std;

vector<float> resample(
    const vector<float>& input_samples,
    size_t num_channels,
    double src_ratio,
    int resample_method) {
  size_t input_frame_count = input_samples.size() / num_channels;
  size_t output_frame_count = (input_frame_count * src_ratio) + 1;
  size_t output_sample_count = output_frame_count * num_channels;

  vector<float> output_samples(output_sample_count, 0.0f);

  SRC_DATA data;
  data.data_in = const_cast<float*>(input_samples.data());
  data.data_out = output_samples.data();
  data.input_frames = input_frame_count;
  data.output_frames = output_frame_count;
  data.input_frames_used = 0;
  data.output_frames_gen = 0;
  data.end_of_input = 0;
  data.src_ratio = src_ratio;

  int error = src_simple(&data, resample_method, num_channels);
  if (error) {
    throw runtime_error(format("src_simple failed (ratio={}): {}", src_ratio, src_strerror(error)));
  }

  output_samples.resize(data.output_frames_gen * num_channels);
  return output_samples;
}
