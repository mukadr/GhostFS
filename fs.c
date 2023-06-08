#include <err.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "fs.h"
#include "lsb.h"
#include "md5.h"
#include "stegger.h"

#define min(a, b) ((a) < (b) ? (a) : (b))

enum {
	CLUSTER_SIZE = 4096,
	CLUSTER_DATA = 4092,
	CLUSTER_DIRENTS = 66,
	FILENAME_SIZE = 56,
	FILESIZE_MAX = 0x7FFFFFFF
};

/*
 * MD5(header+cluster0) | header | cluster0 .. clusterN
 */
struct ghostfs_header {
	uint16_t cluster_count;
} __attribute__((packed));

/*
 * Root directory '/' is stored at cluster 0.
 *
 * Each directory cluster have 66 entries(62 bytes each) summing 4092 bytes.
 * The remaining 4 bytes of the cluster are used to store the cluster_header
 *
 * An empty filename (filename[0] == '\0') means that the entry is empty
 */
struct dir_entry {
	char filename[FILENAME_SIZE];
	uint32_t size;
	uint16_t cluster;
} __attribute__((packed));

static inline bool dir_entry_is_directory(const struct dir_entry *e)
{
	return (e->size & 0x80000000) != 0;
}

static inline void dir_entry_set_size(struct dir_entry *e, uint32_t new_size, bool is_dir)
{
	e->size = new_size & 0x7FFFFFFF;
	if (is_dir)
		e->size |= 0x80000000;
}

static inline bool dir_entry_used(const struct dir_entry *e)
{
	return e->filename[0] != '\0';
}

struct ghostfs {
	struct ghostfs_header hdr;
	struct stegger *stegger;
	struct cluster **clusters;
	struct dir_entry root_entry;
	uid_t uid;
	gid_t gid;
	time_t mount_time;
	uint16_t free_clusters;
};

struct cluster_header {
	uint16_t next;
	uint8_t used;

        /* unused byte. we use it only in-memory to know if the cache entry is dirty */
	uint8_t dirty;
} __attribute__((packed));

struct cluster {
	unsigned char data[CLUSTER_DATA];
	struct cluster_header hdr;
} __attribute__((packed));

static inline void mark_cluster(struct cluster *c)
{
	c->hdr.dirty = 1;
}

static inline void unmark_cluster(struct cluster *c)
{
        c->hdr.dirty = 0;
}

static inline bool is_dirty(const struct cluster *c)
{
	return c->hdr.dirty != 0;
}

struct dir_iter {
	struct ghostfs *gfs;
	struct cluster *cluster;
	struct dir_entry *entry;
	int entry_nr;
};

struct ghostfs_entry {
	struct dir_iter it;
};

static int cluster_get(struct ghostfs *gfs, int nr, struct cluster **pcluster);
static int cluster_get_next(struct ghostfs *gfs, struct cluster **pcluster);
static int cluster_at(struct ghostfs *gfs, int nr, int index, struct cluster **pcluster);
static int write_cluster(struct ghostfs *gfs, struct cluster *cluster, int nr);
static int read_cluster(struct ghostfs *gfs, struct cluster *cluster, int nr);
static int ghostfs_check(struct ghostfs *gfs);
static void ghostfs_free(struct ghostfs *gfs);

static int dir_iter_init(struct ghostfs *gfs, struct dir_iter *it, int cluster_nr)
{
	int ret;

	ret = cluster_get(gfs, cluster_nr, &it->cluster);
	if (ret < 0)
		return ret;

	it->gfs = gfs;
	it->entry = (struct dir_entry *)it->cluster->data;
	it->entry_nr = 0;
	return 0;
}

static int dir_iter_next(struct dir_iter *it)
{
	int ret;

	if (it->entry_nr >= CLUSTER_DIRENTS - 1) {
		if (it->cluster->hdr.next == 0)
			return -ENOENT;

		ret = cluster_get_next(it->gfs, &it->cluster);
		if (ret < 0)
			return ret;

		it->entry_nr = 0;
		it->entry = (struct dir_entry *)it->cluster->data;
		return 0;
	}

	it->entry_nr++;
	it->entry++;
	return 0;
}

