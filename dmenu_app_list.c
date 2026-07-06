/* See LICENSE file for copyright and license details. */
#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "util.h"

#define CACHE_MAX_AGE    3600
#define CACHE_DIR_NAME   "app-launcher"
#define CACHE_FILE_NAME  "desktop-apps.cache"
#define LAST_UPDATE_NAME "last-update"
#define SCAN_MAX_DEPTH   3
#define INIT_APP_CAP     512
#define LINE_BUF_SIZE    8192
#define MAX_SEEN_FILES   8192

struct DesktopApp {
	char *name;
	char *exec;
	char *path;
};

enum Mode { MODE_LIST, MODE_RUN, MODE_UPDATE, MODE_STATS, MODE_CLEAR };

static char cache_dir[PATH_MAX];
static char cache_file[PATH_MAX];
static char last_update_file[PATH_MAX];

static struct DesktopApp *apps = NULL;
static int app_count = 0;
static int app_capacity = 0;

static const char *seen_files[MAX_SEEN_FILES];
static int seen_count = 0;

static void
init_paths(void)
{
	const char *cachedir = getenv("XDG_CACHE_HOME");
	char buf[PATH_MAX];

	if (cachedir && cachedir[0])
		snprintf(buf, sizeof(buf), "%s/%s", cachedir, CACHE_DIR_NAME);
	else
		snprintf(buf, sizeof(buf), "%s/.cache/%s",
		         getenv("HOME") ? getenv("HOME") : "/tmp", CACHE_DIR_NAME);

	snprintf(cache_dir, sizeof(cache_dir), "%s", buf);
	snprintf(cache_file, sizeof(cache_file), "%s/%s", buf, CACHE_FILE_NAME);
	snprintf(last_update_file, sizeof(last_update_file), "%s/%s", buf, LAST_UPDATE_NAME);
}

static void
ensure_cache_dir(void)
{
	struct stat st;
	if (stat(cache_dir, &st) == 0)
		return;
	if (mkdir(cache_dir, 0755) < 0)
		die("mkdir %s:", cache_dir);
}

static int
is_seen(const char *basename)
{
	int i;
	for (i = 0; i < seen_count; i++) {
		if (strcmp(seen_files[i], basename) == 0)
			return 1;
	}
	return 0;
}

static void
mark_seen(const char *basename)
{
	if (seen_count < MAX_SEEN_FILES)
		seen_files[seen_count++] = strdup(basename);
}

static const char *
base_name(const char *path)
{
	const char *p = strrchr(path, '/');
	return p ? p + 1 : path;
}

/* Trim leading and trailing whitespace in-place */
static char *
trim(char *s)
{
	char *end;
	while (isspace((unsigned char)*s))
		s++;
	if (*s == '\0')
		return s;
	end = s + strlen(s) - 1;
	while (end > s && isspace((unsigned char)*end))
		end--;
	*(end + 1) = '\0';
	return s;
}

/* Strip XDG field codes from Exec string, in-place */
static void
sanitize_exec(char *s)
{
	char *src = s, *dst = s;
	while (*src) {
		if (*src == '%' && *(src + 1)) {
			char c = *(src + 1);
			if (c == '%') {
				*dst++ = '?';
				src += 2;
			} else if (isalpha((unsigned char)c)) {
				src += 2;
			} else {
				*dst++ = *src++;
			}
		} else if (*src == '\\' && *(src + 1) == 's') {
			*dst++ = ' ';
			src += 2;
		} else {
			*dst++ = *src++;
		}
	}
	*dst = '\0';
}

/* Strip surrounding quotes */
static void
strip_quotes(char *s)
{
	size_t len = strlen(s);
	if (len >= 2 && s[0] == '"' && s[len - 1] == '"') {
		memmove(s, s + 1, len - 2);
		s[len - 2] = '\0';
	}
}

static int
check_tryexec(const char *cmd)
{
	char full[PATH_MAX];
	const char *path_env, *dir;
	char *path_copy;

	if (strchr(cmd, '/'))
		return access(cmd, X_OK) == 0;

	path_env = getenv("PATH");
	if (!path_env)
		return 0;

	path_copy = strdup(path_env);
	if (!path_copy)
		return 0;

	dir = strtok(path_copy, ":");
	while (dir) {
		snprintf(full, sizeof(full), "%s/%s", dir, cmd);
		if (access(full, X_OK) == 0) {
			free(path_copy);
			return 1;
		}
		dir = strtok(NULL, ":");
	}
	free(path_copy);
	return 0;
}

