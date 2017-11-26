#include "audio.hh"

#ifdef MACOSX
#include <OpenAL/al.h>
#include <OpenAL/alc.h>
#else
#include <AL/al.h>
#include <AL/alc.h>
#endif

#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

using namespace std;


const char* al_err_str(ALenum err) {
  switch(err) {
    case AL_NO_ERROR:
      return "AL_NO_ERROR";
    case AL_INVALID_NAME:
      return "AL_INVALID_NAME";
    case AL_INVALID_ENUM:
      return "AL_INVALID_ENUM";
    case AL_INVALID_VALUE:
      return "AL_INVALID_VALUE";
    case AL_INVALID_OPERATION:
      return "AL_INVALID_OPERATION";
    case AL_OUT_OF_MEMORY:
      return "AL_OUT_OF_MEMORY";
  }
  return "unknown";
}

#define __al_check_error(file,line) \
  do { \
    for (ALenum err = alGetError(); err != AL_NO_ERROR; err = alGetError()) \
      fprintf(stderr, "AL error %s at %s:%d\n", al_err_str(err), file, line); \
  } while(0)

#define al_check_error() \
    __al_check_error(__FILE__, __LINE__)


void init_al() {
  const char *defname = alcGetString(NULL, ALC_DEFAULT_DEVICE_SPECIFIER);

  ALCdevice* dev = alcOpenDevice(defname);
  ALCcontext* ctx = alcCreateContext(dev, NULL);
  alcMakeContextCurrent(ctx);
}

void exit_al() {
  ALCcontext* ctx = alcGetCurrentContext();
  ALCdevice* dev = alcGetContextsDevice(ctx);

  alcMakeContextCurrent(NULL);
  alcDestroyContext(ctx);
  alcCloseDevice(dev);
}


static size_t bytes_per_sample(int format) {
  int is_16bit = (format == AL_FORMAT_MONO16) || (format == AL_FORMAT_STEREO16);
  int is_stereo = (format == AL_FORMAT_STEREO8) || (format == AL_FORMAT_STEREO16);
  return 1 << (is_16bit + is_stereo);
}


al_stream::al_stream(int sample_rate, int format, size_t num_buffers) :
    sample_rate(sample_rate), format(format), num_buffers(num_buffers),
    queued_buffers(0) {
  this->buffer_ids.resize(this->num_buffers);
  alGenBuffers(this->buffer_ids.size(), this->buffer_ids.data());
  al_check_error();

  alGenSources(1, &this->source_id);
  al_check_error();
}

al_stream::~al_stream() {
  alDeleteSources(1, &this->source_id);
  alDeleteBuffers(this->buffer_ids.size(), this->buffer_ids.data());
}

void al_stream::add_samples(const void* buffer, size_t sample_count) {
  ALuint buffer_id;
  if (this->queued_buffers < this->buffer_ids.size()) {
    buffer_id = this->buffer_ids[this->queued_buffers];
    this->queued_buffers++;

  } else {
    // wait until there's a buffer to be dequeued
    this->wait_until_buffers_available(1);

    // dequeue the buffer so we can write into it
    alSourceUnqueueBuffers(this->source_id, 1, &buffer_id);
    al_check_error();
  }

  string& buffer_data = this->buffer_id_to_data[buffer_id];
  buffer_data.assign((const char*)buffer, sample_count * bytes_per_sample(this->format));

  // add the new data to the buffer and queue it
  alBufferData(buffer_id, this->format, buffer_data.data(), buffer_data.size(),
      this->sample_rate);
  al_check_error();
  alSourceQueueBuffers(this->source_id, 1, &buffer_id);
  al_check_error();

  // start playing the source if it isn't already playing
  ALint source_state;
  alGetSourcei(this->source_id, AL_SOURCE_STATE, &source_state);
  al_check_error();
  if (source_state != AL_PLAYING) {
    alSourcePlay(this->source_id);
    al_check_error();
  }
}

void al_stream::wait() const {
  // when all queued buffers are available, the sound is done playing
  this->wait_until_buffers_available(this->queued_buffers);
}

void al_stream::wait_until_buffers_available(int num_buffers) const {
  for (;;) {
    int buffers_processed;
    alGetSourcei(this->source_id, AL_BUFFERS_PROCESSED, &buffers_processed);
    al_check_error();
    if (buffers_processed >= num_buffers) {
      return;
    }
    usleep(1);
  }
}
