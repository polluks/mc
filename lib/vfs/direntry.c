/** \file
 *  \brief Source: directory cache support
 *
 *  So that you do not have copy of this in each and every filesystem.
 *
 *  Very loosely based on tar.c from midnight and archives.[ch] from
 *  avfs by Miklos Szeredi (mszeredi@inf.bme.hu)
 *
 *  Unfortunately, I was unable to keep all filesystems
 *  uniform. tar-like filesystems use tree structure where each
 *  directory has pointers to its subdirectories. We can do this
 *  because we have full information about our archive.
 *
 *  At ftp-like filesystems, situation is a little bit different. When
 *  you cd /usr/src/linux/drivers/char, you do _not_ want /usr,
 *  /usr/src, /usr/src/linux and /usr/src/linux/drivers to be
 *  listed. That means that we do not have complete information, and if
 *  /usr is symlink to /4, we will not know. Also we have to time out
 *  entries and things would get messy with tree-like approach. So we
 *  do different trick: root directory is completely special and
 *  completely fake, it contains entries such as 'usr', 'usr/src', ...,
 *  and we'll try to use custom find_entry function.
 *
 *  \author Pavel Machek <pavel@ucw.cz>, distribute under LGPL.
 *  \date 1998
 *
 *  \warning Paths here do _not_ begin with '/', so root directory of
 *  archive/site is simply "".
 */

#include <config.h>

#include <errno.h>
#include <fcntl.h>              /* include fcntl.h -> sys/fcntl.h only       */
                                /* includes fcntl.h see IEEE Std 1003.1-2008 */
#include <time.h>
#include <sys/time.h>           /* gettimeofday() */
#include <inttypes.h>           /* uintmax_t */
#include <stdarg.h>

#include "lib/global.h"

#include "lib/tty/tty.h"        /* enable/disable interrupt key */
#include "lib/util.h"           /* concat_dir_and_file */
#if 0
#include "lib/widget.h"         /* message() */
#endif

#include "vfs.h"
#include "utilvfs.h"
#include "xdirentry.h"
#include "gc.h"                 /* vfs_rmstamp */

/*** global variables ****************************************************************************/

/*** file scope macro definitions ****************************************************************/

#define CALL(x) if (MEDATA->x) MEDATA->x

/*** file scope type declarations ****************************************************************/

struct dirhandle
{
    GList *cur;
    struct vfs_s_inode *dir;
};

/*** file scope variables ************************************************************************/

static volatile int total_inodes = 0, total_entries = 0;

/*** file scope functions ************************************************************************/
/* --------------------------------------------------------------------------------------------- */

static int
vfs_s_entry_compare (const void *a, const void *b)
{
    const struct vfs_s_entry *e = (const struct vfs_s_entry *) a;
    const char *name = (const char *) b;

    return strcmp (e->name, name);
}

/* --------------------------------------------------------------------------------------------- */

static void
vfs_s_free_inode (struct vfs_class *me, struct vfs_s_inode *ino)
{
    if (ino == NULL)
        vfs_die ("Don't pass NULL to me");

    /* ==0 can happen if freshly created entry is deleted */
    if (ino->st.st_nlink > 1)
    {
        ino->st.st_nlink--;
        return;
    }

    while (ino->subdir != NULL)
        vfs_s_free_entry (me, (struct vfs_s_entry *) ino->subdir->data);

    CALL (free_inode) (me, ino);
    g_free (ino->linkname);
    if (ino->localname != NULL)
    {
        unlink (ino->localname);
        g_free (ino->localname);
    }
    total_inodes--;
    ino->super->ino_usage--;
    g_free (ino);
}

/* --------------------------------------------------------------------------------------------- */
/* We were asked to create entries automagically */

static struct vfs_s_entry *
vfs_s_automake (struct vfs_class *me, struct vfs_s_inode *dir, char *path, int flags)
{
    struct vfs_s_entry *res;
    char *sep;

    sep = strchr (path, PATH_SEP);
    if (sep != NULL)
        *sep = '\0';

    res = vfs_s_generate_entry (me, path, dir, flags & FL_MKDIR ? (0777 | S_IFDIR) : 0777);
    vfs_s_insert_entry (me, dir, res);

    if (sep != NULL)
        *sep = PATH_SEP;

    return res;
}

/* --------------------------------------------------------------------------------------------- */
/* If the entry is a symlink, find the entry for its target */

static struct vfs_s_entry *
vfs_s_resolve_symlink (struct vfs_class *me, struct vfs_s_entry *entry, int follow)
{
    char *linkname;
    char *fullname = NULL;
    struct vfs_s_entry *target;

    if (follow == LINK_NO_FOLLOW)
        return entry;
    if (follow == 0)
        ERRNOR (ELOOP, NULL);
    if (!entry)
        ERRNOR (ENOENT, NULL);
    if (!S_ISLNK (entry->ino->st.st_mode))
        return entry;

    linkname = entry->ino->linkname;
    if (linkname == NULL)
        ERRNOR (EFAULT, NULL);

    /* make full path from relative */
    if (*linkname != PATH_SEP)
    {
        char *fullpath = vfs_s_fullpath (me, entry->dir);
        if (fullpath)
        {
            fullname = g_strconcat (fullpath, "/", linkname, (char *) NULL);
            linkname = fullname;
            g_free (fullpath);
        }
    }

    target = (MEDATA->find_entry) (me, entry->dir->super->root, linkname, follow - 1, 0);
    g_free (fullname);
    return target;
}

