/*
  (C) 2006, 2007 Z RESEARCH Inc. <http://www.zresearch.com>
  
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

/**
 * xlators/cluster/unify 
 *    - This xlator is one of the main translator in GlusterFS, which
 *   actually does the clustering work of the file system.One need to 
 *   understand that, unify assumes file to be existing in only one of 
 *   the child node, and directories to be present on all the nodes. 
 */
#include "glusterfs.h"
#include "unify.h"
#include "dict.h"
#include "xlator.h"
#include "hashfn.h"
#include "logging.h"
#include "stack.h"

#define INIT_LOCK(x)    pthread_mutex_init (x, NULL);
#define LOCK(x)         pthread_mutex_lock (x);
#define UNLOCK(x)       pthread_mutex_unlock (x);
#define LOCK_DESTROY(x) pthread_mutex_destroy (x);
#define LOCK_NODE(xl) (((unify_private_t *)xl->private)->lock_node)

/**
 * gcd_path - function used for adding two strings, on which a namespace lock is taken
 * @path1 - 
 * @path2 - 
 */
static char *
gcd_path (const char *path1, const char *path2)
{
  char *s1 = (char *)path1;
  char *s2 = (char *)path2;
  int32_t diff = -1;

  while (*s1 && *s2 && (*s1 == *s2)) {
    if (*s1 == '/')
      diff = s1 - path1;
    s1++;
    s2++;
  }

  return (diff == -1) ? NULL : strndup (path1, diff + 1);
}

/**
 * unify_lookup_cbk - one of the tricky function of whole GlusterFS. :D
 */

static int32_t 
unify_lookup_cbk (call_frame_t *frame,
		  void *cookie,
		  xlator_t *this,
		  int32_t op_ret,
		  int32_t op_errno,
		  inode_t *inode,
		  struct stat *buf)
{
  
  return 0;
}

/**
 * unify_lookup - hope you read commets for above function. :O
 */
int32_t 
unify_lookup (call_frame_t *frame,
	      xlator_t *this,
	      inode_t *parent,
	      const char *name)
{
  struct list_head *list = NULL;
  unify_inode_list_t *ino_list = NULL;
  unify_local_t *local = NULL;
  inode_t *inode = NULL;
  xlator_t *xl = NULL;
  
  inode = inode_search (this->itable, parent->ino, name);
  /* If an entry is found, send the lookup call to only that node. 
   * If not, send call to all the nodes.
   */
  if (inode) {
    if (S_ISDIR (inode->buf.st_mode)) {
      list = parent->private;
      
      list_for_each_entry (ino_list, list, list_head) 
	STACK_WIND (frame,
		    unify_lookup_cbk,
		    ino_list->xl,
		    ino_list->xl->fops->lookup,
		    ino_list->inode,
		    name);
    }
  } else {
    /*TODO: if parent inode mapping is not there for all the nodes, how to handle it? */

    /* The inode is not there yet. Forward the request to all the nodes
     */
    
    inode_t *pinode = NULL;
    xlator_list_t *trav = NULL;
    /* Get the parent inode, which will e a directory, so, will be present 
     * on all the nodes 
     */
    pinode = inode_search (this->itable, parent->ino, NULL);
    while (trav) {
      STACK_WIND (frame,
		  unify_lookup_cbk,
		  trav->xlator,
		  trav->xlator->fops->lookup,
		  parent,
		  name);
      
      trav = trav->next;
    }
  }
  return 0;
}

static int32_t 
unify_forget_cbk (call_frame_t *frame,
		  void *cookie,
		  xlator_t *this,
		  int32_t op_ret,
		  int32_t op_errno)
{
  return 0;
}

int32_t 
unify_forget (call_frame_t *frame,
	      xlator_t *this,
	      inode_t *inode,
	      unsigned long nlookup)
{
}

/**
 * unify_getattr_cbk - This function will be called many times in the 
 *   same frame, only if stat call is made on directory. When a stat 
 *   entry for directory returns, unify should see latest mtime,atime 
 *   and ctime of all the directories of children, and send the latest 
 *   to the above layer. Its done for the 'st_size' entry also to give 
 *   consistant size accross stat calls.
 */
static int32_t
unify_getattr_cbk (call_frame_t *frame,
		   void *cookie,
		   xlator_t *this,
		   int32_t op_ret,
		   int32_t op_errno,
		   struct stat *buf)
{
  int32_t callcnt = 0;
  unify_local_t *local = frame->local;

  LOCK (&frame->mutex);
  callcnt = --local->call_count;
  UNLOCK (&frame->mutex);

  if (op_ret == -1 && op_errno != ENOTCONN)
    local->op_errno = op_errno;

  if (op_ret = 0 && local->op_ret == -1) {
    /* This is for only one time */
    local->op_ret = 0;
    local->stbuf = *buf; /* memcpy (local->stbuf, buf, sizeof (struct stat)); */
  }
  
  if (op_ret == 0) {
    /* For all the successful returns.. compare the values, and set it to max */
    if (local->stbuf.st_mtime < buf->st_mtime)
      local->stbuf.st_mtime = buf->st_mtime;
    if (local->stbuf.st_ctime < buf->st_ctime)
      local->stbuf.st_ctime = buf->st_ctime;
    if (local->stbuf.st_atime < buf->st_atime)
      local->stbuf.st_atime = buf->st_atime;

    if (local->stbuf.st_size < buf->st_size)
      local->stbuf.st_size = buf->st_size;
  }
  
  if (!callcnt) {
    LOCK_DESTROY (&frame->mutex);
    STACK_UNWIND (frame, local->op_ret, local->op_errno, &local->stbuf);
  }
  return 0;
}

int32_t
unify_getattr (call_frame_t *frame,
	       xlator_t *this,
	       inode_t *inode)
{
  struct list_head *list = NULL;
  unify_inode_list_t *ino_list = NULL;
  unify_local_t *local = NULL;

  /* Initialization */
  local = calloc (1, sizeof (unify_local_t));
  LOCK_INIT (&frame->mutex);
  local->op_ret = -1;
  local->op_ret = ENOENT;
  frame->local = local;

  /* Initialize call_count - which will be >1 for directories only */
  list = inode->private;
  list_for_each_entry (ino_list, list, list_head)
    local->call_count++;
  
  /* wind the stack to all the mapped entries */
  list= inode->private;
  list_for_each_entry (ino_list, list, list_head) {
    STACK_WIND (frame,
		unify_getattr_cbk,
		ino_list->xl,
		ino_list->xl->fops->getattr,
		ino_list->inode);
  }
  return 0;
}

unify_chmod_unlock_cbk (call_frame_t *frame,
			void *cookie,
			xlator_t *this,
			int32_t op_ret,
			int32_t op_errno)
{
  return 0;
}

static int32_t
unify_chmod_cbk (call_frame_t *frame,
		 void *cookie,
		 xlator_t *this,
		 int32_t op_ret,
		 int32_t op_errno,
		 struct stat *buf)
{
  return 0;
}

static int32_t 
unify_chmod_lock_cbk (call_frame_t *frame,
		      void *cookie,
		      xlator_t *this,
		      int32_t op_ret,
		      int32_t op_errno)
{
  struct list_head *list = NULL;
  unify_inode_list_t *ino_list = NULL;
  unify_local_t *local = frame->local;

  if (op_ret == 0) {
    /* wind the stack to all the mapped entries */
    list = local->inode->private;
    list_for_each_entry (ino_list, list, list_head) {
      STACK_WIND (frame,
		  unify_chmod_cbk,
		  ino_list->xl,
		  ino_list->xl->fops->chmod,
		  ino_list->inode,
		  local->mode);
    }
  } else {
    free (local->path);
    LOCK_DESTROY (&frame->mutex);
    STACK_UNWIND (frame, -1, ENOENT, NULL);
  }
  return 0;
}

int32_t
unify_chmod (call_frame_t *frame,
	     xlator_t *this,
	     inode_t *inode,
	     mode_t mode)
{
  struct list_head *list = NULL;
  unify_inode_list_t *ino_list = NULL;
  unify_local_t *local = NULL;

  /* Initialization */
  local = calloc (1, sizeof (unify_local_t));
  LOCK_INIT (&frame->mutex);
  local->op_ret = -1;
  local->op_ret = ENOENT;
  frame->local = local;

  if (S_ISDIR(inode->buf.st_mode)) {
    /* Initialize call_count - which will be >1 for directories only */
    char lock_path[4096] = {0,};

    list = inode->private;
    list_for_each_entry (ino_list, list, list_head)
      local->call_count++;
  
    local->inode = inode;
    local->mode = mode;
    local->lock_taken = 1;

    /* get the lock on directory name */
    inode_path (inode, NULL, lock_path, 4096);
    local->path = strdup (lock_path);

    list= inode->private;
    list_for_each_entry (ino_list, list, list_head) {
      if (ino_list->xl == LOCK_NODE(this))
	STACK_WIND (frame,
		    unify_chmod_lock_cbk,
		    ino_list->xl,
		    ino_list->xl->mops->lock,
		    local->path);
    }
  } else {
    /* if its a file, no lock required */
    local->call_count = 1;
    list= inode->private;
    list_for_each_entry (ino_list, list, list_head) {
      STACK_WIND (frame,
		  unify_chmod_cbk,
		  ino_list->xl,
		  ino_list->xl->fops->chmod,
		  ino_list->inode,
		  mode);
    }
  }
  return 0;
}


static int32_t
unify_fchmod_cbk (call_frame_t *frame,
		    void *cookie,
		    xlator_t *this,
		    int32_t op_ret,
		    int32_t op_errno,
		    struct stat *buf)
{
}

int32_t 
unify_fchmod (call_frame_t *frame,
		xlator_t *this,
		fd_t *fd,
		mode_t mode)
{
}

static int32_t
unify_chown_cbk (call_frame_t *frame,
		   void *cookie,
		   xlator_t *this,
		   int32_t op_ret,
		   int32_t op_errno,
		   struct stat *buf)
{
  STACK_UNWIND (frame,
		op_ret,
		op_errno,
		buf);
  return 0;
}

int32_t
unify_chown (call_frame_t *frame,
	       xlator_t *this,
	       inode_t *inode,
	       uid_t uid,
	       gid_t gid)
{
  STACK_WIND (frame,	      
	      unify_chown_cbk,
	      FIRST_CHILD(this),
	      FIRST_CHILD(this)->fops->chown,
	      inode,
	      uid,
	      gid);
  return 0;
}

static int32_t
unify_fchown_cbk (call_frame_t *frame,
		    void *cookie,
		    xlator_t *this,
		    int32_t op_ret,
		    int32_t op_errno,
		    struct stat *buf)
{
  STACK_UNWIND (frame,
		op_ret,
		op_errno,
		buf);
  return 0;
}

int32_t 
unify_fchown (call_frame_t *frame,
		xlator_t *this,
		fd_t *fd,
		uid_t uid,
		gid_t gid)
{
  STACK_WIND (frame,	      
	      unify_fchown_cbk,
	      FIRST_CHILD(this),
	      FIRST_CHILD(this)->fops->fchown,
	      fd,
	      uid,
	      gid);
  return 0;
}

static int32_t
unify_truncate_cbk (call_frame_t *frame,
		      void *cookie,
		      xlator_t *this,
		      int32_t op_ret,
		      int32_t op_errno,
		      struct stat *buf)
{
  STACK_UNWIND (frame,
		op_ret,
		op_errno,
		buf);
  return 0;
}

int32_t
unify_truncate (call_frame_t *frame,
		  xlator_t *this,
		  inode_t *inode,
		  off_t offset)
{
  STACK_WIND (frame,
	      unify_truncate_cbk,
	      FIRST_CHILD(this),
	      FIRST_CHILD(this)->fops->truncate,
	      inode,
	      offset);
  return 0;
}

static int32_t
unify_ftruncate_cbk (call_frame_t *frame,
		       void *cookie,
		       xlator_t *this,
		       int32_t op_ret,
		       int32_t op_errno,
		       struct stat *buf)
{
  STACK_UNWIND (frame,
		op_ret,
		op_errno,
		buf);
  return 0;
}

int32_t
unify_ftruncate (call_frame_t *frame,
		   xlator_t *this,
		   fd_t *fd,
		   off_t offset)
{
  STACK_WIND (frame,
	      unify_ftruncate_cbk,
	      FIRST_CHILD(this),
	      FIRST_CHILD(this)->fops->ftruncate,
	      fd,
	      offset);
  return 0;
}

int32_t 
unify_utimens_cbk (call_frame_t *frame,
		     void *cookie,
		     xlator_t *this,
		     int32_t op_ret,
		     int32_t op_errno,
		     struct stat *buf)
{
  STACK_UNWIND (frame,
		op_ret,
		op_errno,
		buf);
  return 0;
}


int32_t 
unify_utimens (call_frame_t *frame,
		 xlator_t *this,
		 inode_t *inode,
		 struct timespec tv[2])
{
  STACK_WIND (frame,
	      unify_utimens_cbk,
	      FIRST_CHILD(this),
	      FIRST_CHILD(this)->fops->utimens,
	      inode,
	      tv);
  return 0;
}

int32_t 
unify_futimens_cbk (call_frame_t *frame,
		      void *cookie,
		      xlator_t *this,
		      int32_t op_ret,
		      int32_t op_errno,
		      struct stat *buf)
{
  STACK_UNWIND (frame,
		op_ret,
		op_errno,
		buf);
  return 0;
}

int32_t 
unify_futimens (call_frame_t *frame,
		  xlator_t *this,
		  fd_t *fd,
		  struct timespec tv[2])
{
  STACK_WIND (frame,
	      unify_futimens_cbk,
	      FIRST_CHILD(this),
	      FIRST_CHILD(this)->fops->futimens,
	      fd,
	      tv);
  return 0;
}

static int32_t
unify_access_cbk (call_frame_t *frame,
		    void *cookie,
		    xlator_t *this,
		    int32_t op_ret,
		    int32_t op_errno)
{
  STACK_UNWIND (frame,
		op_ret,
		op_errno);
  return 0;
}

int32_t
unify_access (call_frame_t *frame,
		xlator_t *this,
		inode_t *inode,
		int32_t mask)
{
  STACK_WIND (frame,
	      unify_access_cbk,
	      FIRST_CHILD(this),
	      FIRST_CHILD(this)->fops->access,
	      inode,
	      mask);
  return 0;
}


