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
#include <vector>


void init_al();
void exit_al();


class al_stream {
public:
  al_stream(int sample_rate, int format, size_t num_buffers = 5);
  ~al_stream();

  void add_samples(const void* buffer, size_t sample_count);

  void wait() const;

private:
  void wait_until_buffers_available(int num_buffers) const;

  int sample_rate;
  int format;

  size_t num_buffers;
  size_t queued_buffers;
  std::vector<ALuint> buffer_ids;
  std::unordered_map<ALuint, std::string> buffer_id_to_data;
  ALuint source_id;
};