static int dir_iter_next_used(struct dir_iter *it)
{
	struct dir_iter temp = *it;
	int ret;

	do {
		ret = dir_iter_next(&temp);
		if (ret < 0)
			return ret;
	} while (!dir_entry_used(temp.entry));

	*it = temp;
	return 0;
}

static bool component_eq(const char *comp, const char *name, size_t n)
{
	while (n > 0 && *comp && *comp != '/' && *comp == *name) {
		comp++;
		name++;
		n--;
	}
	if (n == 0)
		return false;

	return (!*comp || *comp == '/') && !*name;
}

static int dir_iter_lookup(struct ghostfs *gfs, struct dir_iter *it, const char *path,
			   bool skip_last)
{
	const char *comp;
	int ret;

	if (path[0] != '/')
		return -EINVAL;

	ret = dir_iter_init(gfs, it, 0);
	if (ret < 0)
		return ret;

	comp = path + 1;
	if (!comp[0] || (skip_last && !strchr(comp, '/'))) {
		it->entry = &gfs->root_entry;
		return 0;
	}

	for (;;) {
		if (component_eq(comp, it->entry->filename, FILENAME_SIZE)) {
			const char *next = strchr(comp, '/');

			// finished
			if (!next || (skip_last && !strchr(next + 1, '/')))
				return 0;

			if (!dir_entry_is_directory(it->entry))
				return -ENOTDIR;

			// start searching child directory
			ret = dir_iter_init(gfs, it, it->entry->cluster);
			if (ret < 0)
				return ret;

			comp = next + 1;
		} else {
			ret = dir_iter_next_used(it);
			if (ret < 0)
				return ret;
		}
	}
}

static const char *last_component(const char *path)
{
	const char *s;

	while ((s = strchr(path, '/')) != NULL)
		path = s + 1;

	return path;
}

/*
 * Updates iter to point to the first unused entry in the cluster.
 * If no entry is available, iter is updated to the last entry and -ENOENT is returned.
 */
static int find_empty_entry(struct ghostfs *gfs, struct dir_iter *iter, int cluster_nr)
{
	struct dir_iter it;
	int ret;

	ret = dir_iter_init(gfs, &it, cluster_nr);
	if (ret < 0)
		return ret;

	while (dir_entry_used(it.entry)) {
		ret = dir_iter_next(&it);
		if (ret < 0)
			break;
	}

	if (ret == 0 || ret == -ENOENT)
		*iter = it;

	return ret;
}

static int dir_contains(struct ghostfs *gfs, int cluster_nr, const char *name)
{
	struct dir_iter it;
	int ret;

	ret = dir_iter_init(gfs, &it, cluster_nr);
	if (ret < 0)
		return ret;

	for (;;) {
		if (!strncmp(it.entry->filename, name, FILENAME_SIZE))
			return 0;

		ret = dir_iter_next_used(&it);
		if (ret < 0)
			break;
	}

	return ret;
}

// allocates a list of clusters
static int alloc_clusters(struct ghostfs *gfs, int count, struct cluster **pfirst, bool zero)
{
	struct cluster *prev = NULL;
	struct cluster *c;
	int first = 0;
	int pos = 1;
	int alloc = 0;
	int ret;

	while (alloc < count) {
		for (;;) {
			if (pos >= gfs->hdr.cluster_count) {
				ret = -ENOSPC;
				goto undo;
			}

			ret = cluster_get(gfs, pos, &c);
			if (ret < 0)
				goto undo;

			if (!c->hdr.used) {
				if (zero)
					memset(c->data, 0, sizeof(c->data));

				c->hdr.used = 1;
				mark_cluster(c);
				gfs->free_clusters--;

				if (!first) {
					first = pos;
					if (pfirst)
						*pfirst = c;
				} else {
					prev->hdr.next = pos;
				}
				prev = c;
				pos++;
				break;
			}
			pos++;
		}
		alloc++;
	}

	prev->hdr.next = 0;

	return first;
undo:
	pos = first;

	while (alloc > 0) {
		int r = cluster_get(gfs, pos, &c);
		if (r < 0)
			return r;

		c->hdr.used = 0;
		mark_cluster(c);
		gfs->free_clusters++;

		pos = c->hdr.next;
		alloc--;
	}

	return ret;
}

