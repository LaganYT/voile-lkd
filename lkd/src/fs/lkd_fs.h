/* lkd_fs.h: services of the fs module for the engine and for other modules (procfs, mm, task).
 * Announced in docs/requests/fs-engine.md and docs/requests/fs-procfs.md. Include as "lkd_fs.h"
 * (src/include/lkd_fs.h forwards here, request fs-1). */
#ifndef LKD_FS_H
#define LKD_FS_H

#include "fd.h"

/* ELF loader: an fd object for lkd_mm_map from a host fd the caller keeps (img->host_fd of
 * lkd_exec_prepare). The host fd is dup'ed (CLOEXEC); the object is a read-only regular file whose
 * guest path is guest_path (shown in /proc/<pid>/maps). Returns 0 and *out (one reference, release
 * with lkd_fdobj_put) or -errno (-EBADF for a bad fd, -EACCES if not a regular file). ANY thread. */
int lkd_fs_open_host_file(const char *guest_path, int host_fd, lkd_fdobj **out);

/* Same from a guest path, resolved through the vfs of task t (O_RDONLY|O_CLOEXEC open). */
int lkd_fs_open_guest_file(lkd_task *t, const char *guest_path, lkd_fdobj **out);

/* Wrap a host descriptor (takes ownership) as an fd object: regular file, directory, tty or
 * character device, FIFO or socket stream, chosen from fstat. lx_flags are the Linux status flags to
 * record (access mode; O_NONBLOCK and O_APPEND are read from the host fd). */
int lkd_fs_wrap_host_fd(int host_fd, uint32_t lx_flags, const char *guest_path, lkd_fdobj **out);

/* Install the host stdin, stdout and stderr (dup'ed) as guest fds 0, 1, 2 of t. Returns 0 or -errno. */
int lkd_fs_install_stdio(lkd_task *t);

/* Seals of a memfd object (LX_F_SEAL_* bits), or -1 if o is not a memfd. mm asks it before creating
 * a writable shared mapping (F_SEAL_WRITE and F_SEAL_FUTURE_WRITE forbid it with -EPERM). */
int lkd_fs_memfd_seals(lkd_fdobj *o);

/* The query behind -EBUSY of F_ADD_SEALS(F_SEAL_WRITE). Points to lkd_mm_has_writable_shared_mapping;
 * tests replace it to simulate a writable shared mapping. */
struct lkd_mm;
extern int (*lkd_fs_wsm_query)(struct lkd_mm *mm, struct lkd_fdobj *file);

/* Text of readlink(/proc/<pid>/fd/N) for o: "/path", "/path (deleted)", "pipe:[ino]",
 * "/memfd:name (deleted)", "anon_inode:[...]" (other modules' describe). Returns length or -errno. */
int lkd_fs_describe(lkd_fdobj *o, char *buf, size_t cap);

/* mmap(2) checks that belong to the file (mm/mmap.c do_mmap, mm/memfd.c memfd_check_seals_mmap):
 * -EBADF O_PATH, -ENODEV no mappable host fd (pipes, directories, virtual files), -EACCES not open for
 * reading, -EACCES MAP_SHARED|PROT_WRITE on a description without write access or with O_APPEND,
 * -EPERM MAP_SHARED|PROT_WRITE on a memfd sealed F_SEAL_WRITE or F_SEAL_FUTURE_WRITE. prot/flags are
 * LX_PROT_* / LX_MAP_*. Returns 0 or the negative errno. mm calls it before mapping a file. */
int lkd_fs_mmap_check(lkd_fdobj *o, int prot, uint32_t flags);

/* /proc/<pid>/fdinfo/N basics: position, F_GETFL flags (octal in fdinfo), mount id. 0 or -errno. */
int lkd_fs_fdinfo_basic(lkd_task *t, lkd_fdobj *o, int64_t *pos, uint32_t *flags, int *mnt_id);

/* Mount the guest root read-only (tests on a read-only tree). Call after lkd_vfs_set_root. */
void lkd_fs_set_root_ro(int ro);

/* Host directories bound at /tmp and /dev/shm by lkd_fs_init (empty string if none). */
const char *lkd_fs_host_tmp_dir(void);
const char *lkd_fs_host_shm_dir(void);

/* Guest path of the host path, through the mount table (reverse map); 0 or -ENOENT. */
int lkd_fs_host_to_guest(const char *host_path, char *out, size_t cap);

/* Host phase of a mappable object (request wine-7, see src/fs/memfd.c): the host offset of guest file byte x
 * is x + phase. want < 0: any phase (the decided one, else 0 is decided); want >= 0: exactly that phase
 * (0, 4096, 8192, 12288). Returns the phase or -EINVAL. Host files have phase 0 and cannot change it. The
 * first call on a memfd decides its phase for good (content moved when not 0). */
int lkd_fs_map_phase(lkd_fdobj *o, int64_t want);
#define LKD_PHASE_ANY (-1)
#define LKD_PHASE_PEEK (-2)   /* the decided phase, or 0 when undecided; decides nothing */
/* Before handing the host fd out raw (SCM_RIGHTS, exec, a reopen served to another process): 0 when the
 * host offsets are the guest offsets (phase pinned at 0), -EOPNOTSUPP otherwise. */
int lkd_fs_pin_raw(lkd_fdobj *o);

/* Used by procfs (request procfs-4 point 1); names and signatures kept stable. Canonical guest root of a
 * task (for /proc/<pid>/root); copy between a host fd and guest memory at an explicit offset (NULL: the
 * fd's own offset); Linux stat from a host stat. */
void fs_get_root(lkd_task *t, char *out, size_t cap);
int64_t fs_host_io(int host_fd, uint64_t gva, uint64_t count, const int64_t *off, int is_write);
struct stat;
void fs_stat_from_host(const struct stat *hs, struct lx_stat *ls);

#endif
