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

static int producer_get_frame( mlt_producer parent, mlt_frame_ptr frame, int index );
static void producer_close( mlt_producer parent );

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
    pthread_rwlock_t *rwlock = (pthread_rwlock_t*)share;
    int lock_size = sizeof(pthread_rwlock_t);

    void *readspace = share + lock_size;
    int read_size = memsize - lock_size;

    // shared memory space properties
    mlt_properties_set_data(properties, "_share", share, memsize, NULL, NULL);
    mlt_properties_set_int(properties, "_shareSize", memsize);
    mlt_properties_set(properties, "_sharedKey", sharedKey);
    // rwlock
    mlt_properties_set_data(properties, "_rwlock", rwlock, lock_size, NULL, NULL);
    // frame readspace
    mlt_properties_set_data(properties, "_readspace", readspace,read_size , NULL, NULL);

    // last read frame
    mlt_properties_set_int(properties, "_last_frame", -1);

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

static int producer_get_image( mlt_frame this, uint8_t **buffer, mlt_image_format *format, int *width, int *height, int writable ) {
  mlt_properties properties = MLT_FRAME_PROPERTIES(this);

  int image_size;
  // Assign width and height according to the frame
  *width = mlt_properties_get_int(properties, "width");
  *height = mlt_properties_get_int(properties, "height");

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


static void producer_read_frame_data(mlt_producer this, mlt_frame_ptr frame) {
  // get the producer properties
  mlt_properties properties = MLT_PRODUCER_PROPERTIES(this);
  // Get the frames properties
  mlt_properties frame_procs = MLT_FRAME_PROPERTIES(*frame);

  int last_frame = mlt_properties_get_int(properties, "_last_frame");

  void *readspace = mlt_properties_get_data(properties, "_readspace", NULL);
  pthread_rwlock_t *rwlock = mlt_properties_get_data(properties, "_rwlock", NULL);

  uint32_t *header = readspace;
  void *data = header + 7;

  pthread_rwlock_rdlock(rwlock);

  int cur_frame = header[0];
  /*
  while( cur_frame <= last_frame ) {
    pthread_rwlock_unlock(rwlock);
    pthread_rwlock_rdlock(rwlock);
    cur_frame = header[0];
  }
  
  printf("found frame %d\n", cur_frame);
  */
  int image_size = header[3];
  int width = header[5];
  int height = header[6];

  mlt_properties_set_int(frame_procs, "width", width);
  mlt_properties_set_int(frame_procs, "height", height);

  void *buffer = mlt_pool_alloc(image_size);
  memcpy(buffer, data, image_size);
  mlt_properties_set_data(frame_procs, "_video_data", buffer, image_size, mlt_pool_release, NULL);

  header = data + image_size;
  int audio_size = header[0];
  int afmt = header[1];
  int frequency = header[2];
  int channels = header[3];
  int samples = header[4];

  mlt_properties_set_int(frame_procs, "audio_format", afmt);
  mlt_properties_set_int(frame_procs, "audio_frequency", frequency);
  mlt_properties_set_int(frame_procs, "audio_channels", channels);
  mlt_properties_set_int(frame_procs, "audio_samples", samples);

  data = header + 5;
  buffer = mlt_pool_alloc(audio_size);
  memcpy(buffer, data, audio_size);
  mlt_properties_set_data(frame_procs, "_audio_data", buffer, audio_size, mlt_pool_release, NULL);

  // release read image lock
  pthread_rwlock_unlock(rwlock);
}

static int producer_get_frame( mlt_producer producer, mlt_frame_ptr frame, int index )
{
  // Create an empty frame
  *frame = mlt_frame_init( MLT_PRODUCER_SERVICE( producer ) );

  // Get the frames properties
  mlt_properties properties = MLT_FRAME_PROPERTIES( *frame );

  mlt_properties_set_int( properties, "test_image", 0 );
  //mlt_properties_set_int( properties, "test_audio", 0 );

  // Update other info on the frame
  // TODO: ??????
  mlt_properties_set_int( properties, "colorspace", 601 );

  //mlt_properties_set_int( properties, "progressive", dv_is_progressive( dv_decoder ) );
  //mlt_properties_set_double(properties, "aspect_ratio", (double)width / (double)height);

  producer_read_frame_data(producer, frame);

  // Push the get_image method onto the stack
  mlt_frame_push_get_image( *frame, producer_get_image );

  // Push the get_audio method onto the stack
  mlt_frame_push_audio( *frame, producer_get_audio );

  // Update timecode on the frame we're creating
  if ( *frame != NULL )
    mlt_frame_set_position( *frame, mlt_producer_position( producer ) );

  // Calculate the next timecode
  mlt_producer_prepare_next( producer );
  return 0;
}

static void producer_close( mlt_producer this )
{
  // Close the parent
  this->close = NULL;
  mlt_producer_close(this);
}