/* --------------------------------------------------------------------------------------------- */
/*
 * Follow > 0: follow links, serves as loop protect,
 *       == -1: do not follow links
 */

static struct vfs_s_entry *
vfs_s_find_entry_tree (struct vfs_class *me, struct vfs_s_inode *root,
                       const char *a_path, int follow, int flags)
{
    size_t pseg;
    struct vfs_s_entry *ent = NULL;
    char *const pathref = g_strdup (a_path);
    char *path = pathref;

    /* canonicalize as well, but don't remove '../' from path */
    custom_canonicalize_pathname (path, CANON_PATH_ALL & (~CANON_PATH_REMDOUBLEDOTS));

    while (root != NULL)
    {
        GList *iter;

        while (*path == PATH_SEP)       /* Strip leading '/' */
            path++;

        if (path[0] == '\0')
        {
            g_free (pathref);
            return ent;
        }

        for (pseg = 0; path[pseg] != '\0' && path[pseg] != PATH_SEP; pseg++)
            ;

        for (iter = root->subdir; iter != NULL; iter = g_list_next (iter))
        {
            ent = (struct vfs_s_entry *) iter->data;
            if (strlen (ent->name) == pseg && strncmp (ent->name, path, pseg) == 0)
                /* FOUND! */
                break;
        }

        ent = iter != NULL ? (struct vfs_s_entry *) iter->data : NULL;

        if (ent == NULL && (flags & (FL_MKFILE | FL_MKDIR)) != 0)
            ent = vfs_s_automake (me, root, path, flags);
        if (ent == NULL)
        {
            me->verrno = ENOENT;
            goto cleanup;
        }

        path += pseg;
        /* here we must follow leading directories always;
           only the actual file is optional */
        ent = vfs_s_resolve_symlink (me, ent,
                                     strchr (path, PATH_SEP) != NULL ? LINK_FOLLOW : follow);
        if (ent == NULL)
            goto cleanup;
        root = ent->ino;
    }
  cleanup:
    g_free (pathref);
    return NULL;
}

/* --------------------------------------------------------------------------------------------- */

static void
split_dir_name (struct vfs_class *me, char *path, char **dir, char **name, char **save)
{
    char *s;

    (void) me;

    s = strrchr (path, PATH_SEP);
    if (s == NULL)
    {
        *save = NULL;
        *name = path;
        *dir = path + strlen (path);    /* an empty string */
    }
    else
    {
        *save = s;
        *dir = path;
        *s++ = '\0';
        *name = s;
    }
}

/* --------------------------------------------------------------------------------------------- */

static struct vfs_s_entry *
vfs_s_find_entry_linear (struct vfs_class *me, struct vfs_s_inode *root,
                         const char *a_path, int follow, int flags)
{
    struct vfs_s_entry *ent = NULL;
    char *const path = g_strdup (a_path);
    struct vfs_s_entry *retval = NULL;
    GList *iter;

    if (root->super->root != root)
        vfs_die ("We have to use _real_ root. Always. Sorry.");

    /* canonicalize as well, but don't remove '../' from path */
    custom_canonicalize_pathname (path, CANON_PATH_ALL & (~CANON_PATH_REMDOUBLEDOTS));

    if ((flags & FL_DIR) == 0)
    {
        char *dirname, *name, *save;
        struct vfs_s_inode *ino;
        split_dir_name (me, path, &dirname, &name, &save);
        ino = vfs_s_find_inode (me, root->super, dirname, follow, flags | FL_DIR);
        if (save != NULL)
            *save = PATH_SEP;
        retval = vfs_s_find_entry_tree (me, ino, name, follow, flags);
        g_free (path);
        return retval;
    }

    iter = g_list_find_custom (root->subdir, path, (GCompareFunc) vfs_s_entry_compare);
    ent = iter != NULL ? (struct vfs_s_entry *) iter->data : NULL;

    if (ent != NULL && !MEDATA->dir_uptodate (me, ent->ino))
    {
#if 1
        vfs_print_message (_("Directory cache expired for %s"), path);
#endif
        vfs_s_free_entry (me, ent);
        ent = NULL;
    }

    if (ent == NULL)
    {
        struct vfs_s_inode *ino;

        ino = vfs_s_new_inode (me, root->super, vfs_s_default_stat (me, S_IFDIR | 0755));
        ent = vfs_s_new_entry (me, path, ino);
        if (MEDATA->dir_load (me, ino, path) == -1)
        {
            vfs_s_free_entry (me, ent);
            g_free (path);
            return NULL;
        }

        vfs_s_insert_entry (me, root, ent);

        iter = g_list_find_custom (root->subdir, path, (GCompareFunc) vfs_s_entry_compare);
        ent = iter != NULL ? (struct vfs_s_entry *) iter->data : NULL;
    }
    if (ent == NULL)
        vfs_die ("find_linear: success but directory is not there\n");

#if 0
    if (!vfs_s_resolve_symlink (me, ent, follow))
    {
        g_free (path);
        return NULL;
    }
#endif
    g_free (path);
    return ent;
}