static int32_t
unify_readlink_cbk (call_frame_t *frame,
		      void *cookie,
		      xlator_t *this,
		      int32_t op_ret,
		      int32_t op_errno,
		      const char *path)
{
  STACK_UNWIND (frame,
		op_ret,
		op_errno,
		path);
  return 0;
}

int32_t
unify_readlink (call_frame_t *frame,
		  xlator_t *this,
		  inode_t *inode,
		  size_t size)
{
  STACK_WIND (frame,
	      unify_readlink_cbk,
	      FIRST_CHILD(this),
	      FIRST_CHILD(this)->fops->readlink,
	      inode,
	      size);
  return 0;
}


static int32_t
unify_mknod_cbk (call_frame_t *frame,
		   void *cookie,
		   xlator_t *this,
		   int32_t op_ret,
		   int32_t op_errno,
		   inode_t *inode,
		   struct stat *buf)
{
  STACK_UNWIND (frame,
		op_ret,
		op_errno,
		inode,
		buf);
  return 0;
}

int32_t
unify_mknod (call_frame_t *frame,
	       xlator_t *this,
	       inode_t *parent,
	       const char *name,
	       mode_t mode,
	       dev_t rdev)
{
  STACK_WIND (frame,
	      unify_mknod_cbk,
	      FIRST_CHILD(this),
	      FIRST_CHILD(this)->fops->mknod,
	      parent,
	      name,
	      mode,
	      rdev);
  return 0;
}

static int32_t
unify_mkdir_cbk (call_frame_t *frame,
		   void *cookie,
		   xlator_t *this,
		   int32_t op_ret,
		   int32_t op_errno,
		   inode_t *inode,
		   struct stat *buf)
{
  STACK_UNWIND (frame,
		op_ret,
		op_errno,
		inode,
		buf);
  return 0;
}

int32_t
unify_mkdir (call_frame_t *frame,
	       xlator_t *this,
	       inode_t *parent,
	       const char *name,
	       mode_t mode)
{
  STACK_WIND (frame,
	      unify_mkdir_cbk,
	      FIRST_CHILD(this),
	      FIRST_CHILD(this)->fops->mkdir,
	      parent,
	      name,
	      mode);
  return 0;
}

static int32_t
unify_unlink_cbk (call_frame_t *frame,
		    void *cookie,
		    xlator_t *this,
		    int32_t op_ret,
		    int32_t op_errno)
{
  STACK_UNWIND (frame,
		op_ret,
		op_errno);
  return 0;
}

int32_t
unify_unlink (call_frame_t *frame,
		xlator_t *this,
		inode_t *parent,
		const char *name)
{
  STACK_WIND (frame,
	      unify_unlink_cbk,
	      FIRST_CHILD(this),
	      FIRST_CHILD(this)->fops->unlink,
	      parent,
	      name);
  return 0;
}

static int32_t
unify_rmdir_cbk (call_frame_t *frame,
		   void *cookie,
		   xlator_t *this,
		   int32_t op_ret,
		   int32_t op_errno)
{
  STACK_UNWIND (frame,
		op_ret,
		op_errno);
  return 0;
}

int32_t
unify_rmdir (call_frame_t *frame,
	       xlator_t *this,
	       inode_t *parent,
	       const char *name)
{
  STACK_WIND (frame,
	      unify_rmdir_cbk,
	      FIRST_CHILD(this),
	      FIRST_CHILD(this)->fops->rmdir,
	      parent,
	      name);
  return 0;
}

static int32_t
unify_symlink_cbk (call_frame_t *frame,
		     void *cookie,
		     xlator_t *this,
		     int32_t op_ret,
		     int32_t op_errno,
		     inode_t *inode,
		     struct stat *buf)
{
  STACK_UNWIND (frame,
		op_ret,
		op_errno,
		inode,
		buf);
  return 0;
}

int32_t
unify_symlink (call_frame_t *frame,
		 xlator_t *this,
		 const char *linkname,
		 inode_t *parent,
		 const char *name)
{
  STACK_WIND (frame,
	      unify_symlink_cbk,
	      FIRST_CHILD(this),
	      FIRST_CHILD(this)->fops->symlink,
	      linkname,
	      parent,
	      name);
  return 0;
}


static int32_t
unify_rename_cbk (call_frame_t *frame,
		    void *cookie,
		    xlator_t *this,
		    int32_t op_ret,
		    int32_t op_errno,
		    inode_t *inode,
		    struct stat *buf)
{
  STACK_UNWIND (frame,
		op_ret,
		op_errno,
		inode,
		buf);
  return 0;
}

int32_t
unify_rename (call_frame_t *frame,
		xlator_t *this,
		inode_t *olddir,
		const char *oldname,
		inode_t *newdir,
		const char *newname)
{
  STACK_WIND (frame,
	      unify_rename_cbk,
	      FIRST_CHILD(this),
	      FIRST_CHILD(this)->fops->rename,
	      olddir,
	      oldname,
	      newdir,
	      newname);
  return 0;
}


static int32_t
unify_link_cbk (call_frame_t *frame,
		  void *cookie,
		  xlator_t *this,
		  int32_t op_ret,
		  int32_t op_errno,
		  inode_t *inode,
		  struct stat *buf)
{
  STACK_UNWIND (frame,
		op_ret,
		op_errno,
		inode,
		buf);
  return 0;
}

int32_t
unify_link (call_frame_t *frame,
	      xlator_t *this,
	      inode_t *inode,
	      inode_t *newparent,
	      const char *newname)
{
  STACK_WIND (frame,
	      unify_link_cbk,
	      FIRST_CHILD(this),
	      FIRST_CHILD(this)->fops->link,
	      inode,
	      newparent,
	      newname);
  return 0;
}


static int32_t
unify_create_cbk (call_frame_t *frame,
		    void *cookie,
		    xlator_t *this,
		    int32_t op_ret,
		    int32_t op_errno,
		    fd_t *fd,
		    inode_t *inode,
		    struct stat *buf)
{
  STACK_UNWIND (frame,
		op_ret,
		op_errno,
		fd,
		inode,
		buf);
  return 0;
}

int32_t
unify_create (call_frame_t *frame,
		xlator_t *this,
		inode_t *inode,
		const char *name,
		int32_t flags,
		mode_t mode)
{
  STACK_WIND (frame,
	      unify_create_cbk,
	      FIRST_CHILD(this),
	      FIRST_CHILD(this)->fops->create,
	      inode,
	      name,
	      flags,
	      mode);
  return 0;
}

static int32_t
unify_open_cbk (call_frame_t *frame,
		  void *cookie,
		  xlator_t *this,
		  int32_t op_ret,
		  int32_t op_errno,
		  fd_t *fd)
{
  STACK_UNWIND (frame,
		op_ret,
		op_errno,
		fd);
  return 0;
}

int32_t
unify_open (call_frame_t *frame,
	      xlator_t *this,
	      inode_t *inode,
	      int32_t flags,
	      mode_t mode)
{
  STACK_WIND (frame,
	      unify_open_cbk,
	      FIRST_CHILD(this),
	      FIRST_CHILD(this)->fops->open,
	      inode,
	      flags,
	      mode);
  return 0;
}

static int32_t
unify_readv_cbk (call_frame_t *frame,
		   void *cookie,
		   xlator_t *this,
		   int32_t op_ret,
		   int32_t op_errno,
		   struct iovec *vector,
		   int32_t count)
{
  STACK_UNWIND (frame,
		op_ret,
		op_errno,
		vector,
		count);
  return 0;
}

int32_t
unify_readv (call_frame_t *frame,
	       xlator_t *this,
	       fd_t *fd,
	       size_t size,
	       off_t offset)
{
  STACK_WIND (frame,
	      unify_readv_cbk,
	      FIRST_CHILD(this),
	      FIRST_CHILD(this)->fops->readv,
	      fd,
	      size,
	      offset);
  return 0;
}


static int32_t
unify_writev_cbk (call_frame_t *frame,
		    void *cookie,
		    xlator_t *this,
		    int32_t op_ret,
		    int32_t op_errno)
{
  STACK_UNWIND (frame,
		op_ret,
		op_errno);
  return 0;
}

int32_t
unify_writev (call_frame_t *frame,
		xlator_t *this,
		fd_t *fd,
		struct iovec *vector,
		int32_t count,
		off_t off)
{
  STACK_WIND (frame,
	      unify_writev_cbk,
	      FIRST_CHILD(this),
	      FIRST_CHILD(this)->fops->writev,
	      fd,
	      vector,
	      count,
	      off);
  return 0;
}

static int32_t
unify_flush_cbk (call_frame_t *frame,
		   void *cookie,
		   xlator_t *this,
		   int32_t op_ret,
		   int32_t op_errno)
{
  STACK_UNWIND (frame,
		op_ret,
		op_errno);
  return 0;
}

int32_t
unify_flush (call_frame_t *frame,
	       xlator_t *this,
	       fd_t *fd)
{
  STACK_WIND (frame,
	      unify_flush_cbk,
	      FIRST_CHILD(this),
	      FIRST_CHILD(this)->fops->flush,
	      fd);
  return 0;
}

static int32_t
unify_release_cbk (call_frame_t *frame,
		     void *cookie,
		     xlator_t *this,
		     int32_t op_ret,
		     int32_t op_errno)
{
  STACK_UNWIND (frame,
		op_ret,
		op_errno);
  return 0;
}

int32_t
unify_release (call_frame_t *frame,
		 xlator_t *this,
		 fd_t *fd)
{
  STACK_WIND (frame,
	      unify_release_cbk,
	      FIRST_CHILD(this),
	      FIRST_CHILD(this)->fops->release,
	      fd);
  return 0;
}


static int32_t
unify_fsync_cbk (call_frame_t *frame,
		   void *cookie,
		   xlator_t *this,
		   int32_t op_ret,
		   int32_t op_errno)
{
  STACK_UNWIND (frame,
		op_ret,
		op_errno);
  return 0;
}

int32_t
unify_fsync (call_frame_t *frame,
	       xlator_t *this,
	       fd_t *fd,
	       int32_t flags)
{
  STACK_WIND (frame,
	      unify_fsync_cbk,
	      FIRST_CHILD(this),
	      FIRST_CHILD(this)->fops->fsync,
	      fd,
	      flags);
  return 0;
}

static int32_t
unify_fgetattr_cbk (call_frame_t *frame,
		      void *cookie,
		      xlator_t *this,
		      int32_t op_ret,
		      int32_t op_errno,
		      struct stat *buf)
{
  STACK_UNWIND (frame,
		op_ret,
		op_errno,
		buf);
  return 0;
}

int32_t
unify_fgetattr (call_frame_t *frame,
		  xlator_t *this,
		  fd_t *fd)
{
  STACK_WIND (frame,
	      unify_fgetattr_cbk,
	      FIRST_CHILD(this),
	      FIRST_CHILD(this)->fops->fgetattr,
	      fd);
  return 0;
}

static int32_t
unify_opendir_cbk (call_frame_t *frame,
		     void *cookie,
		     xlator_t *this,
		     int32_t op_ret,
		     int32_t op_errno,
		     fd_t *fd)
{
  STACK_UNWIND (frame,
		op_ret,
		op_errno,
		fd);
  return 0;
}

int32_t
unify_opendir (call_frame_t *frame,
		 xlator_t *this,
		 inode_t *inode)
{
  STACK_WIND (frame,
	      unify_opendir_cbk,
	      FIRST_CHILD(this),
	      FIRST_CHILD(this)->fops->opendir,
	      inode);
  return 0;
}


static int32_t
unify_readdir_cbk (call_frame_t *frame,
		     void *cookie,
		     xlator_t *this,
		     int32_t op_ret,
		     int32_t op_errno,
		     dir_entry_t *entries,
		     int32_t count)
{
  STACK_UNWIND (frame,
		op_ret,
		op_errno,
		entries,
		count);
  return 0;
}

int32_t
unify_readdir (call_frame_t *frame,
		 xlator_t *this,
		 size_t size,
		 off_t offset,
		 fd_t *fd)
{
  STACK_WIND (frame,
	      unify_readdir_cbk,
	      FIRST_CHILD(this),
	      FIRST_CHILD(this)->fops->readdir,
	      size,
	      offset,
	      fd);
  return 0;
}


static int32_t
unify_releasedir_cbk (call_frame_t *frame,
			void *cookie,
			xlator_t *this,
			int32_t op_ret,
			int32_t op_errno)
{
  STACK_UNWIND (frame,
		op_ret,
		op_errno);
  return 0;
}

int32_t
unify_releasedir (call_frame_t *frame,
		    xlator_t *this,
		    fd_t *fd)
{
  STACK_WIND (frame,
	      unify_releasedir_cbk,
	      FIRST_CHILD(this),
	      FIRST_CHILD(this)->fops->releasedir,
	      fd);
  return 0;
}

static int32_t
unify_fsyncdir_cbk (call_frame_t *frame,
		      void *cookie,
		      xlator_t *this,
		      int32_t op_ret,
		      int32_t op_errno)
{
  STACK_UNWIND (frame,
		op_ret,
		op_errno);
  return 0;
}

int32_t
unify_fsyncdir (call_frame_t *frame,
		  xlator_t *this,
		  fd_t *fd,
		  int32_t flags)
{
  STACK_WIND (frame,
	      unify_fsyncdir_cbk,
	      FIRST_CHILD(this),
	      FIRST_CHILD(this)->fops->fsyncdir,
	      fd,
	      flags);
  return 0;
}


static int32_t
unify_statfs_cbk (call_frame_t *frame,
		    void *cookie,
		    xlator_t *this,
		    int32_t op_ret,
		    int32_t op_errno,
		    struct statvfs *buf)
{
  STACK_UNWIND (frame,
		op_ret,
		op_errno,
		buf);
  return 0;
}

int32_t
unify_statfs (call_frame_t *frame,
		xlator_t *this,
		inode_t *inode)
{
  STACK_WIND (frame,
	      unify_statfs_cbk,
	      FIRST_CHILD(this),
	      FIRST_CHILD(this)->fops->statfs,
	      inode);
  return 0;
}


static int32_t
unify_setxattr_cbk (call_frame_t *frame,
		      void *cookie,
		      xlator_t *this,
		      int32_t op_ret,
		      int32_t op_errno)
{
  STACK_UNWIND (frame,
		op_ret,
		op_errno);
  return 0;
}

