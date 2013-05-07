#define _GNU_SOURCE
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <dirent.h>
#include <sys/ioctl.h>
#include <sys/param.h>
#include <lxc/lxccontainer.h>

/*
 * (C) Copyright Canonical Ltd. 2012
 *
 * Authors:
 * Serge Hallyn <serge.hallyn@ubuntu.com>
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
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 */

void usage(char *me, int ret)
{
	printf("Usage: %s -l: list containers which have snapshots\n", me);
	printf("       %s -l c: list all snapshots of container c\n", me);
	printf("       %s c: create a new snapshot of container c\n", me);
	printf("       %s -r c_n c_tmp: restore container c_n to c_tmp\n", me);
	printf("       %s -P lxcpath: use given lxcpath\n", me);
	printf("          Snapshots will be placed in ${lxcpath}snaps\n");
	exit(ret);
}

int add_if_unique(char ***listp, char *s, int *szp, int *nump)
{
	int i, found=0, tmp;
	char **list = *listp;

	for (i = 0; i < *nump; i++) {
		tmp = strcmp(list[i], s);
		if (tmp == 0)
			found=1;
		if (tmp >= 0)
			break;
	}

	if (found)
		return 1;

	if (*nump + 1 == *szp) {
		*szp += 100;
		*listp = realloc(*listp, *szp * sizeof(char *));
		list = *listp;
	}

	for (tmp = *nump; tmp >= i; tmp--)
		list[tmp+1] = list[tmp];
	list[i] = strdup(s);
	if (!list[i])
		return 0;

	(*nump)++;

	return 1;
}

int list_unique_bases(char *path)
{
	DIR *d = opendir(path);
	struct dirent *direntp;
	char **names, *p;
	int i, size = 100, entries = 0;

	if (!d) {
		fprintf(stderr, "Failed to open %s\n", path);
		return EXIT_FAILURE;
	}
	names = malloc(size * sizeof(char *));
	if (!names) {
		fprintf(stderr, "Out of memory\n");
		closedir(d);
		return EXIT_FAILURE;
	}
	while ((direntp = readdir(d)) != NULL) {
		p = rindex(direntp->d_name, '_');
		if (!p)
			continue;
		*p = '\0';
		if (!add_if_unique(&names, direntp->d_name, &size, &entries))
			return EXIT_FAILURE;
	}
	closedir(d);

	for (i=0; i<entries; i++)
		printf("%s\n", names[i]);

	free(names);

	return EXIT_SUCCESS;
}

void print_timestamp(char *p, char *c)
{
	char *path, ts[100];
	int len;
	FILE *f;
	int i;

	memset(ts, 0, 100);
	/* $p + '/' + $c + '/ts' + '\0' */
	len = strlen(p) + strlen(c) + 5;
	path = alloca(len);
	sprintf(path, "%s/%s/ts", p, c);
	if ((f = fopen(path, "r")) == NULL)
		return;
	ts[99] = '\0';
	if (fread(ts, 1, 99, f) < 0) {
		fclose(f);
		return;
	}
	for (i=0; i<100; i++)
		if (ts[i] == '\n')
			ts[i] = '\0';
	printf("\t%s", ts);
	fclose(f);
}

void print_comment(char *p, char *c)
{
	char *path, *line = NULL;
	size_t len;
	FILE *f;
	int i;

	/* $p + '/' + $c + '/comment' + '\0' */
	len = strlen(p) + strlen(c) + 10;
	path = alloca(len);
	sprintf(path, "%s/%s/comment", p, c);
	if ((f = fopen(path, "r")) == NULL)
		return;

	printf("Commit comment:\n");
	while (getline(&line, &len, f) != -1) {
		printf(" %s", line);
	}

	if (line)
		free(line);
	fclose(f);
}

int list_bases(char *path, char *cname)
{
	struct dirent **nlist;
	int i, n;
	char *p;

	n = scandir(path, &nlist, NULL, alphasort);
	if (n < 0) {
		perror("scansort");
		return EXIT_FAILURE;
	}
	for (i=0; i<n; i++) {
		p = rindex(nlist[i]->d_name, '_');
		if (!p)
			goto next;
		p++;
		if (strncmp(nlist[i]->d_name, cname, strlen(cname)) != 0)
			goto next;
		printf("%s %s", cname, p);
		// Now print the timestamp (if properly configured)
		print_timestamp(path, nlist[i]->d_name);
		printf("\n");
		print_comment(path, nlist[i]->d_name);
next:
		free(nlist[i]);
	}
	free(nlist);

	return EXIT_SUCCESS;
}

