/**
 * pacdep - Copyright (C) 2012 Olivier Brunel
 *
 * main.c
 * Copyright (C) 2012 Olivier Brunel <i.am.jack.mail@gmail.com>
 *
 * This file is part of pacdep.
 *
 * pacdep is free software: you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version.
 *
 * pacdep is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * pacdep. If not, see http://www.gnu.org/licenses/
 */

#include "config.h"

#include <stdlib.h>
#include <stdio.h>
#include <getopt.h>
#include <string.h>
#include <glob.h>
#include <ctype.h>
#include <stdbool.h>

#include <alpm_list.h>
#include <alpm.h>

#define BUF_LEN                 255

/* pacman default values */
#define PACMAN_CONFFILE         "/etc/pacman.conf"
#define PACMAN_ROOTDIR          "/"
#define PACMAN_DBPATH           "/var/lib/pacman/"

#if defined(GIT_VERSION)
#undef PACKAGE_VERSION
#define PACKAGE_VERSION GIT_VERSION
#endif
#define PACKAGE_TAG             "Package Dependencies listing"

enum {
    E_OK = 0,
    E_NOMEM,
    E_FILEREAD,
    E_PARSING,
    E_ALPM,
    E_NOTHING,
};

/* config data loaded from parsing pacman.conf */
typedef struct _pacman_config_t {
    /* from pacman.conf */
    char            *rootdir;
    char            *dbpath;
    /* dbs/repos */
    alpm_list_t     *databases;
} pacman_config_t;

typedef enum {
    DEP_UNKNOWN = 0,
    DEP_EXCLUSIVE,
    DEP_EXCLUSIVE_EXPLICIT, /* xxx_EXPLICIT *must* be xxx + 1 */
    DEP_SHARED,
    DEP_SHARED_EXPLICIT,
    DEP_OPTIONAL,
    DEP_OPTIONAL_EXPLICIT,
    NB_DEPS
} dep_t;

typedef struct _pkg_t {
    const char  *name_asked;    /* from cmdline */
    const char  *name;          /* can be a provider */
    const char  *repo;
    unsigned int is_provided : 1;
    unsigned int need_free : 1;
    alpm_pkg_t  *pkg;
    alpm_list_t *deps;
    dep_t        dep;
} pkg_t;

typedef struct _group_t {
    const char  *title;
    off_t        size;
    off_t        size_local;
    alpm_list_t *pkgs;
    int          len_max;
} group_t;

typedef enum {
    SCE_UNKNOWN = 0,
    SCE_LOCAL,
    SCE_SYNC,
    SCE_MIXED
} source_t;

typedef struct _data_t {
    alpm_list_t *pkgs;
    source_t     source;
    group_t      group[NB_DEPS];
    alpm_list_t *deps;
} data_t;

typedef struct _config_t {
    alpm_handle_t   *alpm;
    alpm_list_t     *localdb;
    alpm_list_t     *syncdbs;

    unsigned int     is_debug : 1;
    unsigned int     from_sync : 1;
    unsigned int     quiet : 1;
    unsigned int     raw_sizes : 1;
    unsigned int     sort_size : 1;
    unsigned int     show_optional : 2;
    unsigned int     explicit : 1;
    unsigned int     reverse : 2;
    unsigned int     list_requiredby : 1;
    unsigned int     list_exclusive : 1;
    unsigned int     list_exclusive_explicit : 1;
    unsigned int     list_shared : 1;
    unsigned int     list_shared_explicit : 1;
    unsigned int     list_optional : 1;
    unsigned int     list_optional_explicit : 1;
} config_t;

static config_t config;

static void
set_pkg_dep (data_t *data, alpm_list_t *refs, pkg_t *pkg, dep_t dep);

#define print_size(size)    do {         \
    if (config.raw_sizes)                \
    {                                    \
        fprintf (stdout, "%ld", size);   \
    }                                    \
    else                                 \
    {                                    \
        _print_size (size);              \
    }                                    \
} while (0)

#define FOR_LIST(i, val)    for (i = val; i; i = i->next)


static void
debug (const char *fmt, ...)
{
    va_list args;

    if (!config.is_debug)
    {
        return;
    }

    va_start (args, fmt);
    vfprintf (stdout, fmt, args);
    va_end (args);
}

static char *
strdup (const char *str)
{
    char    *s;
    size_t   len;

    if (!str)
    {
        return NULL;
    }

    len = strlen (str);
    s = malloc (sizeof (*s) * (len + 1));
    if (!s)
    {
        return NULL;
    }
    while (*str != '\0')
    {
        *s++ = *str++;
    }
    *s = '\0';
    s -= len;
    return s;
}

static int
set_error (char **msg, const char *fmt, ...)
{
    size_t   len;
    va_list  args;

    *msg = malloc (sizeof (**msg) * BUF_LEN);
    if (!*msg)
    {
        return E_NOMEM;
    }
    va_start (args, fmt);
    len = (size_t) vsnprintf (*msg, BUF_LEN, fmt, args);
    va_end (args);
    if (len >= BUF_LEN)
    {
        *msg = realloc (*msg, (sizeof (**msg) * ++len));
        if (!*msg)
        {
            return E_NOMEM;
        }
        va_start (args, fmt);
        vsnprintf (*msg, (size_t) len, fmt, args);
        va_end (args);
    }
    return E_OK;
}


static void
show_version (void)
{
    puts (PACKAGE_NAME " - " PACKAGE_TAG " v" PACKAGE_VERSION);
    puts ("Copyright (C) 2012 Olivier Brunel");
    puts ("License GPLv3+: GNU GPL version 3 or later <http://gnu.org/licenses/gpl.html>");
    puts ("This is free software: you are free to change and redistribute it.");
    puts ("There is NO WARRANTY, to the extent permitted by law.");
    exit (0);
}

static void
show_help (const char *prgname)
{
    puts ("Usage:");
    printf (" %s [OPTION..] PACKAGE..\n", prgname);
    putchar ('\n');
    puts (" -h, --help                      Show this help screen and exit");
    puts (" -V, --version                   Show version information and exit");
    putchar ('\n');
    puts (" -d, --debug                     Flood debug info to stdout");
    puts (" -c, --config=FILE               pacman.conf file to use (else /etc/pacman.conf)");
    puts (" -d, --dbpath=PATH               Specify an alternate database location");
    puts ("     --from-sync                 Only look for specified package(s) in sync dbs");
    puts (" -q, --quiet                     Only output packages name & size");
    puts (" -w, --raw-sizes                 Show sizes in bytes (no formatting)");
    puts (" -z, --sort-size                 Sort packages by size (else by name)");
    puts (" -p, --show-optional             Show optional dependencies (see man page)");
    puts (" -x, --explicit                  Don't ignore explicitly installed dependencies");
    putchar ('\n');
    puts (" -r, --reverse                   Enable reverse mode (see man page)");
    puts (" -R, --list-requiredby           List packages requiring the specified package(s)");
    putchar ('\n');
    puts (" -e, --list-exclusive            List exclusive dependencies");
    puts (" -E, --list-exclusive-explicit   List exclusive explicit dependencies");
    puts (" -s, --list-shared               List shared dependencies");
    puts (" -S, --list-shared-explicit      List shared explicit dependencies");
    puts (" -o, --list-optional             List optional dependencies");
    puts (" -O, --list-optional-explicit    List optional explicit dependencies");
    exit (0);
}

