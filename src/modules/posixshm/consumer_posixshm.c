/*
 * consumer_posixshm.c -- a melted consumer that copies
 * frame data onto POSIX shared memory
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

// mlt Header files
#include <framework/mlt.h>

// System header files
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

// Forward references.
static int consumer_start( mlt_consumer this );
static int consumer_stop( mlt_consumer this );
static int consumer_is_stopped( mlt_consumer this );
static void consumer_output( mlt_consumer this, void *share, int size, mlt_frame frame );
static void *consumer_thread( void *arg );
static void consumer_close( mlt_consumer this );

/** Initialise the posixshm consumer.
 */

mlt_consumer consumer_posixshm_init( mlt_profile profile, mlt_service_type type, const char *id, char *arg ) {
  // Allocate the consumer
  mlt_consumer this = mlt_consumer_new(profile);

  // If memory allocated and initialises without error
  if ( this != NULL ) {

    // Get properties from the consumer
    mlt_properties properties = MLT_CONSUMER_PROPERTIES( this );

    // Assign close callback
    this->close = consumer_close;

    // Interpret the argument
    if ( arg != NULL )
      mlt_properties_set( properties, "target", arg );
    else
      mlt_properties_set( properties, "target", "/posixshm_share.mlt" );

    // Set the output handling method
    mlt_properties_set_data( properties, "output", consumer_output, 0, NULL, NULL );

    // Terminate at end of the stream by default
    mlt_properties_set_int( properties, "terminate_on_pause", 0 );

    mlt_properties_set_int(properties, "frame_rate_den", profile->frame_rate_den);
    mlt_properties_set_int(properties, "frame_rate_num", profile->frame_rate_num);

    // Set up start/stop/terminated callbacks
    this->start = consumer_start;
    this->stop = consumer_stop;
    this->is_stopped = consumer_is_stopped;
  }

  // Return this
  return this;
}

/** Start the consumer.
 */

static int consumer_start( mlt_consumer this ) {
  // Get the properties
  mlt_properties properties = MLT_CONSUMER_PROPERTIES( this );

  // Check that we're not already running
  if ( !mlt_properties_get_int( properties, "running" ) ) {

    // set up the shared memory
    mlt_image_format ifmt = mlt_image_yuv422;
    mlt_properties_set_int(properties, "mlt_image_format", ifmt);
    int width = mlt_properties_get_int( properties, "width");
    int height = mlt_properties_get_int( properties, "height");

    if( width <= 0 || height <= 0 ) {
      width = 1920;
      height = 1080;
      mlt_properties_set_int(properties, "width", width);
      mlt_properties_set_int(properties, "height", height);
    }

    mlt_frame frame = mlt_consumer_rt_frame(this);
    mlt_properties fprops = MLT_FRAME_PROPERTIES(frame);
    mlt_audio_format afmt = mlt_audio_s16;
    int channels = mlt_properties_get_int(fprops, "audio_channels");
    int samples = mlt_properties_get_int(fprops, "audio_samples");

    mlt_frame_close(frame);

    mlt_properties_set_int(properties, "mlt_audio_format", afmt);

    // initialize shared memory
    char *sharedKey = mlt_properties_get(properties, "target");
    int memsize = sizeof(pthread_rwlock_t); // access semaphore
    memsize += 3 * sizeof(uint32_t); // frame number, frame rate num/den
    memsize += 4 * sizeof(uint32_t); // size, image format, height, width
    memsize += mlt_image_format_size(ifmt, width, height, NULL); // image size
    memsize += 5 * sizeof(uint32_t); // size, audio format, frequency, channels, samples
    memsize += mlt_audio_format_size(afmt, samples, channels); // audio size

    /* security concerns: if we want to keep malicious clients from DoS'ing the
       server via the semaphores, or corrupting videos, we should create both the
       semaphore and the shared memory patch  with 644 and have server and clients
       run with different users */

    // create shared memory
    int shareId = shm_open(sharedKey, O_RDWR | O_CREAT, 0666);
    ftruncate(shareId, memsize);
    void *share = mmap(NULL, memsize, PROT_READ | PROT_WRITE, MAP_SHARED, shareId, 0);

    // create semaphore
    pthread_rwlockattr_t rwlock_attr;
    pthread_rwlockattr_init(&rwlock_attr);
    pthread_rwlockattr_setpshared(&rwlock_attr, PTHREAD_PROCESS_SHARED);
    pthread_rwlock_init(share, &rwlock_attr);
    pthread_rwlock_t *rwlock = (pthread_rwlock_t*)share;

    close(shareId);

    // all the shared memory space
    mlt_properties_set_data(properties, "_share", share, memsize, NULL, NULL);
    mlt_properties_set_int(properties, "_shareSize", memsize);
    mlt_properties_set(properties, "_sharedKey", sharedKey);
    // the rwlock at the beginning of _share
    mlt_properties_set_data(properties, "_rwlock", rwlock, sizeof(pthread_rwlock_t), NULL, NULL);
    // the writespace for each frame, after rwlock
    mlt_properties_set_data(properties, "_writespace", share + sizeof(pthread_rwlock_t),
                            memsize - sizeof(pthread_rwlock_t), NULL, NULL);

    // Allocate a thread
    pthread_t *thread = calloc( 1, sizeof( pthread_t ) );

    // Assign the thread to properties
    mlt_properties_set_data( properties, "thread", thread, sizeof( pthread_t ), free, NULL );

    // Set the running state
    mlt_properties_set_int( properties, "running", 1 );

    // Create the thread
    pthread_create( thread, NULL, consumer_thread, this );
  }

  return 0;
}