void list_containers(const char *lxcpath, char *cname)
{
	char *snappath;
	int ret;

	snappath = alloca(strlen(lxcpath) + strlen("/snapshots") + 1);
	sprintf(snappath, "%s/snapshots", lxcpath);

	if (!cname)
		ret = list_unique_bases(snappath);
	else
		ret = list_bases(snappath, cname);

	exit(ret);
}

int get_next_index(const char *lxcpath, char *cname)
{
	char *fname;
	struct stat sb;
	int i = 0, ret;

	fname = alloca(strlen(lxcpath) + strlen(cname) + 20);
	while (1) {
		sprintf(fname, "%s/%s_%d", lxcpath, cname, i);
		ret = stat(fname, &sb);
		if (ret != 0)
			return i;
		i++;
	}
}

static bool file_exists(char *f)
{
	struct stat statbuf;

	return stat(f, &statbuf) == 0;
}

static int copy_file(char *old, char *new)
{
	int in, out;
	ssize_t len, ret;
	char buf[8096];
	struct stat sbuf;

	if (file_exists(new)) {
		fprintf(stderr, "copy destination %s exists", new);
		return EXIT_FAILURE;
	}
	ret = stat(old, &sbuf);
	if (ret < 0) {
		fprintf(stderr, "stat'ing %s", old);
		return EXIT_FAILURE;
	}

	in = open(old, O_RDONLY);
	if (in < 0) {
		fprintf(stderr, "opening original file %s", old);
		return EXIT_FAILURE;
	}
	out = open(new, O_CREAT | O_EXCL | O_WRONLY, 0644);
	if (out < 0) {
		fprintf(stderr, "opening new file %s", new);
		close(in);
		return EXIT_FAILURE;
	}

	while (1) {
		len = read(in, buf, 8096);
		if (len < 0) {
			fprintf(stderr, "reading old file %s", old);
			goto err;
		}
		if (len == 0)
			break;
		ret = write(out, buf, len);
		if (ret < len) {  // should we retry?
			fprintf(stderr, "write to new file %s was interrupted", new);
			goto err;
		}
	}
	close(in);
	close(out);

	return EXIT_SUCCESS;

err:
	close(in);
	close(out);
	return EXIT_FAILURE;
}

/* detect lvm */
static int is_lvm_dev(const char *path)
{
	char devp[MAXPATHLEN], buf[4];
	FILE *fout;
	int ret;
	struct stat statbuf;

	if (strncmp(path, "lvm:", 4) == 0)
		return 1; // take their word for it

	ret = stat(path, &statbuf);
	if (ret != 0)
		return 0;
	if (!S_ISBLK(statbuf.st_mode))
		return 0;

	ret = snprintf(devp, MAXPATHLEN, "/sys/dev/block/%d:%d/dm/uuid",
			major(statbuf.st_rdev), minor(statbuf.st_rdev));
	if (ret < 0 || ret >= MAXPATHLEN) {
		fprintf(stderr, "lvm uuid pathname too long");
		return 0;
	}
	fout = fopen(devp, "r");
	if (!fout)
		return 0;
	ret = fread(buf, 1, 4, fout);
	fclose(fout);
	if (ret != 4 || strncmp(buf, "LVM-", 4) != 0)
		return 0;
	return 1;
}

static int is_lvm(const char *lxcpath, char *cname)
{
	FILE *f;
	size_t len;
	int found = 0;
	char *path = alloca(strlen(lxcpath) + strlen(cname) + 9), *line = NULL, *p;

	sprintf(path, "%s/%s/config", lxcpath, cname);
	if ((f = fopen(path, "r")) == NULL)
		return 0;

	while (getline(&line, &len, f) != -1) {
		for (p=line; p && (*p == ' ' || *p == '\t'); p++) ;
		if (!p || strncmp(p, "lxc.rootfs", 10) != 0)
			continue;
		p += 10;
		while (*p && (*p == ' ' || *p == '\t' || *p == '=')) p++;
		found=1;
		break;
	}
	fclose(f);

	if (!found)
		return 0;

	return is_lvm_dev(p);
}