static void
free_pacman_config (pacman_config_t *pac_conf)
{
    if (pac_conf == NULL)
    {
        return;
    }

    /* alpm */
    free (pac_conf->rootdir);
    free (pac_conf->dbpath);
    /* dbs/repos */
    FREELIST (pac_conf->databases);

    free (pac_conf);
}

static size_t
strtrim (char *str)
{
    char *s, *ss, *e;
    size_t len;

    /* nothing to do */
    if (!str || *str == '\0')
    {
        return 0;
    }

    for (s = str; isspace (*s); ++s)
        ;
    for (ss = s, e = s; *ss != '\0'; ++ss)
    {
        if (!isspace (*ss))
        {
            e = ss;
        }
    }
    len = (size_t) (e - s + 1);
    /* nothing to trim */
    if (s == str && e == ss - 1)
    {
        return len;
    }
    /* string is all space */
    else if (s == ss)
    {
        *str = '\0';
        return 0;
    }
    /* space on the left: we move the string */
    if (s > str)
    {
        while (s <= e)
        {
            *str++ = *s++;
        }
    }
    /* space on the right only: just place a new \0 */
    else
    {
        str = e + 1;
    }
    *str = '\0';
    return len;
}

/** inspired from pacman's function */
static int
parse_pacman_conf (const char        *file,
                   char              *name,
                   bool               is_options,
                   int                depth,
                   pacman_config_t  **pacconf,
                   char             **error)
{
    FILE       *fp              = NULL;
    char        line[BUF_LEN];
    int         linenum         = 0;
    int         rc              = 0;
    const int   max_depth       = 10;

    *error = NULL;

    /* if struct is not init yet, we do it */
    if (*pacconf == NULL)
    {
        *pacconf = calloc (1, sizeof (**pacconf));
        if (*pacconf == NULL)
        {
            rc = E_NOMEM;
            goto cleanup;
        }
    }
    pacman_config_t *pac_conf = *pacconf;
    /* the db/repo we're currently parsing, if any */
    static char *cur_db = NULL;

    debug ("config: attempting to read file %s\n", file);
    fp = fopen (file, "r");
    if (fp == NULL)
    {
        set_error (error, "Config file %s could not be read\n", file);
        rc = E_FILEREAD;
        goto cleanup;
    }

    while (fgets (line, BUF_LEN, fp))
    {
        char *key, *value, *ptr;
        size_t line_len;

        ++linenum;
        line_len = strtrim (line);

        /* ignore whole line and end of line comments */
        if (line_len == 0 || line[0] == '#')
        {
            continue;
        }
        if ((ptr = strchr (line, '#')))
        {
            *ptr = '\0';
        }

        if (line[0] == '[' && line[line_len - 1] == ']')
        {
            /* only possibility here is a line == '[]' */
            if (line_len <= 2)
            {
                set_error (error, "Invalid section name\n");
                rc = E_PARSING;
                goto cleanup;
            }
            if (name != NULL)
            {
                free (name);
            }
            /* new config section, skip the '[' */
            name = strdup (line + 1);
            name[line_len - 2] = '\0';
            debug ("config: new section '%s'\n", name);
            is_options = (strcmp (name, "options") == 0);
            /* parsed a db/repo? if so we add it */
            if (cur_db != NULL)
            {
                pac_conf->databases = alpm_list_add (pac_conf->databases, cur_db);
            }
            cur_db = strdup (name);
            continue;
        }

        key = line;
        value = strchr (line, '=');
        if (value)
        {
            *value++ = '\0';
        }
        strtrim (key);
        strtrim (value);

        if (key == NULL)
        {
            set_error (error, "syntax error: missing key\n");
            rc = E_PARSING;
            goto cleanup;
        }
        /* For each directive, compare to the camelcase string. */
        if (name == NULL)
        {
            set_error (error, "All directives must belong to a section\n");
            rc= E_PARSING;
            goto cleanup;
        }
        /* Include is allowed in both options and repo sections */
        if (strcmp(key, "Include") == 0)
        {
            glob_t globbuf;
            int globret;
            size_t gindex;

            if (depth + 1 >= max_depth)
            {
                set_error (error,
                        "Parsing exceeded max recursion depth of %d\n",
                        max_depth);
                rc = E_PARSING;
                goto cleanup;
            }

            if (value == NULL)
            {
                set_error (error, "Directive %s needs a value\n", key);
                rc = E_PARSING;
                goto cleanup;
            }

            /* Ignore include failures... assume non-critical */
            globret = glob (value, GLOB_NOCHECK, NULL, &globbuf);
            switch (globret)
            {
                case GLOB_NOSPACE:
                    debug ("config file %s, line %d: include globbing out of space\n",
                            file, linenum);
                break;
                case GLOB_ABORTED:
                    debug ("config file %s, line %d: include globbing read error for %s\n",
                            file, linenum, value);
                break;
                case GLOB_NOMATCH:
                    debug ("config file %s, line %d: no include found for %s\n",
                            file, linenum, value);
                break;
                default:
                    for (gindex = 0; gindex < globbuf.gl_pathc; gindex++)
                    {
                        debug ("config file %s, line %d: including %s\n",
                               file, linenum, globbuf.gl_pathv[gindex]);
                        parse_pacman_conf (globbuf.gl_pathv[gindex], name,
                            is_options, depth + 1, &pac_conf, error);
                    }
                break;
            }
            globfree (&globbuf);
            continue;
        }
        /* we are either in options ... */
        if (is_options)
        {
            /* we only parse what we care about, ignoring everything else */
            if (value != NULL)
            {
                if (strcmp (key, "DBPath") == 0)
                {
                    pac_conf->dbpath = strdup (value);
                    debug ("config: dbpath: %s\n", value);
                }
                else if (strcmp (key, "RootDir") == 0)
                {
                    pac_conf->rootdir = strdup (value);
                    debug ("config: rootdir: %s\n", value);
                }
            }
        }
    }

    if (depth == 0)
    {
        /* parsed a db/repo? if so we add it */
        if (cur_db != NULL)
        {
            pac_conf->databases = alpm_list_add (pac_conf->databases, cur_db);
            cur_db = NULL;
        }
        /* set some default values for undefined options */
        if (NULL == pac_conf->rootdir)
        {
            pac_conf->rootdir = strdup (PACMAN_ROOTDIR);
        }
        if (NULL == pac_conf->dbpath)
        {
            pac_conf->dbpath = strdup (PACMAN_DBPATH);
        }
    }

cleanup:
    if (fp)
    {
        fclose (fp);
    }
    if (depth == 0)
    {
        /* section name is for internal processing only */
        if (name != NULL)
        {
            free (name);
            name = NULL;
        }
    }
    debug ("config: finished parsing %s\n", file);
    return rc;
}


