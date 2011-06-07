#define _ATFILE_SOURCE
#include "file.h"
#include "access_event.h"
#include "debug.h"
#include "linux/list.h"
#include "db.h"
#include "fileio.h"
#include "pel_group.h"
#include "config.h"
#include "entry.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>

struct file_entry {
	tupid_t dt;
	char *filename;
	struct pel_group pg;
	struct list_head list;
};

struct sym_entry {
	tupid_t dt;
	char *from;
	char *to;
	struct list_head list;
};

struct dfd_info {
	tupid_t dt;
	int dfd;
};

static struct file_entry *new_entry(const char *filename, tupid_t dt);
static void del_entry(struct file_entry *fent);
static int handle_symlink(const char *from, const char *to, tupid_t dt,
			  struct file_info *info);
static void check_unlink_list(const struct pel_group *pg, struct list_head *u_list);
static void handle_unlink(struct file_info *info);
static int update_write_info(tupid_t cmdid, const char *debug_name,
			     struct file_info *info, int *warnings,
			     struct list_head *entrylist);
static int update_read_info(tupid_t cmdid, struct file_info *info,
			    struct list_head *entrylist);

int init_file_info(struct file_info *info)
{
	INIT_LIST_HEAD(&info->read_list);
	INIT_LIST_HEAD(&info->write_list);
	INIT_LIST_HEAD(&info->unlink_list);
	INIT_LIST_HEAD(&info->var_list);
	INIT_LIST_HEAD(&info->sym_list);
	INIT_LIST_HEAD(&info->ghost_list);
	INIT_LIST_HEAD(&info->mapping_list);
	INIT_LIST_HEAD(&info->tmpdir_list);
	return 0;
}

int handle_file(enum access_type at, const char *filename, const char *file2,
		struct file_info *info, tupid_t dt)
{
	DEBUGP("received file '%s' in mode %i\n", filename, at);

	if(at == ACCESS_RENAME) {
		return handle_rename(filename, file2, info);
	}
	if(at == ACCESS_SYMLINK) {
		return handle_symlink(filename, file2, dt, info);
	}

	return handle_open_file(at, filename, info, dt);
}

int handle_open_file(enum access_type at, const char *filename,
		     struct file_info *info, tupid_t dt)
{
	struct file_entry *fent;
	int rc = 0;

	fent = new_entry(filename, dt);
	if(!fent) {
		return -1;
	}

	switch(at) {
		case ACCESS_READ:
			list_add(&fent->list, &info->read_list);
			break;
		case ACCESS_WRITE:
			check_unlink_list(&fent->pg, &info->unlink_list);
			list_add(&fent->list, &info->write_list);
			break;
		case ACCESS_UNLINK:
			list_add(&fent->list, &info->unlink_list);
			break;
		case ACCESS_VAR:
			list_add(&fent->list, &info->var_list);
			break;
		case ACCESS_GHOST:
			list_add(&fent->list, &info->ghost_list);
			break;
		default:
			fprintf(stderr, "Invalid event type: %i\n", at);
			rc = -1;
			break;
	}

	return rc;
}

int write_files(tupid_t cmdid, const char *debug_name, struct file_info *info,
		int *warnings)
{
	struct list_head *entrylist;
	struct tmpdir *tmpdir;
	int tmpdir_bork = 0;
	int rc1, rc2;

	handle_unlink(info);

	list_for_each_entry(tmpdir, &info->tmpdir_list, list) {
		fprintf(stderr, "tup error: Directory '%s' was created by command '%s', but not subsequently removed. Only temporary directories can be created by commands.\n", tmpdir->dirname, debug_name);
		tmpdir_bork = 1;
	}
	if(tmpdir_bork)
		return -1;

	entrylist = tup_entry_get_list();
	rc1 = update_write_info(cmdid, debug_name, info, warnings, entrylist);
	tup_entry_release_list();

	entrylist = tup_entry_get_list();
	rc2 = update_read_info(cmdid, info, entrylist);
	tup_entry_release_list();

	if(rc1 == 0 && rc2 == 0)
		return 0;
	return -1;
}