int32_t
unify_setxattr (call_frame_t *frame,
		  xlator_t *this,
		  inode_t *inode,
		  const char *name,
		  const char *value,
		  size_t size,
		  int32_t flags)
{
  STACK_WIND (frame,
	      unify_setxattr_cbk,
	      FIRST_CHILD(this),
	      FIRST_CHILD(this)->fops->setxattr,
	      inode,
	      name,
	      value,
	      size,
	      flags);
  return 0;
}

static int32_t
unify_getxattr_cbk (call_frame_t *frame,
		      void *cookie,
		      xlator_t *this,
		      int32_t op_ret,
		      int32_t op_errno,
		      void *value)
{
  STACK_UNWIND (frame,
		op_ret,
		op_errno,
		value);
  return 0;
}

int32_t
unify_getxattr (call_frame_t *frame,
		  xlator_t *this,
		  inode_t *inode,
		  const char *name,
		  size_t size)
{
  STACK_WIND (frame,
	      unify_getxattr_cbk,
	      FIRST_CHILD(this),
	      FIRST_CHILD(this)->fops->getxattr,
	      inode,
	      name,
	      size);
  return 0;
}

static int32_t
unify_listxattr_cbk (call_frame_t *frame,
		       void *cookie,
		       xlator_t *this,
		       int32_t op_ret,
		       int32_t op_errno,
		       void *value)
{
  STACK_UNWIND (frame,
		op_ret,
		op_errno,
		value);
  return 0;
}

int32_t
unify_listxattr (call_frame_t *frame,
		   xlator_t *this,
		   inode_t *inode,
		   size_t size)
{
  STACK_WIND (frame,
	      unify_listxattr_cbk,
	      FIRST_CHILD(this),
	      FIRST_CHILD(this)->fops->listxattr,
	      inode,
	      size);
  return 0;
}

static int32_t
unify_removexattr_cbk (call_frame_t *frame,
			 void *cookie,
			 xlator_t *this,
			 int32_t op_ret,
			 int32_t op_errno)
{
  STACK_UNWIND (frame,
		op_ret,
		op_errno);
  return 0;
}

int32_t
unify_removexattr (call_frame_t *frame,
		     xlator_t *this,
		     inode_t *inode,
		     const char *name)
{
  STACK_WIND (frame,
	      unify_removexattr_cbk,
	      FIRST_CHILD(this),
	      FIRST_CHILD(this)->fops->removexattr,
	      inode,
	      name);
  return 0;
}

static int32_t
unify_lk_cbk (call_frame_t *frame,
		void *cookie,
		xlator_t *this,
		int32_t op_ret,
		int32_t op_errno,
		struct flock *lock)
{
  STACK_UNWIND (frame,
		op_ret,
		op_errno,
		lock);
  return 0;
}

int32_t
unify_lk (call_frame_t *frame,
	    xlator_t *this,
	    fd_t *fd,
	    int32_t cmd,
	    struct flock *lock)
{
  STACK_WIND (frame,
	      unify_lk_cbk,
	      FIRST_CHILD(this),
	      FIRST_CHILD(this)->fops->lk,
	      fd,
	      cmd,
	      lock);
  return 0;
}


/* Management operations */

static int32_t
unify_stats_cbk (call_frame_t *frame,
		   void *cookie,
		   xlator_t *this,
		   int32_t op_ret,
		   int32_t op_errno,
		   struct xlator_stats *stats)
{
  STACK_UNWIND (frame,
		op_ret,
		op_errno,
		stats);
  return 0;
}

int32_t
unify_stats (call_frame_t *frame,
	       xlator_t *this,
	       int32_t flags)
{
  STACK_WIND (frame,
	      unify_stats_cbk,
	      FIRST_CHILD(this),
	      FIRST_CHILD(this)->mops->stats,
	      flags);
  return 0;
}


static int32_t
unify_fsck_cbk (call_frame_t *frame,
		  void *cookie,
		  xlator_t *this,
		  int32_t op_ret,
		  int32_t op_errno)
{
  STACK_UNWIND (frame,
		op_ret,
		op_errno);
  return 0;
}

int32_t
unify_fsck (call_frame_t *frame,
	      xlator_t *this,
	      int32_t flags)
{
  STACK_WIND (frame,
	      unify_fsck_cbk,
	      FIRST_CHILD(this),
	      FIRST_CHILD(this)->mops->fsck,
	      flags);
  return 0;
}


static int32_t
unify_lock_cbk (call_frame_t *frame,
		  void *cookie,
		  xlator_t *this,
		  int32_t op_ret,
		  int32_t op_errno)
{
  STACK_UNWIND (frame,
		op_ret,
		op_errno);
  return 0;
}

int32_t
unify_lock (call_frame_t *frame,
	      xlator_t *this,
	      const char *path)
{
  STACK_WIND (frame,
	      unify_lock_cbk,
	      FIRST_CHILD(this),
	      FIRST_CHILD(this)->mops->lock,
	      path);
  return 0;
}

static int32_t
unify_unlock_cbk (call_frame_t *frame,
		    void *cookie,
		    xlator_t *this,
		    int32_t op_ret,
		    int32_t op_errno)
{
  STACK_UNWIND (frame,
		op_ret,
		op_errno);
  return 0;
}

int32_t
unify_unlock (call_frame_t *frame,
		xlator_t *this,
		const char *path)
{
  STACK_WIND (frame,
	      unify_unlock_cbk,
	      FIRST_CHILD(this),
	      FIRST_CHILD(this)->mops->unlock,
	      path);
  return 0;
}


static int32_t
unify_listlocks_cbk (call_frame_t *frame,
		       void *cookie,
		       xlator_t *this,
		       int32_t op_ret,
		       int32_t op_errno,
		       char *locks)
{
  STACK_UNWIND (frame,
		op_ret,
		op_errno,
		locks);
  return 0;
}

int32_t
unify_listlocks (call_frame_t *frame,
		   xlator_t *this,
		   const char *pattern)
{
  STACK_WIND (frame,
	      unify_listlocks_cbk,
	      FIRST_CHILD(this),
	      FIRST_CHILD(this)->mops->listlocks,
	      pattern);
  return 0;
}

static int32_t
unify_getspec_cbk (call_frame_t *frame,
		     void *cookie,
		     xlator_t *this,
		     int32_t op_ret,
		     int32_t op_errno,
		     char *spec_data)
{
  STACK_UNWIND (frame,
		op_ret,
		op_errno,
		spec_data);
  return 0;
}

int32_t
unify_getspec (call_frame_t *frame,
		 xlator_t *this,
		 int32_t flags)
{
  STACK_WIND (frame,
	      unify_getspec_cbk,
	      FIRST_CHILD(this),
	      FIRST_CHILD(this)->mops->getspec,
	      flags);
  return 0;
}

#if 0
/* setxattr */
static int32_t  
unify_setxattr_cbk (call_frame_t *frame,
		    void *cookie,
		    xlator_t *xl,
		    int32_t op_ret,
		    int32_t op_errno)
{
  unify_local_t *local = (unify_local_t *)frame->local;
  int32_t callcnt;

  if (op_ret == -1 && op_errno != ENOENT && op_errno != ENOTCONN) {
    local->op_errno = op_errno;
  }
  if (op_ret == 0) 
    local->op_ret = 0;

  LOCK (&frame->mutex);
  callcnt = ++local->call_count;
  UNLOCK (&frame->mutex);

  if (callcnt == ((struct cement_private *)xl->private)->child_count) {
    LOCK_DESTROY (&frame->mutex);
    STACK_UNWIND (frame, local->op_ret, local->op_errno);

  }
  return 0;
}

static int32_t  
unify_setxattr (call_frame_t *frame,
		xlator_t *xl,
		const char *path,
		const char *name,
		const char *value,
		size_t size,
		int32_t flags)
{
  unify_local_t *local = calloc (1, sizeof (unify_local_t));
  xlator_list_t *trav = xl->children;

  frame->local = (void *)local;
  local->op_ret = -1;
  local->op_errno = ENOENT;
  INIT_LOCK (&frame->mutex);

  while (trav) {
    STACK_WIND (frame, 
		unify_setxattr_cbk,
		trav->xlator,
		trav->xlator->fops->setxattr,
		path, 
		name, 
		value, 
		size, 
		flags);
    trav = trav->next;
  }
  return 0;
} 


/* getxattr */
static int32_t  
unify_getxattr_cbk (call_frame_t *frame,
		    void *cookie,
		    xlator_t *xl,
		    int32_t op_ret,
		    int32_t op_errno,
		    void *value)
{
  int32_t callcnt;
  unify_local_t *local = (unify_local_t *)frame->local;

  if (op_ret == -1 && op_errno != ENOENT && op_errno != ENOTCONN) {
    local->op_errno = op_errno;
  }

  if (op_ret >= 0) {

    char *tmp_value = calloc (1, sizeof (op_ret));
    LOCK (&frame->mutex);
    memcpy (tmp_value, value, op_ret);
    if (local->buf)
      /* if file existed in two places by corruption */
      free (local->buf);
    local->buf = tmp_value;
    UNLOCK (&frame->mutex);
    local->op_ret = op_ret;
  }

  LOCK (&frame->mutex);
  callcnt = ++local->call_count;
  UNLOCK (&frame->mutex);

  if (callcnt == ((struct cement_private *)xl->private)->child_count) {
    frame->local = NULL;
    LOCK_DESTROY (&frame->mutex);
    STACK_UNWIND (frame,
		  local->op_ret,
		  local->op_errno,
		  local->buf ? local->buf : "");
    if (local->buf)
      free (local->buf);
    free (local);
  }
  return 0;
}

static int32_t  
unify_getxattr (call_frame_t *frame,
		xlator_t *xl,
		const char *path,
		const char *name,
		size_t size)
{
  unify_local_t *local = calloc (1, sizeof (unify_local_t));
  xlator_list_t *trav = xl->children;
  
  frame->local = (void *)local;
  local->op_ret = -1;
  local->op_errno = ENOENT;
  INIT_LOCK (&frame->mutex);
  
  while (trav) {
    STACK_WIND (frame, 
		unify_getxattr_cbk,
		trav->xlator,
		trav->xlator->fops->getxattr,
		path,
		name,
		size);
    trav = trav->next;
  }
  return 0;
} 


/* listxattr */
static int32_t  
unify_listxattr_cbk (call_frame_t *frame,
		     void *cookie,
		     xlator_t *xl,
		     int32_t op_ret,
		     int32_t op_errno,
		     void *value)
{
  int32_t callcnt;
  unify_local_t *local = (unify_local_t *)frame->local;

  if (op_ret == -1 && op_errno != ENOENT && op_errno != ENOTCONN) {
    local->op_errno = op_errno;
  }

  if (op_ret >= 0) {
    char *tmp_value = calloc (1, sizeof (op_ret));
    LOCK (&frame->mutex);
    memcpy (tmp_value, value, op_ret);
    if (local->buf)
      free (local->buf);
    local->buf = tmp_value;    
    UNLOCK (&frame->mutex);
    local->op_ret = op_ret;
  }

  LOCK (&frame->mutex);
  callcnt = ++local->call_count;
  UNLOCK (&frame->mutex);
  if (callcnt == ((struct cement_private *)xl->private)->child_count) {
    frame->local = NULL;
    LOCK_DESTROY (&frame->mutex);
    STACK_UNWIND (frame,
		  local->op_ret,
		  local->op_errno,
		  local->buf ? local->buf : "");
    if (local->buf)
      free (local->buf);
    free (local);
  }
  return 0;
}

static int32_t  
unify_listxattr (call_frame_t *frame,
		 xlator_t *xl,
		 const char *path,
		 size_t size)
{
  unify_local_t *local = calloc (1, sizeof (unify_local_t));
  xlator_list_t *trav = xl->children;
  
  frame->local = (void *)local;
  local->op_ret = -1;
  local->op_errno = ENOENT;
  INIT_LOCK (&frame->mutex);

  while (trav) {
    STACK_WIND (frame, 
		unify_listxattr_cbk,
		trav->xlator,
		trav->xlator->fops->listxattr,
		path,
		size);
    trav = trav->next;
  }
  return 0;
} 


/* removexattr */     
static int32_t  
unify_removexattr_cbk (call_frame_t *frame,
		       void *cookie,
		       xlator_t *xl,
		       int32_t op_ret,
		       int32_t op_errno)
{
  int32_t callcnt;
  unify_local_t *local = (unify_local_t *)frame->local;

  if (op_ret == -1 && op_errno != ENOENT && op_errno != ENOTCONN) {
    local->op_errno = op_errno;
  }
  if (op_ret == 0) 
    local->op_ret = 0;

  LOCK (&frame->mutex);
  callcnt = ++local->call_count;
  UNLOCK (&frame->mutex);

  if (callcnt == ((struct cement_private *)xl->private)->child_count) {
    LOCK_DESTROY (&frame->mutex);
    STACK_UNWIND (frame, local->op_ret, local->op_errno);
  }
  return 0;
}

static int32_t  
unify_removexattr (call_frame_t *frame,
		   xlator_t *xl,
		   const char *path,
		   const char *name)
{
  unify_local_t *local = calloc (1, sizeof (unify_local_t));
  xlator_list_t *trav = xl->children;

  frame->local = (void *)local;
  local->op_ret = -1;
  local->op_errno = ENOENT;
  INIT_LOCK (&frame->mutex);

  while (trav) {
    STACK_WIND (frame, 
		unify_removexattr_cbk,
		trav->xlator,
		trav->xlator->fops->removexattr,
		path,
		name);
    trav = trav->next;
  }
  return 0;
} 


/* open */
static int32_t  
unify_open_cbk (call_frame_t *frame,
		void *cookie,
		xlator_t *xl,
		int32_t op_ret,
		int32_t op_errno,
		dict_t *file_ctx,
		struct stat *stbuf)
{
  int32_t callcnt;
  unify_local_t *local = (unify_local_t *)frame->local;

  if (op_ret != 0 && op_errno != ENOTCONN && op_errno != ENOENT) {
    local->op_errno = op_errno;
  }
  if (op_ret >= 0) {
    // put the child node's address in ctx->contents
    dict_set (file_ctx,
	      xl->name,
	      data_from_ptr (cookie));

    if (local->orig_frame) {
      STACK_UNWIND (local->orig_frame,
		    op_ret,
		    op_errno,
		    file_ctx,
		    stbuf);
      local->orig_frame = NULL;
    }
  }