static int free_clusters(struct ghostfs *gfs, struct cluster *c)
{
	int ret;

	for (;;) {
		c->hdr.used = 0;
		mark_cluster(c);
		gfs->free_clusters++;

		if (!c->hdr.next)
			break;

		ret = cluster_get(gfs, c->hdr.next, &c);
		if (ret < 0) {
			errno = -ret;
			warn("failed to free cluster");
			return ret;
		}
	}

	return 0;
}

static int create_entry(struct ghostfs *gfs, const char *path, bool is_dir,
		struct dir_entry **entry)
{
	struct dir_iter it;
	struct cluster *prev = NULL, *next = NULL;
	const char *name;
	int cluster_nr = 0;
	int ret;

	ret = dir_iter_lookup(gfs, &it, path, true);
	if (ret < 0)
		return ret;

	if (!dir_entry_is_directory(it.entry))
		return -ENOTDIR;

	name = last_component(path);
	if (strlen(name) > FILENAME_SIZE - 1)
		return -ENAMETOOLONG;

	if (!name[0])
		return -EINVAL;

	if (dir_contains(gfs, it.entry->cluster, name) == 0)
		return -EEXIST;

	ret = find_empty_entry(gfs, &it, it.entry->cluster);
	if (ret < 0) {
		int nr;

		if (ret != -ENOENT)
			return ret;

		nr = alloc_clusters(gfs, 1, &next, true);
		if (nr < 0)
			return nr;

		prev = it.cluster;
		find_empty_entry(gfs, &it, nr);

		prev->hdr.next = nr;
		mark_cluster(prev);
	}

	if (is_dir) {
		cluster_nr = alloc_clusters(gfs, 1, NULL, true);
		if (cluster_nr < 0) {
			if (next) {
				free_clusters(gfs, next);
				prev->hdr.next = 0;
			}
			return cluster_nr;
		}
	}

	strcpy(it.entry->filename, name);
	dir_entry_set_size(it.entry, 0, is_dir);
	it.entry->cluster = cluster_nr;
	mark_cluster(it.cluster);

	if (entry)
		*entry = it.entry;

	return 0;
}

int ghostfs_create(struct ghostfs *gfs, const char *path)
{
	return create_entry(gfs, path, false, NULL);
}

int ghostfs_mkdir(struct ghostfs *gfs, const char *path)
{
	return create_entry(gfs, path, true, NULL);
}

static int remove_entry(struct ghostfs *gfs, const char *path, bool is_dir)
{
	struct dir_iter link, it;
	int ret;

	ret = dir_iter_lookup(gfs, &link, path, false);
	if (ret < 0)
		return ret;

	if (link.entry == &gfs->root_entry)
		return -EINVAL;

	if (is_dir != dir_entry_is_directory(link.entry))
		return is_dir ? -ENOTDIR : -EISDIR;

	// no clusters, we are done
	if (!link.entry->cluster)
		goto unlink;

	ret = dir_iter_init(gfs, &it, link.entry->cluster);
	if (ret < 0)
		return ret;

	// make sure directory is empty
	if (is_dir) {
		if (dir_entry_used(it.entry))
			return -ENOTEMPTY;

		ret = dir_iter_next_used(&it);
		if (ret != -ENOENT)
			return ret == 0 ? -ENOTEMPTY : ret;
	}

	free_clusters(gfs, it.cluster);
unlink:
	link.entry->filename[0] = '\0';
	mark_cluster(link.cluster);

	return 0;
}

int ghostfs_unlink(struct ghostfs *gfs, const char *path)
{
	return remove_entry(gfs, path, false);
}

int ghostfs_rmdir(struct ghostfs *gfs, const char *path)
{
	return remove_entry(gfs, path, true);
}

static int size_to_clusters(int size)
{
	return size / CLUSTER_DATA + (size % CLUSTER_DATA ? 1 : 0);
}