/** Stop the consumer.
 */

static int consumer_stop( mlt_consumer this )
{
  // Get the properties
  mlt_properties properties = MLT_CONSUMER_PROPERTIES( this );

  // Check that we're running
  if ( mlt_properties_get_int( properties, "running" ) )
    {
      // Get the thread
      pthread_t *thread = mlt_properties_get_data( properties, "thread", NULL );

      // Stop the thread
      mlt_properties_set_int( properties, "running", 0 );

      // Wait for termination
      pthread_join( *thread, NULL );
    }

  return 0;
}

/** Determine if the consumer is stopped.
 */

static int consumer_is_stopped( mlt_consumer this ) {
  // Get the properties
  mlt_properties properties = MLT_CONSUMER_PROPERTIES( this );
  return !mlt_properties_get_int( properties, "running" );
}

/** The posixshm output method.
 */

static void consumer_output( mlt_consumer this, void *share, int size, mlt_frame frame ) {
  // Get the properties
  mlt_properties properties = MLT_CONSUMER_PROPERTIES( this );
  mlt_properties fprops = MLT_FRAME_PROPERTIES(frame);

  int fr_num = mlt_properties_get_int(properties, "frame_rate_num");
  int fr_den = mlt_properties_get_int(properties, "frame_rate_den");
  mlt_image_format ifmt = mlt_properties_get_int(properties, "mlt_image_format");
  int width = mlt_properties_get_int(properties, "width");
  int height = mlt_properties_get_int(properties, "height");
  int32_t frameno = mlt_consumer_position(this);
  pthread_rwlock_t *rwlock = mlt_properties_get_data(properties, "_rwlock", NULL);
  uint8_t *image=NULL;
  mlt_frame_get_image(frame, &image, &ifmt, &width, &height, 0);
  int image_size = mlt_image_format_size(ifmt, width, height, NULL);

  pthread_rwlock_wrlock(rwlock);

  void *walk = share;

  uint32_t *header = (uint32_t*) walk;

  *header++ = frameno;
  *header++ = fr_num;
  *header++ = fr_den;
  *header++ = image_size;
  *header++ = ifmt;
  *header++ = width;
  *header++ = height;
  walk = header;
  
  memcpy(walk, image, image_size);
  walk += image_size;

  header = (uint32_t*) walk;

  // try to get the format defined by the consumer
  mlt_audio_format afmt = mlt_properties_get_int(properties, "mlt_audio_format");
  // all other data provided by the producer
  int frequency = mlt_properties_get_int(fprops, "audio_frequency");
  int channels = mlt_properties_get_int(fprops, "audio_channels");
  int samples = mlt_properties_get_int(fprops, "audio_samples");
  void *audio=NULL;
  mlt_frame_get_audio(frame, &audio, &afmt, &frequency, &channels, &samples);
  int audio_size = mlt_audio_format_size(afmt, samples, channels);

  header[0] = audio_size;
  header[1] = afmt;
  header[2] = frequency;
  header[3] = channels;
  header[4] = samples;
  walk = header + 5;
  
  memcpy(walk, audio, audio_size);

  pthread_rwlock_unlock(rwlock);
}

/** The main thread - the argument is simply the consumer.
 */

static void *consumer_thread( void *arg ) {
  // Map the argument to the object
  mlt_consumer this = arg;

  // Get the properties
  mlt_properties properties = MLT_CONSUMER_PROPERTIES( this );

  // Get the terminate_on_pause property
  int top = mlt_properties_get_int( properties, "terminate_on_pause" );

  // Get the handling methods
  int ( *output )( mlt_consumer, uint8_t *, int, mlt_frame ) = mlt_properties_get_data( properties, "output", NULL );

  // Frame and size
  mlt_frame frame = NULL;

  // shared memory info
  int size = 0;
  uint8_t *share = mlt_properties_get_data(properties, "_writespace", &size);
  int fr_den = mlt_properties_get_int(properties, "frame_rate_den");
  int fr_num = mlt_properties_get_int(properties, "frame_rate_num");
  struct timespec sleeptime;

  // Loop while running
  while( mlt_properties_get_int( properties, "running" ) ) {
    clock_gettime(CLOCK_REALTIME, &sleeptime);
    // Get the frame
    frame = mlt_consumer_rt_frame( this );

    // Check that we have a frame to work with
    if ( frame != NULL ) {
      // Terminate on pause
      if ( top && mlt_properties_get_double( MLT_FRAME_PROPERTIES( frame ), "_speed" ) == 0 ) {
        mlt_frame_close( frame );
        break;
      }
      output( this, share, size, frame );
      mlt_events_fire( properties, "consumer-frame-show", frame, NULL );
      mlt_frame_close(frame);
    }
    struct timespec endtime;
    clock_gettime(CLOCK_REALTIME, &endtime);
    long int elapsed = 1000000000 * (endtime.tv_sec - sleeptime.tv_sec) + (endtime.tv_nsec - sleeptime.tv_nsec);
    sleeptime.tv_sec = 0;
    sleeptime.tv_nsec = ((double)fr_den / (double)fr_num) * 1000000000 - elapsed;
    clock_nanosleep(CLOCK_REALTIME, 0, &sleeptime, NULL);
  }

  mlt_consumer_stopped( this );

  return NULL;
}

/** Close the consumer.
 */

static void consumer_close( mlt_consumer this )
{

  // Stop the consumer
  mlt_consumer_stop( this );
  
  // Close the parent
  mlt_consumer_close( this );
}