  LOCK (&frame->mutex);
  callcnt = ++local->call_count;
  UNLOCK (&frame->mutex);

  if (callcnt == ((struct cement_private *)xl->private)->child_count) {
    if (local->orig_frame) {
      STACK_UNWIND (local->orig_frame,
		    local->op_ret,
		    local->op_errno,
		    file_ctx,
		    stbuf);
      local->orig_frame = NULL;
    }

    frame->local = NULL;
    
    LOCK_DESTROY (&frame->mutex);
    STACK_DESTROY (frame->root);

    free (local);
  }
  return 0;
}


static int32_t  
unify_open (call_frame_t *frame,
	    xlator_t *xl,
	    const char *path,
	    int32_t flags,
	    mode_t mode)
{
  call_frame_t *open_frame = copy_frame (frame);
  unify_local_t *local = calloc (1, sizeof (unify_local_t));  
  xlator_list_t *trav = xl->children;
  char *lpath = alloca (strlen (path) + 1);

  strcpy (lpath, path);
  open_frame->local = local;
  local->op_ret = -1;
  local->op_errno = ENOENT;
  local->orig_frame = frame;
  INIT_LOCK (&frame->mutex);  

  while (trav) {
    _STACK_WIND (open_frame,
		 unify_open_cbk,
		 trav->xlator,  //cookie
		 trav->xlator,
		 trav->xlator->fops->open,
		 lpath,
		 flags,
		 mode);
    trav = trav->next;
  }

  return 0;
} 


/* read */
static int32_t  
unify_readv_cbk (call_frame_t *frame,
		 void *cookie,
		 xlator_t *xl,
		 int32_t op_ret,
		 int32_t op_errno,
		 struct iovec *vector,
		 int32_t count)
{
  STACK_UNWIND (frame, op_ret, op_errno, vector, count);
  return 0;
}

static int32_t  
unify_readv (call_frame_t *frame,
	     xlator_t *xl,
	     dict_t *file_ctx,
	     size_t size,
	     off_t offset)
{
  data_t *fd_data = dict_get (file_ctx, xl->name);

  if (!fd_data) {
    STACK_UNWIND (frame, -1, EBADFD, "");
    return -1;
  }  
  xlator_t *child = data_to_ptr (fd_data);

  STACK_WIND (frame, 
	      unify_readv_cbk,
	      child,
	      child->fops->readv,
	      file_ctx,
	      size,
	      offset);
  return 0;
} 

/* write */
static int32_t  
unify_writev_cbk (call_frame_t *frame,
		  void *cookie,
		  xlator_t *xl,
		  int32_t op_ret,
		  int32_t op_errno)
{
  STACK_UNWIND (frame, op_ret, op_errno);
  return 0;
}

static int32_t  
unify_writev (call_frame_t *frame,
	      xlator_t *xl,
	      dict_t *file_ctx,
	      struct iovec *vector,
	      int32_t count,
	      off_t offset)

{
  data_t *fd_data = dict_get (file_ctx, xl->name);

  if (!fd_data) {
    STACK_UNWIND (frame, -1, EBADFD);
    return -1;
  }

  xlator_t *child = data_to_ptr (fd_data);

  STACK_WIND (frame, 
	      unify_writev_cbk,
	      child,
	      child->fops->writev,
	      file_ctx,
	      vector,
	      count,
	      offset);
  return 0;
} 


/* ftruncate */
static int32_t  
unify_ftruncate_cbk (call_frame_t *frame,
		     void *cookie,
		     xlator_t *xl,
		     int32_t op_ret,
		     int32_t op_errno,
		     struct stat *stbuf)
{
  STACK_UNWIND (frame, op_ret, op_errno, stbuf);
  return 0;
}

static int32_t  
unify_ftruncate (call_frame_t *frame,
		 xlator_t *xl,
		 dict_t *file_ctx,
		 off_t offset)
{
  data_t *fd_data = dict_get (file_ctx, xl->name);

  if (!fd_data) {
    struct stat nullbuf = {0, };
    STACK_UNWIND (frame, -1, EBADFD, &nullbuf);
    return -1;
  }

  xlator_t *child = data_to_ptr (fd_data);

  STACK_WIND (frame, 
	      unify_ftruncate_cbk,
	      child,
	      child->fops->ftruncate,
	      file_ctx,
	      offset);
  return 0;
} 


/* fgetattr */
static int32_t  
unify_fgetattr_cbk (call_frame_t *frame,
		    void *cookie,
		    xlator_t *xl,
		    int32_t op_ret,
		    int32_t op_errno,
		    struct stat *stbuf)
{
  STACK_UNWIND (frame, op_ret, op_errno, stbuf);
  return 0;
}

static int32_t  
unify_fgetattr (call_frame_t *frame,
		xlator_t *xl,
		dict_t *file_ctx)
{
  data_t *fd_data = dict_get (file_ctx, xl->name);

  if (!fd_data) {
    struct stat nullbuf = {0, };
    STACK_UNWIND (frame, -1, EBADFD, &nullbuf);
    return -1;
  }
  xlator_t *child = data_to_ptr (fd_data);

  STACK_WIND (frame, 
	      unify_fgetattr_cbk,
	      child,
	      child->fops->fgetattr,
	      file_ctx);
  return 0;
} 

/* flush */
static int32_t  
unify_flush_cbk (call_frame_t *frame,
		 void *cookie,
		 xlator_t *xl,
		 int32_t op_ret,
		 int32_t op_errno)
{
  STACK_UNWIND (frame, op_ret, op_errno);
  return 0;
}

static int32_t  
unify_flush (call_frame_t *frame,
	     xlator_t *xl,
	     dict_t *file_ctx)
{
  data_t *fd_data = dict_get (file_ctx, xl->name);

  if (!fd_data) {
    STACK_UNWIND (frame, -1, EBADFD);
    return -1;
  }
  xlator_t *child = data_to_ptr (fd_data);
  
  STACK_WIND (frame,
	      unify_flush_cbk,
	      child,
	      child->fops->flush,
	      file_ctx);
  return 0;
} 

/* release */
static int32_t  
unify_release_cbk (call_frame_t *frame,
		   void *cookie,
		   xlator_t *xl,
		   int32_t op_ret,
		   int32_t op_errno)
{
  STACK_UNWIND (frame, op_ret, op_errno);
  return 0;
}

static int32_t  
unify_release (call_frame_t *frame,
	       xlator_t *xl,
	       dict_t *file_ctx)
{
  data_t *fd_data = dict_get (file_ctx, xl->name);

  if (!fd_data) {
    STACK_UNWIND (frame, -1, EBADFD);
    return -1;
  }

  xlator_t *child = data_to_ptr (fd_data);

  STACK_WIND (frame, 
	      unify_release_cbk,
	      child,
	      child->fops->release,
	      file_ctx);

  return 0;
} 


/* fsync */
static int32_t  
unify_fsync_cbk (call_frame_t *frame,
		 void *cookie,
		 xlator_t *xl,
		 int32_t op_ret,
		 int32_t op_errno)
{
  STACK_UNWIND (frame, op_ret, op_errno);
  return 0;
}

static int32_t  
unify_fsync (call_frame_t *frame,
	     xlator_t *xl,
	     dict_t *file_ctx,
	     int32_t flags)
{
  data_t *fd_data = dict_get (file_ctx, xl->name);

  if (!fd_data) {
    STACK_UNWIND (frame, -1, EBADFD);
    return -1;
  }

  xlator_t *child = data_to_ptr (fd_data);
  
  STACK_WIND (frame, 
	      unify_fsync_cbk,
	      child,
	      child->fops->fsync,
	      file_ctx,
	      flags);
  
  return 0;
} 

/* lk */
static int32_t  
unify_lk_cbk (call_frame_t *frame,
	      void *cookie,
	      xlator_t *xl,
	      int32_t op_ret,
	      int32_t op_errno,
	      struct flock *lock)
{
  STACK_UNWIND (frame, op_ret, op_errno, lock);
  return 0;
}

static int32_t  
unify_lk (call_frame_t *frame,
	  xlator_t *xl,
	  dict_t *file_ctx,
	  int32_t cmd,
	  struct flock *lock)
{
  data_t *fd_data = dict_get (file_ctx, xl->name);

  if (!fd_data) {
    STACK_UNWIND (frame, -1, EBADFD, "");
    return -1;
  }  
  xlator_t *child = data_to_ptr (fd_data);

  STACK_WIND (frame, 
	      unify_lk_cbk,
	      child,
	      child->fops->lk,
	      file_ctx,
	      cmd,
	      lock);
  return 0;
} 


/* getattr */
static int32_t  
unify_getattr_cbk (call_frame_t *frame,
		   void *cookie,
		   xlator_t *xl,
		   int32_t op_ret,
		   int32_t op_errno,
		   struct stat *stbuf)
{
  unify_local_t *local = (unify_local_t *)frame->local;
  call_frame_t *orig_frame;
  int32_t callcnt;

  if (op_ret == -1 && op_errno != ENOENT && op_errno != ENOTCONN) {
    local->op_errno = op_errno;
  }

  if (op_ret == 0) {
    LOCK (&frame->mutex);
    orig_frame = local->orig_frame;
    local->orig_frame = NULL;
    UNLOCK (&frame->mutex);

    if (orig_frame) {
      STACK_UNWIND (orig_frame, op_ret, op_errno, stbuf);
    }
  }

  LOCK (&frame->mutex);
  callcnt = ++local->call_count;
  UNLOCK (&frame->mutex);

  if (callcnt == ((struct cement_private *)xl->private)->child_count) {
    if (local->orig_frame)
      STACK_UNWIND (local->orig_frame,
		    local->op_ret,
		    local->op_errno,
		    &local->stbuf);
    LOCK_DESTROY (&frame->mutex);
    STACK_DESTROY (frame->root);
  }
  return 0;
}

static int32_t  
unify_getattr (call_frame_t *frame,
	       xlator_t *xl,
	       const char *path)
{
  call_frame_t *getattr_frame = copy_frame (frame);
  unify_local_t *local = calloc (1, sizeof (unify_local_t));
  xlator_list_t *trav = xl->children;
  char *lpath = alloca (strlen (path) + 1);

  strcpy (lpath, path);
  INIT_LOCK (&frame->mutex);
  getattr_frame->local = local;
  local->op_ret = -1;
  local->op_errno = ENOENT;
  local->orig_frame = frame;
  
  while (trav) {
    STACK_WIND (getattr_frame,
		unify_getattr_cbk,
		trav->xlator,
		trav->xlator->fops->getattr,
		lpath);
    trav = trav->next;
  }
  return 0;
} 


/* statfs */
static int32_t  
unify_statfs_cbk (call_frame_t *frame,
		  void *cookie,
		  xlator_t *xl,
		  int32_t op_ret,
		  int32_t op_errno,
		  struct statvfs *stbuf)
{
  unify_local_t *local = (unify_local_t *)frame->local;
  int32_t callcnt;

  if (op_ret != 0 && op_errno != ENOTCONN) {
    local->op_errno = op_errno;
  }
  if (op_ret == 0) {
    LOCK (&frame->mutex);
    struct statvfs *dict_buf = &local->statvfs_buf;
    dict_buf->f_bsize   = stbuf->f_bsize;
    dict_buf->f_frsize  = stbuf->f_frsize;
    dict_buf->f_blocks += stbuf->f_blocks;
    dict_buf->f_bfree  += stbuf->f_bfree;
    dict_buf->f_bavail += stbuf->f_bavail;
    dict_buf->f_files  += stbuf->f_files;
    dict_buf->f_ffree  += stbuf->f_ffree;
    dict_buf->f_favail += stbuf->f_favail;
    dict_buf->f_fsid    = stbuf->f_fsid;
    dict_buf->f_flag    = stbuf->f_flag;
    dict_buf->f_namemax = stbuf->f_namemax;
    local->op_ret = 0;
    UNLOCK (&frame->mutex);
  }

  LOCK (&frame->mutex);
  callcnt = ++local->call_count;
  UNLOCK (&frame->mutex);

  if (callcnt == ((struct cement_private *)xl->private)->child_count) {
    LOCK_DESTROY (&frame->mutex);
    STACK_UNWIND (frame, local->op_ret, local->op_errno, &local->statvfs_buf);
  }
  return 0;
}


static int32_t  
unify_statfs (call_frame_t *frame,
	      xlator_t *xl,
	      const char *path)
{
  xlator_list_t *trav = xl->children;
  unify_local_t *local = calloc (1, sizeof (unify_local_t));

  frame->local = (void *)local;
  local->op_ret = -1;
  local->op_errno = ENOTCONN;
  INIT_LOCK (&frame->mutex);

  while (trav) {
    STACK_WIND (frame, 
		unify_statfs_cbk,
		trav->xlator,
		trav->xlator->fops->statfs,
		path);
    trav = trav->next;
  }
  return 0;
} 


/* truncate */
static int32_t  
unify_truncate_cbk (call_frame_t *frame,
		    void *cookie,
		    xlator_t *xl,
		    int32_t op_ret,
		    int32_t op_errno,
		    struct stat *stbuf)
{
  unify_local_t *local = (unify_local_t *)frame->local;
  int32_t callcnt;

  if (op_ret == -1 && op_errno != ENOENT && op_errno != ENOTCONN) {
    local->op_errno = op_errno;
  }

  if (op_ret == 0) {
    LOCK (&frame->mutex);
    local->stbuf = *stbuf;
    local->op_ret = 0;
    UNLOCK (&frame->mutex);
  }
  
  LOCK (&frame->mutex);
  callcnt = ++local->call_count;
  UNLOCK (&frame->mutex);

  if (callcnt == ((struct cement_private *)xl->private)->child_count) {
    LOCK_DESTROY (&frame->mutex);
    STACK_UNWIND (frame, local->op_ret, local->op_errno, &local->stbuf);
  }
  return 0;
}

static int32_t  
unify_truncate (call_frame_t *frame,
		xlator_t *xl,
		const char *path,
		off_t offset)
{
  unify_local_t *local = calloc (1, sizeof (unify_local_t));
  xlator_list_t *trav = xl->children;

  frame->local = (void *)local;
  INIT_LOCK (&frame->mutex);
  local->op_ret = -1;
  local->op_errno = ENOENT;
  
  while (trav) {
    STACK_WIND (frame, 
		unify_truncate_cbk,
		trav->xlator,
		trav->xlator->fops->truncate,
		path,
		offset);
    trav = trav->next;
  }
  return 0;
} 

