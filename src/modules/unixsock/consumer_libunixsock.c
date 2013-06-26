/*
 * consumer_libunixsock.c -- a DV encoder based on libunixsock
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

// mlt Header files
#include <framework/mlt.h>

// System header files
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sys/shm.h>
#include <fcntl.h>
#include <unistd.h>

// Forward references.
static int consumer_start( mlt_consumer this );
static int consumer_stop( mlt_consumer this );
static int consumer_is_stopped( mlt_consumer this );
static void consumer_output( mlt_consumer this, uint8_t *dv_frame, int size, mlt_frame frame );
static void *consumer_thread( void *arg );
static void consumer_close( mlt_consumer this );

/** Initialise the unixsock consumer.
 */

mlt_consumer consumer_libunixsock_init( mlt_profile profile, mlt_service_type type, const char *id, char *arg ) {
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
      mlt_properties_set( properties, "target", "/tmp/unixsock.mlt" );

    // Set the output handling method
    mlt_properties_set_data( properties, "output", consumer_output, 0, NULL, NULL );

    // Terminate at end of the stream by default
    mlt_properties_set_int( properties, "terminate_on_pause", 0 );

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
    mlt_image_format fmt = mlt_image_yuv422;
    int width = mlt_properties_get_int( properties, "width");
    int height = mlt_properties_get_int( properties, "height");

    if( width <= 0 || height <= 0 ) {
      width = 1920;
      height = 1080;
      mlt_properties_set_int(properties, "width", width);
      mlt_properties_set_int(properties, "height", height);
    }

    int memsize = mlt_image_format_size(fmt, width, height, NULL);
    // initialize shared memory
    char *keyfile = mlt_properties_get(properties, "target");
    // make sure the keyfile exists
    int desc = open(keyfile, O_CREAT | O_RDWR, S_IRWXU);
    close(desc);
    // get shared memory unique key number
    key_t sharedKey = ftok(keyfile, 1);
    int shareId = shmget(sharedKey, memsize, IPC_CREAT | S_IRUSR | S_IWUSR);
    void *share = shmat(shareId, NULL, 0);
    mlt_properties_set_int(properties, "_shareSize", memsize);
    mlt_properties_set_int(properties, "_sharedKey", sharedKey);
    mlt_properties_set_int(properties, "_shareId", shareId);
    mlt_properties_set_data(properties, "_share", share, memsize, NULL, NULL);

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

      // Close the output file :-) - this is obtuse - doesn't matter if output file
      // exists or not - the destructor will kick in if it does
      mlt_properties_set_data( properties, "output_file", NULL, 0, NULL, NULL );
    }

  return 0;
}

/** Determine if the consumer is stopped.
 */

static int consumer_is_stopped( mlt_consumer this )
{
  // Get the properties
  mlt_properties properties = MLT_CONSUMER_PROPERTIES( this );
  return !mlt_properties_get_int( properties, "running" );
}

/** The libunixsock output method.
 */

static void consumer_output( mlt_consumer this, uint8_t *dv_frame, int size, mlt_frame frame )
{
  // Get the properties
  mlt_properties properties = MLT_CONSUMER_PROPERTIES( this );

  FILE *output = stdout;
  char *target = mlt_properties_get( properties, "target" );

  if ( target != NULL )
    {
      output = mlt_properties_get_data( properties, "output_file", NULL );
      if ( output == NULL )
        {
          output = fopen( target, "w" );
          if ( output != NULL )
            mlt_properties_set_data( properties, "output_file", output, 0, ( mlt_destructor )fclose, 0 );
        }
    }

  if ( output != NULL )
    {
      fwrite( dv_frame, size, 1, output );
      fflush( output );
    }
  else
    {
      fprintf( stderr, "Unable to open %s\n", target );
    }
}

/** The main thread - the argument is simply the consumer.
 */

static void *consumer_thread( void *arg )
{
  // Map the argument to the object
  mlt_consumer this = arg;

  // Get the properties
  mlt_properties properties = MLT_CONSUMER_PROPERTIES( this );

  // Get the terminate_on_pause property
  int top = mlt_properties_get_int( properties, "terminate_on_pause" );

  // Get the handling methods
  int ( *output )( mlt_consumer, uint8_t *, int, mlt_frame ) = mlt_properties_get_data( properties, "output", NULL );

  // Allocate a single PAL frame for encoding
  uint8_t *dv_frame = mlt_pool_alloc( 0 );

  // Frame and size
  mlt_frame frame = NULL;
  int size = 0;

  // Loop while running
  while( mlt_properties_get_int( properties, "running" ) )
    {
      // Get the frame
      frame = mlt_consumer_rt_frame( this );

      // Check that we have a frame to work with
      if ( frame != NULL )
        {
          // Terminate on pause
          if ( top && mlt_properties_get_double( MLT_FRAME_PROPERTIES( frame ), "_speed" ) == 0 )
            {
              mlt_frame_close( frame );
              break;
            }
        }
    }

  // Tidy up
  mlt_pool_release( dv_frame );

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

  // Free the memory
  free( this );
}
