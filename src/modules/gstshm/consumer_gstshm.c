/*
 * consumer_gstshm.c -- a melted consumer that copies
 * frame data onto POSIX shared memory that is compatible
 * with the GStreamer protocol.
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
#include <errno.h>

#include <glib.h>
#include <glib/gstdio.h>

#include "common.h"

// Forward references.
static int consumer_start( mlt_consumer this );
static int consumer_stop( mlt_consumer this );
static int consumer_is_stopped( mlt_consumer this );
static void consumer_output( mlt_consumer this, int size, mlt_frame frame );
static void *consumer_thread( void *arg );
static void consumer_close( mlt_consumer this );

static gboolean shm_client_read_cb(GIOChannel *source, GIOCondition condition, gpointer data);
void   buffer_free_callback(void *tag, void *user_data);
static gboolean shm_client_error_cb(GIOChannel *source, GIOCondition condition, gpointer data);
static gboolean shm_read_cb(GIOChannel *source, GIOCondition condition, gpointer data);
static gboolean shm_error_cb(GIOChannel *source, GIOCondition condition, gpointer data);

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
      mlt_properties_set( properties, "target", "/dev/shm/mlt.shm" );

    mlt_properties_set_data(properties, "_shmpipe", NULL, 0, NULL, NULL);

    pthread_mutex_t *mutex = malloc(sizeof(pthread_mutex_t));
    pthread_mutex_init(mutex, NULL);
    mlt_properties_set_data(properties, "_shm_mutex", mutex, sizeof(pthread_mutex_t), free, NULL);

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
    int memsize = 0;
    memsize += sizeof(struct posix_shm_header);
    memsize += mlt_image_format_size(ifmt, width, height, NULL); // image size
    memsize += mlt_audio_format_size(afmt, samples, channels); // audio size
    memsize += 32;


    // all the shared memory space
    mlt_properties_set_int(properties, "_shareSize", memsize);

    // Allocate a thread
    pthread_t *thread = calloc( 1, sizeof( pthread_t ) );

    // Assign the thread to properties
    mlt_properties_set_data( properties, "thread", thread, sizeof( pthread_t ), free, NULL );

    char *target = mlt_properties_get(properties, "target");
    if (g_file_test(target, G_FILE_TEST_EXISTS)) {
        write_log(0, "Control socket at %s already exists, unlinking.\n", target);
        g_unlink(target);
    }

    ShmPipe *shmpipe = NULL;
    shmpipe = sp_writer_create(target, 30*memsize, 0777);
    if (!shmpipe) {
      mlt_consumer_close(this);
      write_log(0, "Can't open control socket");
      return 1;
    }

    write_log(0, "Created socket at: %s", sp_writer_get_path(shmpipe));
    mlt_properties_set_data(properties, "_shmpipe", shmpipe, 0, NULL, NULL);

    GHashTable *fdtoclient = g_hash_table_new(NULL, NULL);
    mlt_properties_set_data(properties, "_fdtoclient", fdtoclient, 0,  NULL, NULL);

    GHashTable *fdtowatch = g_hash_table_new(NULL, NULL);
    mlt_properties_set_data(properties, "_fdtowatch", fdtowatch, 0,  NULL, NULL);

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

static void consumer_output( mlt_consumer this, int size, mlt_frame frame ) {
#ifdef GSTSHM_DEBUG_TIME
  struct timespec starttime;
  struct timespec endtime;
  float  tdelta;
  clock_gettime(CLOCK_REALTIME, &starttime);
#endif

  // Get the properties
  mlt_properties properties = MLT_CONSUMER_PROPERTIES( this );
  mlt_properties fprops = MLT_FRAME_PROPERTIES(frame);

  int fr_num = mlt_properties_get_int(properties, "frame_rate_num");
  int fr_den = mlt_properties_get_int(properties, "frame_rate_den");
  mlt_image_format ifmt = mlt_properties_get_int(properties, "mlt_image_format");
  int width = mlt_properties_get_int(properties, "width");
  int height = mlt_properties_get_int(properties, "height");
  int32_t frameno = mlt_consumer_position(this);
  uint8_t *image=NULL;
  mlt_frame_get_image(frame, &image, &ifmt, &width, &height, 0);
  int image_size = mlt_image_format_size(ifmt, width, height, NULL);

  size_t memsize = mlt_properties_get_int(properties, "_shareSize");
  ShmPipe  *shmpipe = mlt_properties_get_data(properties, "_shmpipe", NULL);

  ShmBlock *block = sp_writer_alloc_block(shmpipe, memsize);
  if (!block) {
    goto alloc_error;
  }

  void *share = sp_writer_block_get_buf(block);
  void *walk = share;

  struct posix_shm_header *header = (struct posix_shm_header*) walk;
  walk += sizeof(struct posix_shm_header);

  header->frame = frameno;
  header->frame_rate_num = fr_num;
  header->frame_rate_den = fr_den;
  header->image_size = image_size;
  header->image_format = ifmt;
  header->width = width;
  header->height = height;

  memcpy(walk, image, image_size);
  walk += image_size;

  // try to get the format defined by the consumer
  mlt_audio_format afmt = mlt_properties_get_int(properties, "mlt_audio_format");
  // all other data provided by the producer
  int frequency = mlt_properties_get_int(fprops, "audio_frequency");
  int channels = mlt_properties_get_int(fprops, "audio_channels");
  int samples = mlt_properties_get_int(fprops, "audio_samples");
  void *audio=NULL;
  mlt_frame_get_audio(frame, &audio, &afmt, &frequency, &channels, &samples);
  int audio_size = mlt_audio_format_size(afmt, samples, channels);

  header->audio_size = audio_size;
  header->audio_format = afmt;
  header->frequency = frequency;
  header->channels = channels;
  header->samples = samples;

  memcpy(walk, audio, audio_size);

  int rv = sp_writer_send_buf(shmpipe, share, memsize, block);

  if (rv == 0) {
    //write_log(1, "No clients connected, unreffing buffer");
    sp_writer_free_block(block);
  } else if (rv == -1) {
    write_log(1, "Invalid allocated buffer. The shmpipe library rejects our buffer, this is a bug");
  } else {
#ifdef GSTSHM_DEBUG
    log_header(header);
    write_log(1, "sent frame: %li block: %p memsize: %li width: %i, height: %i , samples: %li\n", frameno, block, memsize, width, height, samples);
#endif
  }

#ifdef GSTSHM_DEBUG_TIME
    clock_gettime(CLOCK_REALTIME, &endtime);
    tdelta = ((float)(endtime.tv_nsec - starttime.tv_nsec)) / 1000000.0f + 1000*(endtime.tv_sec - starttime.tv_sec);

    if (tdelta > 4.0)
      write_log(1, "send_buf(): loop time: %f frame no:%i\n", tdelta, frameno);
#endif


alloc_error:
  //write_log(1, "ALLOC ERROR\n");
  return;
}

static gboolean shm_client_read_cb(GIOChannel *source, GIOCondition condition, gpointer data)
{
  mlt_producer this = (mlt_producer)data;
  mlt_properties properties = MLT_PRODUCER_PROPERTIES(this);

  pthread_mutex_t *mutex = mlt_properties_get_data(properties, "_shm_mutex", NULL);
  ShmPipe    *shmpipe    = mlt_properties_get_data(properties, "_shmpipe", NULL);
  GHashTable *fdtoclient = mlt_properties_get_data(properties, "_fdtoclient", NULL);

  pthread_mutex_lock(mutex);

  int fd = g_io_channel_unix_get_fd(source);
  ShmClient  *client = g_hash_table_lookup(fdtoclient, GINT_TO_POINTER(fd));

  ShmBlock *block = NULL;
  int rv = sp_writer_recv(shmpipe, client, (void **)&block);
#ifdef GSTSHM_DEBUG
  write_log(1, "Client read rv: %i, block: %p\n", rv, block);
#endif
  if (rv == 0 && block) {
    sp_writer_free_block(block);
  }

  pthread_mutex_unlock(mutex);

  return TRUE;
}

void buffer_free_callback(void *tag, void *user_data)
{
  sp_writer_free_block(tag);
  write_log(1, "BUFFER FREE CB\n");
}

static gboolean shm_client_error_cb(GIOChannel *source, GIOCondition condition, gpointer data)
{
  write_log(1, "CLIENT ERROR CB\n");
  mlt_producer this = (mlt_producer)data;
  mlt_properties properties = MLT_PRODUCER_PROPERTIES(this);

  ShmPipe    *shmpipe    = mlt_properties_get_data(properties, "_shmpipe", NULL);
  pthread_mutex_t *mutex = mlt_properties_get_data(properties, "_shm_mutex", NULL);
  GHashTable *fdtoclient = mlt_properties_get_data(properties, "_fdtoclient", NULL);
  GHashTable *fdtowatch  = mlt_properties_get_data(properties, "_fdtowatch", NULL);

  int fd = g_io_channel_unix_get_fd(source);
  ShmClient  *client = g_hash_table_lookup(fdtoclient, GINT_TO_POINTER(fd));

  pthread_mutex_lock(mutex);
  sp_writer_close_client(shmpipe, client, buffer_free_callback, NULL);
  pthread_mutex_unlock(mutex);

  g_hash_table_remove(fdtoclient, GINT_TO_POINTER(fd));

  guint id = GPOINTER_TO_UINT(g_hash_table_lookup(fdtowatch, GINT_TO_POINTER(fd)));
  g_source_remove(id);
  g_hash_table_remove(fdtowatch, GINT_TO_POINTER(fd));
  return FALSE;
}

static gboolean shm_read_cb(GIOChannel *source, GIOCondition condition, gpointer data)
{
  mlt_producer this = (mlt_producer)data;
  mlt_properties properties = MLT_PRODUCER_PROPERTIES(this);

  ShmPipe *shmpipe = mlt_properties_get_data(properties, "_shmpipe", NULL);
  pthread_mutex_t *mutex = mlt_properties_get_data(properties, "_shm_mutex", NULL);
  GHashTable *fdtoclient = mlt_properties_get_data(properties, "_fdtoclient", NULL);
  GHashTable *fdtowatch  = mlt_properties_get_data(properties, "_fdtowatch", NULL);

  pthread_mutex_lock(mutex);
  ShmClient *client = sp_writer_accept_client(shmpipe);

  int fd = sp_writer_get_client_fd(client);
  GIOChannel *iochannel = g_io_channel_unix_new(fd);

  g_hash_table_insert(fdtoclient, GINT_TO_POINTER(fd), client);
  guint id = g_io_add_watch(iochannel, G_IO_IN, shm_client_read_cb, this);
  g_hash_table_insert(fdtowatch, GINT_TO_POINTER(fd), GUINT_TO_POINTER(id));
  g_io_add_watch(iochannel, G_IO_ERR | G_IO_HUP | G_IO_NVAL, shm_client_error_cb, this);

  pthread_mutex_unlock(mutex);

  write_log(1, "New client, fd: %i", fd);
  return TRUE;
}

static gboolean shm_error_cb(GIOChannel *source, GIOCondition condition, gpointer data)
{
  mlt_producer this = (mlt_producer)data;
  mlt_properties properties = MLT_PRODUCER_PROPERTIES(this);

  ShmPipe *shmpipe = mlt_properties_get_data(properties, "_shmpipe", NULL);
  pthread_mutex_t *mutex = mlt_properties_get_data(properties, "_shm_mutex", NULL);

  pthread_mutex_lock(mutex);
  mlt_properties_set_data(properties, "_shmpipe", NULL, 0, NULL, NULL);

  sp_writer_close(shmpipe, NULL, NULL);
  pthread_mutex_unlock(mutex);

  return TRUE;
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
  pthread_mutex_t *mutex = mlt_properties_get_data(properties, "_shm_mutex", NULL);


  // Get the handling methods
  int ( *output )( mlt_consumer, int, mlt_frame ) = mlt_properties_get_data( properties, "output", NULL );

  // Frame and size
  mlt_frame frame = NULL;

  // shared memory info
  int size = 0;
  int fr_den = mlt_properties_get_int(properties, "frame_rate_den");
  int fr_num = mlt_properties_get_int(properties, "frame_rate_num");

  struct timespec sleeptime;
  struct timespec starttime;

  clock_gettime(CLOCK_REALTIME, &starttime);
  uint64_t nanosec = starttime.tv_sec;
  nanosec *= 1000000000;
  nanosec += starttime.tv_nsec;

  //uint64_t frametime = (fr_den * 1000000000) / fr_num;
  uint64_t frametime = fr_den;
  frametime *= 1000000000;
  frametime /= fr_num;

  GMainContext *context = g_main_context_default();
  ShmPipe *shmpipe = mlt_properties_get_data(properties, "_shmpipe", NULL);
  GIOChannel *iochannel = g_io_channel_unix_new(sp_get_fd(shmpipe));

  g_io_add_watch(iochannel, G_IO_IN, shm_read_cb, this);
  g_io_add_watch(iochannel, G_IO_ERR | G_IO_HUP, shm_error_cb, this);

  // Loop while running
  while( mlt_properties_get_int( properties, "running" ) ) {
    // Get the frame
    frame = mlt_consumer_rt_frame( this );

    // Check that we have a frame to work with
    if ( frame != NULL ) {
      // Terminate on pause
      if ( top && mlt_properties_get_double( MLT_FRAME_PROPERTIES( frame ), "_speed" ) == 0 ) {
        mlt_frame_close( frame );
        break;
      }

      pthread_mutex_lock(mutex);
      output( this, size, frame );
      pthread_mutex_unlock(mutex);

      mlt_events_fire( properties, "consumer-frame-show", frame, NULL );
      mlt_frame_close(frame);
    }

    nanosec += frametime;
    sleeptime.tv_sec = nanosec / 1000000000;
    sleeptime.tv_nsec = nanosec % 1000000000;
    clock_nanosleep(CLOCK_REALTIME, TIMER_ABSTIME, &sleeptime, NULL);

    while(g_main_context_pending(context)) {
      g_main_context_iteration(context, FALSE);
    }
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

  write_log(0, "Finish!\n");
}
