
#include "config.h"

#include <stdlib.h>
#include <stdio.h>
#include <getopt.h>
#include <string.h>
#include <glob.h>
#include <ctype.h>

#include <alpm_list.h>
#include <alpm.h>

#define BUF_LEN     255

/* pacman default values */
#define PACMAN_CONFFILE         "/etc/pacman.conf"
#define PACMAN_ROOTDIR          "/"
#define PACMAN_DBPATH           "/var/lib/pacman/"

#if defined(GIT_VERSION)
#undef PACKAGE_VERSION
#define PACKAGE_VERSION GIT_VERSION
#endif
#define PACKAGE_TAG             "Package Dependencies Helper"

enum {
    E_OK = 0,
    E_NOMEM,
    E_FILEREAD,
    E_PARSING,
    E_NOPKG,
    E_ALPM,
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
    const char  *name;
    const char  *repo;
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

typedef struct _data_t {
    pkg_t       *pkg;
    group_t      group[NB_DEPS];
    alpm_list_t *deps;
} data_t;

typedef struct _config_t {
    alpm_handle_t   *alpm;
    alpm_list_t     *localdb;
    alpm_list_t     *syncdbs;

    unsigned int     is_debug : 1;
    unsigned int     explicit : 1;
    unsigned int     list_exclusive : 1;
    unsigned int     list_exclusive_explicit : 1;
    unsigned int     list_shared : 1;
    unsigned int     list_shared_explicit : 1;
    unsigned int     show_optional : 2;
    unsigned int     list_optional : 1;
    unsigned int     list_optional_explicit : 1;
} config_t;

static config_t config;

static void
set_pkg_dep (data_t *data, alpm_list_t *refs, pkg_t *pkg, dep_t dep);


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
    putchar ('\n');
    puts (" -e, --list-exclusive            List exclusive dependencies");
    puts (" -E, --list-exclusive-explicit   List exclusive explicit dependencies");
    puts (" -s, --list-shared               List shared dependencies");
    puts (" -S, --list-shared-explicit      List shared explicit dependencies");
    puts (" -p, --show-optional             Show optional dependencies (see man page)");
    puts (" -o, --list-optional             List optional dependencies");
    puts (" -O, --list-optional-explicit    List optional explicit dependencies");
    puts (" -x, --explicit                  Don't ignore explicitly installed dependencies");
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
                   int                is_options,
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
alpm_load (alpm_handle_t **handle, const char *conffile, char **error)
{
    int                  rc = E_OK;
    enum _alpm_errno_t   err;
    pacman_config_t     *pac_conf = NULL;

    /* parse pacman.conf */
    debug ("parsing pacman.conf (%s) for options\n", conffile);
    rc = parse_pacman_conf (conffile, NULL, 0, 0, &pac_conf, error);
    if (rc != E_OK)
    {
        free_pacman_config (pac_conf);
        return rc;
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
    alpm_list_t *i;
    for (i = pac_conf->databases; i; i = alpm_list_next (i))
    {
        char *db_name = i->data;
        alpm_db_t *db;

        /* register db */
        debug ("register %s\n", db_name);
        db = alpm_db_register_sync (*handle, db_name, ALPM_SIG_USE_DEFAULT);
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
print_size (off_t size)
{
    const char *units[]  = { "B", "KiB", "MiB", "GiB" };
    int         nb_units = sizeof (units) / sizeof (units[0]);

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
        fmt ="%6.0f %s";
    }
    else
    {
        fmt = "%6.2f %s";
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
        return NULL;
    }

    p = malloc (sizeof (*p));
    p->name = alpm_pkg_get_name (pkg);
    p->pkg  = pkg;
    p->deps = NULL;
    p->dep  = DEP_UNKNOWN;
    if (alpm_pkg_get_origin (pkg) == PKG_FROM_SYNCDB)
    {
        p->repo = alpm_db_get_name (alpm_pkg_get_db (pkg));
    }
    else
    {
        p->repo = NULL;
    }

    /* add it right now, so it's found when adding its own dep */
    debug ("adding %s to deps\n", p->name);
    data->deps = alpm_list_add (data->deps, p);

    /* go through dep tree to list all dependencies involved */
    for (i = alpm_pkg_get_depends (pkg); i; i = alpm_list_next (i))
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
                && alpm_pkg_get_origin (dep) == PKG_FROM_LOCALDB
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
_get_dep_explicit (pkg_t *pkg, dep_t dep)
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
    for (i = reqs; i; i = alpm_list_next (i))
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

            d = _get_dep_explicit (pkg, DEP_SHARED);
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
            d = _get_dep_explicit (pkg, DEP_SHARED);
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
                    d = _get_dep_explicit (pkg, DEP_SHARED);
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

    d = _get_dep_explicit (pkg, DEP_EXCLUSIVE);
    debug ("%s=%d\n", pkg->name, d);
    return d;
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
    if (pkg != data->pkg)
    {
        if (pkg->dep != DEP_UNKNOWN)
        {
            data->group[pkg->dep].size -= alpm_pkg_get_isize (pkg->pkg);
            if (!pkg->repo)
            {
                data->group[pkg->dep].size_local -= alpm_pkg_get_isize (pkg->pkg);
            }
            for (i = data->group[dep].pkgs; i; i = alpm_list_next (i))
            {
                if (i->data == pkg)
                {
                    alpm_list_remove_item (data->group[dep].pkgs, i);
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
                    (alpm_list_fn_cmp) pkg_origin_name_cmp);
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

    for (i = pkg->deps; i; i = alpm_list_next (i))
    {
        pkg_t *p = i->data;
        debug ("%s depends on %s\n", pkg->name, p->name);
        if (pkg->dep == DEP_SHARED)
        {
            dep_t d;

            d = _get_dep_explicit (p, DEP_SHARED);
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

static void
free_pkg (pkg_t *pkg)
{
    alpm_list_free (pkg->deps);
    free (pkg);
}

static void
list_dependencies (data_t *data, dep_t dep)
{
    alpm_list_t *i;
    int flag = 0;

    /* is this group mixed (pkgs from local & sync) ? */
    if (data->group[dep].size_local > 0
            && data->group[dep].size > data->group[dep].size_local)
    {
        flag = 1;
    }

    for (i = data->group[dep].pkgs; i; i = alpm_list_next (i))
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
            fprintf (stdout, (flag) ? "  %s/%*s" : " %s/%*s",
                    p->repo,
                    /* +1 for the slash */
                    -data->group[dep].len_max + (int) strlen (p->repo) + 1,
                    p->name);
        }
        else
        {
            fprintf (stdout, (flag) ? "  %*s" : " %*s",
                    -data->group[dep].len_max,
                    p->name);
        }
        print_size (alpm_pkg_get_isize (p->pkg));
        fputc ('\n', stdout);
    }
}

int
main (int argc, char *argv[])
{
    const char *conffile = PACMAN_CONFFILE;

    memset (&config, 0, sizeof (config_t));

    int o;
    int index = 0;
    struct option options[] = {
        { "help",                       no_argument,        0,  'h' },
        { "version",                    no_argument,        0,  'V' },
        { "debug",                      no_argument,        0,  'd' },
        { "config",                     required_argument,  0,  'c' },
        { "list-exclusive",             no_argument,        0,  'e' },
        { "list-exclusive-explicit",    no_argument,        0,  'E' },
        { "list-shared",                no_argument,        0,  's' },
        { "list-shared-explicit",       no_argument,        0,  'S' },
        { "show-optional",              no_argument,        0,  'p' },
        { "list-optional",              no_argument,        0,  'o' },
        { "list-optional-explicit",     no_argument,        0,  'O' },
        { "explicit",                   no_argument,        0,  'x' },
        { 0,                            0,                  0,    0 },
    };
    for (;;)
    {
        o = getopt_long (argc, argv, "hVdc:eEsSpoOx", options, &index);
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
                config.is_debug = 1;
                break;
            case 'c':
                conffile = optarg;
                break;
            case 'e':
                config.list_exclusive = 1;
                break;
            case 'E':
                config.list_exclusive_explicit = 1;
                break;
            case 's':
                config.list_shared = 1;
                break;
            case 'S':
                config.list_shared_explicit = 1;
                break;
            case 'p':
                if (config.show_optional >= 3)
                {
                    fprintf (stderr,
                            "Option --show-optional can only be used once or twice\n");
                    return 1;
                }
                config.show_optional++;
                break;
            case 'o':
                config.list_optional = 1;
                break;
            case 'O':
                config.list_optional_explicit = 1;
                break;
            case 'x':
                config.explicit = 1;
                break;
            case '?': /* unknown option */
            default:
                return 1;
        }
    }
    if (optind == argc)
    {
        fprintf (stderr, "Missing package name(s)\n");
        return E_NOPKG;
    }
    /* options -o/-O implies -p */
    if ((config.list_optional || config.list_optional_explicit)
            && !config.show_optional)
    {
        config.show_optional = 1;
    }

    char *error;
    int rc;

    rc = alpm_load (&config.alpm, conffile, &error);
    if (rc != E_OK)
    {
        fprintf (stderr, "Error: %s", error);
        free (error);
        return rc;
    }

    config.localdb = alpm_list_add (NULL, alpm_option_get_localdb (config.alpm));
    config.syncdbs = alpm_option_get_syncdbs (config.alpm);

    while (optind < argc)
    {
        data_t      data;
        alpm_pkg_t *pkg;
        const char *name = argv[optind];
        const char *s;

        memset (&data, 0, sizeof (data_t));

        /* seach all dbs (local, then sync) and find match even the name
         * was a provider */
        pkg = alpm_find_dbs_satisfier (config.alpm, config.localdb, name);
        if (!pkg)
        {
            pkg = alpm_find_dbs_satisfier (config.alpm, config.syncdbs, name);
        }
        if (!pkg)
        {
            fprintf (stderr, "Package not found: %s\n", name);
            ++optind;
            continue;
        }

        int is_provided  = 0;
        int len_max      = 0;

        /* package */
        s = alpm_pkg_get_name (pkg);
        if (strcmp (name, s) == 0)
        {
            len_max = (int) strlen (name) + 1;
        }
        else
        {
            is_provided = 1;
            len_max = (int) strlen (s) + 1;
            /* 16 == strlen (" is provided by ") */
            len_max += (int) strlen (name) + 16;
        }
        if (alpm_pkg_get_origin (pkg) == PKG_FROM_SYNCDB)
        {
            /* +1 for the / */
            len_max += 1 + (int) strlen (
                    alpm_db_get_name (alpm_pkg_get_db (pkg)));
        }

        alpm_list_t *i;
        pkg_t *p;

        debug ("create list of all dependencies\n");
        data.pkg = add_to_deps (&data, pkg);
        if (config.show_optional)
        {
            debug ("add optional dependencies\n");
            for (i = alpm_pkg_get_optdepends (pkg); i; i = alpm_list_next (i))
            {
                alpm_pkg_t *pkg;
                char *name = i->data;
                char *s;

                /* optdepends are info strings: "package: some desc" */
                s = strchr (name, ':');
                if (s)
                {
                    *s = '\0';
                }

                /* is this dependency installed ? */
                pkg = alpm_find_dbs_satisfier (config.alpm,
                        config.localdb,
                        name);
                if (!pkg)
                {
                    /* should we list non-installed deps ? */
                    if (config.show_optional < 3)
                    {
                        debug ("ignoring non-installed %s\n", name);
                        if (s)
                        {
                            *s = ':';
                        }
                        continue;
                    }
                    pkg = alpm_find_dbs_satisfier (config.alpm,
                            config.syncdbs,
                            name);
                }
                if (!pkg)
                {
                    debug ("ignoring non-found %s\n", name);
                    if (s)
                    {
                        *s = ':';
                    }
                    continue;
                }
                if (s)
                {
                    *s = ':';
                }

                /* explicitly installed optdep are ignored by default */
                if (config.show_optional < 3
                        && !config.explicit
                        && alpm_pkg_get_origin (pkg) == PKG_FROM_LOCALDB
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
                    int ignore = 0;

                    reqs = alpm_pkg_compute_requiredby (pkg);
                    for (j = reqs; j; j = alpm_list_next (j))
                    {
                        const char *name = j->data;

                        p = alpm_list_find (data.deps,
                                name,
                                (alpm_list_fn_cmp) pkg_find_name_fn);
                        if (!p)
                        {
                            /* not in our tree, is it installed? */
                            if (alpm_find_dbs_satisfier (config.alpm,
                                        config.syncdbs,
                                        name))
                            {
                                ignore = 1;
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

                add_to_deps (&data, pkg);
            }
        }
        debug ("determine dependencies type (exclusive/shared)\n");
        set_pkg_dep (&data, NULL, data.pkg, DEP_EXCLUSIVE);
        if (config.show_optional)
        {
            for (i = alpm_pkg_get_optdepends (data.pkg->pkg);
                    i;
                    i = alpm_list_next (i))
            {
                char *name = i->data;
                char *s;

                /* optdepends are info strings: "package: some desc" */
                s = strchr (name, ':');
                if (s)
                {
                    *s = '\0';
                }

                /* if it's in data.deps it is a optdep to list/count as such */
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
        /* put the package size under DEP_UNKNOWN (not used otherwise) */
        data.group[DEP_UNKNOWN].size_local = alpm_pkg_get_isize (data.pkg->pkg);

        /* exclusive dependencies */
        data.group[DEP_UNKNOWN].title            = "Total dependencies:";
        data.group[DEP_EXCLUSIVE].title          = "Exclusive dependencies:";
        data.group[DEP_EXCLUSIVE_EXPLICIT].title = "Exclusive explicit dependencies:";
        data.group[DEP_OPTIONAL].title           = "Optional dependencies:";
        data.group[DEP_OPTIONAL_EXPLICIT].title  = "Optional explicit dependencies:";
        data.group[DEP_SHARED].title             = "Shared dependencies:";
        data.group[DEP_SHARED_EXPLICIT].title    = "Shared explicit dependencies:";

        int len = (int) strlen (data.group[DEP_UNKNOWN].title) + 1;
        if (len > len_max)
        {
            len_max = len;
        }

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
            if ((t - titles) % 2 && !config.explicit)
            {
                continue;
            }
            int len = (int) strlen (*t) + 1;
            if (len > len_max)
            {
                len_max = len;
            }
        }

        /* printing results */
        off_t size_exclusive = data.group[DEP_EXCLUSIVE].size
            + data.group[DEP_EXCLUSIVE_EXPLICIT].size;
        off_t size_shared = data.group[DEP_SHARED].size
            + data.group[DEP_SHARED_EXPLICIT].size;
        off_t size_optional = data.group[DEP_OPTIONAL].size
            + data.group[DEP_OPTIONAL_EXPLICIT].size;

        /* package */
        if (data.pkg->repo)
        {
            if (!is_provided)
            {
                fprintf (stdout, "%s/%*s",
                        data.pkg->repo,
                        -len_max + (int) strlen (data.pkg->repo) + 1,
                        name);
            }
            else
            {
                fprintf (stdout, "%s is provided by %s/%*s",
                        name,
                        data.pkg->repo,
                        -len_max + (int) strlen (name) + 16
                            + (int) strlen (data.pkg->repo) + 1,
                        data.pkg->name);
            }
        }
        else if (!is_provided)
        {
            fprintf (stdout, "%*s", -len_max, name);
        }
        else
        {
            fprintf (stdout, "%s is provided by %*s",
                    name,
                    -len_max + (int) strlen (name) + 16,
                    data.pkg->name);
        }
        print_size (data.group[DEP_UNKNOWN].size_local);
        /* pkg size + exclusive & optional deps of its kind (local/sync) */
        data.group[DEP_UNKNOWN].size = data.group[DEP_EXCLUSIVE].size_local
            + data.group[DEP_EXCLUSIVE_EXPLICIT].size_local
            + data.group[DEP_OPTIONAL].size_local
            + data.group[DEP_OPTIONAL_EXPLICIT].size_local;
        if (data.pkg->repo)
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

        /* exclusive deps */
        fprintf (stdout, "%*s", -len_max, data.group[DEP_EXCLUSIVE].title);
        print_size (data.group[DEP_EXCLUSIVE].size);
        fputc ('\n', stdout);
        if (config.list_exclusive)
        {
            list_dependencies (&data, DEP_EXCLUSIVE);
        }

        if (config.explicit)
        {
            /* exclusive explicit deps */
            fprintf (stdout, "%*s",
                    -len_max,
                    data.group[DEP_EXCLUSIVE_EXPLICIT].title);
            print_size (data.group[DEP_EXCLUSIVE_EXPLICIT].size);
            if (data.group[DEP_EXCLUSIVE].size > 0
                    && data.group[DEP_EXCLUSIVE_EXPLICIT].size > 0)
            {
                fputs (" (", stdout);
                print_size (size_exclusive);
                fputs (")\n", stdout);
            }
            else
            {
                fputc ('\n', stdout);
            }
            if (config.list_exclusive_explicit)
            {
                list_dependencies (&data, DEP_EXCLUSIVE_EXPLICIT);
            }
        }

        if (config.show_optional)
        {
            /* optional deps */
            fprintf (stdout, "%*s", -len_max, data.group[DEP_OPTIONAL].title);
            print_size (data.group[DEP_OPTIONAL].size);
            fputc ('\n', stdout);
            if (config.list_optional)
            {
                list_dependencies (&data, DEP_OPTIONAL);
            }

            if (config.explicit)
            {
                /* exclusive explicit deps */
                fprintf (stdout, "%*s",
                        -len_max,
                        data.group[DEP_OPTIONAL_EXPLICIT].title);
                print_size (data.group[DEP_OPTIONAL_EXPLICIT].size);
                if (data.group[DEP_OPTIONAL].size > 0
                        && data.group[DEP_OPTIONAL_EXPLICIT].size > 0)
                {
                    fputs (" (", stdout);
                    print_size (size_optional);
                    fputs (")\n", stdout);
                }
                else
                {
                    fputc ('\n', stdout);
                }
                if (config.list_optional_explicit)
                {
                    list_dependencies (&data, DEP_OPTIONAL_EXPLICIT);
                }
            }
        }

        /* shared deps */
        fprintf (stdout, "%*s", -len_max, data.group[DEP_SHARED].title);
        print_size (data.group[DEP_SHARED].size);
        fputc ('\n', stdout);
        if (config.list_shared)
        {
            list_dependencies (&data, DEP_SHARED);
        }

        if (config.explicit)
        {
            /* shared explicit deps */
            fprintf (stdout, "%*s",
                    -len_max,
                    data.group[DEP_SHARED_EXPLICIT].title);
            print_size (data.group[DEP_SHARED_EXPLICIT].size);
            if (data.group[DEP_SHARED].size > 0
                    && data.group[DEP_SHARED_EXPLICIT].size > 0)
            {
                fputs (" (", stdout);
                print_size (size_shared);
                fputs (")\n", stdout);
            }
            else
            {
                fputc ('\n', stdout);
            }
            if (config.list_shared_explicit)
            {
                list_dependencies (&data, DEP_SHARED_EXPLICIT);
            }
        }

        /* total deps */
        fprintf (stdout, "%*s", -len_max, data.group[DEP_UNKNOWN].title);
        print_size (size_exclusive + size_shared + size_optional);
        fputs (" (", stdout);
        print_size (data.group[DEP_UNKNOWN].size
                + size_exclusive
                + size_shared
                + size_optional);
        fputs (")\n", stdout);

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

        /* next */
        if (++optind < argc)
        {
            fputc ('\n', stdout);
        }
    }

    debug ("release libalpm\n");
    alpm_release (config.alpm);
    alpm_list_free (config.localdb);
    return 0;
}