/* detect zfs */
static int is_zfs(const char *lxcpath, char *cname)
{
	FILE *f;
	char output[4096];
	int found=0;
	char *path = alloca(strlen(lxcpath) + strlen(cname) + 9);

	sprintf(path, "%s/%s/rootfs", lxcpath, cname);

	if ((f = popen("zfs list 2> /dev/null", "r")) == NULL) {
		fprintf(stderr, "popen failed");
		return 0;
	}
	while (fgets(output, 4096, f)) {
		if (strstr(output, path)) {
			found = 1;
			break;
		}
	}
	(void) pclose(f);

	return found;
}

/* Some defines needed to detect btrfs */
struct btrfs_ioctl_space_info {
	unsigned long long flags;
	unsigned long long total_bytes;
	unsigned long long used_bytes;
};

struct btrfs_ioctl_space_args {
	unsigned long long space_slots;
	unsigned long long total_spaces;
	struct btrfs_ioctl_space_info spaces[0];
};

#define BTRFS_IOCTL_MAGIC 0x94
#define BTRFS_IOC_SUBVOL_GETFLAGS _IOR(BTRFS_IOCTL_MAGIC, 25, unsigned long long)
#define BTRFS_IOC_SPACE_INFO _IOWR(BTRFS_IOCTL_MAGIC, 20, \
                                    struct btrfs_ioctl_space_args)

int is_btrfs(const char *lxcpath, const char *cname)
{
	struct stat st;
	int fd, ret;
	struct btrfs_ioctl_space_args sargs;
	char *path = alloca(strlen(lxcpath) + strlen(cname) + 9);

	sprintf(path, "%s/%s/rootfs", lxcpath, cname);

	// make sure this is a btrfs filesystem
	fd = open(path, O_RDONLY);
	if (fd < 0)
		return 0;
	sargs.space_slots = 0;
	sargs.total_spaces = 0;
	ret = ioctl(fd, BTRFS_IOC_SPACE_INFO, &sargs);
	close(fd);
	if (ret < 0)
		return 0;

	// and make sure it's a subvolume.
	ret = stat(path, &st);
	if (ret < 0)
		return 0;

	if (st.st_ino == 256 && S_ISDIR(st.st_mode))
		return 1;

	return 0;
}

/*
 * Make sure the container to be snapshotted is backed by overlayfs.
 * Otherwise it's not safe to snapshot it.  See README.
 */
int is_overlayfs(const char *lxcpath, const char *cname)
{
	/* $lxcpath + "/" +  $cname + "/config" + '\0' */
	char *path, *line = NULL, *p;
	FILE *f;
	size_t len;
	int ret = 0;

	path = alloca(strlen(cname) + strlen(lxcpath) + 9);
	sprintf(path, "%s/%s/config", lxcpath, cname);

	if ((f = fopen(path, "r")) == NULL) {
		fprintf(stderr, "Failed to open %s\n", path);
		return 0;
	}

	while (getline(&line, &len, f) != -1) {
		for (p=line; p && (*p == ' ' || *p == '\t'); p++) ;
		if (!p || strncmp(p, "lxc.rootfs", 10) != 0)
			continue;
		if (strncmp(p, "lxc.rootfs.mount", 16) == 0)
			continue;
		if (strstr(p, "overlayfs") != NULL)
			ret = 1;
		else
			ret = 0;
		break;
	}

	if (line)
		free(line);
	fclose(f);
	return ret;
}