/* --------------------------------------------------------------------------------------------- */
/* Ook, these were functions around directory entries / inodes */
/* -------------------------------- superblock games -------------------------- */

static struct vfs_s_super *
vfs_s_new_super (struct vfs_class *me)
{
    struct vfs_s_super *super;

    super = g_new0 (struct vfs_s_super, 1);
    super->me = me;
    return super;
}

/* --------------------------------------------------------------------------------------------- */

static inline void
vfs_s_insert_super (struct vfs_class *me, struct vfs_s_super *super)
{
    MEDATA->supers = g_list_prepend (MEDATA->supers, super);
}

/* --------------------------------------------------------------------------------------------- */

static void
vfs_s_free_super (struct vfs_class *me, struct vfs_s_super *super)
{
    if (super->root != NULL)
    {
        vfs_s_free_inode (me, super->root);
        super->root = NULL;
    }

#if 0
    /* FIXME: We currently leak small ammount of memory, sometimes. Fix it if you can. */
    if (super->ino_usage)
        message (D_ERROR, "Direntry warning",
                 "Super ino_usage is %d, memory leak", super->ino_usage);

    if (super->want_stale)
        message (D_ERROR, "Direntry warning", "%s", "Super has want_stale set");
#endif

    MEDATA->supers = g_list_remove (MEDATA->supers, super);

    CALL (free_archive) (me, super);
    g_free (super->name);
    g_free (super);
}

/* --------------------------------------------------------------------------------------------- */
/**
 * Dissect the path and create corresponding superblock.
 * The result should be freed.
 */

static char *
vfs_s_get_path (struct vfs_class *me, const char *inname, struct vfs_s_super **archive, int flags)
{
    char *buf, *retval;

    buf = g_strdup (inname);
    retval = g_strdup (vfs_s_get_path_mangle (me, buf, archive, flags));
    g_free (buf);
    return retval;
}

/* --------------------------------------------------------------------------------------------- */
/* Support of archives */
/* ------------------------ readdir & friends ----------------------------- */

static struct vfs_s_inode *
vfs_s_inode_from_path (struct vfs_class *me, const char *name, int flags)
{
    struct vfs_s_super *super;
    struct vfs_s_inode *ino;
    char *q;

    if (!(q = vfs_s_get_path (me, name, &super, 0)))
        return NULL;

    ino =
        vfs_s_find_inode (me, super, q,
                          flags & FL_FOLLOW ? LINK_FOLLOW : LINK_NO_FOLLOW, flags & ~FL_FOLLOW);
    if ((!ino) && (!*q))
        /* We are asking about / directory of ftp server: assume it exists */
        ino =
            vfs_s_find_inode (me, super, q,
                              flags & FL_FOLLOW ? LINK_FOLLOW :
                              LINK_NO_FOLLOW, FL_DIR | (flags & ~FL_FOLLOW));
    g_free (q);
    return ino;
}

/* --------------------------------------------------------------------------------------------- */

static void *
vfs_s_opendir (struct vfs_class *me, const char *dirname)
{
    struct vfs_s_inode *dir;
    struct dirhandle *info;

    dir = vfs_s_inode_from_path (me, dirname, FL_DIR | FL_FOLLOW);
    if (dir == NULL)
        return NULL;
    if (!S_ISDIR (dir->st.st_mode))
        ERRNOR (ENOTDIR, NULL);

    dir->st.st_nlink++;
#if 0
    if (dir->subdir == NULL)    /* This can actually happen if we allow empty directories */
        ERRNOR (EAGAIN, NULL);
#endif
    info = g_new (struct dirhandle, 1);
    info->cur = dir->subdir;
    info->dir = dir;

    return info;
}

/* --------------------------------------------------------------------------------------------- */

static void *
vfs_s_readdir (void *data)
{
    static union vfs_dirent dir;
    struct dirhandle *info = (struct dirhandle *) data;
    const char *name;

    if (info->cur == NULL || info->cur->data == NULL)
        return NULL;

    name = ((struct vfs_s_entry *) info->cur->data)->name;
    if (name != NULL)
        g_strlcpy (dir.dent.d_name, name, MC_MAXPATHLEN);
    else
        vfs_die ("Null in structure-cannot happen");

    compute_namelen (&dir.dent);
    info->cur = g_list_next (info->cur);

    return (void *) &dir;
}

/* --------------------------------------------------------------------------------------------- */

static int
vfs_s_closedir (void *data)
{
    struct dirhandle *info = (struct dirhandle *) data;
    struct vfs_s_inode *dir = info->dir;

    vfs_s_free_inode (dir->super->me, dir);
    g_free (data);
    return 0;
}

/* --------------------------------------------------------------------------------------------- */

static int
vfs_s_chdir (struct vfs_class *me, const char *path)
{
    void *data;

    data = vfs_s_opendir (me, path);
    if (data == NULL)
        return -1;
    vfs_s_closedir (data);
    return 0;
}

/* --------------------------------------------------------------------------------------------- */
/* --------------------------- stat and friends ---------------------------- */