static void
add_app(const char *name, const char *exec, const char *path)
{
	if (app_count >= app_capacity) {
		app_capacity = app_capacity ? app_capacity * 2 : INIT_APP_CAP;
		apps = realloc(apps, app_capacity * sizeof(struct DesktopApp));
		if (!apps)
			die("realloc:");
	}
	apps[app_count].name = strdup(name);
	apps[app_count].exec = strdup(exec);
	apps[app_count].path = strdup(path);
	if (!apps[app_count].name || !apps[app_count].exec || !apps[app_count].path)
		die("strdup:");
	app_count++;
}

static void
free_apps(void)
{
	int i;
	for (i = 0; i < app_count; i++) {
		free(apps[i].name);
		free(apps[i].exec);
		free(apps[i].path);
	}
	free(apps);
	apps = NULL;
	app_count = 0;
	app_capacity = 0;
}

static void
parse_desktop_file(const char *filepath)
{
	FILE *fp;
	char line[LINE_BUF_SIZE];
	char name[256] = "", exec_cmd[1024] = "";
	char tryexec[256] = "", hidden_str[32] = "", nodisplay_str[32] = "";
	char notshowin[1024] = "", onlyshowin[1024] = "";
	int in_desktop_entry = 0;
	const char *bn;

	/* Dedup by basename */
	bn = base_name(filepath);
	if (is_seen(bn))
		return;
	mark_seen(bn);

	fp = fopen(filepath, "r");
	if (!fp)
		return;

	while (fgets(line, sizeof(line), fp)) {
		char *p, *val;
		size_t len;

		/* Strip trailing newline */
		len = strlen(line);
		while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
			line[--len] = '\0';

		p = trim(line);

		if (p[0] == '\0' || p[0] == '#')
			continue;

		if (p[0] == '[') {
			in_desktop_entry = (strcmp(p, "[Desktop Entry]") == 0);
			continue;
		}

		if (!in_desktop_entry)
			continue;

		val = strchr(p, '=');
		if (!val)
			continue;
		*val++ = '\0';
		trim(p);

		if (strcmp(p, "Name") == 0 && name[0] == '\0')
			snprintf(name, sizeof(name), "%s", trim(val));
		else if (strcmp(p, "Exec") == 0 && exec_cmd[0] == '\0')
			snprintf(exec_cmd, sizeof(exec_cmd), "%s", val);
		else if (strcmp(p, "Hidden") == 0)
			snprintf(hidden_str, sizeof(hidden_str), "%s", trim(val));
		else if (strcmp(p, "NoDisplay") == 0)
			snprintf(nodisplay_str, sizeof(nodisplay_str), "%s", trim(val));
		else if (strcmp(p, "TryExec") == 0)
			snprintf(tryexec, sizeof(tryexec), "%s", trim(val));
		else if (strcmp(p, "NotShowIn") == 0)
			snprintf(notshowin, sizeof(notshowin), "%s", trim(val));
		else if (strcmp(p, "OnlyShowIn") == 0)
			snprintf(onlyshowin, sizeof(onlyshowin), "%s", trim(val));
	}
	fclose(fp);

	if (name[0] == '\0' || exec_cmd[0] == '\0')
		return;
	if (strcmp(hidden_str, "true") == 0 || strcmp(nodisplay_str, "true") == 0)
		return;

	/* Check TryExec */
	if (tryexec[0] && !check_tryexec(tryexec))
		return;

	/* Check OnlyShowIn / NotShowIn */
	if (onlyshowin[0] || notshowin[0]) {
		const char *desktop = getenv("XDG_CURRENT_DESKTOP");
		if (onlyshowin[0]) {
			if (!desktop || !strcasestr(onlyshowin, desktop))
				return;
		}
		if (notshowin[0]) {
			if (desktop && strcasestr(notshowin, desktop))
				return;
		}
	}

	sanitize_exec(exec_cmd);
	strip_quotes(exec_cmd);
	trim(exec_cmd);

	/* Strip & from name and replace <>| with _ */
	{
		char clean[256];
		char *d = clean;
		const char *s = name;
		while (*s) {
			if (*s == '&') {
				s++;
			} else if (*s == '<' || *s == '>' || *s == '|') {
				*d++ = '_';
				s++;
			} else {
				*d++ = *s++;
			}
		}
		*d = '\0';
		trim(clean);
		if (clean[0] == '\0')
			return;
		add_app(clean, exec_cmd, filepath);
	}
}