int snapshot_container(const char *lxcpath, char *cname, char *commentfile)
{
	char *snappath, *newname;
	int i, flags;
	struct lxc_container *c, *c2;

	snappath = alloca(strlen(lxcpath) + strlen("/snapshots") + 1);
	sprintf(snappath, "%s/snapshots", lxcpath);
	i = get_next_index(snappath, cname);

	if (mkdir(snappath, 0755) < 0 && errno != EEXIST) {
		fprintf(stderr, "Failed to create snapshot directory %s\n", snappath);
		return EXIT_FAILURE;
	}

	if (!is_overlayfs(lxcpath, cname) && !is_btrfs(lxcpath, cname) && !is_zfs(lxcpath, cname)
			&& !is_lvm(lxcpath, cname)) {
		fprintf(stderr, "%s is not snapshottable.", cname);
		fprintf(stderr, "Only overlayfs, zfs and btrfs are supported\n");
		return EXIT_FAILURE;
	}

	newname = alloca(strlen(cname) + 15);
	sprintf(newname, "%s_%d", cname, i);
	c = lxc_container_new(cname, lxcpath);
	if (!c) {
		fprintf(stderr, "Error opening %s:%s\n", lxcpath, cname);
		return EXIT_FAILURE;
	}
	flags = LXC_CLONE_SNAPSHOT | LXC_CLONE_KEEPMACADDR | LXC_CLONE_KEEPNAME;
	c2 = c->clone(c, newname, snappath, flags, NULL, NULL, 0);
	if (!c2) {
		fprintf(stderr, "clone of %s:%s failed\n", lxcpath, cname);
		lxc_container_put(c);
		return EXIT_FAILURE;
	}

	lxc_container_put(c2);
	lxc_container_put(c);

	// Now write down the creation time
	time_t timer;
	char buffer[25];
	struct tm* tm_info;

	time(&timer);
	tm_info = localtime(&timer);

	strftime(buffer, 25, "%Y:%m:%d %H:%M:%S", tm_info);

	char *dfnam = alloca(strlen(snappath) + strlen(newname) + 5);
	sprintf(dfnam, "%s/%s/ts", snappath, newname);
	FILE *f = fopen(dfnam, "w");
	if (!f) {
		fprintf(stderr, "Failed to open %s\n", dfnam);
		return EXIT_FAILURE;
	}
	fprintf(f, "%s", buffer);
	fclose(f);

	if (commentfile) {
		// $p / $name / comment \0
		int len = strlen(snappath) + strlen(newname) + 10;
		char *path = alloca(len);
		sprintf(path, "%s/%s/comment", snappath, newname);
		return copy_file(commentfile, path);
	}

	return EXIT_SUCCESS;
}

void restore_container(const char *lxcpath, char *cname, char *newname)
{
	char *orig, *p, *snappath;
	struct lxc_container *c, *c2;
	int flags;

	snappath = alloca(strlen(lxcpath) + strlen("/snapshots") + 1);
	sprintf(snappath, "%s/snapshots", lxcpath);
	orig = strdupa(cname);
	for (p = orig + strlen(orig) - 1; p >= orig; p--) {
		if (*p == '_') {
			*p = '\0';
			break;
		}
	}
	if (p < orig)
		exit(EXIT_FAILURE);

	c = lxc_container_new(cname, snappath);
	if (!c) {
		fprintf(stderr, "Failure create internal container object\n");
		exit(EXIT_FAILURE);
	} else if (!c->is_defined(c)) {
		fprintf(stderr, "could not open snapshotted container %s\n", cname);
		exit(EXIT_FAILURE);
	}
	flags = LXC_CLONE_KEEPMACADDR | LXC_CLONE_KEEPNAME;
	if (!is_lvm(snappath, cname))
		flags |= LXC_CLONE_SNAPSHOT;

	c2 = c->clone(c, newname, lxcpath, flags, NULL, NULL, 0);
	if (!c2) {
		fprintf(stderr, "Failed restoring the container %s from %s to %s\n", orig, cname, newname);
		lxc_container_put(c);
		exit(EXIT_FAILURE);
	}
	printf("Restored container %s from %s to %s\n", orig, cname, newname);
	lxc_container_put(c2);
	lxc_container_put(c);

	exit(EXIT_SUCCESS);
}


#define SNAP 0
#define LIST 1
#define RESTORE 2

int main(int argc, char *argv[])
{
	int opt;
	int fn = SNAP;
	char *cname = NULL, *commentfile = NULL;
	int ret;
	const char *lxcpath = lxc_get_default_config_path();

	while ((opt = getopt(argc, argv, "l::P:r:hc:")) != -1) {
		switch (opt) {
		case 'l':
			fn = LIST;
			if (optarg)
				cname = optarg;
			break;
		case 'r':
			fn = RESTORE;
			cname = optarg;
			break;
		case 'P':
			lxcpath = optarg;
			break;
		case 'c':
			commentfile = optarg;
			break;
		case 'h': usage(argv[0], EXIT_SUCCESS);
		default: usage(argv[0], EXIT_FAILURE);
		}
	}

	if (!cname && optind < argc)
		cname = argv[optind];

	if (fn == LIST)
		list_containers(lxcpath, cname);

	if (optind >= argc)
		usage(argv[0], EXIT_FAILURE);

	if (fn == RESTORE)
		restore_container(lxcpath, cname, argv[optind]);
	ret = snapshot_container(lxcpath, argv[optind], commentfile);
	exit(ret);
}