static int do_truncate(struct ghostfs *gfs, struct dir_iter *it, off_t new_size)
{
	int ret;
	int count;
	int next;
	struct cluster *c = NULL;

	if (new_size < 0)
		return -EINVAL;

	if (new_size > FILESIZE_MAX)
		return -EFBIG;

	if (dir_entry_is_directory(it->entry))
		return -EISDIR;

	next = it->entry->cluster;
	count = size_to_clusters(min(it->entry->size, new_size));

	if (count) {
		ret = cluster_at(gfs, next, count - 1, &c);
		if (ret < 0)
			return ret;

		next = c->hdr.next;
	}

	if (new_size > it->entry->size) {
		int alloc;
		long used = it->entry->size % CLUSTER_DATA;

		// zero remaining cluster space
		if (used) {
			memset(c->data + used, 0, CLUSTER_DATA - used);
			mark_cluster(c);
		}

		alloc = size_to_clusters(new_size) - count;
		if (alloc) {
			ret = alloc_clusters(gfs, alloc, NULL, true);
			if (ret < 0)
				return ret;

			if (c) {
				c->hdr.next = ret;
				mark_cluster(c);
			} else {
				it->entry->cluster = ret;
			}
		}
	} else if (new_size < it->entry->size) {
		if (next) {
			if (c) {
				c->hdr.next = 0;
				mark_cluster(c);
			}

			ret = cluster_get(gfs, next, &c);
			if (ret < 0)
				return ret;

			free_clusters(gfs, c);
		}
	}

	dir_entry_set_size(it->entry, new_size, false);
	mark_cluster(it->cluster);

	return 0;
}

int ghostfs_truncate(struct ghostfs *gfs, const char *path, off_t new_size)
{
	struct dir_iter it;
	int ret;

	ret = dir_iter_lookup(gfs, &it, path, false);
	if (ret < 0)
		return ret;

	return do_truncate(gfs, &it, new_size);
}

int ghostfs_rename(struct ghostfs *gfs, const char *path, const char *newpath)
{
	struct dir_iter it;
	struct dir_entry *entry;
	int ret;

	ret = dir_iter_lookup(gfs, &it, path, false);
	if (ret < 0)
		return ret;

	if (it.entry == &gfs->root_entry)
		return -EINVAL;

	remove_entry(gfs, newpath, false);

	ret = create_entry(gfs, newpath, false, &entry);
	if (ret < 0)
		return ret;

	// remove old entry
	it.entry->filename[0] = '\0';
	mark_cluster(it.cluster);

	// fix new entry
	entry->size = it.entry->size;
	entry->cluster = it.entry->cluster;

	return 0;
}

int ghostfs_open(struct ghostfs *gfs, const char *filename, struct ghostfs_entry **pentry)
{
	struct dir_iter it;
	int ret;

	ret = dir_iter_lookup(gfs, &it, filename, false);
	if (ret < 0)
		return ret;

	if (dir_entry_is_directory(it.entry))
		return -EISDIR;

	if (pentry) {
		*pentry = malloc(sizeof(**pentry));
		if (!*pentry)
			return -ENOMEM;
		(*pentry)->it = it;
	}

	return 0;
}

void ghostfs_release(struct ghostfs_entry *entry)
{
	free(entry);
}

int ghostfs_write(struct ghostfs *gfs, struct ghostfs_entry *gentry, const char *buf,
		  size_t size, off_t offset)
{
	struct dir_entry *entry = gentry->it.entry;
	struct cluster *c;
	int ret;
	int written = 0;

	if (offset < 0)
		return -EINVAL;

	if (size + offset < size)
		return -EOVERFLOW;

	if (entry->size < offset + size) {
		ret = do_truncate(gfs, &gentry->it, offset + size);
		if (ret < 0)
			return ret;
	}

	ret = cluster_at(gfs, entry->cluster, offset/CLUSTER_DATA, &c);
	if (ret < 0)
		return ret;

	offset %= CLUSTER_DATA;

	for (;;) {
		int w = min(size, CLUSTER_DATA);
		if (offset + w > CLUSTER_DATA)
			w -= (offset + w) - CLUSTER_DATA;

		memcpy(c->data + offset, buf, w);
		mark_cluster(c);

		size -= w;
		buf += w;
		written += w;
		offset = 0;

		if (!size)
			break;

		ret = cluster_get_next(gfs, &c);
		if (ret < 0)
			return ret;
	}

	return written;
}