static int
alpm_load (alpm_handle_t **handle,
        const char *conffile,
        const char *dbpath,
        char **error)
{
    int                  rc = E_OK;
    enum _alpm_errno_t   err;
    pacman_config_t     *pac_conf = NULL;
    alpm_list_t         *i;

    /* parse pacman.conf */
    debug ("parsing pacman.conf (%s) for options\n", conffile);
    rc = parse_pacman_conf (conffile, NULL, 0, 0, &pac_conf, error);
    if (rc != E_OK)
    {
        free_pacman_config (pac_conf);
        return rc;
    }

    /* --dbpath override */
    if (dbpath)
    {
        pac_conf->dbpath = strdup (dbpath);
        debug ("cmdline: dbpath: %s\n", dbpath);
    }

    /* init libalpm */
    debug ("setting up libalpm\n");
    *handle = alpm_initialize (pac_conf->rootdir,
            pac_conf->dbpath,
            &err);
    if (*handle == NULL)
    {
        set_error (error, "Failed to initialize alpm library: %s\n",
                alpm_strerror (err));
        free_pacman_config (pac_conf);
        return E_ALPM;
    }

    /* now we need to add dbs */
    FOR_LIST (i, pac_conf->databases)
    {
        char *db_name = i->data;
        alpm_db_t *db;

        /* register db */
        debug ("register %s\n", db_name);
        db = alpm_register_syncdb (*handle, db_name, ALPM_SIG_USE_DEFAULT);
        if (db == NULL)
        {
            set_error (error,
                    "Could not register database %s: %s\n",
                    db_name,
                    alpm_strerror (alpm_errno (*handle)));
            free_pacman_config (pac_conf);
            alpm_release (*handle);
            *handle = NULL;
            return E_ALPM;
        }
    }

    free_pacman_config (pac_conf);
    return E_OK;
}

static void
_print_size (off_t size)
{
    const char *units[]  = { "B", "KiB", "MiB", "GiB" };
    int         nb_units = (int) (sizeof (units) / sizeof (units[0]));

    double hsize;
    int    unit;
    const char *fmt;

    hsize = (double) size;
    unit = 1;
    while (hsize > 1024.0 && unit < nb_units)
    {
        ++unit;
        hsize /= 1024.0;
    }
    if (unit == 1)
    {
        fmt = (config.quiet) ? "%.0f %s" : "%6.0f %s";
    }
    else
    {
        fmt = (config.quiet) ? "%.2f %s" : "%6.2f %s";
    }
    fprintf (stdout, fmt, hsize, units[unit - 1]);
}

static int
pkg_find_name_fn (pkg_t *pkg, const char *name)
{
    return strcmp (pkg->name, name);
}

static int
pkg_find_pkg_fn (pkg_t *pkg, pkg_t *p)
{
    return strcmp (pkg->name, p->name);
}

static int
pkg_find_fn (pkg_t *pkg, alpm_pkg_t *p)
{
    return strcmp (pkg->name, alpm_pkg_get_name (p));
}

static inline pkg_t *
new_package (data_t *data, alpm_pkg_t *pkg)
{
    pkg_t *p;

    p = calloc (1, sizeof (*p));
    p->name = alpm_pkg_get_name (pkg);
    p->pkg  = pkg;
    p->dep  = DEP_UNKNOWN;
    if (alpm_pkg_get_origin (pkg) == ALPM_PKG_FROM_SYNCDB)
    {
        p->repo = alpm_db_get_name (alpm_pkg_get_db (pkg));
    }

    /* add it right now, so it's found when adding its own dep */
    debug ("adding %s to deps\n", p->name);
    data->deps = alpm_list_add (data->deps, p);

    return p;
}

static pkg_t *
add_to_deps (data_t *data, alpm_pkg_t *pkg)
{
    pkg_t       *p;
    alpm_list_t *i;

    /* if package is already in there, no need to do anything */
    p = alpm_list_find (data->deps, pkg, (alpm_list_fn_cmp) pkg_find_fn);
    if (p)
    {
        debug ("%s already in deps\n", alpm_pkg_get_name (pkg));
        return p;
    }

    p = new_package (data, pkg);

    /* go through dep tree to list all dependencies involved */
    FOR_LIST (i, alpm_pkg_get_depends (pkg))
    {
        char        *n = alpm_dep_compute_string (i->data);
        alpm_pkg_t  *dep;
        pkg_t       *d;

        debug ("[%s] look for satisfier of %s\n", p->name, n);
        dep = alpm_find_dbs_satisfier (config.alpm, config.localdb, n);
        if (!dep)
        {
            dep = alpm_find_dbs_satisfier (config.alpm, config.syncdbs, n);
        }
        if (!dep)
        {
            fprintf (stderr, "Error: no package found for dependency %s\n",
                    n);
            free (n);
            continue;
        }
        free (n);

        if (!config.explicit
                && alpm_pkg_get_origin (dep) == ALPM_PKG_FROM_LOCALDB
                && alpm_pkg_get_reason (dep) == ALPM_PKG_REASON_EXPLICIT)
        {
            debug ("ignoring dependency %s, explicitly installed\n",
                    alpm_pkg_get_name (dep));
            continue;
        }

        debug ("add to deps: %s\n", alpm_pkg_get_name (dep));
        d = add_to_deps (data, dep);
        if (d)
        {
            debug ("%s new in deps, adding to %s's dependencies\n",
                    d->name, p->name);
            p->deps = alpm_list_add (p->deps, d);
        }
    }

    return p;
}

static dep_t
get_dep_explicit (pkg_t *pkg, dep_t dep)
{
    if (!config.explicit || dep == DEP_UNKNOWN)
    {
        return dep;
    }

    if (!pkg->repo
            && alpm_pkg_get_reason (pkg->pkg) == ALPM_PKG_REASON_EXPLICIT)
    {
        return dep + 1;
    }
    return dep;
}

