/*
  (C) 2006 Z RESEARCH Inc. <http://www.zresearch.com>
  
  This program is free software; you can redistribute it and/or
  modify it under the terms of the GNU General Public License as
  published by the Free Software Foundation; either version 2 of
  the License, or (at your option) any later version.
    
  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
  GNU General Public License for more details.
    
  You should have received a copy of the GNU General Public
  License along with this program; if not, write to the Free
  Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
  Boston, MA 02110-1301 USA
*/ 


#include <stdint.h>
#include <signal.h>

#include "glusterfs.h"
#include "logging.h"
#include "xlator.h"
#include "glusterfs.h"
#include "transport.h"

#include <fuse.h>
#include <fuse/fuse_lowlevel.h>

#include "fuse-internals.h"

struct fuse_private {
  int fd;
  struct fuse *fuse;
  struct fuse_session *se;
  struct fuse_chan *ch;
  char *buf;
  size_t bufsize;
  char *mountpoint;
};

static int32_t
fuse_transport_flush (transport_t *this)
{
  return 0;
}

static int32_t
fuse_transport_disconnect (transport_t *this)
{
  struct fuse_private *priv = this->private;

  gf_log ("glusterfs-fuse",
	  GF_LOG_DEBUG,
	  "cleaning up fuse transport in disconnect handler");

  free(priv->buf);
  fuse_session_reset(priv->se);
  fuse_session_exit (priv->se);
#if 1 /* fuse_version >= 2.6 */
  fuse_teardown(priv->fuse,
		priv->mountpoint);
#endif
  free (priv);
  this->private = NULL;

  /* TODO: need graceful exit. every xlator should be ->fini()'ed
     and come out of main poll loop cleanly
  */
  exit (0);

  return -1;
}

static int32_t
fuse_transport_recieve (transport_t *this,
			char *buf,
			int32_t len)
{
  return 0;
}

static int32_t
fuse_transport_submit (transport_t *this,
		       char *buf,
		       int32_t len)
{
  return 0;
}

static int32_t
fuse_transport_except (transport_t *this)
{
  struct fuse_private *priv = this->private;
  fuse_session_exit (priv->se);

  return -1;
}

static int32_t
fuse_transport_init (transport_t *this,
		     dict_t *options,
		     int32_t (*notify) (xlator_t *xl,
					transport_t *trans,
					int32_t event))
{
  char *mountpoint = strdup (data_to_str (dict_get (options, 
						    "mountpoint")));
  char *source;
  asprintf (&source, "fsname=glusterfs:%d", getpid ());
  char *argv[] = { "glusterfs",
                   "-o", "nonempty",
                   "-o", "allow_other",
                   "-o", "default_permissions",
		   "-o", source,
		   "-o", "suid",
		   "-o", "dev",
		   "-o", "max_readahead=1048576",
		   "-o", "max_read=1048576",
		   "-o", "max_write=1048576",
                   NULL };
  int argc = 17;

  struct fuse_args args = FUSE_ARGS_INIT(argc,
					 argv);
  int fd;
  struct fuse *fuse;
  struct fuse_private *priv = calloc (1, sizeof (*priv));
  int32_t res;

  this->notify = notify;
  this->private = (void *)priv;

  priv->ch = fuse_mount(mountpoint, &args);
  if (!priv->ch) {
    gf_log ("fuse", GF_LOG_ERROR, "fuse_mount failed (%s)\n", strerror (errno));
    fuse_opt_free_args(&args);
    goto err_free;
  }

  fuse = glusterfs_fuse_new_common (priv->ch, &args);
  fuse_opt_free_args(&args);

  if (fuse == NULL) {
    gf_log ("fuse", GF_LOG_ERROR, "fuse_new_common failed");
    goto err_unmount;
  }

  res = fuse_set_signal_handlers(fuse->se);
  if (res == -1) {
    gf_log ("fuse", GF_LOG_ERROR, "fuse_set_signal_handlers failed");
    goto err_destroy;
  }

  fd = fuse_chan_fd (priv->ch);
  priv->fd = fd;
  priv->fuse = (void *)fuse;
  priv->se = fuse->se;
  this->buf = data_ref (data_from_dynptr (malloc (fuse_chan_bufsize (priv->ch)),
					  fuse_chan_bufsize (priv->ch)));
  this->buf->lock = calloc (1, sizeof (pthread_mutex_t));
  pthread_mutex_init (this->buf->lock, NULL);

  priv->mountpoint = mountpoint;
  fuse->user_data = this;//->xl;

  if (!this->buf) {
    gf_log ("fuse", GF_LOG_ERROR, "failed to allocate read buffer");
    goto err_destroy;
  }

  poll_register (this->xl_private, fd, this);

  return 0;

 err_destroy:
    fuse_destroy(fuse);
 err_unmount:
    fuse_unmount(mountpoint, priv->ch);
 err_free:
    free(mountpoint);
  return -1;
}

