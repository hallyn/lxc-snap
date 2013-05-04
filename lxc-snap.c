#define _GNU_SOURCE
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <dirent.h>
#include "lxccontainer.h"

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


/*
 * lxc-create -t ubuntu -n s1
 * lxc-snap s1
 *      . creates /var/lib/lxcsnaps/s1_$n
 *      . adds 's1_n date' to /var/lib/lxcsnaps/data
 * 'lxc-snap -l' lists all containers which have snapshots
 * 'lxc-snap -l s1' shows all snapshots and timestamps for s1
 * 'lxc-snap -r s1_5 s1_peek' will snapshot s1 (if it exists) then clone s1_n to s1_peek
 *
 * Note that since c_n is overlayfs referencing /var/lib/lxc/c/rootfs, we can't just
 * restore c_n to /var/lib/lxc/c :)  Hence we require a new name
 *
 * You can always simply fire up a snapshot using
 *     sudo lxc-start -P /var/lib/lxcsnaps -n s1_$n
 */

void usage(char *me, int ret)
{
	printf("Usage: %s -l: list containers which have snapshots\n", me);
	printf("       %s -l c: list all snapshots of container c\n", me);
	printf("       %s c: create a new snapshot of container c\n", me);
	printf("       %s -r c_n c_tmp: restore container c_n to c_tmp\n", me);
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

int list_bases(char *path, char *cname)
{
	DIR *d = opendir(path);
	struct dirent *direntp;
	char *p;

	while ((direntp = readdir(d)) != NULL) {
		p = rindex(direntp->d_name, '_');
		if (!p)
			continue;
		p++;
		if (strncmp(direntp->d_name, cname, strlen(cname)) != 0)
			continue;
		printf("%s %s", cname, p);
		// Now print the timestamp (if properly configured)
		print_timestamp(path, direntp->d_name);
		printf("\n");
	}
	closedir(d);

	return EXIT_SUCCESS;
}

void list_containers(char *cname)
{
	const char *lxcpath;
	char *snappath;
	int ret;

	lxcpath = lxc_get_default_config_path();
	snappath = alloca(strlen(lxcpath) + strlen("snaps") + 1);
	sprintf(snappath, "%ssnaps", lxcpath);

	if (!cname)
		ret = list_unique_bases(snappath);
	else
		ret = list_bases(snappath, cname);

	exit(ret);
}

int get_next_index(char *cname, char *lxcpath)
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

int snapshot_container(char *cname)
{
	const char *lxcpath;
	char *snappath;
	lxcpath = lxc_get_default_config_path();
	snappath = alloca(strlen(lxcpath) + strlen("snaps") + 1);
	sprintf(snappath, "%ssnaps", lxcpath);
	int i = get_next_index(cname, snappath);
	char *newname;
	struct lxc_container *c, *c2;
	int flags;

	if (mkdir(snappath, 0755) < 0 && errno != EEXIST) {
		printf("Failed to create snapshot directory %s\n", snappath);
		return EXIT_FAILURE;
	}
	newname = alloca(strlen(cname) + 15);
	sprintf(newname, "%s_%d", cname, i);
	c = lxc_container_new(cname, NULL);
	if (!c)
		return EXIT_FAILURE;
	flags = LXC_CLONE_SNAPSHOT | LXC_CLONE_KEEPMACADDR | LXC_CLONE_KEEPNAME;
	c2 = c->clone(c, newname, snappath, flags, "overlayfs", NULL, 0);
	if (!c2) {
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
		printf("Failed to open %s\n", dfnam);
		return EXIT_FAILURE;
	}
	fprintf(f, "%s", buffer);
	fclose(f);

	return EXIT_SUCCESS;
}

void restore_container(char *cname, char *newname)
{
	// first try to snapshot the existing container
	// then destroy it (again if it exists)
	// finally restore the original
	char *orig, *p;
	const char *lxcpath;
	char *snappath;
	lxcpath = lxc_get_default_config_path();
	snappath = alloca(strlen(lxcpath) + strlen("snaps") + 1);
	struct lxc_container *c, *c2;
	int flags;

	sprintf(snappath, "%ssnaps", lxcpath);
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
		printf("Failure create new internal container object\n");
		exit(EXIT_FAILURE);
	} else if (!c->is_defined(c)) {
		printf("could not open snapshotted container %s\n", cname);
		exit(EXIT_FAILURE);
	}
	flags = LXC_CLONE_KEEPNAME | LXC_CLONE_KEEPMACADDR;
	c2 = c->clone(c, newname, lxcpath, flags, "dir", NULL, 0);
	if (!c2) {
		printf("Failed restoring the container %s from %s to %s\n", orig, cname, newname);
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
	char *cname = NULL;
	int ret;

	while ((opt = getopt(argc, argv, "l::r:h")) != -1) {
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
		case 'h': usage(argv[0], EXIT_SUCCESS);
		default: usage(argv[0], EXIT_FAILURE);
		}
	}

	if (!cname && optind < argc)
		cname = argv[optind];

	if (fn == LIST)
		list_containers(cname);

	if (optind >= argc)
		usage(argv[0], EXIT_FAILURE);

	if (fn == RESTORE)
		restore_container(cname, argv[optind]);
	ret = snapshot_container(argv[optind]);
	exit(ret);
}
