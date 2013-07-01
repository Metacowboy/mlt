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
  // Get the frames properties
  mlt_properties properties = MLT_FRAME_PROPERTIES( this );

  void *readspace = mlt_frame_pop_service(this);
  pthread_rwlock_t *rwlock = mlt_frame_pop_service(this);


  uint32_t *header = readspace;
  void *data = readspace + sizeof(uint32_t[4]);

  int image_size = header[0];
  mlt_image_format fmt = header[1];
  // Assign width and height according to the frame
  *width = header[2];
  *height = header[3];

  if( *format == fmt ) {
    *buffer = (uint8_t*)mlt_pool_alloc(image_size);
    mlt_frame_set_image(this, *buffer, image_size, mlt_pool_release);
    memcpy(*buffer, data, image_size);
  } else {
    // well, I'll be damned!
  }

  // release read image lock
  pthread_rwlock_unlock(rwlock);

  return 0;
}
/*
static int producer_get_audio( mlt_frame this, void **buffer, mlt_audio_format *format, int *frequency, int *channels, int *samples )
{
  int16_t *p;
  int i, j;
  int16_t *audio_channels[ 4 ];

  // Get the frames properties
  mlt_properties properties = MLT_FRAME_PROPERTIES( this );

  // Get a dv_decoder
  dv_decoder_t *decoder = dv_decoder_alloc( );

  // Get the dv data
  uint8_t *dv_data = mlt_properties_get_data( properties, "dv_data", NULL );

  // Parse the header for meta info
  dv_parse_header( decoder, dv_data );

  // Check that we have audio
  if ( decoder->audio->num_channels > 0 )
    {
      int size = *channels * DV_AUDIO_MAX_SAMPLES * sizeof( int16_t );

      // Obtain required values
      *frequency = decoder->audio->frequency;
      *samples = decoder->audio->samples_this_frame;
      *channels = decoder->audio->num_channels;
      *format = mlt_audio_s16;

      // Create a temporary workspace
      for ( i = 0; i < 4; i++ )
        audio_channels[ i ] = mlt_pool_alloc( DV_AUDIO_MAX_SAMPLES * sizeof( int16_t ) );

      // Create a workspace for the result
      *buffer = mlt_pool_alloc( size );

      // Pass the allocated audio buffer as a property
      mlt_frame_set_audio( this, *buffer, *format, size, mlt_pool_release );

      // Decode the audio
      dv_decode_full_audio( decoder, dv_data, audio_channels );

      // Interleave the audio
      p = *buffer;
      for ( i = 0; i < *samples; i++ )
        for ( j = 0; j < *channels; j++ )
          *p++ = audio_channels[ j ][ i ];

      // Free the temporary work space
      for ( i = 0; i < 4; i++ )
        mlt_pool_release( audio_channels[ i ] );
    }
  else
    {
      // No audio available on the frame, so get test audio (silence)
      mlt_frame_get_audio( this, buffer, format, frequency, channels, samples );
    }

  // Return the decoder
  dv_decoder_return( decoder );

  return 0;
}
*/
static int producer_get_frame( mlt_producer producer, mlt_frame_ptr frame, int index )
{
  mlt_properties proc_properties = MLT_PRODUCER_PROPERTIES(producer);
  // lock
  pthread_rwlock_t *rwlock = (pthread_rwlock_t*)mlt_properties_get_data(proc_properties, "_rwlock", NULL);
  void *readspace = mlt_properties_get_data(proc_properties, "_readspace", NULL);
  uint32_t *header = readspace;

  // Create an empty frame
  *frame = mlt_frame_init( MLT_PRODUCER_SERVICE( producer ) );

  // wait until I can read
  pthread_rwlock_rdlock(rwlock);

  // Get the frames properties
  mlt_properties properties = MLT_FRAME_PROPERTIES( *frame );

  mlt_properties_set_int( properties, "test_image", 0 );
  //mlt_properties_set_int( properties, "test_audio", 0 );

  // Update other info on the frame
  int width = header[2],
    height = header[3];
  mlt_properties_set_int( properties, "width", width);
  mlt_properties_set_int( properties, "height", height);
  // TODO: ??????
  mlt_properties_set_int( properties, "colorspace", 601 );

  //mlt_properties_set_int( properties, "progressive", dv_is_progressive( dv_decoder ) );
  mlt_properties_set_double(properties, "aspect_ratio", (double)width / (double)height);

  // push the lock
  mlt_frame_push_service(*frame, rwlock);
  // and the share
  mlt_frame_push_service(*frame, readspace);

  /* audio stuff */
  /*
  mlt_properties_set_int( properties, "audio_frequency", dv_decoder->audio->frequency );
  mlt_properties_set_int( properties, "audio_channels", dv_decoder->audio->num_channels );

  // Register audio callback
  if ( mlt_properties_get_int( MLT_PRODUCER_PROPERTIES( producer ), "audio_index" ) > 0 ) {
    // we get a second lock for get_audio which will be released there
    pthread_rwlock_rdlock(rwlock);
    mlt_frame_push_audio( *frame, producer_get_audio );
  }
  */

  // Push the get_image method on to the stack
  // we get a second lock for get_image which will be released there
  pthread_rwlock_rdlock(rwlock);
  mlt_frame_push_get_image( *frame, producer_get_image );

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