static int
vfs_s_internal_stat (struct vfs_class *me, const char *path, struct stat *buf, int flag)
{
    struct vfs_s_inode *ino;

    ino = vfs_s_inode_from_path (me, path, flag);
    if (ino == NULL)
        return -1;
    *buf = ino->st;
    return 0;
}

/* --------------------------------------------------------------------------------------------- */

static int
vfs_s_stat (struct vfs_class *me, const char *path, struct stat *buf)
{
    return vfs_s_internal_stat (me, path, buf, FL_FOLLOW);
}

/* --------------------------------------------------------------------------------------------- */

static int
vfs_s_lstat (struct vfs_class *me, const char *path, struct stat *buf)
{
    return vfs_s_internal_stat (me, path, buf, FL_NONE);
}

/* --------------------------------------------------------------------------------------------- */

static int
vfs_s_fstat (void *fh, struct stat *buf)
{
    *buf = FH->ino->st;
    return 0;
}

/* --------------------------------------------------------------------------------------------- */

static int
vfs_s_readlink (struct vfs_class *me, const char *path, char *buf, size_t size)
{
    struct vfs_s_inode *ino;
    size_t len;

    ino = vfs_s_inode_from_path (me, path, 0);
    if (!ino)
        return -1;

    if (!S_ISLNK (ino->st.st_mode))
        ERRNOR (EINVAL, -1);

    if (ino->linkname == NULL)
        ERRNOR (EFAULT, -1);

    len = strlen (ino->linkname);
    if (size < len)
        len = size;
    /* readlink() does not append a NUL character to buf */
    memcpy (buf, ino->linkname, len);
    return len;
}

/* --------------------------------------------------------------------------------------------- */

static ssize_t
vfs_s_read (void *fh, char *buffer, size_t count)
{
    int n;
    struct vfs_class *me = FH_SUPER->me;

    if (FH->linear == LS_LINEAR_PREOPEN)
    {
        if (!MEDATA->linear_start (me, FH, FH->pos))
            return -1;
    }

    if (FH->linear == LS_LINEAR_CLOSED)
        vfs_die ("linear_start() did not set linear_state!");

    if (FH->linear == LS_LINEAR_OPEN)
        return MEDATA->linear_read (me, FH, buffer, count);

    if (FH->handle != -1)
    {
        n = read (FH->handle, buffer, count);
        if (n < 0)
            me->verrno = errno;
        return n;
    }
    vfs_die ("vfs_s_read: This should not happen\n");
    return -1;
}

/* --------------------------------------------------------------------------------------------- */

static ssize_t
vfs_s_write (void *fh, const char *buffer, size_t count)
{
    int n;
    struct vfs_class *me = FH_SUPER->me;

    if (FH->linear)
        vfs_die ("no writing to linear files, please");

    FH->changed = 1;
    if (FH->handle != -1)
    {
        n = write (FH->handle, buffer, count);
        if (n < 0)
            me->verrno = errno;
        return n;
    }
    vfs_die ("vfs_s_write: This should not happen\n");
    return 0;
}

/* --------------------------------------------------------------------------------------------- */

static off_t
vfs_s_lseek (void *fh, off_t offset, int whence)
{
    off_t size = FH->ino->st.st_size;

    if (FH->linear == LS_LINEAR_OPEN)
        vfs_die ("cannot lseek() after linear_read!");

    if (FH->handle != -1)
    {                           /* If we have local file opened, we want to work with it */
        off_t retval = lseek (FH->handle, offset, whence);
        if (retval == -1)
            FH->ino->super->me->verrno = errno;
        return retval;
    }

    switch (whence)
    {
    case SEEK_CUR:
        offset += FH->pos;
        break;
    case SEEK_END:
        offset += size;
        break;
    }
    if (offset < 0)
        FH->pos = 0;
    else if (offset < size)
        FH->pos = offset;
    else
        FH->pos = size;
    return FH->pos;
}

/* --------------------------------------------------------------------------------------------- */

static int
vfs_s_close (void *fh)
{
    int res = 0;
    struct vfs_class *me = FH_SUPER->me;

    FH_SUPER->fd_usage--;
    if (!FH_SUPER->fd_usage)
        vfs_stamp_create (me, FH_SUPER);

    if (FH->linear == LS_LINEAR_OPEN)
        MEDATA->linear_close (me, fh);
    if (MEDATA->fh_close)
        res = MEDATA->fh_close (me, fh);
    if (FH->changed && MEDATA->file_store)
    {
        char *s = vfs_s_fullpath (me, FH->ino);
        if (!s)
            res = -1;
        else
        {
            res = MEDATA->file_store (me, fh, s, FH->ino->localname);
            g_free (s);
        }
        vfs_s_invalidate (me, FH_SUPER);
    }
    if (FH->handle != -1)
        close (FH->handle);

    vfs_s_free_inode (me, FH->ino);
    g_free (fh);
    return res;
}

/* --------------------------------------------------------------------------------------------- */