static int file_set_mtime(struct tup_entry *tent, int dfd, const char *file)
{
	struct stat buf;
	if(fstatat(dfd, file, &buf, AT_SYMLINK_NOFOLLOW) < 0) {
		fprintf(stderr, "tup error: file_set_mtime() fstatat failed.\n");
		perror(file);
		return -1;
	}
	if(tup_db_set_mtime(tent, buf.st_mtime) < 0)
		return -1;
	return 0;
}

static struct file_entry *new_entry(const char *filename, tupid_t dt)
{
	struct file_entry *fent;

	fent = malloc(sizeof *fent);
	if(!fent) {
		perror("malloc");
		return NULL;
	}

	fent->filename = strdup(filename);
	if(!fent->filename) {
		perror("strdup");
		free(fent);
		return NULL;
	}

	if(get_path_elements(fent->filename, &fent->pg) < 0) {
		free(fent->filename);
		free(fent);
		return NULL;
	}
	fent->dt = dt;
	return fent;
}

static void del_entry(struct file_entry *fent)
{
	list_del(&fent->list);
	del_pel_group(&fent->pg);
	free(fent->filename);
	free(fent);
}

int handle_rename(const char *from, const char *to, struct file_info *info)
{
	struct file_entry *fent;
	struct pel_group pg_from;
	struct pel_group pg_to;

	if(get_path_elements(from, &pg_from) < 0)
		return -1;
	if(get_path_elements(to, &pg_to) < 0)
		return -1;

	list_for_each_entry(fent, &info->write_list, list) {
		if(pg_eq(&fent->pg, &pg_from)) {
			del_pel_group(&fent->pg);
			free(fent->filename);

			fent->filename = strdup(to);
			if(!fent->filename) {
				perror("strdup");
				return -1;
			}
			if(get_path_elements(fent->filename, &fent->pg) < 0)
				return -1;
		}
	}
	list_for_each_entry(fent, &info->read_list, list) {
		if(pg_eq(&fent->pg, &pg_from)) {
			del_pel_group(&fent->pg);
			free(fent->filename);

			fent->filename = strdup(to);
			if(!fent->filename) {
				perror("strdup");
				return -1;
			}
			if(get_path_elements(fent->filename, &fent->pg) < 0)
				return -1;
		}
	}

	check_unlink_list(&pg_to, &info->unlink_list);
	del_pel_group(&pg_to);
	del_pel_group(&pg_from);
	return 0;
}

void del_map(struct mapping *map)
{
	list_del(&map->list);
	free(map->tmpname);
	free(map->realname);
	free(map);
}

static int handle_symlink(const char *from, const char *to, tupid_t dt,
			  struct file_info *info)
{
	struct sym_entry *sym;

	sym = malloc(sizeof *sym);
	if(!sym) {
		perror("malloc");
		return -1;
	}
	sym->from = strdup(from);
	if(!sym->from) {
		perror("strdup");
		free(sym);
		return -1;
	}
	sym->to = strdup(to);
	if(!sym->to) {
		perror("strdup");
		free(sym->from);
		free(sym);
		return -1;
	}
	sym->dt = dt;
	list_add(&sym->list, &info->sym_list);
	return 0;
}

static void check_unlink_list(const struct pel_group *pg, struct list_head *u_list)
{
	struct file_entry *fent, *tmp;

	list_for_each_entry_safe(fent, tmp, u_list, list) {
		if(pg_eq(&fent->pg, pg)) {
			del_entry(fent);
		}
	}
}

static void handle_unlink(struct file_info *info)
{
	struct file_entry *u, *fent, *tmp;

	while(!list_empty(&info->unlink_list)) {
		u = list_entry(info->unlink_list.next, struct file_entry, list);

		list_for_each_entry_safe(fent, tmp, &info->write_list, list) {
			if(pg_eq(&fent->pg, &u->pg)) {
				del_entry(fent);
			}
		}
		list_for_each_entry_safe(fent, tmp, &info->read_list, list) {
			if(pg_eq(&fent->pg, &u->pg)) {
				del_entry(fent);
			}
		}
		list_for_each_entry_safe(fent, tmp, &info->ghost_list, list) {
			if(pg_eq(&fent->pg, &u->pg)) {
				del_entry(fent);
			}
		}

		del_entry(u);
	}
}

