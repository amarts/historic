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

#include "sdp_inet.h"

#include "dict.h"
#include "glusterfs.h"
#include "transport.h"
#include "protocol.h"
#include "logging.h"
#include "xlator.h"
#include "ib-verbs.h"

static int32_t  
do_handshake (transport_t *this, dict_t *options)
{
  GF_ERROR_IF_NULL (this);
  ib_verbs_private_t *priv = this->private;
  GF_ERROR_IF_NULL (priv);
  
  dict_t *request = get_new_dict ();
  dict_t *reply = get_new_dict ();
  int32_t ret;
  int32_t remote_errno;

  if (priv->is_debug) {
    FUNCTION_CALLED;
  }
  
  dict_set (request, 
	    "remote-subvolume",
	    dict_get (options, "remote-subvolume"));
  
  {
    int32_t dict_len = dict_serialized_length (request);
    char *dict_buf = malloc (dict_len);
    dict_serialize (request, dict_buf);

    gf_block *blk = gf_block_new (424242); /* "random" number */
    blk->type = OP_TYPE_MOP_REQUEST;
    blk->op = OP_SETVOLUME;
    blk->size = dict_len;
    blk->data = dict_buf;

    int32_t blk_len = gf_block_serialized_length (blk);
    char *blk_buf = malloc (blk_len);
    gf_block_serialize (blk, blk_buf);

    ret = ib_verbs_full_write (priv, blk_buf, blk_len);

    free (blk_buf);
    free (dict_buf);
    free (blk);
  }

  if (ret == -1) { 
    struct sockaddr_in sin;
    sin.sin_addr.s_addr = priv->addr;
    
    gf_log ("transport: ib-verbs: ",
	    GF_LOG_ERROR,
	    "handshake with %s failed", 
	    inet_ntoa (sin.sin_addr));
    goto ret;
  }

  gf_block *reply_blk = gf_block_unserialize_transport (this);
  if (!reply_blk) {
    gf_log ("transport: ib-verbs: ",
	    GF_LOG_ERROR,
	    "gf_block_unserialize failed during handshake");
    ret = -1;
    goto reply_err;
  }

  if (!((reply_blk->type == OP_TYPE_FOP_REPLY) || 
	(reply_blk->type == OP_TYPE_MOP_REPLY))) {
    gf_log ("transport: ib-verbs: ",
	    GF_LOG_DEBUG,
	    "unexpected block type %d recieved during handshake",
	    reply_blk->type);
    ret = -1;
    goto reply_err;
  }

  dict_unserialize (reply_blk->data, reply_blk->size, &reply);
  
  if (reply == NULL) {
    gf_log ("transport: ib-verbs: ",
	    GF_LOG_ERROR,
	    "dict_unserialize failed");
    ret = -1;
    goto reply_err;
  }

  ret = data_to_int (dict_get (reply, "RET"));
  remote_errno = data_to_int (dict_get (reply, "ERRNO"));
  
  if (ret < 0) {
    gf_log ("ib-verbs/client",
	    GF_LOG_ERROR,
	    "SETVOLUME on remote server failed (%s)",
	    strerror (errno));
    errno = remote_errno;
    goto reply_err;
  }

 reply_err:
  if (reply_blk) {
    if (reply_blk->data)
      free (reply_blk->data);
    free (reply_blk);
  }

 ret:
  dict_destroy (request);
  dict_destroy (reply);
  return ret;
}