static void
scan_directory(const char *dirpath, int depth)
{
	DIR *dir;
	struct dirent *entry;
	char full[PATH_MAX];

	if (depth > SCAN_MAX_DEPTH)
		return;

	dir = opendir(dirpath);
	if (!dir)
		return;

	while ((entry = readdir(dir)) != NULL) {
		struct stat st;

		if (entry->d_name[0] == '.')
			continue;

		snprintf(full, sizeof(full), "%s/%s", dirpath, entry->d_name);

		if (stat(full, &st) < 0)
			continue;

		if (S_ISDIR(st.st_mode)) {
			scan_directory(full, depth + 1);
		} else if (S_ISREG(st.st_mode) || S_ISLNK(st.st_mode)) {
			const char *ext = strrchr(entry->d_name, '.');
			if (ext && strcmp(ext, ".desktop") == 0 && access(full, R_OK) == 0)
				parse_desktop_file(full);
		}
	}
	closedir(dir);
}

static int
app_cmp(const void *a, const void *b)
{
	return strcasecmp(((const struct DesktopApp *)a)->name,
	                  ((const struct DesktopApp *)b)->name);
}

static int
dedup_apps(void)
{
	int i, j = 0;
	if (app_count == 0)
		return 0;

	for (i = 1; i < app_count; i++) {
		if (strcasecmp(apps[i].name, apps[j].name) != 0) {
			j++;
			if (i != j) {
				apps[j] = apps[i];
			}
		} else {
			free(apps[i].name);
			free(apps[i].exec);
			free(apps[i].path);
		}
	}
	return j + 1;
}

static int
find_desktop_files(void)
{
	const char *xdg_dirs, *home;
	char dirs_buf[4096], *dir, *saveptr;
	char path[PATH_MAX];

	home = getenv("HOME");
	if (!home)
		home = "/tmp";

	/* XDG_DATA_DIRS */
	xdg_dirs = getenv("XDG_DATA_DIRS");
	if (!xdg_dirs || !xdg_dirs[0])
		xdg_dirs = "/usr/local/share:/usr/share";

	snprintf(dirs_buf, sizeof(dirs_buf), "%s", xdg_dirs);
	dir = strtok_r(dirs_buf, ":", &saveptr);
	while (dir) {
		snprintf(path, sizeof(path), "%s/applications", dir);
		scan_directory(path, 0);
		dir = strtok_r(NULL, ":", &saveptr);
	}

	/* User local */
	snprintf(path, sizeof(path), "%s/.local/share/applications", home);
	scan_directory(path, 0);

	/* Flatpak system */
	scan_directory("/var/lib/flatpak/exports/share/applications", 0);

	/* Flatpak user */
	snprintf(path, sizeof(path), "%s/.local/share/flatpak/exports/share/applications", home);
	scan_directory(path, 0);

	/* Snap */
	scan_directory("/var/lib/snapd/desktop/applications", 0);

	return app_count;
}

static int
is_cache_valid(void)
{
	struct stat st;
	FILE *fp;
	time_t last_update, now;

	if (stat(cache_file, &st) < 0 || st.st_size == 0)
		return 0;
	if (stat(last_update_file, &st) < 0)
		return 0;

	fp = fopen(last_update_file, "r");
	if (!fp)
		return 0;
	if (fscanf(fp, "%ld", &last_update) != 1) {
		fclose(fp);
		return 0;
	}
	fclose(fp);

	now = time(NULL);
	return (now - last_update) <= CACHE_MAX_AGE;
}