static void
vfs_s_print_stats (const char *fs_name, const char *action,
                   const char *file_name, off_t have, off_t need)
{
    static const char *i18n_percent_transf_format = NULL;
    static const char *i18n_transf_format = NULL;

    if (i18n_percent_transf_format == NULL)
    {
        i18n_percent_transf_format = "%s: %s: %s %3d%% (%" PRIuMAX " %s";
        i18n_transf_format = "%s: %s: %s %" PRIuMAX " %s";
    }

    if (need)
        vfs_print_message (i18n_percent_transf_format, fs_name, action,
                           file_name, (int) ((double) have * 100 / need), (uintmax_t) have,
                           _("bytes transferred"));
    else
        vfs_print_message (i18n_transf_format, fs_name, action, file_name, (uintmax_t) have,
                           _("bytes transferred"));
}

/* --------------------------------------------------------------------------------------------- */
/* ------------------------------- mc support ---------------------------- */

static void
vfs_s_fill_names (struct vfs_class *me, fill_names_f func)
{
    GList *iter;

    for (iter = MEDATA->supers; iter != NULL; iter = g_list_next (iter))
    {
        const struct vfs_s_super *super = (const struct vfs_s_super *) iter->data;
        char *name;

        name = g_strconcat (super->name, "#", me->prefix, "/",
                            /* super->current_dir->name, */ (char *) NULL);
        func (name);
        g_free (name);
    }
}

/* --------------------------------------------------------------------------------------------- */

static int
vfs_s_ferrno (struct vfs_class *me)
{
    return me->verrno;
}

/* --------------------------------------------------------------------------------------------- */
/**
 * Get local copy of the given file.  We reuse the existing file cache
 * for remote filesystems.  Archives use standard VFS facilities.
 */

static char *
vfs_s_getlocalcopy (struct vfs_class *me, const char *path)
{
    vfs_file_handler_t *fh;
    char *local = NULL;

    fh = vfs_s_open (me, path, O_RDONLY, 0);

    if (fh != NULL)
    {
        if ((fh->ino != NULL) && (fh->ino->localname != NULL))
            local = g_strdup (fh->ino->localname);

        vfs_s_close (fh);
    }

    return local;
}

/* --------------------------------------------------------------------------------------------- */
/**
 * Return the local copy.  Since we are using our cache, we do nothing -
 * the cache will be removed when the archive is closed.
 */

static int
vfs_s_ungetlocalcopy (struct vfs_class *me, const char *path, const char *local, int has_changed)
{
    (void) me;
    (void) path;
    (void) local;
    (void) has_changed;
    return 0;
}

/* --------------------------------------------------------------------------------------------- */

static int
vfs_s_setctl (struct vfs_class *me, const char *path, int ctlop, void *arg)
{
    switch (ctlop)
    {
    case VFS_SETCTL_STALE_DATA:
        {
            struct vfs_s_inode *ino = vfs_s_inode_from_path (me, path, 0);

            if (ino == NULL)
                return 0;
            if (arg)
                ino->super->want_stale = 1;
            else
            {
                ino->super->want_stale = 0;
                vfs_s_invalidate (me, ino->super);
            }
            return 1;
        }
    case VFS_SETCTL_LOGFILE:
        MEDATA->logfile = fopen ((char *) arg, "w");
        return 1;
    case VFS_SETCTL_FLUSH:
        MEDATA->flush = 1;
        return 1;
    }
    return 0;
}

/* --------------------------------------------------------------------------------------------- */
/* ----------------------------- Stamping support -------------------------- */

static vfsid
vfs_s_getid (struct vfs_class *me, const char *path)
{
    struct vfs_s_super *archive = NULL;
    char *p;

    p = vfs_s_get_path (me, path, &archive, FL_NO_OPEN);
    if (p == NULL)
        return NULL;
    g_free (p);
    return (vfsid) archive;
}

/* --------------------------------------------------------------------------------------------- */

static int
vfs_s_nothingisopen (vfsid id)
{
    (void) id;
    /* Our data structures should survive free of superblock at any time */
    return 1;
}

/* --------------------------------------------------------------------------------------------- */

static void
vfs_s_free (vfsid id)
{
    vfs_s_free_super (((struct vfs_s_super *) id)->me, (struct vfs_s_super *) id);
}

/* --------------------------------------------------------------------------------------------- */

static int
vfs_s_dir_uptodate (struct vfs_class *me, struct vfs_s_inode *ino)
{
    struct timeval tim;

    if (MEDATA->flush)
    {
        MEDATA->flush = 0;
        return 0;
    }

    gettimeofday (&tim, NULL);
    if (tim.tv_sec < ino->timestamp.tv_sec)
        return 1;
    return 0;
}


/* --------------------------------------------------------------------------------------------- */
/*** public functions ****************************************************************************/
/* --------------------------------------------------------------------------------------------- */

struct vfs_s_inode *
vfs_s_new_inode (struct vfs_class *me, struct vfs_s_super *super, struct stat *initstat)
{
    struct vfs_s_inode *ino;

    ino = g_try_new0 (struct vfs_s_inode, 1);
    if (ino == NULL)
        return NULL;

    if (initstat)
        ino->st = *initstat;
    ino->super = super;
    ino->st.st_nlink = 0;
    ino->st.st_ino = MEDATA->inode_counter++;
    ino->st.st_dev = MEDATA->rdev;

    super->ino_usage++;
    total_inodes++;

    CALL (init_inode) (me, ino);

    return ino;
}