static int32_t
ib_verbs_connect (struct transport *this, 
	     dict_t *options)
{
  GF_ERROR_IF_NULL (this);

  ib_verbs_private_t *priv = this->private;
  GF_ERROR_IF_NULL (priv);
  
  if (!priv->options)
    priv->options = dict_copy (options);

  //ibv_init

  ib_verbs_ibv_init (priv);

  struct sockaddr_in sin;
  struct sockaddr_in sin_src;
  int32_t ret = 0;
  uint16_t try_port = CLIENT_PORT_CIELING;

  if (!priv->connected)
    priv->sock = socket (AF_INET, SOCK_STREAM, 0);

  gf_log ("transport: ib-verbs: ",
	  GF_LOG_DEBUG,
	  "try_connect: socket fd = %d", priv->sock);

  if (priv->sock == -1) {
    gf_log ("transport: ib-verbs: ",
	    GF_LOG_ERROR,
	    "try_connect: socket () - error: %s",
	    strerror (errno));
    return -errno;
  }

  while (try_port) { 
    sin_src.sin_family = AF_INET;
    sin_src.sin_port = htons (try_port); //FIXME: have it a #define or configurable
    sin_src.sin_addr.s_addr = INADDR_ANY;
    
    if ((ret = bind (priv->sock,
		     (struct sockaddr *)&sin_src,
		     sizeof (sin_src))) == 0) {
      gf_log ("transport: ib-verbs: ",
	      GF_LOG_DEBUG,
	      "try_connect: finalized on port `%d'",
	      try_port);
      break;
    }
    
    try_port--;
  }
  
  if (ret != 0) {
      gf_log ("transport: ib-verbs: ",
	      GF_LOG_ERROR,
	      "try_connect: bind loop failed - error: %s",
	      strerror (errno));
      close (priv->sock);
      return -errno;
  }

  sin.sin_family = AF_INET;

  if (dict_get (options, "remote-port")) {
    sin.sin_port = htons (data_to_int (dict_get (options,
						 "remote-port")));
  } else {
    gf_log ("ib-verbs/client",
	    GF_LOG_DEBUG,
	    "try_connect: defaulting remote-port to %d", GF_DEFAULT_LISTEN_PORT);
    sin.sin_port = htons (GF_DEFAULT_LISTEN_PORT);
  }

  if (dict_get (options, "remote-host")) {
    sin.sin_addr.s_addr = resolve_ip (data_to_str (dict_get (options, "remote-host")));
  } else {
    gf_log ("ib-verbs/client",
	    GF_LOG_DEBUG,
	    "try_connect: error: missing 'option remote-host <hostname>'");
    close (priv->sock);
    return -errno;
  }

  if (connect (priv->sock, (struct sockaddr *)&sin, sizeof (sin)) != 0) {
    gf_log ("transport/ib-verbs",
	    GF_LOG_ERROR,
	    "try_connect: connect () - error: %s",
	    strerror (errno));
    close (priv->sock);
    return -errno;
  }
  
  char msg[256] = {0,};
  read (priv->sock, msg, sizeof msg);
  sscanf (msg, "%04x:%06x:%06x", &priv->remote.lid, &priv->remote.qpn, &priv->remote.psn);
  sprintf (msg, "%04x:%06x:%06x", priv->local.lid, priv->local.qpn, priv->local.psn);
  write (priv->sock, msg, sizeof msg);

  ib_verbs_ibv_connect (priv, 1, priv->local.psn, IBV_MTU_1024);

  ret = do_handshake (this, options);
  if (ret != 0) {
    gf_log ("transport: ib-verbs: ", GF_LOG_ERROR, "handshake failed");
    close (priv->sock);
    return ret;
  }

  priv->connected = 1;

  return ret;
}

static int32_t
ib_verbs_client_submit (transport_t *this, char *buf, int32_t len)
{
  ib_verbs_private_t *priv = this->private;

  if (!priv->connected) {
    int ret = ib_verbs_connect (this, priv->options);
    if (ret == 0) {
      register_transport (this, ((ib_verbs_private_t *)this->private)->sock);
      return ib_verbs_full_write (priv, buf, len);
    }
    else
      return -1;
  }

  return ib_verbs_full_write (priv, buf, len);
}

static int32_t
ib_verbs_client_except (transport_t *this)
{
  GF_ERROR_IF_NULL (this);

  ib_verbs_private_t *priv = this->private;
  GF_ERROR_IF_NULL (priv);

  priv->connected = 0;
  int ret = ib_verbs_connect (this, priv->options);

  return ret;
}

struct transport_ops transport_ops = {
  //  .flush = ib_verbs_flush,
  .recieve = ib_verbs_recieve,

  .submit = ib_verbs_client_submit,

  .disconnect = ib_verbs_disconnect,
  .except = ib_verbs_client_except
};

int 
init (struct transport *this,
      dict_t *options,
      int32_t (*notify) (xlator_t *xl, transport_t *trans, int32_t event))
{
  this->private = calloc (1, sizeof (ib_verbs_private_t));
  this->notify = notify;

  pthread_mutex_init (&((ib_verbs_private_t *)this->private)->read_mutex, NULL);
  pthread_mutex_init (&((ib_verbs_private_t *)this->private)->write_mutex, NULL);

  int ret = ib_verbs_connect (this, options);
  if (ret != 0) {
    gf_log ("transport: ib-verbs: client: ", GF_LOG_ERROR, "init failed");
    return -1;
  }

  register_transport (this, ((ib_verbs_private_t *)this->private)->sock);
  return 0;
}

int 
fini (struct transport *this)
{
  ib_verbs_private_t *priv = this->private;
  //  this->ops->flush (this);

  dict_destroy (priv->options);
  close (priv->sock);
  free (priv);
  return 0;
}