static void
write_cache(void)
{
	FILE *fp;
	char tmp_path[PATH_MAX];
	int i;
	time_t now;

	snprintf(tmp_path, sizeof(tmp_path), "%s.tmp.%d", cache_file, getpid());

	fp = fopen(tmp_path, "w");
	if (!fp)
		die("fopen %s:", tmp_path);

	for (i = 0; i < app_count; i++)
		fprintf(fp, "%s|%s|%s\n", apps[i].name, apps[i].exec, apps[i].path);

	fclose(fp);

	if (rename(tmp_path, cache_file) < 0)
		die("rename %s:", cache_file);

	now = time(NULL);
	fp = fopen(last_update_file, "w");
	if (!fp)
		die("fopen %s:", last_update_file);
	fprintf(fp, "%ld\n", (long)now);
	fclose(fp);
}

static void
reset_seen(void)
{
	int i;
	for (i = 0; i < seen_count; i++)
		free((void *)seen_files[i]);
	seen_count = 0;
}

static void
update_cache(void)
{
	free_apps();
	reset_seen();

	find_desktop_files();

	if (app_count > 1) {
		qsort(apps, app_count, sizeof(struct DesktopApp), app_cmp);
		app_count = dedup_apps();
	}

	if (app_count > 0) {
		write_cache();
		fprintf(stderr, "Cache updated: %d apps\n", app_count);
	} else if (stat(cache_file, &(struct stat){0}) < 0) {
		fprintf(stderr, "Error: no applications found\n");
	}
}

static int
read_cache(void)
{
	FILE *fp;
	char line[LINE_BUF_SIZE];

	fp = fopen(cache_file, "r");
	if (!fp)
		return -1;

	while (fgets(line, sizeof(line), fp)) {
		char *p, *name, *exec_cmd, *path;
		size_t len = strlen(line);

		while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
			line[--len] = '\0';

		p = line;
		name = p;
		p = strchr(p, '|');
		if (!p) continue;
		*p++ = '\0';

		exec_cmd = p;
		p = strchr(p, '|');
		if (!p) continue;
		*p++ = '\0';

		path = p;

		add_app(name, exec_cmd, path);
	}
	fclose(fp);
	return app_count;
}

static void
ensure_cache(void)
{
	ensure_cache_dir();
	if (!is_cache_valid())
		update_cache();
}

static void
list_apps(void)
{
	int i;
	ensure_cache();
	if (app_count == 0)
		read_cache();
	if (app_count == 0)
		update_cache();

	for (i = 0; i < app_count; i++)
		puts(apps[i].name);
}

static int
run_app(const char *app_name)
{
	int i;
	pid_t pid;
	int found_index = -1;

	if (!app_name || !app_name[0]) {
		fprintf(stderr, "Error: no app name specified\n");
		return 1;
	}

	ensure_cache();
	if (app_count == 0 && read_cache() <= 0) {
		update_cache();
	}

	/* Exact match first */
	for (i = 0; i < app_count; i++) {
		if (strcmp(apps[i].name, app_name) == 0) {
			found_index = i;
			break;
		}
	}

	/* Fallback to case-insensitive */
	if (found_index < 0) {
		for (i = 0; i < app_count; i++) {
			if (strcasecmp(apps[i].name, app_name) == 0) {
				found_index = i;
				break;
			}
		}
	}

	if (found_index < 0) {
		fprintf(stderr, "App '%s' not found\n", app_name);
		return 1;
	}

	{
		struct DesktopApp *app = &apps[found_index];
		const char *exec_cmd = app->exec;
		char flatpak_cmd[1024];

		fprintf(stderr, "Launching: %s\n", app->name);

		/* Flatpak special handling */
		if (strstr(app->path, "flatpak")) {
			const char *bn = base_name(app->path);
			snprintf(flatpak_cmd, sizeof(flatpak_cmd), "flatpak run %.*s",
			         (int)(strrchr(bn, '.') ? strrchr(bn, '.') - bn : (int)strlen(bn)),
			         bn);
			exec_cmd = flatpak_cmd;
		}

		pid = fork();
		if (pid < 0) {
			perror("fork");
			return 1;
		}

		if (pid == 0) {
			/* Child */
			int fd;
			setsid();
			fd = open("/dev/null", O_RDWR);
			if (fd >= 0) {
				dup2(fd, STDIN_FILENO);
				dup2(fd, STDOUT_FILENO);
				dup2(fd, STDERR_FILENO);
				if (fd > 2)
					close(fd);
			}
			execlp("sh", "sh", "-c", exec_cmd, (char *)NULL);
			_exit(127);
		}

		/* Parent: don't wait, app runs detached */
		{ struct timespec ts = { .tv_sec = 0, .tv_nsec = 100000000 };
		  nanosleep(&ts, NULL); }
		fprintf(stderr, "App launched\n");
	}

	return 0;
}