static dep_t
get_pkg_dep_state (data_t *data, alpm_list_t *refs, pkg_t *pkg)
{
    alpm_list_t *reqs, *i;
    pkg_t *p;
    dep_t d;

    /* is pkg state already known? */
    if (pkg->dep != DEP_UNKNOWN)
    {
        return pkg->dep;
    }

    debug ("compute dep state for %s\n", pkg->name);
    reqs = alpm_pkg_compute_requiredby (pkg->pkg);
    FOR_LIST (i, reqs)
    {
        const char *name = i->data;

        p = alpm_list_find (data->deps,
                name,
                (alpm_list_fn_cmp) pkg_find_name_fn);
        if (!p)
        {
            /* required by a pkg outside our tree -- if it's one not installed
             * locally, we ignore it, else it's a shared dependency */

            if (!alpm_db_get_pkg (config.localdb->data, name))
            {
                continue;
            }

            d = get_dep_explicit (pkg, DEP_SHARED);
            debug ("%s=%d: required by outsider: %s\n",
                    pkg->name,
                    d,
                    name);
            FREELIST (reqs);
            return d;
        }
        else if (p->dep == DEP_SHARED || p->dep == DEP_SHARED_EXPLICIT)
        {
            /* required by a shared dep */
            d = get_dep_explicit (pkg, DEP_SHARED);
            debug ("%s=%d: required by shared dep (%s=%d)\n",
                    pkg->name,
                    d,
                    name,
                    (p) ? p->dep : DEP_UNKNOWN);
            FREELIST (reqs);
            return d;
        }
        else if (p->dep == DEP_UNKNOWN)
        {
            if (!alpm_list_find_ptr (refs, p))
            {
                debug ("%s required by %s, determining state\n",
                        pkg->name,
                        name);
                refs = alpm_list_add (refs, p);
                d = get_pkg_dep_state (data, refs, p);
                set_pkg_dep (data, refs, p, d);
                if (d == DEP_SHARED || d == DEP_SHARED_EXPLICIT
                        || (config.explicit && d == DEP_EXCLUSIVE_EXPLICIT))
                {
                    debug ("%s=SHARED: %s not exclusive (%d)\n",
                            pkg->name,
                            name,
                            d);
                    refs = alpm_list_remove (refs,
                            p,
                            (alpm_list_fn_cmp) pkg_find_pkg_fn,
                            NULL);
                    d = get_dep_explicit (pkg, DEP_SHARED);
                    debug ("%s=%d\n", pkg->name, d);
                    FREELIST (reqs);
                    return d;
                }
                debug ("moving on\n");
                refs = alpm_list_remove (refs,
                        p,
                        (alpm_list_fn_cmp) pkg_find_pkg_fn,
                        NULL);
            }
            else
            {
                debug ("%s required by %s, already found in refs\n",
                        pkg->name,
                        name);
            }
        }
    }
    FREELIST (reqs);

    d = get_dep_explicit (pkg, DEP_EXCLUSIVE);
    debug ("%s=%d\n", pkg->name, d);
    return d;
}

static int
pkg_origin_size_cmp (pkg_t *pkg1, pkg_t *pkg2)
{
    off_t size1, size2;

    if (pkg1->repo && !pkg2->repo)
    {
        return 1;
    }
    else if (!pkg1->repo && pkg2->repo)
    {
        return 0;
    }

    size1 = alpm_pkg_get_isize (pkg1->pkg);
    size2 = alpm_pkg_get_isize (pkg2->pkg);

    if (size1 > size2)
    {
        return -1;
    }
    else if (size1 == size2)
    {
        return 0;
    }
    else
    {
        return 1;
    }
}

static int
pkg_origin_name_cmp (pkg_t *pkg1, pkg_t *pkg2)
{
    if (pkg1->repo && !pkg2->repo)
    {
        return 1;
    }
    else if (!pkg1->repo && pkg2->repo)
    {
        return 0;
    }
    return strcmp (pkg1->name, pkg2->name);
}

static void
set_pkg_dep (data_t *data, alpm_list_t *refs, pkg_t *pkg, dep_t dep)
{
    alpm_list_t *i;

    debug ("set %s to dep %d\n", pkg->name, dep);
    if (pkg->dep == dep)
    {
        return;
    }
    /* size & list are only done for dependencies, not the main package */
    if (!alpm_list_find_ptr (data->pkgs, pkg))
    {
        if (pkg->dep != DEP_UNKNOWN)
        {
            data->group[pkg->dep].size -= alpm_pkg_get_isize (pkg->pkg);
            if (!pkg->repo)
            {
                data->group[pkg->dep].size_local -= alpm_pkg_get_isize (pkg->pkg);
            }
            FOR_LIST (i, data->group[pkg->dep].pkgs)
            {
                if (i->data == pkg)
                {
                    data->group[pkg->dep].pkgs = alpm_list_remove_item (
                            data->group[pkg->dep].pkgs, i);
                    break;
                }
            }
        }
        if ((config.list_exclusive && dep == DEP_EXCLUSIVE)
                || (config.list_exclusive_explicit && dep == DEP_EXCLUSIVE_EXPLICIT)
                || (config.list_shared && dep == DEP_SHARED)
                || (config.list_shared_explicit && dep == DEP_SHARED_EXPLICIT)
                || (config.list_optional && dep == DEP_OPTIONAL)
                || (config.list_optional_explicit && dep == DEP_OPTIONAL_EXPLICIT))
        {
            int len = (int) strlen (pkg->name) + 1; /* +1 for space after */
            if (pkg->repo)
            {
                len += (int) strlen (pkg->repo) + 1; /* +1 for slash */
            }

            data->group[dep].pkgs = alpm_list_add_sorted (data->group[dep].pkgs,
                    pkg,
                    (alpm_list_fn_cmp) ((config.sort_size)
                        ? pkg_origin_size_cmp
                        : pkg_origin_name_cmp));
            if (len > data->group[dep].len_max)
            {
                data->group[dep].len_max = len;
            }
        }
        data->group[dep].size += alpm_pkg_get_isize (pkg->pkg);
        if (!pkg->repo)
        {
            data->group[dep].size_local += alpm_pkg_get_isize (pkg->pkg);
        }
    }
    pkg->dep = dep;

    FOR_LIST (i, pkg->deps)
    {
        pkg_t *p = i->data;
        debug ("%s depends on %s\n", pkg->name, p->name);
        if (pkg->dep == DEP_SHARED)
        {
            dep_t d;

            d = get_dep_explicit (p, DEP_SHARED);
            set_pkg_dep (data, refs, p, d);
        }
        else
        {
            dep_t d;

            refs = alpm_list_add (refs, pkg);
            d = get_pkg_dep_state (data, refs, p);
            set_pkg_dep (data, refs, p, d);
            refs = alpm_list_remove (refs,
                    pkg,
                    (alpm_list_fn_cmp) pkg_find_pkg_fn,
                    NULL);
        }
    }
}