/* utimes */
static int32_t  
unify_utimes_cbk (call_frame_t *frame,
		 void *cookie,
		 xlator_t *xl,
		 int32_t op_ret,
		 int32_t op_errno,
		 struct stat *stbuf)
{
  int32_t callcnt;
  unify_local_t *local = (unify_local_t *)frame->local;

  if (op_ret == -1 && op_errno != ENOENT && op_errno != ENOTCONN) {
    local->op_errno = op_errno;
  }

  if (op_ret == 0) {
    LOCK (&frame->mutex);
    local->stbuf = *stbuf;
    local->op_ret = 0;  
    UNLOCK (&frame->mutex);
  }
  
  LOCK (&frame->mutex);
  callcnt = ++local->call_count;
  UNLOCK (&frame->mutex);

  if (callcnt == ((struct cement_private *)xl->private)->child_count) {
    LOCK_DESTROY (&frame->mutex);
    STACK_UNWIND (frame, local->op_ret, local->op_errno, &local->stbuf);
  }
  return 0;
}

static int32_t  
unify_utimes (call_frame_t *frame,
	      xlator_t *xl,
	      const char *path,
	      struct timespec *buf)
{
  unify_local_t *local = calloc (1, sizeof (unify_local_t));
  xlator_list_t *trav = xl->children;

  frame->local = (void *)local;
  INIT_LOCK (&frame->mutex);
  local->op_ret = -1;
  local->op_errno = ENOENT;

  while (trav) {
    STACK_WIND (frame, 
		unify_utimes_cbk,
		trav->xlator,
		trav->xlator->fops->utimes,
		path,
		buf);
    trav = trav->next;
  }
  return 0;
}


/* opendir */
static int32_t  
unify_opendir_cbk (call_frame_t *frame,
		   void *cookie,
		   xlator_t *xl,
		   int32_t op_ret,
		   int32_t op_errno,
		   dict_t *file_ctx)
{
  unify_local_t *local = frame->local;
  dict_t *ctx = local->file_ctx;
  int32_t callcnt;
  if (op_ret != 0 && op_errno != ENOENT && op_errno != ENOTCONN) {
    local->op_errno = op_errno;
  }
  if (op_ret == 0 && local->op_ret != 0) {
    local->op_ret = op_ret;
    local->op_errno = op_errno;
  }
  
  LOCK (&frame->mutex);
  if (op_ret == 0) {
    dict_set (ctx, (char *)cookie, data_from_ptr (file_ctx));
  }
  callcnt = ++local->call_count;
  UNLOCK (&frame->mutex);

  if (callcnt == ((unify_private_t *)xl->private)->child_count) {
    LOCK_DESTROY (&frame->mutex);
    STACK_UNWIND (frame, local->op_ret, local->op_errno, ctx);
  }
  return 0;
}

static int32_t  
unify_opendir (call_frame_t *frame,
	       xlator_t *xl,
	       const char *path)
{
  unify_local_t *local = calloc (1, sizeof (unify_local_t));
  xlator_list_t *trav = xl->children;

  frame->local = (void *)local;
  local->file_ctx = get_new_dict ();
  while (trav) {
    _STACK_WIND (frame,
		 unify_opendir_cbk,
		 trav->xlator->name, //cookie
		 trav->xlator,
		 trav->xlator->fops->opendir,
		 path);
    trav = trav->next;
  }
  
  return 0;
} 


/* readlink */
static int32_t  
unify_readlink_cbk (call_frame_t *frame,
		    void *cookie,
		    xlator_t *xl,
		    int32_t op_ret,
		    int32_t op_errno,
		    char *buf)
{
  int32_t callcnt;
  unify_local_t *local = (unify_local_t *)frame->local;

  if (op_ret == -1 && op_errno != ENOENT && op_errno != ENOTCONN) {
    local->op_errno = op_errno;
  } else if (op_ret >= 0) {
    LOCK (&frame->mutex);
    if (local->buf)
      free (local->buf);
    local->buf = strdup (buf);
    local->op_ret = op_ret;
    UNLOCK (&frame->mutex);
  }

  LOCK (&frame->mutex);
  callcnt = ++local->call_count;
  UNLOCK (&frame->mutex);

  if (callcnt == ((struct cement_private *)xl->private)->child_count) {
    frame->local = NULL;
    LOCK_DESTROY (&frame->mutex);
    STACK_UNWIND (frame,
		  local->op_ret,
		  local->op_errno,
		  local->buf ? local->buf : "");

    if (local->buf)
      free (local->buf);
    free (local);

  }
  return 0;
}

static int32_t  
unify_readlink (call_frame_t *frame,
		xlator_t *xl,
		const char *path,
		size_t size)
{
  unify_local_t *local = calloc (1, sizeof (unify_local_t));
  xlator_list_t *trav = xl->children;

  frame->local = (void *)local;
  INIT_LOCK (&frame->mutex);
  local->op_ret = -1;
  local->op_errno = ENOENT;
  
  while (trav) {
    STACK_WIND (frame, 
		unify_readlink_cbk,
		trav->xlator,
		trav->xlator->fops->readlink,
		path,
		size);
    trav = trav->next;
  }
  return 0;
} 

/* readdir */
static int32_t  
unify_readdir_cbk (call_frame_t *frame,
		   void *cookie,
		   xlator_t *xl,
		   int32_t op_ret,
		   int32_t op_errno,
		   dir_entry_t *entry,
		   int32_t count)
{
  int32_t callcnt, tmp_count;
  dir_entry_t *trav, *prev, *tmp, *unify_entry;
  unify_local_t *local = (unify_local_t *)frame->local;

  if (op_ret >= 0) {
    LOCK (&frame->mutex);
    trav = entry->next;
    prev = entry;
    if (local->entry == NULL) {
      unify_entry = calloc (1, sizeof (dir_entry_t));
      unify_entry->next = trav;

      while (trav->next) 
	trav = trav->next;
      local->entry = unify_entry;
      local->last = trav;
      local->count = count;
    } else {
      // copy only file names
      tmp_count = count;
      while (trav) {
	tmp = trav;
	if (S_ISDIR (tmp->buf.st_mode)) {
	  prev->next = tmp->next;
	  trav = tmp->next;
	  free (tmp->name);
	  free (tmp);
	  tmp_count--;
	  continue;
	}
	prev = trav;
	trav = trav->next;
      }
      // append the current dir_entry_t at the end of the last node
      local->last->next = entry->next;
      local->count += tmp_count;
      while (local->last->next)
	local->last = local->last->next;
    }
    entry->next = NULL;
    UNLOCK (&frame->mutex);
  }
  if ((op_ret == -1 && op_errno != ENOTCONN) ||
      (op_ret == -1 && op_errno == ENOTCONN &&
       (!((struct cement_private *)xl->private)->readdir_force_success))) {
    local->op_ret = -1;
    local->op_errno = op_errno;
  }

  LOCK (&frame->mutex);
  callcnt = ++local->call_count;
  UNLOCK (&frame->mutex);

  if (callcnt == ((struct cement_private *)xl->private)->child_count) {
    prev = local->entry;

    STACK_UNWIND (frame, local->op_ret, local->op_errno, local->entry, local->count);
    if (prev) {
      trav = prev->next;
      while (trav) {
	prev->next = trav->next;
	free (trav->name);
	free (trav);
	trav = prev->next;
      }
      free (prev);
    }
  }
  return 0;
}

static int32_t  
unify_readdir (call_frame_t *frame,
	       xlator_t *xl,
	       const char *path)
{
  xlator_list_t *trav = xl->children;
  frame->local = (void *)calloc (1, sizeof (unify_local_t));
  
  INIT_LOCK (&frame->mutex);

  while (trav) {
    STACK_WIND (frame,
		unify_readdir_cbk,
		trav->xlator,
		trav->xlator->fops->readdir,
		path);
    trav = trav->next;
  }
  return 0;
} 

/* FOPS with LOCKs */

/* Start of mkdir */
static int32_t  
unify_mkdir_unlock_cbk (call_frame_t *frame,
			void *cookie,
			xlator_t *xl,
			int32_t op_ret,
			int32_t op_errno)
{ 
  STACK_DESTROY (frame->root);
  return 0;
}

static int32_t  
unify_mkdir_cbk (call_frame_t *frame,
		 void *cookie,
		 xlator_t *xl,
		 int32_t op_ret,
		 int32_t op_errno,
		 struct stat *stbuf)
{
  int32_t callcnt;
  unify_local_t *local = (unify_local_t *)frame->local;

  if (op_ret != 0 && op_errno != ENOTCONN) {
    local->op_ret = -1;
    local->op_errno = op_errno;
  }

  if (op_ret == 0) {
    /* this is done for every op_ret == 0, see if this can be avoided */
    LOCK (&frame->mutex);
    local->stbuf = *stbuf;
    UNLOCK (&frame->mutex);
  }

  LOCK (&frame->mutex);
  callcnt = ++local->call_count;
  UNLOCK (&frame->mutex);

  if (callcnt == ((struct cement_private *)xl->private)->child_count) {
    call_frame_t *unwind_frame = copy_frame (frame);

    /* return to fop first and then call the unlock */
    frame->local = NULL;
    LOCK_DESTROY (&frame->mutex);
    STACK_UNWIND (frame, 
		  local->op_ret,
		  local->op_errno,
		  &local->stbuf);
    
    STACK_WIND (unwind_frame,
		unify_mkdir_unlock_cbk,
		LOCK_NODE(xl),
		LOCK_NODE(xl)->mops->unlock,
		local->path);
    free (local->path);
    free (local);
  }
  return 0;
}

static int32_t  
unify_mkdir_lock_cbk (call_frame_t *frame,
		      void *cookie,
		      xlator_t *xl,
		      int32_t op_ret,
		      int32_t op_errno)
{
  unify_local_t *local = (unify_local_t *)frame->local;
  xlator_list_t *trav;  

  if (op_ret == 0) {
    trav = xl->children;
    INIT_LOCK (&frame->mutex);
    local->op_ret = 0;
    local->op_errno = 0;
    while (trav) {
      STACK_WIND (frame,
		  unify_mkdir_cbk,
		  trav->xlator,
		  trav->xlator->fops->mkdir,
		  local->path,
		  local->mode);
      trav = trav->next;
    }
  } else {
    struct stat nullbuf = {0, };
    frame->local = NULL;
    STACK_UNWIND (frame, -1, op_errno, &nullbuf);
    free (local->path);
    free (local);
  }
  return 0;
}

static int32_t  
unify_mkdir (call_frame_t *frame,
	     xlator_t *xl,
	     const char *path,
	     mode_t mode)
{
  unify_local_t *local = calloc (1, sizeof (unify_local_t));

  frame->local = (void *)local;
  local->mode = mode;
  local->path = strdup (path);
  STACK_WIND (frame, 
	      unify_mkdir_lock_cbk,
	      LOCK_NODE(xl),
	      LOCK_NODE(xl)->mops->lock,
	      path);
  return 0;
} /* End of mkdir */


/* unlink */
static int32_t  
unify_unlink_unlock_cbk (call_frame_t *frame,
			 void *cookie,
			 xlator_t *xl,
			 int32_t op_ret,
			 int32_t op_errno)
{ 
  STACK_DESTROY (frame->root);
  return 0;
}

static int32_t  
unify_unlink_cbk (call_frame_t *frame,
		  void *cookie,
		  xlator_t *xl,
		  int32_t op_ret,
		  int32_t op_errno)
{
  int32_t callcnt;
  unify_local_t *local = (unify_local_t *)frame->local;
  if (op_ret == -1 && op_errno != ENOENT && op_errno != ENOTCONN) {
    local->op_errno = op_errno;
  }
  if (op_ret == 0) 
    local->op_ret = 0;

  LOCK (&frame->mutex);
  callcnt = ++local->call_count;
  UNLOCK (&frame->mutex);

  if (callcnt == ((struct cement_private *)xl->private)->child_count) {
    call_frame_t *unwind_frame = copy_frame (frame);
    frame->local = NULL;
    LOCK_DESTROY (&frame->mutex);
    STACK_UNWIND (frame, local->op_ret, local->op_errno);
    STACK_WIND (unwind_frame,
		unify_unlink_unlock_cbk,
		LOCK_NODE(xl),
		LOCK_NODE(xl)->mops->unlock,
		local->path);
    free (local->path);
    free (local);
  }
  return 0;
}

static int32_t  
unify_unlink_lock_cbk (call_frame_t *frame,
		       void *cookie,
		       xlator_t *xl,
		       int32_t op_ret,
		       int32_t op_errno)
{
  unify_local_t *local = (unify_local_t *)frame->local;
  xlator_list_t *trav;

  if (op_ret == 0) {
    trav = xl->children;
    INIT_LOCK (&frame->mutex);
    local->op_ret = -1;
    local->op_errno = ENOENT;
    while (trav) {
      STACK_WIND (frame,
		  unify_unlink_cbk,
		  trav->xlator,
		  trav->xlator->fops->unlink,
		  local->path);
      trav = trav->next;
    }
  } else {
    frame->local = NULL;
    STACK_UNWIND (frame, -1, op_errno);
    free (local->path);
    free (local);
  }
  return 0;
}

static int32_t  
unify_unlink (call_frame_t *frame,
	      xlator_t *xl,
	      const char *path)
{
  unify_local_t *local = calloc (1, sizeof (unify_local_t));

  frame->local = (void *)local;
  local->path = strdup (path);
  STACK_WIND (frame, 
	      unify_unlink_lock_cbk,
	      LOCK_NODE(xl),
	      LOCK_NODE(xl)->mops->lock,
	      path);
  return 0;
} /* End of unlink */


/* rmdir */
static int32_t  
unify_rmdir_unlock_cbk (call_frame_t *frame,
			void *cookie,
			xlator_t *xl,
			int32_t op_ret,
			int32_t op_errno)
{ 
  STACK_DESTROY (frame->root);
  return 0;
}

