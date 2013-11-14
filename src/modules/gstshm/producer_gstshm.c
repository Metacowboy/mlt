/*
 * producer_gstshm.c -- a melted producer that grabs
 * frame data from POSIX shared memory that is compatible
 * with the GStreamer protocol.
 *
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

#include <glib.h>

#include "common.h"

void dummy_destructor(void *object);
static gboolean pipe_callback(GIOChannel *source, GIOCondition condition, gpointer data);
gboolean _reconnect_idle(gpointer data);
void try_reconnect(mlt_producer this);
void remove_io_channel(mlt_producer this);
static int producer_get_frame( mlt_producer parent, mlt_frame_ptr frame, int index );
static void producer_close( mlt_producer parent );
static void* producer_thread(void *arg);


ShmPipe *create_shm_pipe(mlt_producer this, gboolean silent)
{
  mlt_properties properties = MLT_PRODUCER_PROPERTIES( this );

  char *path = mlt_properties_get(properties, "resource");
  ShmPipe *shmpipe = path ? sp_client_open(path) : NULL;
  mlt_properties_set_data(properties, "_shmpipe", shmpipe, 0, NULL, NULL);

  if (!silent) {
    if (!shmpipe) {
      write_log(0, "Can't open control socket at: %s\n", path);
    } else {
      write_log(0, "Control socket opened at: %s\n", path);
    }
  }
  return shmpipe;
}

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
      mlt_properties_set(properties, "resource", "/dev/shm/mlt.shm");


    // open shared memory
    ShmPipe *shmpipe = NULL;
    shmpipe = create_shm_pipe(this, FALSE);
    if (!shmpipe) {
      mlt_producer_close(this);
      return NULL;
    }
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
    write_log(0, "GstSHM init ok. Thread id: %li", pthread_self());
    return this;
  }

  free( this );
  return NULL;
}

static void producer_read_frame_data(mlt_producer this, mlt_frame_ptr frame, char *readspace) {
  // get the producer properties
  mlt_properties properties = MLT_PRODUCER_PROPERTIES(this);
  // Get the frames properties
  mlt_properties frame_props = MLT_FRAME_PROPERTIES(*frame);

  int last_frame = mlt_properties_get_int(properties, "_last_frame");

  struct posix_shm_header *header = (void *)readspace;
  void *data = readspace + sizeof(struct posix_shm_header);

  if (last_frame == -1) {
    mlt_properties_set_int(frame_props, "_consecutive", 1);
  } else {
    mlt_properties_set_int(frame_props, "_consecutive", header->frame == (last_frame + 1));
  }
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

  data += image_size;
  buffer = mlt_pool_alloc(audio_size);
  memcpy(buffer, data, audio_size);
  mlt_frame_set_audio(*frame, buffer, afmt, audio_size, mlt_pool_release);


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

void remove_io_channel(mlt_producer this)
{
  mlt_properties properties = MLT_PRODUCER_PROPERTIES(this);

  GIOChannel *iochannel = mlt_properties_get_data(properties, "_iochannel", NULL);
  if (iochannel) {
    g_io_channel_shutdown(iochannel, FALSE, NULL);
    g_io_channel_unref(iochannel);
  }

  guint watch_id = mlt_properties_get_int(properties, "_watchid");
  if (watch_id)
    g_source_remove(watch_id);
  mlt_properties_set_data(properties, "_iochannel", NULL, 0, NULL, NULL);
}

gboolean _reconnect_idle(gpointer data)
{
  mlt_producer this = data;
  mlt_properties properties = MLT_PRODUCER_PROPERTIES(this);

  if (!mlt_properties_get_int(properties, "_reconnecting"))
    return FALSE;

  ShmPipe *shmpipe = create_shm_pipe(this, TRUE);
  if (!shmpipe)
    return TRUE;

  int fd = sp_get_fd(shmpipe);
  GIOChannel *iochannel = g_io_channel_unix_new(fd);

  mlt_properties_set_data(properties, "_iochannel", iochannel, 0, dummy_destructor, NULL);

  guint id = g_io_add_watch(iochannel, G_IO_IN, pipe_callback, this);
  mlt_properties_set_int(properties, "_watchid", id);

  mlt_properties_set_int(properties, "_reconnecting", 0);
  return FALSE;
}

void try_reconnect(mlt_producer this)
{
  mlt_properties properties = MLT_PRODUCER_PROPERTIES(this);

  if (mlt_properties_get_int(properties, "_reconnecting"))
    return;

  mlt_properties_set_int(properties, "_reconnecting", 1);

  remove_io_channel(this);

  ShmPipe *shmpipe = mlt_properties_get_data(properties, "_shmpipe", NULL);
  mlt_properties_set_data(properties, "_shmpipe", NULL, 0, NULL, NULL);
  if (shmpipe) {
    sp_client_close(shmpipe); // already takes care of freeing resources.
  }


  write_log(1, "Trying to reconnect...\n");
  g_idle_add(_reconnect_idle, this);
}

static gboolean pipe_callback(GIOChannel *source, GIOCondition condition, gpointer data)
{
  mlt_producer this = (mlt_producer)data;
  mlt_properties properties = MLT_PRODUCER_PROPERTIES(this);
  pthread_mutex_t *mutex = mlt_properties_get_data(properties, "_queue_mutex", NULL);
  pthread_cond_t  *cond  = mlt_properties_get_data(properties, "_queue_cond", NULL);
  mlt_deque queue = mlt_properties_get_data(properties, "_queue", NULL);

  ShmPipe *shmpipe = mlt_properties_get_data(properties, "_shmpipe", NULL);

  if (!shmpipe) {
    return FALSE;
  }

  char *buffer = NULL;
  long int size = sp_client_recv(shmpipe, &buffer);
  if (size == 0) {
    // control message, handled internally.
    return TRUE;
  } else if (size == -1) {
    if (buffer) {
      sp_client_recv_finish(shmpipe, buffer);
    }
    try_reconnect(this);
    return TRUE;
  }

  if (!buffer) {
    return TRUE;
  }

  if (mlt_properties_get_int(properties, "_running")) {

    // Sleep until buffer consumption begins
    // XXX: this may/will block other readers.
    pthread_mutex_lock(mutex);
    if (mlt_deque_count(queue) >= 25) {
      write_log(1, "Wait buffer consumption!");
      pthread_cond_wait(cond, mutex);
      write_log(1, "Buffer consumption started!");
    }

    mlt_frame frame = mlt_frame_init(MLT_PRODUCER_SERVICE(this));
    producer_read_frame_data(this, &frame, buffer);

    if (!mlt_properties_get_int(MLT_FRAME_PROPERTIES(frame), "_consecutive")) {
      write_log(1, "Frame number not consecutive, flushing!");
      while (mlt_deque_count(queue)) {
        mlt_frame_close(mlt_deque_pop_front(queue));
      }
    }

    mlt_deque_push_back(queue, frame);
    pthread_cond_broadcast(cond);
    pthread_mutex_unlock(mutex);
  } else {
  }

  if (buffer) {
    sp_client_recv_finish(shmpipe, buffer);
  }
  return TRUE;

}

void dummy_destructor(void *object)
{
  write_log(0,"called dummy_destructor from: %li", pthread_self());
}

static void* producer_thread(void *arg) {
  mlt_producer this = arg;
  mlt_properties properties = MLT_PRODUCER_PROPERTIES(this);

  ShmPipe *shmpipe = mlt_properties_get_data(properties, "_shmpipe", NULL);
  GMainLoop *loop = g_main_loop_new(NULL, FALSE);

  if (loop) {
    int fd = sp_get_fd(shmpipe);
    GIOChannel *iochannel = g_io_channel_unix_new(fd);

    guint id = g_io_add_watch(iochannel, G_IO_IN, pipe_callback, this);
    mlt_properties_set_int(properties, "_running", 1);
    mlt_properties_set_int(properties, "_watchid", id);
    mlt_properties_set_data(properties, "_loop", loop, 0, dummy_destructor, NULL);
    mlt_properties_set_data(properties, "_iochannel", iochannel, 0, dummy_destructor, NULL);
    g_main_loop_run(loop);
  } else {
    write_log(1, "Error creating glib main loop");
  }

  write_log(1, "GstSHM Thread Finish! id: %li", pthread_self());
  pthread_exit(NULL);
}


static int producer_get_frame( mlt_producer producer, mlt_frame_ptr frame, int index )
{
  mlt_properties prod_props = MLT_PRODUCER_PROPERTIES(producer);

  mlt_deque queue = mlt_properties_get_data(prod_props, "_queue", NULL);
  pthread_mutex_t *mutex = mlt_properties_get_data(prod_props, "_queue_mutex", NULL);
  pthread_cond_t *cond = mlt_properties_get_data(prod_props, "_queue_cond", NULL);

  // Try to get frame from cache
  mlt_position position = mlt_producer_position( producer );

  pthread_mutex_lock(mutex);
  while(mlt_deque_count(queue) < 1) {
    pthread_cond_wait(cond, mutex);
  }
  *frame = (mlt_frame)mlt_deque_pop_front(queue);

  mlt_frame_set_position( *frame, position );

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
  // XXX write_log(0, "After push_get_image()\n");

  // Push the get_audio method onto the stack
  mlt_frame_push_audio(*frame, mlt_frame_get_audio);

  // Calculate the next timecode
  mlt_producer_prepare_next( producer );
  pthread_cond_broadcast(cond);
  pthread_mutex_unlock(mutex);
  write_log(1, "Signal consumption          %li\n", pthread_self());
  return 0;
}

static void producer_close( mlt_producer this )
{
  write_log(0, "GstSHM Closing producer thread: %li", pthread_self());

  // Close the parent
  this->close = NULL;
  mlt_properties properties = MLT_PRODUCER_PROPERTIES(this);

  remove_io_channel(this);

  GMainLoop *loop = mlt_properties_get_data(properties, "_loop", NULL);
  if (loop) {
    g_main_loop_quit(loop);
    g_main_loop_unref(loop);
  }

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
    write_log(0, "GstSHM about call pthread_join()\n");
    pthread_join( *thread, NULL );
  }

  ShmPipe *shmpipe = mlt_properties_get_data(properties, "_shmpipe", NULL);
  if (shmpipe)
    sp_client_close(shmpipe); // already takes care of freeing resources.

  // Destroy thread attributes
  pthread_attr_t *attr = mlt_properties_get_data(properties, "_thread_attr", NULL);
  if (attr)
      pthread_attr_destroy(attr);

  write_log(0, "GstSHM about to close! tid: %li\n", pthread_self());
  mlt_producer_close(this);
  write_log(0, "GstSHM Finish! tid: %li\n", pthread_self());
}