static inline void
get_pkg_optrequiredby (data_t *data, pkg_t *pkg)
{
    alpm_list_t *dbs;
    alpm_list_t *i;
    size_t       len = strlen (pkg->name);

    debug ("create list of opt-requirers for %s\n", pkg->name);
    dbs = (pkg->repo) ? config.syncdbs : config.localdb;
    FOR_LIST (i, dbs)
    {
        alpm_db_t   *db = i->data;
        alpm_list_t *j;

        FOR_LIST (j, alpm_db_get_pkgcache (db))
        {
            alpm_pkg_t  *p = j->data;
            alpm_list_t *k;

            FOR_LIST (k, alpm_pkg_get_optdepends (p))
            {
                const char *name  = ((alpm_depend_t *) k->data)->name;

                if (strncmp (pkg->name, name, len) == 0 &&
                        (name[len] == ':' || name[len] == '\0'))
                {
                    pkg_t *r;

                    debug ("[%s] found optreq: %s\n",
                            pkg->name,
                            alpm_pkg_get_name (p));
                    r = alpm_list_find (data->deps,
                            alpm_pkg_get_name (p),
                            (alpm_list_fn_cmp) pkg_find_name_fn);
                    if (!r)
                    {
                        r = new_package (data, p);
                        set_pkg_dep (data, NULL, r, DEP_OPTIONAL);
                    }
                    break;
                }
            }
        }
    }
}

static int
get_pkg_requiredby (data_t *data, pkg_t *pkg)
{
    int          nb = 0;
    alpm_list_t *reqs;
    alpm_list_t *j;

    debug ("create list of requirers for %s\n", pkg->name);
    reqs = alpm_pkg_compute_requiredby (pkg->pkg);
    FOR_LIST (j, reqs)
    {
        const char *name = j->data;
        pkg_t *r;

        r = alpm_list_find (data->deps,
                name,
                (alpm_list_fn_cmp) pkg_find_name_fn);
        if (!r)
        {
            /* not in our tree, is it installed? */
            alpm_pkg_t *p = NULL;

            debug ("[%s] found req: %s\n", pkg->name, name);

            if (data->source == SCE_LOCAL || data->source == SCE_MIXED)
            {
                p = alpm_find_dbs_satisfier (config.alpm,
                        config.localdb,
                        name);
            }
            /* SCE_SYNC and SCE_MIXED look in sync dbs (too) */
            if (!p && data->source != SCE_LOCAL)
            {
                p = alpm_find_dbs_satisfier (config.alpm,
                        config.syncdbs,
                        name);
            }

            if (p)
            {
                int nb_r = 0;

                ++nb;
                r = new_package (data, p);
                if (config.reverse >= 2)
                {
                    nb_r = get_pkg_requiredby (data, r);
                }
                if (config.reverse <= 2 || nb_r == 0)
                {
                    set_pkg_dep (data, NULL, r, DEP_EXCLUSIVE);
                }
            }
            else
            {
                fprintf (stderr, "Error: no package found for %s\n",
                        name);
            }
        }
    }
    FREELIST (reqs);
    return nb;
}

static void
free_pkg (pkg_t *pkg)
{
    if (pkg->need_free)
    {
        free ((char *) pkg->name_asked);
    }
    alpm_list_free (pkg->deps);
    free (pkg);
}

static void
list_dependencies (data_t *data, dep_t dep)
{
    alpm_list_t *i;
    int flag = 0;

    /* is this group mixed (pkgs from local & sync) ? */
    if (!config.quiet && data->group[dep].size_local > 0
            && data->group[dep].size > data->group[dep].size_local)
    {
        flag = 1;
    }

    FOR_LIST (i, data->group[dep].pkgs)
    {
        pkg_t *p = i->data;

        if (flag == 1)
        {
            fprintf (stdout, " %*s", -8, "local:");
            print_size (data->group[dep].size_local);
            fputc ('\n', stdout);
            flag = 2;
        }

        if (p->repo)
        {
            if (flag == 2)
            {
                fprintf (stdout, " %*s", -8, "sync:");
                print_size (data->group[dep].size - data->group[dep].size_local);
                fputc ('\n', stdout);
                flag = 3;
            }
            if (config.quiet)
            {
                fprintf (stdout, "%s/%s ", p->repo, p->name);
            }
            else
            {
                fprintf (stdout, (flag) ? "  %s/%*s" : " %s/%*s",
                        p->repo,
                        /* +1 for the slash */
                        -data->group[dep].len_max + (int) strlen (p->repo) + 1,
                        p->name);
            }
        }
        else
        {
            if (config.quiet)
            {
                fprintf (stdout, "%s ", p->name);
            }
            else
            {
                fprintf (stdout, (flag) ? "  %*s" : " %*s",
                        -data->group[dep].len_max,
                        p->name);
            }
        }
        print_size (alpm_pkg_get_isize (p->pkg));
        fputc ('\n', stdout);
    }
}

static void
print_group (data_t *data,
        dep_t        dep,
        int          len_max,
        off_t        size,
        bool         list_deps,
        bool         list_deps_explicit)
{
    if (!config.quiet)
    {
        fprintf (stdout, "%*s", -len_max, data->group[dep].title);
        print_size (data->group[dep].size);
        fputc ('\n', stdout);
    }
    if (list_deps)
    {
        list_dependencies (data, dep);
    }
    /* is this group mixed (pkgs from local & sync) ? */
    else if (!config.quiet && data->group[dep].size_local > 0
            && data->group[dep].size > data->group[dep].size_local)
    {
        fprintf (stdout, " %*s", -8, "local:");
        print_size (data->group[dep].size_local);
        fputc ('\n', stdout);
        fprintf (stdout, " %*s", -8, "sync:");
        print_size (data->group[dep].size - data->group[dep].size_local);
        fputc ('\n', stdout);
    }

    if (config.explicit)
    {
        if (!config.quiet)
        {
            fprintf (stdout, "%*s", -len_max, data->group[dep + 1].title);
            print_size (data->group[dep + 1].size);
            if (data->group[dep].size > 0 && data->group[dep + 1].size > 0)
            {
                fputs (" (", stdout);
                print_size (size);
                fputs (")\n", stdout);
            }
            else
            {
                fputc ('\n', stdout);
            }
        }
        if (list_deps_explicit)
        {
            list_dependencies (data, dep + 1);
        }
        /* is this group mixed (pkgs from local & sync) ? */
        else if (!config.quiet && data->group[dep].size_local > 0
                && data->group[dep].size > data->group[dep].size_local)
        {
            fprintf (stdout, " %*s", -8, "local:");
            print_size (data->group[dep + 1].size_local);
            fputc ('\n', stdout);
            fprintf (stdout, " %*s", -8, "sync:");
            print_size (data->group[dep + 1].size
                    - data->group[dep + 1].size_local);
            fputc ('\n', stdout);
        }
    }
}