static int32_t  
unify_rmdir_cbk (call_frame_t *frame,
		 void *cookie,
		 xlator_t *xl,
		 int32_t op_ret,
		 int32_t op_errno)
{
  int32_t callcnt;
  unify_local_t *local = (unify_local_t *)frame->local;

  if (op_ret != 0 && op_errno != ENOTCONN) {
    local->op_ret = -1;
    local->op_errno = op_errno;
  }

  LOCK (&frame->mutex);
  callcnt = ++local->call_count;
  UNLOCK (&frame->mutex);

  if (callcnt == ((struct cement_private *)xl->private)->child_count) {
    call_frame_t *unwind_frame = copy_frame (frame);
    frame->local = NULL;
    LOCK_DESTROY (&frame->mutex);
    STACK_UNWIND (frame, local->op_ret, local->op_errno);
    STACK_WIND (unwind_frame,
		unify_rmdir_unlock_cbk,
		LOCK_NODE(xl),
		LOCK_NODE(xl)->mops->unlock,
		local->path);
    free (local->path);
    free (local);
  }
  return 0;
}


static int32_t  
unify_rmdir_getattr_cbk (call_frame_t *frame,
			 void *cookie,
			 xlator_t *xl,
			 int32_t op_ret,
			 int32_t op_errno,
			 struct stat *stbuf)
{
  int32_t callcnt;
  unify_local_t *local = (unify_local_t *)frame->local;

  if (op_ret != 0 && op_errno != ENOTCONN) {
    local->op_ret = -1;
    local->op_errno = op_errno;
  }
  if (op_ret == 0) {
    /* Check if the directory is empty or not. */
    if (stbuf->st_nlink > 2) {
      local->op_ret = -1;
      local->op_errno = ENOTEMPTY;
    }
  }

  LOCK (&frame->mutex);
  callcnt = ++local->call_count;
  UNLOCK (&frame->mutex);

  if (callcnt == ((struct cement_private *)xl->private)->child_count) {
    if (local->op_ret == 0) { 
      xlator_list_t *trav = xl->children;
      INIT_LOCK (&frame->mutex);
      local->op_ret = 0;
      local->op_errno = 0;
      local->call_count = 0;
      while (trav) {
	STACK_WIND (frame,
		    unify_rmdir_cbk,
		    trav->xlator,
		    trav->xlator->fops->rmdir,
		    local->path);
	trav = trav->next;
      }
    } else {
      call_frame_t *unwind_frame = copy_frame (frame);
      frame->local = NULL;
      LOCK_DESTROY (&frame->mutex);
      STACK_UNWIND (frame, local->op_ret, local->op_errno);
      STACK_WIND (unwind_frame,
		  unify_rmdir_unlock_cbk,
		  LOCK_NODE(xl),
		  LOCK_NODE(xl)->mops->unlock,
		  local->path);
      free (local->path);
      free (local);
    }
  }
  return 0;
}

static int32_t  
unify_rmdir_lock_cbk (call_frame_t *frame,
		      void *cookie,
		      xlator_t *xl,
		      int32_t op_ret,
		      int32_t op_errno)
{
  unify_local_t *local = (unify_local_t *)frame->local;
  xlator_list_t *trav;

  if (op_ret == 0) {
    trav = xl->children;
    INIT_LOCK (&frame->mutex);
    local->op_ret = 0;
    local->op_errno = 0;
    while (trav) {
      STACK_WIND (frame,
		  unify_rmdir_getattr_cbk,
		  trav->xlator,
		  trav->xlator->fops->getattr,
		  local->path);
      trav = trav->next;
    }
  } else {
    frame->local = NULL;
    STACK_UNWIND (frame, -1, op_errno);
    free (local->path);
    free (local);
  }
  return 0;
}

static int32_t  
unify_rmdir (call_frame_t *frame,
	     xlator_t *xl,
	     const char *path)
{
  unify_local_t *local = calloc (1, sizeof (unify_local_t));

  frame->local = (void *)local;
  local->path = strdup (path);
  STACK_WIND (frame, 
	      unify_rmdir_lock_cbk,
	      LOCK_NODE(xl),
	      LOCK_NODE(xl)->mops->lock,
	      path);
  return 0;
} 


/* create */
static int32_t  
unify_create_unlock_cbk (call_frame_t *frame,
			 void *cookie,
			 xlator_t *xl,
			 int32_t op_ret,
			 int32_t op_errno)
{ 
  STACK_DESTROY (frame->root);
  return 0;
}


static int32_t  
unify_create_cbk (call_frame_t *frame,
		  void *cookie,
		  xlator_t *xl,
		  int32_t op_ret,
		  int32_t op_errno,
		  dict_t *file_ctx,
		  struct stat *stbuf)
{
  unify_local_t *local = (unify_local_t *)frame->local;

  local->op_ret = op_ret;
  local->op_errno = op_errno;

  if (op_ret >= 0) {
    dict_set (file_ctx,
	      xl->name,
	      data_from_ptr (cookie));
    local->file_ctx = file_ctx;
    local->stbuf = *stbuf;
  }

  call_frame_t *unwind_frame = copy_frame (frame);
  frame->local = NULL;
  LOCK_DESTROY (&frame->mutex);
  STACK_UNWIND (frame,
		local->op_ret,
		local->op_errno,
		local->file_ctx,
		&local->stbuf);
  
  STACK_WIND (unwind_frame,
	      unify_create_unlock_cbk,
	      LOCK_NODE(xl),
	      LOCK_NODE(xl)->mops->unlock,
	      local->path);
  free (local->path);
  free (local);

  return 0;
}

static int32_t  
unify_create_getattr_cbk (call_frame_t *frame,
			  void *cookie,
			  xlator_t *xl,
			  int32_t op_ret,
			  int32_t op_errno,
			  struct stat *stbuf)
{
  int32_t callcnt;
  unify_local_t *local = (unify_local_t *)frame->local;
  struct cement_private *priv = xl->private;
  struct sched_ops *ops = priv->sched_ops;
  
  if (op_ret == 0) {
    local->op_ret = -1;
    local->op_errno = EEXIST;
    local->found_xl = ((call_frame_t *)cookie)->this;
  }

  if (op_ret == -1 && op_errno != ENOENT && op_errno != ENOTCONN) {
    local->op_errno = op_errno;
  }

  LOCK (&frame->mutex);
  callcnt = ++local->call_count;
  UNLOCK (&frame->mutex);

  if (callcnt == ((struct cement_private *)xl->private)->child_count) {
    if (local->op_ret == -1 && local->op_errno == ENOENT) {
      xlator_t *sched_xl = NULL;
      
      sched_xl = ops->schedule (xl, 0);
      
      _STACK_WIND (frame,
		   unify_create_cbk,
		   sched_xl, //cookie
		   sched_xl,
		   sched_xl->fops->create,
		   local->path,
		   local->flags,
		   local->mode);
      
      local->sched_xl = sched_xl;
    } else if (local->op_ret == -1 && local->op_errno == EEXIST) {
      if ((local->flags & O_EXCL) == O_EXCL) {
	call_frame_t *unwind_frame = copy_frame (frame);
	frame->local = NULL;
	LOCK_DESTROY (&frame->mutex);
	STACK_UNWIND (frame,
		      local->op_ret,
		      local->op_errno,
		      local->file_ctx,
		      &local->stbuf);
	
	STACK_WIND (unwind_frame,
		    unify_create_unlock_cbk,
		    LOCK_NODE(xl),
		    LOCK_NODE(xl)->mops->unlock,
		    local->path);
	free (local->path);
	free (local);
      } else {
	_STACK_WIND (frame,
		     unify_create_cbk,
		     local->found_xl, //cookie
		     local->found_xl,
		     local->found_xl->fops->create,
		     local->path,
		     local->flags,
		     local->mode);
      }
    } else {
      /* TODO: Not very sure what should be the error */
      call_frame_t *unwind_frame = copy_frame (frame);
      frame->local = NULL;
      LOCK_DESTROY (&frame->mutex);
      STACK_UNWIND (frame,
		    local->op_ret,
		    local->op_errno,
		    local->file_ctx,
		    &local->stbuf);
      
      STACK_WIND (unwind_frame,
		  unify_create_unlock_cbk,
		  LOCK_NODE(xl),
		  LOCK_NODE(xl)->mops->unlock,
		  local->path);
      free (local->path);
      free (local);
    }
  }
  return 0;
}

static int32_t  
unify_create_lock_cbk (call_frame_t *frame,
		       void *cookie,
		       xlator_t *xl,
		       int32_t op_ret,
		       int32_t op_errno)
{
  unify_local_t *local = (unify_local_t *)frame->local;
  xlator_list_t *trav;

  if (op_ret == 0) {
    trav = xl->children;
    INIT_LOCK (&frame->mutex);
    local->op_ret = -1;
    local->op_errno = ENOENT;
    
    while (trav) {
      STACK_WIND (frame,
		  unify_create_getattr_cbk,
		  trav->xlator,
		  trav->xlator->fops->getattr,
		  local->path);
      trav = trav->next;
    }
  } else {
    struct stat nullbuf = {0, };
    frame->local = NULL;
    STACK_UNWIND (frame, -1, op_errno, &nullbuf);
    free (local->path);
    free (local);
  }
  
  return 0;
}

static int32_t  
unify_create (call_frame_t *frame,
	      xlator_t *xl,
	      const char *path,
	      int32_t flags,
	      mode_t mode)
{
  unify_local_t *local = calloc (1, sizeof (unify_local_t));
  
  frame->local = (void *)local;
  local->path = strdup (path);
  local->mode = mode;
  local->flags = flags;

  STACK_WIND (frame, 
	      unify_create_lock_cbk,
	      LOCK_NODE(xl),
	      LOCK_NODE(xl)->mops->lock,
	      path);
  return 0;
} 


/* mknod */
static int32_t  
unify_mknod_unlock_cbk (call_frame_t *frame,
			void *cookie,
			xlator_t *xl,
			int32_t op_ret,
			int32_t op_errno)
{ 
  STACK_DESTROY (frame->root);
  return 0;
}

static int32_t  
unify_mknod_cbk (call_frame_t *frame,
		 void *cookie,
		 xlator_t *xl,
		 int32_t op_ret,
		 int32_t op_errno,
		 struct stat *stbuf)
{
  unify_local_t *local = (unify_local_t *)frame->local;
  call_frame_t *unwind_frame = copy_frame (frame);
  
  if (op_ret == 0)
    memcpy (&local->stbuf, stbuf, sizeof (struct stat));
  local->op_ret = op_ret;
  local->op_errno = op_errno;

  frame->local = NULL;
  LOCK_DESTROY (&frame->mutex);
  STACK_UNWIND (frame, local->op_ret, local->op_errno, &local->stbuf);
  STACK_WIND (unwind_frame,
	      unify_mknod_unlock_cbk,
	      LOCK_NODE(xl),
	      LOCK_NODE(xl)->mops->unlock,
	      local->path);
  free (local->path);
  free (local);
  return 0;
}

static int32_t  
unify_mknod_getattr_cbk (call_frame_t *frame,
			 void *cookie,
			 xlator_t *xl,
			 int32_t op_ret,
			 int32_t op_errno,
			 struct stat *stbuf)
{
  int32_t callcnt;
  unify_local_t *local = (unify_local_t *)frame->local;

  if (op_ret == 0) {
    local->op_ret = -1;
    local->op_errno = EEXIST;
  }

  if (op_ret == -1 && op_errno != ENOENT && op_errno != ENOTCONN) {
    local->op_errno = op_errno;
  }

  LOCK (&frame->mutex);
  callcnt = ++local->call_count;
  UNLOCK (&frame->mutex);

  if (callcnt == ((struct cement_private *)xl->private)->child_count) {
    if (local->op_ret == -1 && local->op_errno == ENOENT) {
      xlator_t *sched_xl = NULL;
      struct cement_private *priv = xl->private;
      struct sched_ops *ops = priv->sched_ops;

      sched_xl = ops->schedule (xl, 0);
      
      STACK_WIND (frame,
		  unify_mknod_cbk,
		  sched_xl,
		  sched_xl->fops->mknod,
		  local->path,
		  local->mode,
		  local->dev);
    } else {
      call_frame_t *unwind_frame = copy_frame (frame);
      frame->local = NULL;
      LOCK_DESTROY (&frame->mutex);
      STACK_UNWIND (frame, local->op_ret, local->op_errno, &local->stbuf);
      STACK_WIND (unwind_frame,
		  unify_mknod_unlock_cbk,
		  LOCK_NODE(xl),
		  LOCK_NODE(xl)->mops->unlock,
		  local->path);
      free (local->path);
      free (local);
    }
  }
  return 0;
}


static int32_t  
unify_mknod_lock_cbk (call_frame_t *frame,
		      void *cookie,
		      xlator_t *xl,
		      int32_t op_ret,
		      int32_t op_errno)
{
  unify_local_t *local = (unify_local_t *)frame->local;
  xlator_list_t *trav;

  if (op_ret == 0) {
    trav = xl->children;
    INIT_LOCK (&frame->mutex);
    local->op_ret = -1;
    local->op_errno = ENOENT;

    while (trav) {
      STACK_WIND (frame,
		  unify_mknod_getattr_cbk,
		  trav->xlator,
		  trav->xlator->fops->getattr,
		  local->path);
      trav = trav->next;
    }
  } else {
    struct stat nullbuf = {0, };
    frame->local = NULL;
    STACK_UNWIND (frame, -1, op_errno, &nullbuf);
    free (local->path);
    free (local);
  }
  return 0;
}

static int32_t  
unify_mknod (call_frame_t *frame,
	     xlator_t *xl,
	     const char *path,
	     mode_t mode,
	     dev_t dev)
{
  unify_local_t *local = calloc (1, sizeof (unify_local_t));

  frame->local = (void *)local;
  local->dev = dev;
  local->mode = mode;
  local->path = strdup (path);
  STACK_WIND (frame, 
	      unify_mknod_lock_cbk,
	      LOCK_NODE(xl),
	      LOCK_NODE(xl)->mops->lock,
	      path);

  return 0;
} 

/* symlink */
static int32_t  
unify_symlink_unlock_cbk (call_frame_t *frame,
			  void *cookie,
			  xlator_t *xl,
			  int32_t op_ret,
			  int32_t op_errno)
{ 
  STACK_DESTROY (frame->root);
  return 0;
}

static int32_t  
unify_symlink_cbk (call_frame_t *frame,
		   void *cookie,
		   xlator_t *xl,
		   int32_t op_ret,
		   int32_t op_errno,
		   struct stat *stbuf)
{
  unify_local_t *local = (unify_local_t *)frame->local;
  call_frame_t *unwind_frame = copy_frame (frame);
  if (op_ret == 0)
    memcpy (&local->stbuf, stbuf, sizeof (struct stat));