static void
fuse_transport_fini (transport_t *this)
{
  /*
  struct fuse_private *priv = this->private;

  free(priv->buf);
  fuse_session_reset(priv->se);
  fuse_teardown(priv->fuse,
		priv->fd,
		priv->mountpoint);
  free (priv);
  this->private = NULL;
  */
  return;
}


static int32_t
fuse_transport_notify (xlator_t *xl,
		       transport_t *trans,
		       int32_t event)
{
  struct fuse_private *priv = trans->private;
  int32_t res = 0;
  data_t *buf;
  int32_t ref = 0;

  if (!((event & POLLIN) || (event & POLLPRI)))
    return 0;

  if (!fuse_session_exited(priv->se)) {
    if (priv->fuse->conf.debug)
      printf ("ACTIVITY /dev/fuse\n");
    int32_t fuse_chan_receive (struct fuse_chan * ch,
			       char *buf,
			       int32_t size);
    buf = trans->buf;
    res = fuse_chan_receive (priv->ch,
			     buf->data,
			     buf->len);
    /*    if (res == -1) {
      transport_destroy (trans);
    */
    if (res && res != -1) {
      fuse_session_process (priv->se,
			    buf->data,
			    res,
			    priv->ch);
    }

    pthread_mutex_lock (buf->lock);
    ref = buf->refcount;
    pthread_mutex_unlock (buf->lock);
    /* TODO do the check with a lock */
    if (ref > 1) {
      data_unref (buf);

      //      trans->buf = data_ref (data_from_dynptr (malloc (fuse_chan_bufsize (priv->ch)),
      trans->buf = data_ref (data_from_dynptr (malloc (131072),
					      fuse_chan_bufsize (priv->ch)));
      if (!trans->buf->data)
	perror ("OUCH!!!");

      trans->buf->lock = calloc (1, sizeof (pthread_mutex_t));
      pthread_mutex_init (trans->buf->lock, NULL);
    }
  } 

  /*
  if (fuse_session_exited (priv->se)) {
    transport_destroy (trans);
    res = -1;
    }*/

  return res >= 0 ? 0 : res;
}

static struct transport_ops fuse_transport_ops = {
  .flush = fuse_transport_flush,
  .recieve = fuse_transport_recieve,
  .submit = fuse_transport_submit,
  .disconnect = fuse_transport_disconnect,
  .except = fuse_transport_except
};

static transport_t fuse_transport = {
  .ops = &fuse_transport_ops,
  .private = NULL,
  .xl = NULL,
  .init = fuse_transport_init,
  .fini = fuse_transport_fini,
  .notify = fuse_transport_notify
};


transport_t *
glusterfs_mount (glusterfs_ctx_t *ctx,
		 const char *mount_point)
{
  dict_t *options = get_new_dict ();
  transport_t *new_fuse = calloc (1, sizeof (*new_fuse));

  memcpy (new_fuse, &fuse_transport, sizeof (*new_fuse));
  new_fuse->ops = &fuse_transport_ops;
  new_fuse->xl_private = ctx;

  dict_set (options,
	    "mountpoint", 
	    str_to_data ((char *)mount_point));

  return (new_fuse->init (new_fuse,
			  options,
			  fuse_transport_notify) == 0 ? new_fuse : NULL);
}

