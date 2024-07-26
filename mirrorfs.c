#define FUSE_USE_VERSION 31

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#define _GNU_SOURCE

#ifdef linux
/* For pread()/pwrite()/utimensat() */
#define _XOPEN_SOURCE 700
#endif

#include <assert.h>
#include <fuse.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>
#include <sys/time.h>

#define QUOTE(str) #str
#define EXPAND_AND_QUOTE(str) QUOTE(str)

#define ABORT_IF_NOT_EQUAL(x, y) \
    do { \
        long _x = (x); \
        long _y = (y); \
        if (_x != _y) { \
            fprintf(stderr, "%s: %s %ld != %ld\n", __func__, EXPAND_AND_QUOTE(x), _x, _y); \
            if (abort_on_difference) { \
                abort(); \
            } \
        } \
    } while (0)

#define ABORT_IF_INCONSISTENT_FD(fd1, fd2) \
    do { \
        if (((fd1) == -1) ^ ((fd2) == -1)) { \
            fprintf(stderr, "%s: %d != %d\n", __func__, (fd1), (fd2)); \
            abort(); \
        } \
    } while (0)

#define LOG_FUSE_OPERATION(fmt, ...) \
    do { \
        if (log_operations) { \
            fprintf(stderr, "%s: " fmt "\n", __func__, ##__VA_ARGS__); \
        } \
    } while (0)

// TODO: add flags to configure these
static int abort_on_difference = 1;
static int log_operations = 1;

#define MAX_MNTPATHS 10  // Maximum number of mount paths

static const char *mntpaths[MAX_MNTPATHS] = {NULL};
static int mntfds[MAX_MNTPATHS] = {-1};
static int mntpath_count = 0;
static int mirror_fds[1024];  // TODO: hardcoded limit

// FUSE delivers paths with a leading slash.  Remove them when possible and
// return dot otherwise.
static const char *safe_path(const char *path)
{
    if (strcmp(path, "/") == 0) {
        return ".";
    }
    return path + 1;
}

static void *mirrorfs_init(struct fuse_conn_info *conn,
                           struct fuse_config *cfg)
{
    cfg->use_ino = 1;

    /* Pick up changes from lower filesystem right away. This is
       also necessary for better hardlink support. When the kernel
       calls the unlink() handler, it does not know the inode of
       the to-be-removed entry and can therefore not invalidate
       the cache of the associated inode - resulting in an
       incorrect st_nlink value being reported for any remaining
       hardlinks to this inode. */
    cfg->entry_timeout = 0;
    cfg->attr_timeout = 0;
    cfg->negative_timeout = 0;

    return NULL;
}

static int mirrorfs_getattr(const char *path, struct stat *stbuf,
                            struct fuse_file_info *fi)
{
    LOG_FUSE_OPERATION("%s", path);

    int res[MAX_MNTPATHS];
    int errnos[MAX_MNTPATHS];
    struct stat stbufs[MAX_MNTPATHS];

    for (int i = 0; i < mntpath_count; i++) {
        memset(&stbufs[i], 0, sizeof(struct stat));
        errno = 0;
        res[i] = fstatat(mntfds[i], safe_path(path), &stbufs[i], AT_SYMLINK_NOFOLLOW);
        errnos[i] = errno;
    }

    // Compare results
    for (int i = 1; i < mntpath_count; i++) {
        ABORT_IF_NOT_EQUAL(res[0], res[i]);
        ABORT_IF_NOT_EQUAL(errnos[0], errnos[i]);
    }

    if (res[0] == -1) {
        return -errnos[0];
    }

    // Compare stat structs
    for (int i = 1; i < mntpath_count; i++) {
        if (memcmp(&stbufs[0], &stbufs[i], sizeof(struct stat)) != 0) {
            ABORT_IF_NOT_EQUAL(stbufs[0].st_mode, stbufs[i].st_mode);
            ABORT_IF_NOT_EQUAL(stbufs[0].st_nlink, stbufs[i].st_nlink);
            ABORT_IF_NOT_EQUAL(stbufs[0].st_uid, stbufs[i].st_uid);
            ABORT_IF_NOT_EQUAL(stbufs[0].st_gid, stbufs[i].st_gid);
            if(!S_ISDIR(stbufs[0].st_mode)){
                ABORT_IF_NOT_EQUAL(stbufs[0].st_size, stbufs[i].st_size);
            }
            // TODO: compare other fields?
            // TODO: compare st_ino?
            // TODO: compare st_dev?
            // TODO: compare st_rdev?
            // TODO: compare st_blksize?
            // TODO: compare st_blocks?
            // TODO: compare st_atime?
            // TODO: compare st_mtime?
            // TODO: compare st_ctime?
        }
    }

    // Copy the result to the output buffer
    memcpy(stbuf, &stbufs[0], sizeof(struct stat));

    return 0;
}

static int mirrorfs_access(const char *path, int mask)
{
    LOG_FUSE_OPERATION("%s 0x%x", path, mask);

    int res[MAX_MNTPATHS];
    int errnos[MAX_MNTPATHS];

    for (int i = 0; i < mntpath_count; i++) {
        errno = 0;
        res[i] = faccessat(mntfds[i], safe_path(path), mask, 0);
        errnos[i] = errno;
    }

    // Compare results
    for (int i = 1; i < mntpath_count; i++) {
        ABORT_IF_NOT_EQUAL(res[0], res[i]);
        ABORT_IF_NOT_EQUAL(errnos[0], errnos[i]);
    }

    if (res[0] == -1) {
        return -errnos[0];
    }

    return 0;
}

static int mirrorfs_readlink(const char *path, char *buf, size_t size)
{
    LOG_FUSE_OPERATION("%s %zu", path, size);

    int res[MAX_MNTPATHS];
    int errnos[MAX_MNTPATHS];
    char *bufs[MAX_MNTPATHS];

    for (int i = 0; i < mntpath_count; i++) {
        bufs[i] = malloc(size);
        errno = 0;
        res[i] = readlinkat(mntfds[i], safe_path(path), bufs[i], size - 1);
        errnos[i] = errno;
    }

    // Compare results
    for (int i = 1; i < mntpath_count; i++) {
        ABORT_IF_NOT_EQUAL(res[0], res[i]);
        ABORT_IF_NOT_EQUAL(errnos[0], errnos[i]);
        if (res[0] != -1 && memcmp(bufs[0], bufs[i], res[0]) != 0) {
            abort();
        }
    }

    if (res[0] == -1) {
        for (int i = 0; i < mntpath_count; i++) {
            free(bufs[i]);
        }
        return -errnos[0];
    }

    memcpy(buf, bufs[0], res[0]);
    buf[res[0]] = '\0';

    for (int i = 0; i < mntpath_count; i++) {
        free(bufs[i]);
    }

    return 0;
}

// TODO: incomplete; compare against dir2fd.  how to handle different directory
// orders?
static int mirrorfs_readdir(const char *path, void *buf,
                            fuse_fill_dir_t filler, off_t offset,
                            struct fuse_file_info *fi,
                            enum fuse_readdir_flags flags)
{
    LOG_FUSE_OPERATION("%s %ld 0x%x", path, offset, flags);

    struct dirent *de;
    DIR *dps[MAX_MNTPATHS];
    int dirfds[MAX_MNTPATHS];

    for (int i = 0; i < mntpath_count; i++) {
        dirfds[i] = openat(mntfds[i], safe_path(path), O_DIRECTORY);
        if (dirfds[i] == -1) {
            for (int j = 0; j < i; j++) {
                close(dirfds[j]);
            }
            return -errno;
        }
        dps[i] = fdopendir(dirfds[i]);
        if (dps[i] == NULL) {
            for (int j = 0; j <= i; j++) {
                close(dirfds[j]);
            }
            return -errno;
        }
    }

    while ((de = readdir(dps[0])) != NULL) {
        struct stat st;
        memset(&st, 0, sizeof(st));
        st.st_ino = de->d_ino;
        st.st_mode = de->d_type << 12;
        if (filler(buf, de->d_name, &st, 0, 0)) {
            break;
        }
        
        // Check if the same entry exists in all directories
        for (int i = 1; i < mntpath_count; i++) {
            struct dirent *de_i = readdir(dps[i]);
            if (de_i == NULL || strcmp(de->d_name, de_i->d_name) != 0) {
                fprintf(stderr, "Inconsistent directory entry: %s\n", de->d_name);
                abort();
            }
        }
    }

    for (int i = 0; i < mntpath_count; i++) {
        closedir(dps[i]);
        close(dirfds[i]);
    }
    return 0;
}

static int mirrorfs_mkdir(const char *path, mode_t mode)
{
    LOG_FUSE_OPERATION("%s 0x%x", path, mode);

    int res[MAX_MNTPATHS];
    int errnos[MAX_MNTPATHS];

    for (int i = 0; i < mntpath_count; i++) {
        errno = 0;
        res[i] = mkdirat(mntfds[i], safe_path(path), mode);
        errnos[i] = errno;
    }

    // Compare results
    for (int i = 1; i < mntpath_count; i++) {
        ABORT_IF_NOT_EQUAL(res[0], res[i]);
        ABORT_IF_NOT_EQUAL(errnos[0], errnos[i]);
    }

    if (res[0] == -1) {
        return -errnos[0];
    }

    return 0;
}

static int mirrorfs_unlink(const char *path)
{
    LOG_FUSE_OPERATION("%s", path);

    int res[MAX_MNTPATHS];
    int errnos[MAX_MNTPATHS];

    for (int i = 0; i < mntpath_count; i++) {
        errno = 0;
        res[i] = unlinkat(mntfds[i], safe_path(path), 0);
        errnos[i] = errno;
    }

    // Compare results
    for (int i = 1; i < mntpath_count; i++) {
        ABORT_IF_NOT_EQUAL(res[0], res[i]);
        ABORT_IF_NOT_EQUAL(errnos[0], errnos[i]);
    }

    if (res[0] == -1) {
        return -errnos[0];
    }

    return 0;
}

static int mirrorfs_rmdir(const char *path)
{
    LOG_FUSE_OPERATION("%s", path);

    int res[MAX_MNTPATHS];
    int errnos[MAX_MNTPATHS];

    for (int i = 0; i < mntpath_count; i++) {
        errno = 0;
        res[i] = unlinkat(mntfds[i], safe_path(path), AT_REMOVEDIR);
        errnos[i] = errno;
    }

    // Compare results
    for (int i = 1; i < mntpath_count; i++) {
        ABORT_IF_NOT_EQUAL(res[0], res[i]);
        ABORT_IF_NOT_EQUAL(errnos[0], errnos[i]);
    }

    if (res[0] == -1) {
        return -errnos[0];
    }

    return 0;
}

static int mirrorfs_symlink(const char *from, const char *to)
{
    LOG_FUSE_OPERATION("%s %s", from, to);

    int res[MAX_MNTPATHS];
    int errnos[MAX_MNTPATHS];

    for (int i = 0; i < mntpath_count; i++) {
        errno = 0;
        res[i] = symlinkat(from, mntfds[i], safe_path(to));
        errnos[i] = errno;
    }

    // Compare results
    for (int i = 1; i < mntpath_count; i++) {
        ABORT_IF_NOT_EQUAL(res[0], res[i]);
        ABORT_IF_NOT_EQUAL(errnos[0], errnos[i]);
    }

    if (res[0] == -1) {
        return -errnos[0];
    }

    return 0;
}

static int mirrorfs_rename(const char *from, const char *to,
                           unsigned int flags)
{
    LOG_FUSE_OPERATION("%s %s 0x%x", from, to, flags);

    if (flags) {
        return -EINVAL;
    }

    int res[MAX_MNTPATHS];
    int errnos[MAX_MNTPATHS];

    for (int i = 0; i < mntpath_count; i++) {
        errno = 0;
        res[i] = renameat(mntfds[i], safe_path(from), mntfds[i], safe_path(to));
        errnos[i] = errno;
    }

    // Compare results
    for (int i = 1; i < mntpath_count; i++) {
        ABORT_IF_NOT_EQUAL(res[0], res[i]);
        ABORT_IF_NOT_EQUAL(errnos[0], errnos[i]);
    }

    if (res[0] == -1) {
        return -errnos[0];
    }

    return 0;
}

static int mirrorfs_link(const char *from, const char *to)
{
    LOG_FUSE_OPERATION("%s %s", from, to);

    int res[MAX_MNTPATHS];
    int errnos[MAX_MNTPATHS];

    for (int i = 0; i < mntpath_count; i++) {
        errno = 0;
        res[i] = linkat(mntfds[i], safe_path(from), mntfds[i], safe_path(to), 0);
        errnos[i] = errno;
    }

    // Compare results
    for (int i = 1; i < mntpath_count; i++) {
        ABORT_IF_NOT_EQUAL(res[0], res[i]);
        ABORT_IF_NOT_EQUAL(errnos[0], errnos[i]);
    }

    if (res[0] == -1) {
        return -errnos[0];
    }

    return 0;
}

static int mirrorfs_chmod(const char *path, mode_t mode,
                          struct fuse_file_info *fi)
{
    LOG_FUSE_OPERATION("%s 0x%x", path, mode);

    int res[MAX_MNTPATHS];
    int errnos[MAX_MNTPATHS];

    for (int i = 0; i < mntpath_count; i++) {
        errno = 0;
        res[i] = fchmodat(mntfds[i], safe_path(path), mode, 0);
        errnos[i] = errno;
    }

    // Compare results
    for (int i = 1; i < mntpath_count; i++) {
        ABORT_IF_NOT_EQUAL(res[0], res[i]);
        ABORT_IF_NOT_EQUAL(errnos[0], errnos[i]);
    }

    if (res[0] == -1) {
        return -errnos[0];
    }

    return 0;
}

static int mirrorfs_chown(const char *path, uid_t uid, gid_t gid,
                          struct fuse_file_info *fi)
{
    LOG_FUSE_OPERATION("%s %d %d", path, uid, gid);

    int res[MAX_MNTPATHS];
    int errnos[MAX_MNTPATHS];

    for (int i = 0; i < mntpath_count; i++) {
        errno = 0;
        res[i] = fchownat(mntfds[i], safe_path(path), uid, gid, 0);
        errnos[i] = errno;
    }

    // Compare results
    for (int i = 1; i < mntpath_count; i++) {
        ABORT_IF_NOT_EQUAL(res[0], res[i]);
        ABORT_IF_NOT_EQUAL(errnos[0], errnos[i]);
    }

    if (res[0] == -1) {
        return -errnos[0];
    }

    return 0;
}

// TODO: not implemented: call open and ftruncate whe fi is NULL?
// TODO: not implemented: call open and ftruncate when fi is NULL?
static int mirrorfs_truncate(const char *path, off_t size,
                             struct fuse_file_info *fi)
{
    int res;

    if (fi != NULL) {
        res = ftruncate(fi->fh, size);
    } else {
        res = truncate(path, size);
    }
    if (res == -1) {
        return -errno;
    }

    return 0;
}

static int mirrorfs_utimens(const char *path, const struct timespec ts[2],
                            struct fuse_file_info *fi)
{
    LOG_FUSE_OPERATION("%s", path);

    int res[MAX_MNTPATHS];
    int errnos[MAX_MNTPATHS];

    for (int i = 0; i < mntpath_count; i++) {
        errno = 0;
        res[i] = utimensat(mntfds[i], safe_path(path), ts, AT_SYMLINK_NOFOLLOW);
        errnos[i] = errno;
    }

    // Compare results
    for (int i = 1; i < mntpath_count; i++) {
        ABORT_IF_NOT_EQUAL(res[0], res[i]);
        ABORT_IF_NOT_EQUAL(errnos[0], errnos[i]);
    }

    if (res[0] == -1) {
        return -errnos[0];
    }

    return 0;
}

static int mirrorfs_create(const char *path, mode_t mode,
                           struct fuse_file_info *fi)
{
    LOG_FUSE_OPERATION("%s %o 0x%x", path, mode, fi->flags);

    int fds[MAX_MNTPATHS];
    int errnos[MAX_MNTPATHS];

    for (int i = 0; i < mntpath_count; i++) {
        errno = 0;
        fds[i] = openat(mntfds[i], safe_path(path), fi->flags, mode);
        errnos[i] = errno;
    }

    // Compare results
    for (int i = 1; i < mntpath_count; i++) {
        ABORT_IF_INCONSISTENT_FD(fds[0], fds[i]);
        ABORT_IF_NOT_EQUAL(errnos[0], errnos[i]);
    }

    if (fds[0] == -1) {
        return -errnos[0];
    }

    fi->fh = fds[0];
    for (int i = 1; i < mntpath_count; i++) {
        assert(mirror_fds[fi->fh * (MAX_MNTPATHS - 1) + i - 1] == -1);
        mirror_fds[fi->fh * (MAX_MNTPATHS - 1) + i - 1] = fds[i];
    }
    return 0;
}

static int mirrorfs_open(const char *path, struct fuse_file_info *fi)
{
    LOG_FUSE_OPERATION("%s", path);

    int fds[MAX_MNTPATHS];
    int errnos[MAX_MNTPATHS];

    for (int i = 0; i < mntpath_count; i++) {
        errno = 0;
        fds[i] = openat(mntfds[i], safe_path(path), fi->flags);
        errnos[i] = errno;
    }

    // Compare results
    for (int i = 1; i < mntpath_count; i++) {
        ABORT_IF_INCONSISTENT_FD(fds[0], fds[i]);
        ABORT_IF_NOT_EQUAL(errnos[0], errnos[i]);
    }

    if (fds[0] == -1) {
        return -errnos[0];
    }

    fi->fh = fds[0];
    for (int i = 1; i < mntpath_count; i++) {
        assert(mirror_fds[fi->fh * (MAX_MNTPATHS - 1) + i - 1] == -1);
        mirror_fds[fi->fh * (MAX_MNTPATHS - 1) + i - 1] = fds[i];
    }
    return 0;
}

static int mirrorfs_read(const char *path, char *buf, size_t size,
                         off_t offset, struct fuse_file_info *fi)
{
    LOG_FUSE_OPERATION("%s %zu %ld %p", path, size, offset, fi);

    int fds[MAX_MNTPATHS];

    if (fi == NULL) {
        for (int i = 0; i < mntpath_count; i++) {
            fds[i] = openat(mntfds[i], safe_path(path), O_RDONLY);
            if (fds[i] == -1) {
                for (int j = 0; j < i; j++) {
                    close(fds[j]);
                }
                return -errno;
            }
        }
    } else {
        fds[0] = fi->fh;
        for (int i = 1; i < mntpath_count; i++) {
            fds[i] = mirror_fds[fi->fh * (MAX_MNTPATHS - 1) + i - 1];
        }
    }

    char *bufs[MAX_MNTPATHS];
    int res[MAX_MNTPATHS];
    int errnos[MAX_MNTPATHS];

    for (int i = 0; i < mntpath_count; i++) {
        bufs[i] = (i == 0) ? buf : malloc(size);
        errno = 0;
        res[i] = pread(fds[i], bufs[i], size, offset);
        errnos[i] = errno;
    }

    // Compare results
    for (int i = 1; i < mntpath_count; i++) {
        ABORT_IF_NOT_EQUAL(res[0], res[i]);
        ABORT_IF_NOT_EQUAL(errnos[0], errnos[i]);
        if (res[0] != -1 && memcmp(bufs[0], bufs[i], res[0]) != 0) {
            abort();
        }
    }

    int result = (res[0] == -1) ? -errnos[0] : res[0];

    for (int i = 1; i < mntpath_count; i++) {
        free(bufs[i]);
    }

    if (fi == NULL) {
        for (int i = 0; i < mntpath_count; i++) {
            close(fds[i]);
        }
    }

    return result;
}

static int mirrorfs_write(const char *path, const char *buf, size_t size,
                          off_t offset, struct fuse_file_info *fi)
{
    LOG_FUSE_OPERATION("%s %lu %ld", path, size, offset);

    int fds[MAX_MNTPATHS];

    LOG_FUSE_OPERATION("%s %zu %ld", path, size, offset);

    if (fi == NULL) {
        LOG_FUSE_OPERATION("fi is NULL, opening files %s", path);
        for (int i = 0; i < mntpath_count; i++) {
            fds[i] = openat(mntfds[i], safe_path(path), O_WRONLY);
            if (fds[i] == -1) {
                LOG_FUSE_OPERATION("Failed to open file %d: %s", i, strerror(errno));
                for (int j = 0; j < i; j++) {
                    close(fds[j]);
                }
                return -errno;
            }
        }
    } else {
        LOG_FUSE_OPERATION("fi is not NULL, using existing file handles %s", path);
        fds[0] = fi->fh;
        for (int i = 1; i < mntpath_count; i++) {
            fds[i] = mirror_fds[fi->fh * (MAX_MNTPATHS - 1) + i - 1];
        }
    }

    int res[MAX_MNTPATHS];
    int errnos[MAX_MNTPATHS];

    for (int i = 0; i < mntpath_count; i++) {
        errno = 0;
        res[i] = pwrite(fds[i], buf, size, offset);
        errnos[i] = errno;
        LOG_FUSE_OPERATION("pwrite to file %d returned %d, errno=%d", i, res[i], errnos[i]);
    }

    // Compare results
    for (int i = 1; i < mntpath_count; i++) {
        ABORT_IF_NOT_EQUAL(res[0], res[i]);
        ABORT_IF_NOT_EQUAL(errnos[0], errnos[i]);
    }

    int result = (res[0] == -1) ? -errnos[0] : res[0];

    if (fi == NULL) {
        for (int i = 0; i < mntpath_count; i++) {
            close(fds[i]);
        }
    }

    LOG_FUSE_OPERATION("returning %d", result);
    return result;
}

static int mirrorfs_release(const char *path, struct fuse_file_info *fi)
{
    LOG_FUSE_OPERATION("%s", path);

    close(mirror_fds[fi->fh]);
    mirror_fds[fi->fh] = -1;
    // Must close fd1 after fd2 since mirror_fds uses fd1 as a key.
    close(fi->fh);
    return 0;
}

static int mirrorfs_fsync(const char *path, int isdatasync,
             struct fuse_file_info *fi)
{
    LOG_FUSE_OPERATION("%s %d", path, isdatasync);

    return 0;
}

static const struct fuse_operations mirrorfs_oper = {
    .init = mirrorfs_init,
    .getattr = mirrorfs_getattr,
    .access = mirrorfs_access,
    .readlink = mirrorfs_readlink,
    .readdir = mirrorfs_readdir,
    .mkdir = mirrorfs_mkdir,
    .symlink = mirrorfs_symlink,
    .unlink = mirrorfs_unlink,
    .rmdir = mirrorfs_rmdir,
    .rename = mirrorfs_rename,
    .link = mirrorfs_link,
    .chmod = mirrorfs_chmod,
    .chown = mirrorfs_chown,
    //.truncate = mirrorfs_truncate,
    .utimens = mirrorfs_utimens,
    .open = mirrorfs_open,
    .create = mirrorfs_create,
    .read = mirrorfs_read,
    .write = mirrorfs_write,
    .release = mirrorfs_release,
    .fsync = mirrorfs_fsync,
};

static void show_help(const char *progname);

static int mirrorfs_opt_proc(void *data, const char *arg, int key,
                             struct fuse_args *outargs)
{
    (void) data;
    (void) outargs;

    switch (key) {
        case 'h':
            show_help(outargs->argv[0]);
            fuse_opt_free_args(outargs);
            exit(0);
        case FUSE_OPT_KEY_NONOPT:
            if (mntpath_count < MAX_MNTPATHS) {
                mntpaths[mntpath_count++] = arg;
                return 0;
            }
            break;
    }
    return 1;
}

static struct fuse_opt mirrorfs_opts[] = {
    FUSE_OPT_KEY("-h", 'h'),
    FUSE_OPT_KEY("--help", 'h'),
    FUSE_OPT_END
};

static void show_help(const char *progname)
{
    printf("usage: %s <mntpath1> <mntpath2> [<mntpath3> ...] <mountpoint> [options]\n\n", progname);
    printf("File-system specific options:\n"
           "    <mntpathN>             Path to mirror (at least 2 required)\n"
           "    <mountpoint>           Where to mount the mirrored file system\n\n");
    printf("general options:\n");
    printf("    -o opt,[opt...]        mount options\n");
    printf("    -h   --help            print help\n");
}

int main(int argc, char *argv[])
{
    memset(mirror_fds, -1, sizeof(mirror_fds));
    umask(0);

    struct fuse_args args = FUSE_ARGS_INIT(argc, argv);

    int res = fuse_opt_parse(&args, NULL, mirrorfs_opts, mirrorfs_opt_proc);

    fuse_opt_free_args(&args);

    if (res != 0 || mntpath_count < 3) {
        show_help(argv[0]);
        return 1;
    }
    
    // The last path is the mount point, so we don't open it
    for (int i = 0; i < mntpath_count - 1; i++) {
        mntfds[i] = open(mntpaths[i], O_DIRECTORY);
        if (mntfds[i] == -1) {
            fprintf(stderr, "Could not open mntpath%d: %s\n", i+1, strerror(errno));
            return 1;
        }
    }
    
    // Adjust mntpath_count to exclude the mount point
    mntpath_count--;
    
    // Set up FUSE arguments
    char *fuse_argv[3];
    fuse_argv[0] = argv[0];
    fuse_argv[1] = (char *)mntpaths[mntpath_count];  // Mount point
    fuse_argv[2] = NULL;
    
    return fuse_main(2, fuse_argv, &mirrorfs_oper, NULL);
}