  local->op_ret = op_ret;
  local->op_errno = op_errno;

  frame->local = NULL;
  LOCK_DESTROY (&frame->mutex);
  STACK_UNWIND (frame, local->op_ret, local->op_errno, &local->stbuf);
  STACK_WIND (unwind_frame,
	      unify_symlink_unlock_cbk,
	      LOCK_NODE(xl),
	      LOCK_NODE(xl)->mops->unlock,
	      local->new_path);
  free (local->path);
  free (local->new_path);
  free (local);

  return 0;
}

static int32_t 
unify_symlink_getattr_cbk (call_frame_t *frame,
			   void *cookie,
			   xlator_t *xl,
			   int32_t op_ret,
			   int32_t op_errno)
{
  int32_t callcnt;
  unify_local_t *local = (unify_local_t *)frame->local;

  if (op_ret == 0) {
    local->op_ret = -1;
    local->op_errno = EEXIST;
  }

  if (op_ret == -1 && op_errno != ENOENT && op_errno != ENOTCONN) {
    local->op_errno = op_errno;
  }

  LOCK (&frame->mutex);
  callcnt = ++local->call_count;
  UNLOCK (&frame->mutex);

  if (callcnt == ((struct cement_private *)xl->private)->child_count) {
    if (local->op_ret == -1 && local->op_errno == ENOENT) {
      xlator_t *sched_xl = NULL;
      struct cement_private *priv = xl->private;
      struct sched_ops *ops = priv->sched_ops;

      sched_xl = ops->schedule (xl, 0);
            
      STACK_WIND (frame,
		  unify_symlink_cbk,
		  sched_xl,
		  sched_xl->fops->symlink,
		  local->path,
		  local->new_path);
    } else {
      call_frame_t *unwind_frame = copy_frame (frame);
      frame->local = NULL;
      LOCK_DESTROY (&frame->mutex);
      STACK_UNWIND (frame, local->op_ret, local->op_errno, &local->stbuf);
      STACK_WIND (unwind_frame,
		  unify_symlink_unlock_cbk,
		  LOCK_NODE(xl),
		  LOCK_NODE(xl)->mops->unlock,
		  local->new_path);
      free (local->path);
      free (local->new_path);
      free (local);
    }
  }

  return 0;
}

static int32_t  
unify_symlink_lock_cbk (call_frame_t *frame,
			void *cookie,
			xlator_t *xl,
			int32_t op_ret,
			int32_t op_errno)
{
  unify_local_t *local = (unify_local_t *)frame->local;
  xlator_list_t *trav;

  if (op_ret == 0) {
    trav = xl->children;
    INIT_LOCK (&frame->mutex);
    local->op_ret = -1;
    local->op_errno = ENOENT;
    while (trav) {
      STACK_WIND (frame,
		  unify_symlink_getattr_cbk,
		  trav->xlator,
		  trav->xlator->fops->getattr,
		  local->new_path);
      trav = trav->next;
    }
  } else {
    struct stat nullbuf = {0, };
    frame->local = NULL;
    STACK_UNWIND (frame, -1, op_errno, &nullbuf);
    free (local->path);
    free (local->new_path);
    free (local);
  }
  return 0;
}

static int32_t  
unify_symlink (call_frame_t *frame,
	       xlator_t *xl,
	       const char *oldpath,
	       const char *newpath)
{
  unify_local_t *local = calloc (1, sizeof (unify_local_t));

  frame->local = (void *)local;
  local->path = strdup (oldpath);
  local->new_path = strdup (newpath);
  STACK_WIND (frame, 
	      unify_symlink_lock_cbk,
	      LOCK_NODE(xl),
	      LOCK_NODE(xl)->mops->lock,
	      newpath);
  return 0;
} 


/* rename */
static int32_t  
unify_rename_unlock_cbk (call_frame_t *frame,
		       void *cookie,
		       xlator_t *xl,
		       int32_t op_ret,
		       int32_t op_errno)
{ 
  STACK_DESTROY (frame->root);
  return 0;
}

static int32_t
unify_rename_unlink_newpath_cbk (call_frame_t *frame,
				 void *cookie,
				 xlator_t *xl,
				 int32_t op_ret,
				 int32_t op_errno)
{
  unify_local_t *local = (unify_local_t *)frame->local;
  call_frame_t *unwind_frame = copy_frame (frame);
  frame->local = NULL;
  LOCK_DESTROY (&frame->mutex);
  STACK_UNWIND (frame, local->op_ret, local->op_errno);
  STACK_WIND (unwind_frame,
	      unify_rename_unlock_cbk,
	      LOCK_NODE(xl),
	      LOCK_NODE(xl)->mops->unlock,
	      local->buf);
  free (local->buf);
  free (local->path);
  free (local->new_path);
  free (local);
  return 0;
}


static int32_t
unify_rename_dir_cbk (call_frame_t *frame,
                      void *cookie,
                      xlator_t *xl,
                      int32_t op_ret,
                      int32_t op_errno)
{
  int32_t callcnt;
  unify_local_t *local = (unify_local_t *)frame->local;

  if (op_ret == -1 && op_errno != ENOENT) {
    local->op_ret = op_ret;
    local->op_errno = op_errno;
  }
  
  LOCK (&frame->mutex);
  callcnt = ++local->call_count;
  UNLOCK (&frame->mutex);

  if (callcnt == ((struct cement_private *)xl->private)->child_count) {
    call_frame_t *unwind_frame = copy_frame (frame);
    frame->local = NULL;
    LOCK_DESTROY (&frame->mutex);
    STACK_UNWIND (frame, local->op_ret, local->op_errno);
    STACK_WIND (unwind_frame,
                unify_rename_unlock_cbk,
                LOCK_NODE(xl),
                LOCK_NODE(xl)->mops->unlock,
                local->buf);
    free (local->buf);
    free (local->path);
    free (local->new_path);
    free (local);
  }
  return 0;
}


static int32_t  
unify_rename_cbk (call_frame_t *frame,
		  void *cookie,
		  xlator_t *xl,
		  int32_t op_ret,
		  int32_t op_errno)
{
  unify_local_t *local = (unify_local_t *)frame->local;

  local->op_ret = op_ret;
  local->op_errno = op_errno;

  if (!op_ret && local->found_xl && local->found_xl != local->sched_xl) {
    STACK_WIND (frame,
		unify_rename_unlink_newpath_cbk,
		local->found_xl,
		local->found_xl->fops->unlink,
		local->new_path);
  } else {
    call_frame_t *unwind_frame = copy_frame (frame);
    frame->local = NULL;
    LOCK_DESTROY (&frame->mutex);
    STACK_UNWIND (frame, local->op_ret, local->op_errno);
    STACK_WIND (unwind_frame,
                unify_rename_unlock_cbk,
                LOCK_NODE(xl),
                LOCK_NODE(xl)->mops->unlock,
                local->buf);
    free (local->buf);
    free (local->path);
    free (local->new_path);
    free (local);
  }
  return 0;
}


static int32_t  
unify_rename_newpath_lookup_cbk (call_frame_t *frame,
				 void *cookie,
				 xlator_t *xl,
				 int32_t op_ret,
				 int32_t op_errno,
				 struct stat *stbuf)
{
  int32_t callcnt;
  unify_local_t *local = (unify_local_t *)frame->local;

  if (op_ret == 0) {
    local->found_xl = cookie;
    if (S_ISDIR(stbuf->st_mode) && !S_ISDIR(local->stbuf.st_mode))
      local->op_errno = EISDIR;
    else if (S_ISDIR(stbuf->st_mode))
      local->op_errno = EEXIST;
  }

  if (op_ret == -1 && op_errno != ENOENT && op_errno != ENOTCONN) {
    local->op_ret = op_ret;
    local->op_errno = op_errno;
  }

  LOCK (&frame->mutex);
  callcnt = ++local->call_count;
  UNLOCK (&frame->mutex);

  if (callcnt == ((struct cement_private *)xl->private)->child_count) {
    if (local->op_ret == -1 && local->op_errno == ENOENT) {
      if (!S_ISDIR(local->stbuf.st_mode)) {
        STACK_WIND (frame,
                    unify_rename_cbk,
                    local->sched_xl,
                    local->sched_xl->fops->rename,
                    local->path,
                    local->new_path);
      } else {
        xlator_list_t *trav = xl->children;
        local->call_count = 0;
        local->op_ret = 0;
        local->op_errno = 0;
        while (trav) {
          STACK_WIND (frame,
                      unify_rename_dir_cbk,
                      trav->xlator,
                      trav->xlator->fops->rename,
                      local->path,
                      local->new_path);
          trav = trav->next;
        }
      }
    } else {
      call_frame_t *unwind_frame = copy_frame (frame);
      frame->local = NULL;
      LOCK_DESTROY (&frame->mutex);
      STACK_UNWIND (frame, local->op_ret, local->op_errno);
      STACK_WIND (unwind_frame,
		  unify_rename_unlock_cbk,
		  LOCK_NODE(xl),
		  LOCK_NODE(xl)->mops->unlock,
		  local->buf);
      free (local->buf);
      free (local->path);
      free (local->new_path);
      free (local);
    }
  }
  return 0;
}

static int32_t  
unify_rename_oldpath_lookup_cbk (call_frame_t *frame,
				 void *cookie,
				 xlator_t *xl,
				 int32_t op_ret,
				 int32_t op_errno,
				 struct stat *stbuf)
{
  int32_t callcnt;
  xlator_list_t *trav;
  unify_local_t *local = (unify_local_t *)frame->local;

  if ((op_ret == -1) && (op_errno != ENOENT) && (op_errno != ENOTCONN)) {
    local->op_errno = op_errno;
  }
  if (op_ret == 0) {
    local->op_ret = 0;
    LOCK (&frame->mutex);
    local->sched_xl = cookie;
    local->stbuf = *stbuf;
    UNLOCK (&frame->mutex);
  }
  
  LOCK (&frame->mutex);
  callcnt = ++local->call_count;
  UNLOCK (&frame->mutex);

  if (callcnt == ((struct cement_private *)xl->private)->child_count) {
    if (local->op_ret == 0) {
      trav = xl->children;
      local->op_ret = -1;
      local->op_errno = ENOENT;

      local->call_count = 0;

      while (trav) {
	_STACK_WIND (frame,
		     unify_rename_newpath_lookup_cbk,
		     trav->xlator, //cookie
		     trav->xlator,
		     trav->xlator->fops->getattr,
		     local->new_path);
	trav = trav->next;
      } 
    } else {
      call_frame_t *unwind_frame = copy_frame (frame);
      local->op_ret = -1;
      local->op_errno = ENOENT;
      
      frame->local = NULL;
      LOCK_DESTROY (&frame->mutex);
      STACK_UNWIND (frame, local->op_ret, local->op_errno);
      STACK_WIND (unwind_frame,
		  unify_rename_unlock_cbk,
		  LOCK_NODE(xl),
		  LOCK_NODE(xl)->mops->unlock,
		  local->buf);
      free (local->buf);
      free (local->path);
      free (local->new_path);
      free (local);

    }
  }
  return 0;
}

static int32_t  
unify_rename_lock_cbk (call_frame_t *frame,
		       void *cookie,
		       xlator_t *xl,
		       int32_t op_ret,
		       int32_t op_errno)
{
  unify_local_t *local = (unify_local_t *)frame->local;
  xlator_list_t *trav;  

  if (op_ret == 0) {
    trav = xl->children;
    INIT_LOCK (&frame->mutex);
    local->op_ret = -1;
    local->op_errno = ENOENT;

    while (trav) {
      _STACK_WIND (frame,
		   unify_rename_oldpath_lookup_cbk,
		   trav->xlator, //cookie
		   trav->xlator,
		   trav->xlator->fops->getattr,
		   local->path);
      trav = trav->next;
    }
  } else {
    frame->local = NULL;
    STACK_UNWIND (frame, -1, op_errno);
    free (local->new_path);
    free (local->buf);
    free (local->path);
    free (local);
  }
  return 0;
}


static int32_t  
unify_rename (call_frame_t *frame,
	      xlator_t *xl,
	      const char *oldpath,
	      const char *newpath)
{
  unify_local_t *local = calloc (1, sizeof (unify_local_t));
  
  frame->local = (void *)local;
  local->buf = gcd_path (oldpath, newpath);
  local->new_path = strdup (newpath);
  local->path = strdup (oldpath);
  STACK_WIND (frame, 
	      unify_rename_lock_cbk,
	      LOCK_NODE(xl),
	      LOCK_NODE(xl)->mops->lock,
	      local->buf);
  return 0;
} 

/* link */
static int32_t  
unify_link_unlock_cbk (call_frame_t *frame,
		       void *cookie,
		       xlator_t *xl,
		       int32_t op_ret,
		       int32_t op_errno)
{ 
  STACK_DESTROY (frame->root);
  return 0;
}

static int32_t  
unify_link_cbk (call_frame_t *frame,
		void *cookie,
		xlator_t *xl,
		int32_t op_ret,
		int32_t op_errno,
		struct stat *stbuf)
{
  unify_local_t *local = (unify_local_t *)frame->local;
  call_frame_t *unwind_frame = copy_frame (frame);
  if (op_ret == 0) 
    memcpy (&local->stbuf, stbuf, sizeof (struct stat));

  local->op_ret = op_ret;
  local->op_errno = op_errno;

  frame->local = NULL;
  LOCK_DESTROY (&frame->mutex);
  STACK_UNWIND (frame, local->op_ret, local->op_errno, &local->stbuf);
  STACK_WIND (unwind_frame,
	      unify_link_unlock_cbk,
	      LOCK_NODE(xl),
	      LOCK_NODE(xl)->mops->unlock,
	      local->buf);
  free (local->buf);
  free (local->path);
  free (local->new_path);
  free (local);

  return 0;
}


static int32_t  
unify_link_newpath_lookup_cbk (call_frame_t *frame,
			       void *cookie,
			       xlator_t *xl,
			       int32_t op_ret,
			       int32_t op_errno,
			       struct stat *stbuf)
{
  int32_t callcnt;
  unify_local_t *local = (unify_local_t *)frame->local;
  