int ghostfs_read(struct ghostfs *gfs, struct ghostfs_entry *gentry, char *buf,
		 size_t size, off_t offset)
{
	struct dir_entry *entry = gentry->it.entry;
	struct cluster *c;
	int ret;
	int read = 0;

	if (offset < 0)
		return -EINVAL;

	if (size + offset < size)
		return -EOVERFLOW;

	if (offset > entry->size)
		return 0;

	if (offset + size > entry->size)
		size = entry->size - offset;

	if (!size)
		return 0;

	ret = cluster_at(gfs, entry->cluster, offset/CLUSTER_DATA, &c);
	if (ret < 0)
		return ret;

	offset %= CLUSTER_DATA;

	for (;;) {
		int r = min(size, CLUSTER_DATA);
		if (offset + r > CLUSTER_DATA)
			r -= (offset + r) - CLUSTER_DATA;

		memcpy(buf, c->data + offset, r);

		size -= r;
		buf += r;
		read += r;
		offset = 0;

		if (!size)
			break;

		ret = cluster_get_next(gfs, &c);
		if (ret < 0)
			return ret;
	}

	return read;
}

int ghostfs_opendir(struct ghostfs *gfs, const char *path, struct ghostfs_entry **pentry)
{
	struct dir_iter it;
	int ret;

	ret = dir_iter_lookup(gfs, &it, path, false);
	if (ret < 0)
		return ret;

	if (!dir_entry_is_directory(it.entry))
		return -ENOTDIR;

	*pentry = calloc(1, sizeof(**pentry));
	if (!*pentry)
		return -ENOMEM;
	(*pentry)->it.entry = it.entry;

	return 0;
}

int ghostfs_next_entry(struct ghostfs *gfs, struct ghostfs_entry *entry)
{
	int ret;

	if (!entry->it.gfs) {
		ret = dir_iter_init(gfs, &entry->it, entry->it.entry->cluster);
		if (ret < 0)
			return ret;

		if (dir_entry_used(entry->it.entry))
			return 0;
	}

	return dir_iter_next_used(&entry->it);
}

void ghostfs_closedir(struct ghostfs_entry *entry)
{
	free(entry);
}

int ghostfs_getattr(struct ghostfs *gfs, const char *filename, struct stat *stat)
{
	struct dir_iter it;
	int ret;

	ret = dir_iter_lookup(gfs, &it, filename, false);
	if (ret < 0)
		return ret;

	memset(stat, 0, sizeof(*stat));

	if (dir_entry_is_directory(it.entry)) {
		stat->st_mode |= S_IFDIR | S_IXUSR;
		stat->st_size = CLUSTER_SIZE;
	} else {
		stat->st_mode |= S_IFREG;
		stat->st_size = it.entry->size;
	}

	// user that mounted filesystem owns all files
	stat->st_uid = gfs->uid;
	stat->st_gid = gfs->gid;
	// only read and write allowed
	stat->st_mode |= S_IRUSR | S_IWUSR;

	stat->st_blocks = stat->st_size / 512 + (stat->st_size % 512 ? 1 : 0);

	// all time fields use the mount time
	memcpy(&stat->st_atime, &gfs->mount_time, sizeof(time_t));
	memcpy(&stat->st_mtime, &gfs->mount_time, sizeof(time_t));
	memcpy(&stat->st_ctime, &gfs->mount_time, sizeof(time_t));

	// only one hardlink
	stat->st_nlink = 1;

	return 0;
}

int ghostfs_statvfs(struct ghostfs *gfs, struct statvfs *stat)
{
	memset(stat, 0, sizeof(*stat));

	stat->f_bsize = CLUSTER_SIZE;
	stat->f_frsize = CLUSTER_SIZE;
	stat->f_blocks = gfs->hdr.cluster_count;
	stat->f_bfree = gfs->free_clusters;
	stat->f_bavail = stat->f_bfree;
	stat->f_files = 0; // FIXME: keep track of how many files we have
	stat->f_ffree = 0; // FIXME: ?
	stat->f_namemax = FILESIZE_MAX;

	return 0;
}

static int cluster_get(struct ghostfs *gfs, int nr, struct cluster **pcluster)
{
	int ret;

	if (nr >= gfs->hdr.cluster_count) {
		warnx("fs: invalid cluster number %d", nr);
		return -ERANGE;
	}

	if (!gfs->clusters[nr]) {
		struct cluster *c;

		c = malloc(CLUSTER_SIZE);
		if (!c)
			return -ENOMEM;

		ret = read_cluster(gfs, c, nr);
		if (ret < 0) {
			free(c);
			return ret;
		}

		gfs->clusters[nr] = c;
	}

	*pcluster = gfs->clusters[nr];

	return 0;
}