static int update_write_info(tupid_t cmdid, const char *debug_name,
			     struct file_info *info, int *warnings,
			     struct list_head *entrylist)
{
	struct file_entry *w;
	struct file_entry *r;
	struct file_entry *g;
	struct file_entry *tmp;
	struct tup_entry *tent;
	int write_bork = 0;
	struct rb_root symtree = RB_ROOT;

	while(!list_empty(&info->write_list)) {
		tupid_t newdt;
		struct path_element *pel = NULL;

		w = list_entry(info->write_list.next, struct file_entry, list);
		if(w->dt < 0) {
			goto out_skip;
		}

		/* Remove duplicate write entries */
		list_for_each_entry_safe(r, tmp, &info->write_list, list) {
			if(r != w && pg_eq(&w->pg, &r->pg)) {
				del_entry(r);
			}
		}

		if(w->pg.pg_flags & PG_HIDDEN) {
			fprintf(stderr, "tup warning: Writing to hidden file '%s' from command '%s'\n", w->filename, debug_name);
			(*warnings)++;
			goto out_skip;
		}

		newdt = find_dir_tupid_dt_pg(w->dt, &w->pg, &pel, NULL, &symtree, 0);
		if(newdt <= 0) {
			fprintf(stderr, "tup error: File '%s' was written to, but is not in .tup/db. You probably should specify it as an output for the command '%s'\n", w->filename, debug_name);
			return -1;
		}
		if(!pel) {
			fprintf(stderr, "[31mtup internal error: find_dir_tupid_dt_pg() in write_files() didn't get a final pel pointer.[0m\n");
			return -1;
		}
		if(!RB_EMPTY_ROOT(&symtree)) {
			struct rb_node *rbn;
			tupid_t tupid;
			fprintf(stderr, "tup error: Attempt to write to a file using a symlink. The command should only  use the full non-symlinked path, or just write to the current directory.\n");
			fprintf(stderr, " -- Command: '%s'\n", debug_name);
			fprintf(stderr, " -- Filename: '%s'\n", w->filename);
			tupid_tree_for_each(tupid, rbn, &symtree) {
				tent = tup_entry_find(tupid);
				if(tent) {
					fprintf(stderr, " -- Symlink %lli -> %lli in dir %lli\n", tent->tnode.tupid, tent->sym, tent->dt);
				} else {
					fprintf(stderr, " -- Unknown symlink %lli\n", tupid);
				}
			}
			return -1;
		}

		if(tup_db_select_tent_part(newdt, pel->path, pel->len, &tent) < 0)
			return -1;
		/* Don't need to follow the syms of tent here, since the output
		 * file was removed by the updater. So our database
		 * representation may not match the filesystem, until we reset
		 * the sym field to -1 later.
		 */
		free(pel);
		if(!tent) {
			fprintf(stderr, "tup error: File '%s' was written to, but is not in .tup/db. You probably should specify it as an output for the command '%s'\n", w->filename, debug_name);
			write_bork = 1;
		} else {
			struct mapping *map;
			int dfd;
			tup_entry_list_add(tent, entrylist);

			/* Some files in Windows still set dt to not be
			 * DOT_DT, so we need to make sure we are in the
			 * right path for fstatat() to work. The fuse
			 * server always sets dt to DOT_DT, so we can just
			 * use the existing tup_top_fd() descriptor in that
			 * case.
			 */
			if(w->dt != DOT_DT) {
				dfd = tup_entry_open_tupid(w->dt);
			} else {
				dfd = tup_top_fd();
			}

			list_for_each_entry(map, &info->mapping_list, list) {
				if(strcmp(map->realname, w->filename) == 0) {
					if(file_set_mtime(tent, dfd, map->tmpname) < 0)
						return -1;
				}
			}
			if(w->dt != DOT_DT) {
				close(dfd);
			}
			if(tup_db_set_sym(tent, -1) < 0)
				return -1;
		}

out_skip:
		del_entry(w);
	}