static void
preprocess_package (data_t *data, const char *pkgname, bool dup_name)
{
    alpm_pkg_t  *pkg = NULL;
    pkg_t       *p;

    if (!config.from_sync)
    {
        /* seach all dbs (local, then sync) and find match even the name
         * was a provider */
        pkg = alpm_find_dbs_satisfier (config.alpm, config.localdb, pkgname);
    }
    if (!pkg)
    {
        pkg = alpm_find_dbs_satisfier (config.alpm, config.syncdbs, pkgname);
    }
    if (!pkg)
    {
        fprintf (stderr, "Package not found: %s\n", pkgname);
        return;
    }

    if (!config.reverse)
    {
        debug ("create list of all dependencies for %s\n", pkgname);
        p = add_to_deps (data, pkg);
    }
    else
    {
        /* we simply create the pkg_t, nothing else to do at this point. This
         * will also add it to data->deps */
        p = new_package (data, pkg);
    }
    p->need_free = dup_name;
    p->name_asked = (dup_name) ? strdup (pkgname) : pkgname;
    /* mark exclusive right now, so when dependencies are sorted out all
     * "main" packages are seen as exclusive */
    p->dep = DEP_EXCLUSIVE;
    if (p->repo)
    {
        if (data->source == SCE_UNKNOWN)
        {
            data->source = SCE_SYNC;
        }
        else if (data->source == SCE_LOCAL)
        {
            data->source = SCE_MIXED;
        }
    }
    else
    {
        if (data->source == SCE_UNKNOWN)
        {
            data->source = SCE_LOCAL;
        }
        else if (data->source == SCE_SYNC)
        {
            data->source = SCE_MIXED;
        }
    }
    /* in case the same package is listed twice on cmdline */
    if (alpm_list_find_ptr (data->pkgs, p))
    {
        return;
    }
    data->pkgs = alpm_list_add (data->pkgs, p);

    if (!config.reverse && config.show_optional)
    {
        alpm_list_t *i;

        debug ("add %s's optional dependencies\n", pkgname);
        FOR_LIST (i, alpm_pkg_get_optdepends (pkg))
        {
            alpm_depend_t *optdep = i->data;

            /* is this dependency installed ? */
            pkg = alpm_find_dbs_satisfier (config.alpm,
                    config.localdb,
                    optdep->name);
            if (!pkg)
            {
                /* should we list non-installed deps ? */
                if (config.show_optional < 3)
                {
                    debug ("ignoring non-installed %s\n", optdep->name);
                    continue;
                }
                pkg = alpm_find_dbs_satisfier (config.alpm,
                        config.syncdbs,
                        optdep->name);
            }
            if (!pkg)
            {
                debug ("ignoring non-found %s\n", optdep->name);
                continue;
            }

            /* explicitly installed optdep are ignored by default */
            if (config.show_optional < 3
                    && !config.explicit
                    && alpm_pkg_get_origin (pkg) == ALPM_PKG_FROM_LOCALDB
                    && alpm_pkg_get_reason (pkg) == ALPM_PKG_REASON_EXPLICIT)
            {
                debug ("ignoring explicitly installed %s\n",
                        alpm_pkg_get_name (pkg));
                continue;
            }

            if (config.show_optional < 2)
            {
                /* we now make sure it isn't required by smthg installed
                 * but outside of our deptree */
                alpm_list_t *reqs;
                alpm_list_t *j;
                bool         ignore = false;

                reqs = alpm_pkg_compute_requiredby (pkg);
                FOR_LIST (j, reqs)
                {
                    const char *name = j->data;

                    p = alpm_list_find (data->deps,
                            name,
                            (alpm_list_fn_cmp) pkg_find_name_fn);
                    if (!p)
                    {
                        /* not in our tree, is it installed? */
                        if (alpm_find_dbs_satisfier (config.alpm,
                                    config.localdb,
                                    name))
                        {
                            ignore = true;
                            debug ("ignoring %s required by %s\n",
                                    alpm_pkg_get_name (pkg),
                                    name);
                            break;
                        }
                    }
                }
                FREELIST (reqs);
                if (ignore)
                {
                    continue;
                }
            }

            add_to_deps (data, pkg);
        }
    }
}