static int cluster_get_next(struct ghostfs *gfs, struct cluster **cluster)
{
        struct cluster *c = *cluster;

	if (!c->hdr.next) {
		warnx("fs: cluster missing, bad filesystem");
		return -EIO;
	}

	return cluster_get(gfs, c->hdr.next, cluster);
}

// cluster_at returns the cluster at the given index starting from cluster nr
static int cluster_at(struct ghostfs *gfs, int nr, int index, struct cluster **cluster)
{
	struct cluster *c = NULL;
	int ret, i;

	for (i = 0; i <= index; i++) {
		if (!nr) {
			warnx("fs: cluster missing, bad filesystem");
			return -EIO;
		}

		ret = cluster_get(gfs, nr, &c);
		if (ret < 0)
			return ret;

		nr = c->hdr.next;
	}

	*cluster = c;

	return 0;
}

static int write_cluster(struct ghostfs *gfs, struct cluster *cluster, int nr)
{
	const size_t c0_offset = 16 + sizeof(struct ghostfs_header);
	int ret;

	ret = stegger_write(gfs->stegger, cluster, CLUSTER_SIZE, c0_offset + nr*CLUSTER_SIZE);
	if (ret < 0)
		return ret;

	unmark_cluster(cluster);
	return 0;
}

static int read_cluster(struct ghostfs *gfs, struct cluster *cluster, int nr)
{
	const size_t c0_offset = 16 + sizeof(struct ghostfs_header);
	int ret;

	ret = stegger_read(gfs->stegger, cluster, CLUSTER_SIZE, c0_offset + nr*CLUSTER_SIZE);
	if (ret < 0)
		return ret;

	unmark_cluster(cluster);
	return 0;
}

static int ghostfs_check(struct ghostfs *gfs)
{
	MD5_CTX md5_ctx;
	unsigned char md5_fs[16];
	unsigned char md5[16];
	struct cluster root;
	int ret;

	ret = stegger_read(gfs->stegger, md5_fs, sizeof(md5_fs), 0);
	if (ret < 0)
		return ret;

	ret = stegger_read(gfs->stegger, &gfs->hdr, sizeof(struct ghostfs_header), 16);
	if (ret < 0)
		return ret;

	ret = stegger_read(gfs->stegger, &root, sizeof(root), 16 + sizeof(struct ghostfs_header));
	if (ret < 0)
		return ret;

	MD5_Init(&md5_ctx);
	MD5_Update(&md5_ctx, &gfs->hdr, sizeof(gfs->hdr));
	MD5_Update(&md5_ctx, &root, sizeof(root));
	MD5_Final(md5, &md5_ctx);

	if (memcmp(md5, md5_fs, 16) == 0) {
		return 0;
	}

	return -EIO;
}

static int write_header(struct ghostfs *gfs, struct cluster *cluster0)
{
	MD5_CTX md5_ctx;
	unsigned char md5[16];
	int ret;

	MD5_Init(&md5_ctx);
	MD5_Update(&md5_ctx, &gfs->hdr, sizeof(gfs->hdr));
	MD5_Update(&md5_ctx, cluster0, sizeof(*cluster0));
	MD5_Final(md5, &md5_ctx);

	// write md5 of header+root
	ret = stegger_write(gfs->stegger, md5, sizeof(md5), 0);
	if (ret < 0)
		return ret;

	// write header
	ret = stegger_write(gfs->stegger, &gfs->hdr, sizeof(gfs->hdr), 16);
	if (ret < 0)
		return ret;

	// write first cluster
	ret = write_cluster(gfs, cluster0, 0);
	if (ret < 0)
		return ret;

	return 0;
}