/* --------------------------------------------------------------------------------------------- */

struct vfs_s_entry *
vfs_s_new_entry (struct vfs_class *me, const char *name, struct vfs_s_inode *inode)
{
    struct vfs_s_entry *entry;

    entry = g_new0 (struct vfs_s_entry, 1);
    total_entries++;

    entry->name = g_strdup (name);
    entry->ino = inode;
    entry->ino->ent = entry;
    CALL (init_entry) (me, entry);

    return entry;
}


/* --------------------------------------------------------------------------------------------- */

void
vfs_s_free_entry (struct vfs_class *me, struct vfs_s_entry *ent)
{
    if (ent->dir != NULL)
        ent->dir->subdir = g_list_remove (ent->dir->subdir, ent);

    g_free (ent->name);
    /* ent->name = NULL; */

    if (ent->ino != NULL)
    {
        ent->ino->ent = NULL;
        vfs_s_free_inode (me, ent->ino);
    }

    total_entries--;
    g_free (ent);
}

/* --------------------------------------------------------------------------------------------- */

void
vfs_s_insert_entry (struct vfs_class *me, struct vfs_s_inode *dir, struct vfs_s_entry *ent)
{
    (void) me;

    ent->dir = dir;

    ent->ino->st.st_nlink++;
    dir->subdir = g_list_append (dir->subdir, ent);
}

/* --------------------------------------------------------------------------------------------- */

struct stat *
vfs_s_default_stat (struct vfs_class *me, mode_t mode)
{
    static struct stat st;
    int myumask;

    (void) me;

    myumask = umask (022);
    umask (myumask);
    mode &= ~myumask;

    st.st_mode = mode;
    st.st_ino = 0;
    st.st_dev = 0;
    st.st_rdev = 0;
    st.st_uid = getuid ();
    st.st_gid = getgid ();
    st.st_size = 0;
    st.st_mtime = st.st_atime = st.st_ctime = time (NULL);

    return &st;
}

/* --------------------------------------------------------------------------------------------- */

struct vfs_s_entry *
vfs_s_generate_entry (struct vfs_class *me, const char *name, struct vfs_s_inode *parent,
                      mode_t mode)
{
    struct vfs_s_inode *inode;
    struct stat *st;

    st = vfs_s_default_stat (me, mode);
    inode = vfs_s_new_inode (me, parent->super, st);

    return vfs_s_new_entry (me, name, inode);
}

/* --------------------------------------------------------------------------------------------- */

struct vfs_s_inode *
vfs_s_find_inode (struct vfs_class *me, const struct vfs_s_super *super,
                  const char *path, int follow, int flags)
{
    struct vfs_s_entry *ent;

    if (((MEDATA->flags & VFS_S_REMOTE) == 0) && (*path == '\0'))
        return super->root;

    ent = (MEDATA->find_entry) (me, super->root, path, follow, flags);
    return (ent != NULL) ? ent->ino : NULL;
}

/* --------------------------------------------------------------------------------------------- */
/* Ook, these were functions around directory entries / inodes */
/* -------------------------------- superblock games -------------------------- */

/*
 * Dissect the path and create corresponding superblock.  Note that inname
 * can be changed and the result may point inside the original string.
 */
const char *
vfs_s_get_path_mangle (struct vfs_class *me, char *inname, struct vfs_s_super **archive, int flags)
{
    GList *iter;
    const char *retval;
    char *local, *op;
    const char *archive_name;
    int result = -1;
    struct vfs_s_super *super;
    void *cookie = NULL;

    archive_name = inname;
    vfs_split (inname, &local, &op);
    retval = (local != NULL) ? local : "";

    if (MEDATA->archive_check != NULL)
    {
        cookie = MEDATA->archive_check (me, archive_name, op);
        if (cookie == NULL)
            return NULL;
    }

    for (iter = MEDATA->supers; iter != NULL; iter = g_list_next (iter))
    {
        int i;

        super = (struct vfs_s_super *) iter->data;

        /* 0 == other, 1 == same, return it, 2 == other but stop scanning */
        i = MEDATA->archive_same (me, super, archive_name, op, cookie);
        if (i != 0)
        {
            if (i == 1)
                goto return_success;
            break;
        }
    }

    if (flags & FL_NO_OPEN)
        ERRNOR (EIO, NULL);

    super = vfs_s_new_super (me);
    result = MEDATA->open_archive (me, super, archive_name, op);
    if (result == -1)
    {
        vfs_s_free_super (me, super);
        ERRNOR (EIO, NULL);
    }
    if (!super->name)
        vfs_die ("You have to fill name\n");
    if (!super->root)
        vfs_die ("You have to fill root inode\n");

    vfs_s_insert_super (me, super);
    vfs_stamp_create (me, super);

  return_success:
    *archive = super;
    return retval;
}

/* --------------------------------------------------------------------------------------------- */

void
vfs_s_invalidate (struct vfs_class *me, struct vfs_s_super *super)
{
    if (!super->want_stale)
    {
        vfs_s_free_inode (me, super->root);
        super->root = vfs_s_new_inode (me, super, vfs_s_default_stat (me, S_IFDIR | 0755));
    }
}

/* --------------------------------------------------------------------------------------------- */