	while(!list_empty(&info->sym_list)) {
		struct sym_entry *sym_entry;
		struct tup_entry *link_tent;
		struct mapping *map;
		tupid_t sym;

		sym_entry = list_entry(info->sym_list.next, struct sym_entry, list);
		if(sym_entry->dt < 0)
			goto skip_sym;

		tent = get_tent_dt(sym_entry->dt, sym_entry->to);
		if(!tent) {
			fprintf(stderr, "tup error: File '%s' was written as a symlink, but is not in .tup/db. You probably should specify it as an output for command '%s'\n", sym_entry->to, debug_name);
			write_bork = 1;
			goto skip_sym;
		}

		tup_entry_list_add(tent, entrylist);

		list_for_each_entry(map, &info->mapping_list, list) {
			int dfd;
			if(sym_entry->dt != DOT_DT) {
				dfd = tup_entry_open_tupid(sym_entry->dt);
			} else {
				dfd = tup_top_fd();
			}
			if(strcmp(map->realname, sym_entry->to) == 0) {
				if(file_set_mtime(tent, dfd, map->tmpname) < 0)
					return -1;
			}
			if(sym_entry->dt != DOT_DT) {
				close(dfd);
			}
		}

		list_for_each_entry_safe(g, tmp, &info->ghost_list, list) {
			/* Use strcmp instead of pg_eq because we don't have
			 * the pgs for sym_entries. Also this should only
			 * happen when 'ln' does a stat() before it does a
			 * symlink().
			 */
			if(strcmp(sym_entry->to, g->filename) == 0)
				del_entry(g);
		}

		/* Don't pass in entrylist for the list parameter - we don't
		 * actually need to track symlinks referenced by the path of
		 * the symlink file. These would get picked up by any command
		 * that reads our symlink.
		 */
		if(gimme_node_or_make_ghost(sym_entry->dt, sym_entry->from, &link_tent) < 0)
			return -1;
		if(link_tent) {
			sym = link_tent->tnode.tupid;
		} else {
			sym = -1;
		}

		if(tup_db_set_sym(tent, sym) < 0)
			return -1;

skip_sym:
		list_del(&sym_entry->list);
		free(sym_entry->from);
		free(sym_entry->to);
		free(sym_entry);
	}

	if(write_bork) {
		while(!list_empty(&info->mapping_list)) {
			struct mapping *map;

			map = list_entry(info->mapping_list.next, struct mapping, list);
			unlink(map->tmpname);
			del_map(map);
		}
		return -1;
	}

	if(tup_db_check_actual_outputs(cmdid, entrylist) < 0)
		return -1;

	while(!list_empty(&info->mapping_list)) {
		struct mapping *map;

		map = list_entry(info->mapping_list.next, struct mapping, list);

		/* TODO: strcmp only here for win32 support */
		if(strcmp(map->tmpname, map->realname) != 0) {
			if(renameat(tup_top_fd(), map->tmpname, tup_top_fd(), map->realname) < 0) {
				perror(map->realname);
				fprintf(stderr, "tup error: Unable to rename temporary file '%s' to destination '%s'\n", map->tmpname, map->realname);
				write_bork = 1;
			}
		}
		del_map(map);
	}

	if(write_bork)
		return -1;

	return 0;
}

static int update_read_info(tupid_t cmdid, struct file_info *info,
			    struct list_head *entrylist)
{
	struct file_entry *r;

	while(!list_empty(&info->read_list)) {
		r = list_entry(info->read_list.next, struct file_entry, list);
		if(r->dt > 0) {
			if(add_node_to_list(r->dt, &r->pg, entrylist, 0) < 0)
				return -1;
		}
		del_entry(r);
	}

	while(!list_empty(&info->var_list)) {
		r = list_entry(info->var_list.next, struct file_entry, list);

		if(add_node_to_list(VAR_DT, &r->pg, entrylist, 1) < 0)
			return -1;
		del_entry(r);
	}

	while(!list_empty(&info->ghost_list)) {
		r = list_entry(info->ghost_list.next, struct file_entry, list);

		if(r->dt > 0) {
			if(add_node_to_list(r->dt, &r->pg, entrylist, 1) < 0)
				return -1;
		}
		del_entry(r);
	}
	if(tup_db_check_actual_inputs(cmdid, entrylist) < 0)
		return -1;
	return 0;
}