static void
show_stats(void)
{
	struct stat st;
	FILE *fp;
	time_t last_update = 0, now;
	int total = 0;

	if (stat(cache_file, &st) == 0) {
		fp = fopen(last_update_file, "r");
		if (fp) {
			fscanf(fp, "%ld", &last_update);
			fclose(fp);
		}

		fp = fopen(cache_file, "r");
		if (fp) {
			char buf[LINE_BUF_SIZE];
			while (fgets(buf, sizeof(buf), fp))
				total++;
			fclose(fp);
		}
	}

	fprintf(stderr, "Statistics:\n");
	fprintf(stderr, "  Apps: %d\n", total);
	fprintf(stderr, "  File: %s\n", cache_file);

	if (last_update > 0) {
		now = time(NULL);
		long age = (long)(now - last_update);
		if (age < 60)
			fprintf(stderr, "  Cache age: %ld sec\n", age);
		else if (age < 3600)
			fprintf(stderr, "  Cache age: %ld min\n", age / 60);
		else
			fprintf(stderr, "  Cache age: %ld h %ld min\n", age / 3600, (age % 3600) / 60);
	} else {
		fprintf(stderr, "  Cache age: none\n");
	}
}

static void
clear_cache(void)
{
	if (unlink(cache_file) < 0 && errno != ENOENT)
		fprintf(stderr, "Warning: could not remove %s\n", cache_file);
	if (unlink(last_update_file) < 0 && errno != ENOENT)
		fprintf(stderr, "Warning: could not remove %s\n", last_update_file);
	if (rmdir(cache_dir) < 0 && errno != ENOENT && errno != ENOTEMPTY)
		fprintf(stderr, "Warning: could not remove %s\n", cache_dir);
	free_apps();
	fprintf(stderr, "Cache cleared\n");
}

static void
usage(void)
{
	die("usage: dmenu_app_list [-l] [-r app] [-u] [-c] [-s] [-h]");
}

int
main(int argc, char *argv[])
{
	enum Mode mode = MODE_LIST;
	const char *app_name = NULL;
	int i;

	init_paths();

	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-l") == 0 || strcmp(argv[i], "--list") == 0) {
			mode = MODE_LIST;
		} else if (strcmp(argv[i], "-r") == 0 || strcmp(argv[i], "--run") == 0) {
			mode = MODE_RUN;
			if (++i < argc) {
				/* Concatenate remaining args as app name */
				static char name_buf[4096];
				int j, pos = 0;
				for (j = i; j < argc; j++) {
					if (j > i)
						pos += snprintf(name_buf + pos, sizeof(name_buf) - pos, " ");
					pos += snprintf(name_buf + pos, sizeof(name_buf) - pos, "%s", argv[j]);
					if (pos >= (int)sizeof(name_buf))
						break;
				}
				app_name = name_buf;
			}
			break;
		} else if (strcmp(argv[i], "-u") == 0 || strcmp(argv[i], "--update") == 0) {
			mode = MODE_UPDATE;
		} else if (strcmp(argv[i], "-c") == 0 || strcmp(argv[i], "--clear") == 0) {
			mode = MODE_CLEAR;
		} else if (strcmp(argv[i], "-s") == 0 || strcmp(argv[i], "--stats") == 0) {
			mode = MODE_STATS;
		} else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
			usage();
		} else if (argv[i][0] == '-') {
			usage();
		} else {
			/* Unknown arg: treat as app name to run */
			mode = MODE_RUN;
			app_name = argv[i];
		}
	}

	switch (mode) {
	case MODE_LIST:
		list_apps();
		break;
	case MODE_RUN:
		return run_app(app_name);
	case MODE_UPDATE:
		ensure_cache_dir();
		update_cache();
		break;
	case MODE_STATS:
		show_stats();
		break;
	case MODE_CLEAR:
		clear_cache();
		break;
	}

	free_apps();
	return 0;
}