int
main (int argc, char *argv[])
{
    const char *conffile = PACMAN_CONFFILE;
    const char *dbpath   = NULL;

    memset (&config, 0, sizeof (config_t));

    int o;
    int index = 0;
    struct option options[] = {
        { "help",                       no_argument,        0,  'h' },
        { "version",                    no_argument,        0,  'V' },
        { "debug",                      no_argument,        0,  'd' },
        { "config",                     required_argument,  0,  'c' },
        { "dbpath",                     required_argument,  0,  'b' },
        { "from-sync",                  no_argument,        0,  'Y' },
        { "quiet",                      no_argument,        0,  'q' },
        { "raw-sizes",                  no_argument,        0,  'w' },
        { "sort-size",                  no_argument,        0,  'z' },
        { "show-optional",              no_argument,        0,  'p' },
        { "explicit",                   no_argument,        0,  'x' },
        { "reverse",                    no_argument,        0,  'r' },
        { "list-requiredby",            no_argument,        0,  'R' },
        { "list-exclusive",             no_argument,        0,  'e' },
        { "list-exclusive-explicit",    no_argument,        0,  'E' },
        { "list-shared",                no_argument,        0,  's' },
        { "list-shared-explicit",       no_argument,        0,  'S' },
        { "list-optional",              no_argument,        0,  'o' },
        { "list-optional-explicit",     no_argument,        0,  'O' },
        { 0,                            0,                  0,    0 },
    };
    for (;;)
    {
        o = getopt_long (argc, argv, "hVdc:b:qwzpxrReEsSoO", options, &index);
        if (o == -1)
        {
            break;
        }

        switch (o)
        {
            case 'h':
                show_help (argv[0]);
                /* not reached */
                break;
            case 'V':
                show_version ();
                /* not reached */
                break;
            case 'd':
                config.is_debug = true;
                break;
            case 'c':
                conffile = optarg;
                break;
            case 'b':
                dbpath = optarg;
                break;
            case 'Y':
                config.from_sync = true;
                break;
            case 'q':
                config.quiet = true;
                break;
            case 'w':
                config.raw_sizes = true;
                break;
            case 'z':
                config.sort_size = true;
                break;
            case 'p':
                if (config.show_optional >= 3)
                {
                    fprintf (stderr,
                            "Option --show-optional can only be used up to three times\n");
                    return 1;
                }
                config.show_optional++;
                break;
            case 'x':
                config.explicit = true;
                break;
            case 'r':
                if (config.reverse >= 3)
                {
                    fprintf (stderr,
                            "Option --reverse can only be used up to three times\n");
                    return 1;
                }
                config.reverse++;
                break;
            case 'R':
                config.list_requiredby = true;
                break;
            case 'e':
                config.list_exclusive = true;
                break;
            case 'E':
                config.list_exclusive_explicit = true;
                config.explicit = true;
                break;
            case 's':
                config.list_shared = true;
                break;
            case 'S':
                config.list_shared_explicit = true;
                config.explicit = true;
                break;
            case 'o':
                config.list_optional = true;
                break;
            case 'O':
                config.list_optional_explicit = true;
                config.explicit = true;
                break;
            case '?': /* unknown option */
            default:
                return 1;
        }
    }
    if (optind == argc)
    {
        fprintf (stderr, "Missing package name(s)\n");
        show_help (argv[0]);
        /* not reached */
        return 0;
    }
    /* options -o/-O implies -p (-O only if not reverse) */
    if (!config.show_optional && (
                config.list_optional ||
                (!config.reverse && config.list_optional_explicit)
                ))
    {
        config.show_optional = 1;
    }
    /* option -R implies -r */
    if (config.list_requiredby && !config.reverse)
    {
        config.reverse = 1;
    }
    /* special handling of options for reverse mode */
    if (config.reverse)
    {
        config.list_exclusive = config.list_requiredby;
        config.explicit = false;
    }

    char *error;
    int rc;

    rc = alpm_load (&config.alpm, conffile, dbpath, &error);
    if (rc != E_OK)
    {
        fprintf (stderr, "Error: %s", error);
        free (error);
        return rc;
    }

    config.localdb = alpm_list_add (NULL, alpm_get_localdb (config.alpm));
    config.syncdbs = alpm_get_syncdbs (config.alpm);

    data_t data;

    memset (&data, 0, sizeof (data_t));
    for ( ; optind < argc; ++optind)
    {
        /* "-" as package name can be used to read from stdin */
        if (argv[optind][0] == '-' && argv[optind][1] == '\0')
        {
            char    *name;
            char    *s;
            char     c;
            size_t   alloc  = BUF_LEN;
            size_t   len    = 0;

            name = malloc (sizeof (*name) * alloc);
            if (!name)
            {
                fprintf (stderr, "Error: out of memory\n");
                rc = E_NOMEM;
                goto release;
            }
            s = name;
            while ((c = (char) fgetc (stdin)))
            {
                if (c == EOF || isspace (c))
                {
                    if (s > name)
                    {
                        *s = '\0';
                        preprocess_package (&data, name, true);
                        s = name;
                        len = 0;
                    }
                    if (c == EOF)
                    {
                        break;
                    }
                }
                else
                {
                    if (++len >= alloc)
                    {
                        alloc += BUF_LEN;
                        name = realloc (name, sizeof (*name) * alloc);
                        s = name + len - 1;
                    }
                    *s++ = c;
                }
            }
            free (name);
        }
        else
        {
            preprocess_package (&data, argv[optind], false);
        }
    }

    if (!data.pkgs)
    {
        fprintf (stderr, "No package to process\n");
        rc = E_NOTHING;
        goto release;
    }

    int len_max = 0;
    int len;

    if (!config.quiet)
    {
        data.group[DEP_UNKNOWN].title            = "Total dependencies:";
        data.group[DEP_EXCLUSIVE].title          = "Exclusive dependencies:";
        data.group[DEP_EXCLUSIVE_EXPLICIT].title = "Exclusive explicit dependencies:";
        data.group[DEP_OPTIONAL].title           = "Optional dependencies:";
        data.group[DEP_OPTIONAL_EXPLICIT].title  = "Optional explicit dependencies:";
        data.group[DEP_SHARED].title             = "Shared dependencies:";
        data.group[DEP_SHARED_EXPLICIT].title    = "Shared explicit dependencies:";
        if (config.reverse)
        {
            data.group[DEP_EXCLUSIVE].title      = "Required by:";
            data.group[DEP_OPTIONAL].title       = "Optionally required by:";
        }

        len = (int) strlen (data.group[DEP_UNKNOWN].title) + 1;
        len_max = len;

        const char **t, *titles[] = { data.group[DEP_EXCLUSIVE].title,
            data.group[DEP_EXCLUSIVE_EXPLICIT].title,
            data.group[DEP_OPTIONAL].title,
            data.group[DEP_OPTIONAL_EXPLICIT].title,
            data.group[DEP_SHARED].title,
            data.group[DEP_SHARED_EXPLICIT].title,
            NULL
        };
        for (t = titles; *t; ++t)
        {
            if (config.reverse && t == titles + 3)
            {
                break;
            }
            if ((t - titles) % 2 && !config.explicit)
            {
                continue;
            }
            len = (int) strlen (*t) + 1;
            if (len > len_max)
            {
                len_max = len;
            }
        }
    }

    /* all packages and their deps are known. time to "sort" everything */
    alpm_list_t *i;
    FOR_LIST (i, data.pkgs)
    {
        pkg_t *pkg = i->data;

        pkg->is_provided = strcmp (pkg->name_asked, pkg->name);
        if (!config.quiet)
        {
            /* calculate len_max to show package name (/w repo/provided if apply) */
            if (!pkg->is_provided)
            {
                len = (int) strlen (pkg->name_asked) + 1;
            }
            else
            {
                len = (int) strlen (pkg->name) + 1;
                /* 16 == strlen (" is provided by ") */
                len += (int) strlen (pkg->name_asked) + 16;
            }
            if (pkg->repo)
            {
                /* +1 for the / */
                len += 1 + (int) strlen (pkg->repo);
            }
            if (len > len_max)
            {
                len_max = len;
            }
        }

        if (!config.reverse)
        {
            debug ("determine dependencies type (exclusive/shared)\n");
            /* restore to DEP_UNKNOWN so it's fully processed */
            pkg->dep = DEP_UNKNOWN;
            set_pkg_dep (&data, NULL, pkg, DEP_EXCLUSIVE);
            if (config.show_optional)
            {
                alpm_list_t *j;

                FOR_LIST (j, alpm_pkg_get_optdepends (pkg->pkg))
                {
                    char *name = ((alpm_depend_t *) j->data)->name;
                    char *s;
                    pkg_t *p;

                    /* optdepends are info strings: "package: some desc" */
                    s = strchr (name, ':');
                    if (s)
                    {
                        *s = '\0';
                    }

                    /* if it's in data.deps it is an optdep to list/count as such */
                    p = alpm_list_find (data.deps,
                            name,
                            (alpm_list_fn_cmp) pkg_find_name_fn);
                    if (s)
                    {
                        *s = ':';
                    }
                    if (!p)
                    {
                        continue;
                    }

                    dep_t dep;

                    if (!config.explicit)
                    {
                        dep = DEP_OPTIONAL;
                    }
                    else
                    {
                        if (!p->repo
                                && alpm_pkg_get_reason (p->pkg) == ALPM_PKG_REASON_EXPLICIT)
                        {
                            dep = DEP_OPTIONAL_EXPLICIT;
                        }
                        else
                        {
                            dep = DEP_OPTIONAL;
                        }
                    }
                    set_pkg_dep (&data, NULL, p, dep);
                }
            }
        }
        else
        {
            get_pkg_requiredby (&data, pkg);
            if (config.show_optional)
            {
                get_pkg_optrequiredby (&data, pkg);
            }
        }
        /* put the package size under DEP_UNKNOWN (not used otherwise) */
        data.group[DEP_UNKNOWN].size_local += alpm_pkg_get_isize (pkg->pkg);
    }

    off_t size_exclusive = data.group[DEP_EXCLUSIVE].size
        + data.group[DEP_EXCLUSIVE_EXPLICIT].size;
    off_t size_shared = data.group[DEP_SHARED].size
        + data.group[DEP_SHARED_EXPLICIT].size;
    off_t size_optional = data.group[DEP_OPTIONAL].size
        + data.group[DEP_OPTIONAL_EXPLICIT].size;

    int nb_pkg = (int) alpm_list_count (data.pkgs);
    FOR_LIST (i, data.pkgs)
    {
        pkg_t *pkg = i->data;

        if (pkg->repo)
        {
            if (!pkg->is_provided)
            {
                fprintf (stdout, "%s/%*s",
                        pkg->repo,
                        -len_max + (int) strlen (pkg->repo) + 1,
                        pkg->name_asked);
            }
            else
            {
                if (config.quiet)
                {
                    fprintf (stdout, "%s %s/%s",
                            pkg->name_asked,
                            pkg->repo,
                            pkg->name);
                }
                else
                {
                    fprintf (stdout, "%s is provided by %s/%*s",
                            pkg->name_asked,
                            pkg->repo,
                            -len_max + (int) strlen (pkg->name_asked) + 16
                                + (int) strlen (pkg->repo) + 1,
                            pkg->name);
                }
            }
        }
        else if (!pkg->is_provided)
        {
            fprintf (stdout, "%*s", -len_max, pkg->name_asked);
        }
        else
        {
            if (config.quiet)
            {
                fprintf (stdout, "%s %s",
                        pkg->name_asked,
                        pkg->name);
            }
            else
            {
                fprintf (stdout, "%s is provided by %*s",
                        pkg->name_asked,
                        -len_max + (int) strlen (pkg->name_asked) + 16,
                        pkg->name);
            }
        }
        if (config.quiet)
        {
            fputc (' ', stdout);
        }
        print_size (alpm_pkg_get_isize (pkg->pkg));

        /* more than one pkg, no package size -- it'll be on a new line, since
         * it's a combined size for all packages.
         * In quiet or reverse mode, no package size either. */
        if (nb_pkg > 1 || config.quiet || config.reverse)
        {
            fputc ('\n', stdout);
        }
    }

    /* package size doesn't apply in quiet, reverse, or with SCE_MIXED */
    if (!config.quiet && !config.reverse && data.source != SCE_MIXED)
    {
        if (nb_pkg > 1)
        {
            fprintf (stdout, "%*s", -len_max, "");
            print_size (data.group[DEP_UNKNOWN].size_local);
        }

        /* pkg size + exclusive & optional deps of its kind (local/sync) */
        data.group[DEP_UNKNOWN].size = data.group[DEP_EXCLUSIVE].size_local
            + data.group[DEP_EXCLUSIVE_EXPLICIT].size_local
            + data.group[DEP_OPTIONAL].size_local
            + data.group[DEP_OPTIONAL_EXPLICIT].size_local;
        if (data.source == SCE_SYNC)
        {
            data.group[DEP_UNKNOWN].size *= -1;
            data.group[DEP_UNKNOWN].size += data.group[DEP_EXCLUSIVE].size
                + data.group[DEP_EXCLUSIVE_EXPLICIT].size
                + data.group[DEP_OPTIONAL].size
                + data.group[DEP_OPTIONAL_EXPLICIT].size;
        }
        data.group[DEP_UNKNOWN].size += data.group[DEP_UNKNOWN].size_local;
        if (data.group[DEP_UNKNOWN].size > data.group[DEP_UNKNOWN].size_local)
        {
            fputs (" (", stdout);
            print_size (data.group[DEP_UNKNOWN].size);
            fputs (")\n", stdout);
        }
        else
        {
            fputc ('\n', stdout);
        }
    }

    /* exclusive deps */
    print_group (&data,
            DEP_EXCLUSIVE,
            len_max,
            size_exclusive,
            config.list_exclusive,
            config.list_exclusive_explicit);

    if (config.show_optional)
    {
        /* optional deps */
        print_group (&data,
                DEP_OPTIONAL,
                len_max,
                size_optional,
                config.list_optional,
                config.list_optional_explicit);
    }

    if (!config.reverse)
    {
        /* shared deps */
        print_group (&data,
                DEP_SHARED,
                len_max,
                size_shared,
                config.list_shared,
                config.list_shared_explicit);
    }

    if (!config.quiet)
    {
        /* total deps */
        fprintf (stdout, "%*s", -len_max, data.group[DEP_UNKNOWN].title);
        print_size (size_exclusive + size_shared + size_optional);
        fputs (" (", stdout);
        print_size (data.group[DEP_UNKNOWN].size_local
                + size_exclusive
                + size_shared
                + size_optional);
        fputs (")\n", stdout);
    }

    /* free list of deps */
    alpm_list_free_inner (data.deps, (alpm_list_fn_free) free_pkg);
    alpm_list_free (data.deps);
    data.deps = NULL;

    /* free groups list of packages */
    int d;
    for (d = 0; d < NB_DEPS; ++d)
    {
        alpm_list_free (data.group[d].pkgs);
    }

release:
    alpm_list_free (data.pkgs);
    debug ("release libalpm\n");
    alpm_release (config.alpm);
    alpm_list_free (config.localdb);
    return rc;
}
