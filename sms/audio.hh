#ifdef MACOSX
#include <OpenAL/al.h>
#include <OpenAL/alc.h>
#else
#include <AL/al.h>
#include <AL/alc.h>
#endif

#include <stdint.h>
#include <stdio.h>
#include <sys/types.h>

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>


void init_al();
void exit_al();


class al_stream {
public:
  al_stream(int sample_rate, int format, size_t num_buffers = 5);
  ~al_stream();

  void add_samples(const void* buffer, size_t sample_count);

  void wait();

  size_t check_buffers();
  size_t buffer_count() const;
  size_t available_buffer_count() const;
  size_t queued_buffer_count() const;

private:
  void wait_for_buffers(size_t num_buffers = 1);

  int sample_rate;
  int format;

  std::vector<ALuint> all_buffer_ids;
  std::unordered_set<ALuint> available_buffer_ids;
  std::unordered_set<ALuint> queued_buffer_ids;
  std::unordered_map<ALuint, std::string> buffer_id_to_data;
  ALuint source_id;
};