char *
vfs_s_fullpath (struct vfs_class *me, struct vfs_s_inode *ino)
{
    if (!ino->ent)
        ERRNOR (EAGAIN, NULL);

    if (!(MEDATA->flags & VFS_S_REMOTE))
    {
        /* archives */
        char *newpath;
        char *path = g_strdup (ino->ent->name);
        while (1)
        {
            ino = ino->ent->dir;
            if (ino == ino->super->root)
                break;
            newpath = g_strconcat (ino->ent->name, "/", path, (char *) NULL);
            g_free (path);
            path = newpath;
        }
        return path;
    }

    /* remote systems */
    if ((!ino->ent->dir) || (!ino->ent->dir->ent))
        return g_strdup (ino->ent->name);

    return g_strconcat (ino->ent->dir->ent->name, PATH_SEP_STR, ino->ent->name, (char *) NULL);
}

/* --------------------------------------------------------------------------------------------- */
/* --------------------------- stat and friends ---------------------------- */

void *
vfs_s_open (struct vfs_class *me, const char *file, int flags, mode_t mode)
{
    int was_changed = 0;
    vfs_file_handler_t *fh;
    struct vfs_s_super *super;
    char *q;
    struct vfs_s_inode *ino;

    q = vfs_s_get_path (me, file, &super, 0);
    if (q == NULL)
        return NULL;
    ino = vfs_s_find_inode (me, super, q, LINK_FOLLOW, FL_NONE);
    if (ino && ((flags & (O_CREAT | O_EXCL)) == (O_CREAT | O_EXCL)))
    {
        g_free (q);
        ERRNOR (EEXIST, NULL);
    }
    if (!ino)
    {
        char *dirname, *name, *save;
        struct vfs_s_entry *ent;
        struct vfs_s_inode *dir;
        int tmp_handle;

        /* If the filesystem is read-only, disable file creation */
        if (!(flags & O_CREAT) || !(me->write))
        {
            g_free (q);
            return NULL;
        }

        split_dir_name (me, q, &dirname, &name, &save);
        dir = vfs_s_find_inode (me, super, dirname, LINK_FOLLOW, FL_DIR);
        if (dir == NULL)
        {
            g_free (q);
            return NULL;
        }
        if (save)
            *save = PATH_SEP;
        ent = vfs_s_generate_entry (me, name, dir, 0755);
        ino = ent->ino;
        vfs_s_insert_entry (me, dir, ent);
        tmp_handle = vfs_mkstemps (&ino->localname, me->name, name);
        if (tmp_handle == -1)
        {
            g_free (q);
            return NULL;
        }
        close (tmp_handle);
        was_changed = 1;
    }

    g_free (q);

    if (S_ISDIR (ino->st.st_mode))
        ERRNOR (EISDIR, NULL);

    fh = g_new (vfs_file_handler_t, 1);
    fh->pos = 0;
    fh->ino = ino;
    fh->handle = -1;
    fh->changed = was_changed;
    fh->linear = 0;

    if (IS_LINEAR (flags))
    {
        if (MEDATA->linear_start)
        {
            vfs_print_message (_("Starting linear transfer..."));
            fh->linear = LS_LINEAR_PREOPEN;
        }
    }
    else if ((MEDATA->fh_open != NULL) && (MEDATA->fh_open (me, fh, flags, mode) != 0))
    {
        g_free (fh);
        return NULL;
    }

    if (fh->ino->localname)
    {
        fh->handle = open (fh->ino->localname, NO_LINEAR (flags), mode);
        if (fh->handle == -1)
        {
            g_free (fh);
            ERRNOR (errno, NULL);
        }
    }

    /* i.e. we had no open files and now we have one */
    vfs_rmstamp (me, (vfsid) super);
    super->fd_usage++;
    fh->ino->st.st_nlink++;
    return fh;
}

/* --------------------------------------------------------------------------------------------- */

int
vfs_s_retrieve_file (struct vfs_class *me, struct vfs_s_inode *ino)
{
    /* If you want reget, you'll have to open file with O_LINEAR */
    off_t total = 0;
    char buffer[8192];
    int handle, n;
    off_t stat_size = ino->st.st_size;
    vfs_file_handler_t fh;

    memset (&fh, 0, sizeof (fh));

    fh.ino = ino;
    fh.handle = -1;

    handle = vfs_mkstemps (&ino->localname, me->name, ino->ent->name);
    if (handle == -1)
    {
        me->verrno = errno;
        goto error_4;
    }

    if (!MEDATA->linear_start (me, &fh, 0))
        goto error_3;

    /* Clear the interrupt status */
    tty_got_interrupt ();
    tty_enable_interrupt_key ();

    while ((n = MEDATA->linear_read (me, &fh, buffer, sizeof (buffer))))
    {
        int t;
        if (n < 0)
            goto error_1;

        total += n;
        vfs_s_print_stats (me->name, _("Getting file"), ino->ent->name, total, stat_size);

        if (tty_got_interrupt ())
            goto error_1;

        t = write (handle, buffer, n);
        if (t != n)
        {
            if (t == -1)
                me->verrno = errno;
            goto error_1;
        }
    }
    MEDATA->linear_close (me, &fh);
    close (handle);

    tty_disable_interrupt_key ();
    g_free (fh.data);
    return 0;

  error_1:
    MEDATA->linear_close (me, &fh);
  error_3:
    tty_disable_interrupt_key ();
    close (handle);
    unlink (ino->localname);
  error_4:
    g_free (ino->localname);
    ino->localname = NULL;
    g_free (fh.data);
    return -1;
}