  if (!op_ret) {
    local->op_ret = -1;
    local->op_errno = EEXIST;
  }

  if (op_ret == -1 && op_errno != ENOENT && op_errno != ENOTCONN) {
    local->op_errno = op_errno;
  }

  LOCK (&frame->mutex);
  callcnt = ++local->call_count;
  UNLOCK (&frame->mutex);

  if (callcnt == ((struct cement_private *)xl->private)->child_count) {
    if (local->op_ret == -1 && local->op_errno == ENOENT) {
      STACK_WIND (frame,
		  unify_link_cbk,
		  local->sched_xl,
		  local->sched_xl->fops->link,
		  local->path,
		  local->new_path);
    } else {
      call_frame_t *unwind_frame = copy_frame (frame);
      frame->local = NULL;
      LOCK_DESTROY (&frame->mutex);
      STACK_UNWIND (frame, local->op_ret, local->op_errno, &local->stbuf);
      STACK_WIND (unwind_frame,
		  unify_link_unlock_cbk,
		  LOCK_NODE(xl),
		  LOCK_NODE(xl)->mops->unlock,
		  local->buf);
      free (local->buf);
      free (local->path);
      free (local->new_path);
      free (local);
    }
  }
  return 0;
}

static int32_t  
unify_link_oldpath_lookup_cbk (call_frame_t *frame,
			       void *cookie,
			       xlator_t *xl,
			       int32_t op_ret,
			       int32_t op_errno,
			       struct stat *stbuf)
{
  int32_t callcnt;
  xlator_list_t *trav;
  unify_local_t *local = (unify_local_t *)frame->local;

  if (op_ret == -1 && op_errno != ENOENT && op_errno != ENOTCONN) {
    local->op_errno = op_errno;
  }
  if (op_ret == 0) {
    local->op_ret = 0;
    local->sched_xl = cookie; //prev_frame->this;
  }

  LOCK (&frame->mutex);
  callcnt = ++local->call_count;
  UNLOCK (&frame->mutex);

  if (callcnt == ((struct cement_private *)xl->private)->child_count) {
    if (local->op_ret == 0) {
      trav = xl->children;
      INIT_LOCK (&frame->mutex);
      local->op_ret = -1;
      local->op_errno = ENOENT;
      local->call_count = 0;
      
      while (trav) {
	STACK_WIND (frame,
		    unify_link_newpath_lookup_cbk,
		    trav->xlator,
		    trav->xlator->fops->getattr,
		    local->new_path);
	trav = trav->next;
      } 
    } else {
      /* op_ret == -1; */
      call_frame_t *unwind_frame = copy_frame (frame);
      frame->local = NULL;
      LOCK_DESTROY (&frame->mutex);
      STACK_UNWIND (frame, local->op_ret, local->op_errno, &local->stbuf);
      STACK_WIND (unwind_frame,
		  unify_link_unlock_cbk,
		  LOCK_NODE(xl),
		  LOCK_NODE(xl)->mops->unlock,
		  local->buf);
      free (local->buf);
      free (local->path);
      free (local->new_path);
      free (local);
    }
  }
  return 0;
}

static int32_t  
unify_link_lock_cbk (call_frame_t *frame,
		     void *cookie,
		     xlator_t *xl,
		     int32_t op_ret,
		     int32_t op_errno)
{
  unify_local_t *local = (unify_local_t *)frame->local;
  xlator_list_t *trav;  

  if (op_ret == 0) {
    trav = xl->children;
    INIT_LOCK (&frame->mutex);
    local->op_ret = -1;
    local->op_errno = ENOENT;

    while (trav) {
      _STACK_WIND (frame,
		   unify_link_oldpath_lookup_cbk,
		   trav->xlator, //cookie
		   trav->xlator,
		   trav->xlator->fops->getattr,
		   local->path);
      trav = trav->next;
    }
  } else {
    struct stat nullbuf = {0, };
    frame->local = NULL;
    STACK_UNWIND (frame, -1, op_errno, &nullbuf);
    free (local->buf);
    free (local->path);
    free (local->new_path);
    free (local);
  }
  return 0;
}

static int32_t  
unify_link (call_frame_t *frame,
	    xlator_t *xl,
	    const char *oldpath,
	    const char *newpath)
{
  unify_local_t *local = calloc (1, sizeof (unify_local_t));
  
  frame->local = (void *)local;
  local->buf = gcd_path (oldpath, newpath);
  local->path = strdup (oldpath);
  local->new_path = strdup (newpath);
  STACK_WIND (frame, 
	      unify_link_lock_cbk,
	      LOCK_NODE(xl),
	      LOCK_NODE(xl)->mops->lock,
	      local->buf);
  return 0;
} 


/* chmod */
static int32_t  
unify_chmod_unlock_cbk (call_frame_t *frame,
			void *cookie,
			xlator_t *xl,
			int32_t op_ret,
			int32_t op_errno)
{ 
  STACK_DESTROY (frame->root);
  return 0;
}

static int32_t  
unify_chmod_cbk (call_frame_t *frame,
		 void *cookie,
		 xlator_t *xl,
		 int32_t op_ret,
		 int32_t op_errno,
		 struct stat *stbuf)
{
  int32_t callcnt;
  unify_local_t *local = (unify_local_t *)frame->local;

  if (op_ret == -1 && op_errno != ENOENT && op_errno != ENOENT) {
    local->op_errno = op_errno;
  }

  if (op_ret == 0) {
    local->op_ret = 0;
    LOCK (&frame->mutex);
    local->stbuf = *stbuf;
    UNLOCK (&frame->mutex);
  }

  LOCK (&frame->mutex);
  callcnt = ++local->call_count;
  UNLOCK (&frame->mutex);

  if (callcnt == ((struct cement_private *)xl->private)->child_count) {
    call_frame_t *unwind_frame = copy_frame (frame);
    frame->local = NULL;
    LOCK_DESTROY (&frame->mutex);
    STACK_UNWIND (frame, local->op_ret, local->op_errno, &local->stbuf);
    STACK_WIND (unwind_frame,
		unify_chmod_unlock_cbk,
		LOCK_NODE(xl),
		LOCK_NODE(xl)->mops->unlock,
		local->path);
    free (local->path);
    free (local);
  }
  return 0;
}

static int32_t  
unify_chmod_lock_cbk (call_frame_t *frame,
		      void *cookie,
		      xlator_t *xl,
		      int32_t op_ret,
		      int32_t op_errno)
{
  unify_local_t *local = (unify_local_t *)frame->local;
  xlator_list_t *trav;
  
  if (op_ret == 0) {
    trav = xl->children;
    INIT_LOCK (&frame->mutex);
    local->op_ret = -1;
    local->op_errno = ENOENT;
    while (trav) {
      STACK_WIND (frame,
		  unify_chmod_cbk,
		  trav->xlator,
		  trav->xlator->fops->chmod,
		  local->path,
		  local->mode);
      trav = trav->next;
    }
  } else {
    struct stat nullbuf = {0, };
    frame->local = NULL;
    STACK_UNWIND (frame, -1, op_errno, &nullbuf);
    free (local->path);
    free (local);
  }
  return 0;
}

static int32_t  
unify_chmod (call_frame_t *frame,
	     xlator_t *xl,
	     const char *path,
	     mode_t mode)
{
  unify_local_t *local = calloc (1, sizeof (unify_local_t));

  frame->local = (void *)local;
  local->mode = mode;
  local->path = strdup (path);

  STACK_WIND (frame, 
	      unify_chmod_lock_cbk,
	      LOCK_NODE(xl),
	      LOCK_NODE(xl)->mops->lock,
	      path);
  return 0;
} 

/* chown */
static int32_t  
unify_chown_unlock_cbk (call_frame_t *frame,
			void *cookie,
			xlator_t *xl,
			int32_t op_ret,
			int32_t op_errno)
{ 
  STACK_DESTROY (frame->root);
  return 0;
}

static int32_t  
unify_chown_cbk (call_frame_t *frame,
		 void *cookie,
		 xlator_t *xl,
		 int32_t op_ret,
		 int32_t op_errno,
		 struct stat *stbuf)
{
  int32_t callcnt;
  unify_local_t *local = (unify_local_t *)frame->local;

  if (op_ret != 0 && op_errno != ENOENT && op_errno != ENOTCONN) {
    local->op_errno = op_errno;
  }
  if (op_ret == 0) {
    local->op_ret = 0;
    LOCK (&frame->mutex);
    local->stbuf = *stbuf;
    UNLOCK (&frame->mutex);
  }
  
  LOCK (&frame->mutex);
  callcnt = ++local->call_count;
  UNLOCK (&frame->mutex);

  if (callcnt == ((struct cement_private *)xl->private)->child_count) {
    call_frame_t *unwind_frame = copy_frame (frame);
    frame->local = NULL;
    LOCK_DESTROY (&frame->mutex);
    STACK_UNWIND (frame, local->op_ret, local->op_errno, &local->stbuf);
    STACK_WIND (unwind_frame,
		unify_chown_unlock_cbk,
		LOCK_NODE(xl),
		LOCK_NODE(xl)->mops->unlock,
		local->path);
    free (local->path);
    free (local);
  }
  return 0;
}

static int32_t  
unify_chown_lock_cbk (call_frame_t *frame,
		      void *cookie,
		      xlator_t *xl,
		      int32_t op_ret,
		      int32_t op_errno)
{
  unify_local_t *local = (unify_local_t *)frame->local;
  xlator_list_t *trav;
  
  if (op_ret == 0) {
    trav = xl->children;
    INIT_LOCK (&frame->mutex);
    local->op_ret = -1;
    local->op_errno = ENOENT;

    while (trav) {
      STACK_WIND (frame,
		  unify_chown_cbk,
		  trav->xlator,
		  trav->xlator->fops->chown,
		  local->path,
		  local->uid,
		  local->gid);

      trav = trav->next;
    }
  } else {
    struct stat nullbuf = {0, };
    frame->local = NULL;
    STACK_UNWIND (frame, -1, op_errno, &nullbuf);
    free (local->path);
    free (local);
  }
  return 0;
}

static int32_t  
unify_chown (call_frame_t *frame,
	     xlator_t *xl,
	     const char *path,
	     uid_t uid,
	     gid_t gid)
{
  unify_local_t *local = calloc (1, sizeof (unify_local_t));
  
  frame->local = (void *)local;
  local->uid = uid;
  local->gid = gid;
  local->path = strdup(path);
  STACK_WIND (frame, 
	      unify_chown_lock_cbk,
	      LOCK_NODE(xl),
	      LOCK_NODE(xl)->mops->lock,
	      path);

  return 0;
} 
#endif

int32_t 
init (xlator_t *this)
{
  unify_private_t *_private = calloc (1, sizeof (*_private));
  data_t *scheduler = dict_get (this->options, "scheduler");
  data_t *lock_node = dict_get (this->options, "lock-node");
  data_t *readdir_conf = dict_get (this->options, "readdir-force-success");
 
  if (!scheduler) {
    gf_log (this->name, GF_LOG_ERROR, "scheduler option is not provided\n");
    return -1;
  }

  if (!this->children) {
    gf_log (this->name,
	    GF_LOG_ERROR,
	    "FATAL - unify configured without children. cannot continue");
    return -1;
  }

  _private->sched_ops = get_scheduler (scheduler->data);

  /* update _private structure */
  {
    xlator_list_t *trav = this->children;
    int32_t count = 0;
    /* Get the number of child count */
    while (trav) {
      count++;
      trav = trav->next;
    }
    _private->child_count = count;
    gf_log (this->name, GF_LOG_DEBUG, "Child node count is %d", count);
    _private->array = (struct xlator **)calloc (1, sizeof (struct xlator *) * count);
    count = 0;
    trav = this->children;
    /* Update the child array */
    while (trav) {
      _private->array[count++] = trav->xlator;
      trav = trav->next;
    }
    if (readdir_conf) {
      if (strcmp (readdir_conf->data, "on") == 0) {
	_private->readdir_force_success = 1;
      }
    }
  }

  if(lock_node) {
    xlator_list_t *trav = this->children;

    gf_log (this->name, GF_LOG_DEBUG, "lock server specified as %s", lock_node->data);

    while (trav) {
      if(strcmp (trav->xlator->name, lock_node->data) == 0)
	break;
      trav = trav->next;
    }
    if (trav == NULL) {
      gf_log(this->name, GF_LOG_ERROR, "lock server not found among the children");
      return -1;
    }
    _private->lock_node = trav->xlator;
  } else {
    gf_log (this->name, GF_LOG_DEBUG, "lock server not specified, defaulting to %s", this->children->xlator->name);
    _private->lock_node = this->children->xlator;
  }

  this->private = (void *)_private;
  _private->sched_ops->init (this); // Initialize the schedular 
  return 0;
}

void
fini (xlator_t *this)
{
  unify_private_t *priv = this->private;
  priv->sched_ops->fini (this);
  free (priv);
  return;
}


struct xlator_fops fops = {
  .getattr     = unify_getattr,
  .readlink    = unify_readlink,
  .mknod       = unify_mknod,
  .mkdir       = unify_mkdir,
  .unlink      = unify_unlink,
  .rmdir       = unify_rmdir,
  .symlink     = unify_symlink,
  .rename      = unify_rename,
  .link        = unify_link,
  .chmod       = unify_chmod,
  .chown       = unify_chown,
  .truncate    = unify_truncate,
  .create      = unify_create,
  .open        = unify_open,
  .readv       = unify_readv,
  .writev      = unify_writev,
  .statfs      = unify_statfs,
  .flush       = unify_flush,
  .release     = unify_release,
  .fsync       = unify_fsync,
  .setxattr    = unify_setxattr,
  .getxattr    = unify_getxattr,
  .listxattr   = unify_listxattr,
  .removexattr = unify_removexattr,
  .opendir     = unify_opendir,
  .readdir     = unify_readdir,
  .releasedir  = unify_releasedir,
  .fsyncdir    = unify_fsyncdir,
  .access      = unify_access,
  .ftruncate   = unify_ftruncate,
  .fgetattr    = unify_fgetattr,
  .lk          = unify_lk,
  .fchown      = unify_fchown,
  .fchmod      = unify_fchmod,
  .utimens     = unify_utimens,
  .futimens    = unify_futimens,
  .lookup      = unify_lookup,
  .forget      = unify_forget,
};

struct xlator_mops mops = {
  .stats = unify_stats
};
