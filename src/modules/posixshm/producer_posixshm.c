/*
 * producer_posixshm.c -- simple libunixsock test case
 * Copyright (C) 2003-2004 Ushodaya Enterprises Limited
 * Author: Charles Yates <charles.yates@pandora.be>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include <framework/mlt.h>
#include <framework/mlt_types.h>
#include <framework/mlt_pool.h>

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include "common.h"

static int producer_get_frame( mlt_producer parent, mlt_frame_ptr frame, int index );
static void producer_close( mlt_producer parent );
static void* producer_thread(void *arg);
static mlt_cache m_cache;

mlt_producer producer_posixshm_init( mlt_profile profile, mlt_service_type type, const char *id, char *source ) {
  mlt_producer this = mlt_producer_new(profile);

  if ( this ) {
    int destroy = 0;
    mlt_properties properties = MLT_PRODUCER_PROPERTIES( this );

    // Register transport implementation with the producer
    this->close = ( mlt_destructor )producer_close;

    // Register our get_frame implementation with the producer
    this->get_frame = producer_get_frame;

    //double fps = mlt_producer_get_fps( this );

    mlt_properties_set_int( properties, "locked", 0 );

    // Set the resource property (required for all producers)
    if(source)
      mlt_properties_set(properties, "resource", source);
    else
      mlt_properties_set(properties, "resource", "/posixshm_share.mlt");

    // open shared memory
    char *sharedKey = mlt_properties_get(properties, "resource");
    int shareId = shm_open(sharedKey, O_RDWR, 0666);
    struct stat filestat;
    fstat(shareId, &filestat);
    off_t memsize = filestat.st_size;
    void *share = mmap(NULL, memsize, PROT_READ | PROT_WRITE, MAP_SHARED, shareId, 0);

    // I'm done with the file descriptor
    close(shareId);

    // initialize semaphore
    struct posixshm_control *control = (struct posixshm_control*) share;

    int control_size = sizeof(struct posixshm_control);

    void *readspace = share + control_size;
    int read_size = memsize - control_size;
    struct posix_shm_header *header = (struct posix_shm_header*)readspace;

    // producer properties
    mlt_properties_set_position(properties, "in", 0);
    mlt_properties_set_position(properties, "out", -1);
    mlt_properties_set_int(properties, "meta.media.width", header->width);
    mlt_properties_set_int(properties, "meta.media.height", header->height);
    mlt_properties_set_int(properties, "meta.media.frame_rate_num", header->frame_rate_num);
    mlt_properties_set_int(properties, "meta.media.frame_rate_den", header->frame_rate_den);
    mlt_properties_set_int(properties, "meta.media.sample_aspect_den", 1);
    mlt_properties_set_int(properties, "meta.media.sample_aspect_num", 1);

    mlt_properties_set_int( properties, "meta.media.progressive", profile->progressive );
    //mlt_properties_set_int( properties, "top_field_first", m_topFieldFirst );
    mlt_properties_set_double( properties, "aspect_ratio", mlt_profile_sar( profile ) );
    mlt_properties_set_int(properties, "meta.media.colorspace", 601 );

    // shared memory space properties
    mlt_properties_set_data(properties, "_share", share, memsize, NULL, NULL);
    mlt_properties_set_int(properties, "_shareSize", memsize);
    mlt_properties_set(properties, "_sharedKey", sharedKey);
    // control
    mlt_properties_set_data(properties, "_control", control, control_size, NULL, NULL);
    // frame readspace
    mlt_properties_set_data(properties, "_readspace", readspace,read_size , NULL, NULL);

    // last read frame
    mlt_properties_set_int(properties, "_last_frame", -1);

    // buffer mutex
    pthread_mutex_t *mutex = malloc(sizeof(pthread_mutex_t));
    pthread_mutex_init(mutex, NULL);
    mlt_properties_set_data(properties, "_queue_mutex", mutex, sizeof(pthread_mutex_t), free, NULL);
    pthread_cond_t *cond = malloc(sizeof(pthread_cond_t));
    pthread_cond_init(cond, NULL);
    mlt_properties_set_data(properties, "_queue_cond", cond, sizeof(pthread_cond_t), free, NULL);

    // buffer
    mlt_deque queue = mlt_deque_init();
    mlt_properties_set_data(properties, "_queue", queue, sizeof(mlt_deque), (mlt_destructor)mlt_deque_close, NULL);
    mlt_properties_set_int(properties, "_buffer", 25);
    mlt_properties_set_int(properties, "_buffering", 1);

    // Cache
    m_cache = mlt_cache_init();
    // 3 covers YADIF and increasing framerate use cases
    mlt_cache_set_size( m_cache, 5 );

    // Read frame thread setup and creation
    pthread_t *thread = malloc(sizeof(pthread_t));
    pthread_attr_t *attr = malloc(sizeof(pthread_attr_t));

    pthread_attr_init(attr);
    pthread_attr_setdetachstate(attr, PTHREAD_CREATE_JOINABLE);

    mlt_properties_set_data(properties, "_thread", thread, sizeof(pthread_t), free, NULL);
    mlt_properties_set_data(properties, "_thread_attr", attr, sizeof(pthread_attr_t), free, NULL);

    pthread_create(thread, attr, producer_thread, this);

    // These properties effectively make it infinite.
    mlt_properties_set_int( properties, "length", INT_MAX );
    mlt_properties_set_int( properties, "out", INT_MAX - 1 );
    mlt_properties_set( properties, "eof", "loop" );

    // If we couldn't open the file, then destroy it now
    if ( destroy ) {
      mlt_producer_close(this);
      this = NULL;
    }

    // Return the producer
    return this;
  }

  free( this );
  return NULL;
}

static void producer_read_frame_data(mlt_producer this, mlt_frame_ptr frame) {
  // get the producer properties
  mlt_properties properties = MLT_PRODUCER_PROPERTIES(this);
  // Get the frames properties
  mlt_properties frame_props = MLT_FRAME_PROPERTIES(*frame);

  int last_frame = mlt_properties_get_int(properties, "_last_frame");

  void *readspace = mlt_properties_get_data(properties, "_readspace", NULL);
  struct posixshm_control *control = mlt_properties_get_data(properties, "_control", NULL);

  struct posix_shm_header *header = readspace;
  void *data = readspace + sizeof(struct posix_shm_header);

  pthread_rwlock_rdlock(&control->rwlock);

  while(header->frame == last_frame) {
    pthread_mutex_lock(&control->fr_mutex);
    pthread_rwlock_unlock(&control->rwlock);
    pthread_cond_wait(&control->frame_ready, &control->fr_mutex);
    pthread_rwlock_rdlock(&control->rwlock);
    pthread_mutex_unlock(&control->fr_mutex);
  }
  mlt_properties_set_int(frame_props, "_consecutive", header->frame == (last_frame + 1));
  mlt_properties_set_int(properties, "_last_frame", header->frame);

  int frame_rate_num = header->frame_rate_num;
  int frame_rate_den = header->frame_rate_den;
  int image_size = header->image_size;
  mlt_image_format ifmt = header->image_format;
  int width = header->width;
  int height = header->height;

  int audio_size = header->audio_size;
  int afmt = header->audio_format;
  int frequency = header->frequency;
  int channels = header->channels;
  int samples = header->samples;

  void *buffer = mlt_pool_alloc(image_size);
  memcpy(buffer, data, image_size);
  mlt_frame_set_image(*frame, buffer, image_size, mlt_pool_release);
  //mlt_properties_set_data(frame_props, "_video_data", buffer, image_size, mlt_pool_release, NULL);

  data += image_size;
  buffer = mlt_pool_alloc(audio_size);
  memcpy(buffer, data, audio_size);
  mlt_frame_set_audio(*frame, buffer, afmt, audio_size, mlt_pool_release);
  //mlt_properties_set_data(frame_props, "_audio_data", buffer, audio_size, mlt_pool_release, NULL);

  // release read image lock
  pthread_rwlock_unlock(&control->rwlock);

  mlt_profile profile = mlt_service_profile( MLT_PRODUCER_SERVICE(this) );

  mlt_properties_set_int(frame_props, "mlt_image_format", ifmt);
  mlt_properties_set_int(frame_props, "width", width);
  mlt_properties_set_int(frame_props, "height", height);

  mlt_properties_set_int(frame_props, "audio_format", afmt);
  mlt_properties_set_int(frame_props, "audio_frequency", frequency);
  mlt_properties_set_int(frame_props, "audio_channels", channels);
  mlt_properties_set_int(frame_props, "audio_samples", samples);

  mlt_properties_set_int( frame_props, "progressive", profile->progressive );
  //mlt_properties_set_int( frame_props, "top_field_first", m_topFieldFirst );
  mlt_properties_set_double( frame_props, "aspect_ratio", mlt_profile_sar( profile ) );
  mlt_properties_set_int(frame_props, "frame_rate_num", frame_rate_num);
  mlt_properties_set_int(frame_props, "frame_rate_den", frame_rate_den);
  mlt_properties_set_int( frame_props, "width", width );
  mlt_properties_set_int( frame_props, "height", height );
  mlt_properties_set_int( frame_props, "format", ifmt );
  mlt_properties_set_int( frame_props, "colorspace", 601 );
  mlt_properties_set_int( frame_props, "audio_frequency", frequency);
  mlt_properties_set_int(frame_props, "audio_channels", channels);
}

static void* producer_thread(void *arg) {
  mlt_producer this = arg;
  mlt_properties properties = MLT_PRODUCER_PROPERTIES(this);
  pthread_mutex_t *mutex = mlt_properties_get_data(properties, "_queue_mutex", NULL);
  pthread_cond_t  *cond  = mlt_properties_get_data(properties, "_queue_cond", NULL);
  mlt_deque queue = mlt_properties_get_data(properties, "_queue", NULL);

  mlt_properties_set_int(properties, "_running", 1);

  while (mlt_properties_get_int(properties, "_running")) {

    // Sleep until buffer consumption begins
    pthread_mutex_lock(mutex);
    if (mlt_deque_count(queue) >= 25) {
      pthread_cond_wait(cond, mutex);
    }

    mlt_frame frame = mlt_frame_init(MLT_PRODUCER_SERVICE(this));
    producer_read_frame_data(this, &frame);

    if (!mlt_properties_get_int(MLT_FRAME_PROPERTIES(frame), "_consecutive")) {
      while (mlt_deque_count(queue)) {
        mlt_frame_close(mlt_deque_pop_front(queue));
      }
    }

    mlt_deque_push_back(queue, frame);
    pthread_cond_broadcast(cond);
    pthread_mutex_unlock(mutex);
  }

  pthread_exit(NULL);
}

static int producer_get_image( mlt_frame this, uint8_t **buffer, mlt_image_format *format, int *width, int *height, int writable ) {
  //mlt_producer producer = mlt_frame_pop_service(this);
  //producer_read_frame_data(producer, &this);

  mlt_properties properties = MLT_FRAME_PROPERTIES(this);

  int image_size;
  // Assign width and height according to the frame
  *width = mlt_properties_get_int(properties, "width");
  *height = mlt_properties_get_int(properties, "height");
  *format = mlt_properties_get_int(properties, "mlt_image_format");

  *buffer = mlt_properties_get_data(properties, "_video_data", &image_size);
  mlt_properties_set_data(properties, "_video_data", NULL, 0, NULL, NULL);
  mlt_frame_set_image(this, *buffer, image_size, mlt_pool_release);

  return 0;
}

static int producer_get_audio( mlt_frame this, void **buffer, mlt_audio_format *format, int *frequency, int *channels, int *samples )
{
  mlt_properties properties = MLT_FRAME_PROPERTIES(this);

  *format = mlt_properties_get_int(properties, "audio_format");
  *frequency = mlt_properties_get_int(properties, "audio_frequency");
  *channels = mlt_properties_get_int(properties, "audio_channels");
  *samples = mlt_properties_get_int(properties, "audio_samples");

  int size;
  *buffer = mlt_properties_get_data(properties, "_audio_data", &size);
  mlt_properties_set_data(properties, "_audio_data", NULL, 0, NULL, NULL);
  mlt_frame_set_audio(this, *buffer, *format, size, mlt_pool_release);

  return 0;
}

static int producer_get_frame( mlt_producer producer, mlt_frame_ptr frame, int index )
{
  mlt_properties prod_props = MLT_PRODUCER_PROPERTIES(producer);
  struct posixshm_control *control = mlt_properties_get_data(prod_props, "_control", NULL);
  mlt_deque queue = mlt_properties_get_data(prod_props, "_queue", NULL);
  pthread_mutex_t *mutex = mlt_properties_get_data(prod_props, "_queue_mutex", NULL);
  pthread_cond_t *cond = mlt_properties_get_data(prod_props, "_queue_cond", NULL);
  int buffering = mlt_properties_get_int(prod_props, "_buffering");

  int frn = mlt_properties_get_int(prod_props, "meta.media.frame_rate_num");
  int frd = mlt_properties_get_int(prod_props, "meta.media.frame_rate_den");

  if(buffering) {
    mlt_properties_set_int(prod_props, "_buffering", 0);
    int buffer = mlt_properties_get_int(prod_props, "_buffer");
    pthread_mutex_lock(mutex);
    while(mlt_deque_count(queue) < buffer) {
      pthread_cond_wait(cond, mutex);
    }
    pthread_mutex_unlock(mutex);
  }

  // Try to get frame from cache
  mlt_position position = mlt_producer_position( producer );
  *frame = mlt_cache_get_frame( m_cache, position );

  if (!*frame) {
    // Get frame from queue
    pthread_mutex_lock(mutex);
    while(mlt_deque_count(queue) < 1) {
      pthread_cond_wait(cond, mutex);
    }

    *frame = (mlt_frame)mlt_deque_pop_front(queue);
    pthread_cond_broadcast(cond);
    pthread_mutex_unlock(mutex);

    /*
    // Generate new frame
    printf("\nGenerate new frame!");
    *frame = mlt_frame_init(MLT_PRODUCER_SERVICE(producer));
    producer_read_frame_data(producer, *frame);
    */

    // Add frame to cache
    if ( *frame )
    {
      mlt_frame_set_position( *frame, position );
      mlt_cache_put_frame( m_cache, *frame );
    }
  } else {
    printf("\ncache hit: %d\n", position);
  }

  // Get the frames properties
  mlt_properties properties = MLT_FRAME_PROPERTIES( *frame );

  mlt_properties_set_int( properties, "test_image", 0 );
  mlt_properties_set_int( properties, "test_audio", 0 );

  // Update other info on the frame
  // TODO: ??????
  mlt_properties_set_int( properties, "colorspace", 601 );

  //mlt_properties_set_int( properties, "progressive", dv_is_progressive( dv_decoder ) );
  //mlt_properties_set_double(properties, "aspect_ratio", (double)width / (double)height);

  // Push the get_image method onto the stack
  mlt_frame_push_get_image(*frame, mlt_frame_get_image);

  // Push the get_audio method onto the stack
  mlt_frame_push_audio(*frame, mlt_frame_get_audio);
  // Update timecode on the frame we're creating
  mlt_frame_set_position( *frame, mlt_producer_position( producer ) );

  // Calculate the next timecode
  mlt_producer_prepare_next( producer );
  return 0;
}

static void producer_close( mlt_producer this )
{
  // Close the parent
  this->close = NULL;
  mlt_properties properties = MLT_PRODUCER_PROPERTIES(this);

  if (mlt_properties_get_int(properties, "_running")) {
    mlt_properties_set_int(properties, "_running", 0);

    // Wake up thread from waiting for shared memory (not solving all problems)
    pthread_t *thread = mlt_properties_get_data(properties, "_thread", NULL);
    pthread_mutex_t *mutex = mlt_properties_get_data(properties, "_queue_mutex", NULL);
    pthread_cond_t  *cond  = mlt_properties_get_data(properties, "_queue_cond", NULL);

    pthread_mutex_lock(mutex);
    pthread_cond_broadcast(cond);
    pthread_mutex_unlock(mutex);

    // Wait for termination
    pthread_join( *thread, NULL );
  }

  // Destroy thread attributes
  pthread_attr_t *attr = mlt_properties_get_data(properties, "_thread_attr", NULL);
  pthread_attr_destroy(attr);

  mlt_producer_close(this);
}