/* --------------------------------------------------------------------------------------------- */
/* ----------------------------- Stamping support -------------------------- */

/* Initialize one of our subclasses - fill common functions */
void
vfs_s_init_class (struct vfs_class *vclass, struct vfs_s_subclass *sub)
{
    vclass->data = sub;
    vclass->fill_names = vfs_s_fill_names;
    vclass->open = vfs_s_open;
    vclass->close = vfs_s_close;
    vclass->read = vfs_s_read;
    if (!(sub->flags & VFS_S_READONLY))
    {
        vclass->write = vfs_s_write;
    }
    vclass->opendir = vfs_s_opendir;
    vclass->readdir = vfs_s_readdir;
    vclass->closedir = vfs_s_closedir;
    vclass->stat = vfs_s_stat;
    vclass->lstat = vfs_s_lstat;
    vclass->fstat = vfs_s_fstat;
    vclass->readlink = vfs_s_readlink;
    vclass->chdir = vfs_s_chdir;
    vclass->ferrno = vfs_s_ferrno;
    vclass->lseek = vfs_s_lseek;
    vclass->getid = vfs_s_getid;
    vclass->nothingisopen = vfs_s_nothingisopen;
    vclass->free = vfs_s_free;
    if (sub->flags & VFS_S_REMOTE)
    {
        vclass->getlocalcopy = vfs_s_getlocalcopy;
        vclass->ungetlocalcopy = vfs_s_ungetlocalcopy;
        sub->find_entry = vfs_s_find_entry_linear;
    }
    else
    {
        sub->find_entry = vfs_s_find_entry_tree;
    }
    vclass->setctl = vfs_s_setctl;
    sub->dir_uptodate = vfs_s_dir_uptodate;
}

/* --------------------------------------------------------------------------------------------- */
/** Find VFS id for given directory name */

vfsid
vfs_getid (struct vfs_class *vclass, const char *dir)
{
    char *dir1;
    vfsid id = NULL;

    /* append slash if needed */
    dir1 = concat_dir_and_file (dir, "");
    if (vclass->getid)
        id = (*vclass->getid) (vclass, dir1);

    g_free (dir1);
    return id;
}

/* --------------------------------------------------------------------------------------------- */
/* ----------- Utility functions for networked filesystems  -------------- */

#ifdef ENABLE_VFS_NET
int
vfs_s_select_on_two (int fd1, int fd2)
{
    fd_set set;
    struct timeval time_out;
    int v;
    int maxfd = (fd1 > fd2 ? fd1 : fd2) + 1;

    time_out.tv_sec = 1;
    time_out.tv_usec = 0;
    FD_ZERO (&set);
    FD_SET (fd1, &set);
    FD_SET (fd2, &set);
    v = select (maxfd, &set, 0, 0, &time_out);
    if (v <= 0)
        return v;
    if (FD_ISSET (fd1, &set))
        return 1;
    if (FD_ISSET (fd2, &set))
        return 2;
    return -1;
}

/* --------------------------------------------------------------------------------------------- */

int
vfs_s_get_line (struct vfs_class *me, int sock, char *buf, int buf_len, char term)
{
    FILE *logfile = MEDATA->logfile;
    int i;
    char c;

    for (i = 0; i < buf_len - 1; i++, buf++)
    {
        if (read (sock, buf, sizeof (char)) <= 0)
            return 0;
        if (logfile)
        {
            size_t ret1;
            int ret2;
            ret1 = fwrite (buf, 1, 1, logfile);
            ret2 = fflush (logfile);
        }
        if (*buf == term)
        {
            *buf = 0;
            return 1;
        }
    }

    /* Line is too long - terminate buffer and discard the rest of line */
    *buf = 0;
    while (read (sock, &c, sizeof (c)) > 0)
    {
        if (logfile)
        {
            size_t ret1;
            int ret2;
            ret1 = fwrite (&c, 1, 1, logfile);
            ret2 = fflush (logfile);
        }
        if (c == '\n')
            return 1;
    }
    return 0;
}

/* --------------------------------------------------------------------------------------------- */

int
vfs_s_get_line_interruptible (struct vfs_class *me, char *buffer, int size, int fd)
{
    int n;
    int i;

    (void) me;

    tty_enable_interrupt_key ();
    for (i = 0; i < size - 1; i++)
    {
        n = read (fd, buffer + i, 1);
        tty_disable_interrupt_key ();
        if (n == -1 && errno == EINTR)
        {
            buffer[i] = 0;
            return EINTR;
        }
        if (n == 0)
        {
            buffer[i] = 0;
            return 0;
        }
        if (buffer[i] == '\n')
        {
            buffer[i] = 0;
            return 1;
        }
    }
    buffer[size - 1] = 0;
    return 0;
}
#endif /* ENABLE_VFS_NET */

/* --------------------------------------------------------------------------------------------- */