// create a new filesystem
int ghostfs_format(struct stegger *stegger)
{
	struct ghostfs gfs;
	size_t count;
	struct cluster cluster;
	int ret, i;
	const int HEADER_SIZE = 16 + sizeof(struct ghostfs_header);

	gfs.stegger = stegger;

	if (gfs.stegger->capacity < HEADER_SIZE + CLUSTER_SIZE)
		return -ENOSPC;

	count = (gfs.stegger->capacity - HEADER_SIZE) / CLUSTER_SIZE;
	if (count > 0xFFFF) {
		warnx("fs: %lu clusters available, using only %d", count, 0xFFFF);
		count = 0xFFFF;
	}

	gfs.hdr.cluster_count = count;

	ret = read_cluster(&gfs, &cluster, 0);
	if (ret < 0)
		return ret;

	cluster.hdr.next = 0;

	for (i = 0; i < CLUSTER_DATA; i += sizeof(struct dir_entry)) {
		struct dir_entry *e = (struct dir_entry *)&cluster.data[i];
		e->filename[0] = '\0';
	}

	ret = write_header(&gfs, &cluster);
	if (ret < 0)
		return ret;

	for (i = 1; i < count; i++) {
		ret = read_cluster(&gfs, &cluster, i);
		if (ret < 0)
			return ret;

		cluster.hdr.used = 0;

		ret = write_cluster(&gfs, &cluster, i);
		if (ret < 0)
			return ret;
	}

	return 0;
}

static int print_dir_entries(struct ghostfs *gfs, int cluster_nr, const char *parent)
{
	struct dir_iter it;
	char buf[256] = "";
	int ret;

	ret = dir_iter_init(gfs, &it, cluster_nr);
	if (ret < 0)
		return ret;

	do {
		if (dir_entry_used(it.entry)) {
			snprintf(buf, sizeof(buf), "%s/%s", parent, it.entry->filename);
			printf("%s", buf);
			if (dir_entry_is_directory(it.entry)) {
				printf("/\n");
				ret = print_dir_entries(gfs, it.entry->cluster, buf);
				if (ret < 0)
					return ret;
			} else {
				printf(" {%d}\n", it.entry->size);
			}
		}
	} while ((ret = dir_iter_next_used(&it)) == 0);

	if (ret != -ENOENT)
		return ret;

	return 0;
}

int ghostfs_debug(struct ghostfs *gfs)
{
	return print_dir_entries(gfs, 0, "");
}

int ghostfs_mount(struct ghostfs **pgfs, struct stegger *stegger)
{
	struct ghostfs *gfs;
	int i, ret;

	gfs = calloc(1, sizeof(*gfs));
	if (!gfs)
		return -ENOMEM;

	gfs->stegger = stegger;
	gfs->root_entry.size = 0x80000000;

	ret = ghostfs_check(gfs);
	if (ret < 0) {
		ghostfs_free(gfs);
		return ret;
	}

	gfs->clusters = calloc(1, sizeof(struct cluster *) * gfs->hdr.cluster_count);
	if (!gfs->clusters) {
		ghostfs_free(gfs);
		return -ENOMEM;
	}

	gfs->uid = getuid();
	gfs->gid = getgid();
	time(&gfs->mount_time);

	// check free clusters
	for (i = 1; i < gfs->hdr.cluster_count; i++) {
		struct cluster *c;

		ret = cluster_get(gfs, i, &c);
		if (ret < 0) {
			ghostfs_free(gfs);
			return ret;
		}

		if (!c->hdr.used)
			gfs->free_clusters++;
	}

	*pgfs = gfs;

	return 0;
}

int ghostfs_sync(struct ghostfs *gfs)
{
	struct cluster *c;
	int ret, i;

	ret = cluster_get(gfs, 0, &c);
	if (ret < 0)
		return ret;

	ret = write_header(gfs, c);
	if (ret < 0)
		return ret;

	if (!gfs->clusters) // nothing more to sync
		return 0;

	for (i = 1; i < gfs->hdr.cluster_count; i++) {
		c = gfs->clusters[i];

		if (!c || !is_dirty(c))
			continue;

		ret = write_cluster(gfs, c, i);
		if (ret < 0)
			return ret;
	}

	return 0;
}

static void ghostfs_free(struct ghostfs *gfs)
{
	if (gfs->clusters) {
		int i;

		for (i = 0; i < gfs->hdr.cluster_count; i++)
			free(gfs->clusters[i]);

		free(gfs->clusters);
	}

	free(gfs);
}

int ghostfs_umount(struct ghostfs *gfs)
{
	int ret = ghostfs_sync(gfs);

        ghostfs_free(gfs);

        return ret;
}

int ghostfs_cluster_count(const struct ghostfs *gfs)
{
	return gfs->hdr.cluster_count;
}

const char *ghostfs_entry_name(const struct ghostfs_entry *entry)
{
	return entry->it.entry->filename;
}
