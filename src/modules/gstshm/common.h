#ifndef _POSIXSHM_COMMON_H_
#define _POSIXSHM_COMMON_H_

#include <pthread.h>
#include <framework/mlt.h>
#include "shmpipe.h"

int _gstshm_push;

struct posixshm_control {
  uint32_t size;
  pthread_rwlock_t rwlock;
  pthread_cond_t frame_ready;
  pthread_mutex_t fr_mutex; // a mutex is mandatory for condition wait
};

struct posix_shm_header {
  uint32_t frame;
  uint32_t frame_rate_num;
  uint32_t frame_rate_den;
  uint32_t image_size;
  mlt_image_format image_format;
  uint32_t width;
  uint32_t height;
  uint32_t audio_size;
  mlt_audio_format audio_format;
  uint32_t frequency;
  uint32_t channels;
  uint32_t samples;
};

int write_log(int priority, const char *format, ...);
void log_header(struct posix_shm_header *header);

#endif
