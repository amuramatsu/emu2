#define _GNU_SOURCE

#include "dosnames.h"
#include "codepage.h"
#include "dbg.h"
#include "emu.h"
#include "env.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static enum DOSNAME_MODE mode;
void dosname_mode(enum DOSNAME_MODE m)
{
    mode = m;
}

// DOS directory entries.
// Functions to open/search files.

static char dos_valid_char(char c, int *dos_valid_char_in_dbcs)
{
    if(mode == DOSNAME_DBCS && *dos_valid_char_in_dbcs)
    {
        *dos_valid_char_in_dbcs = 0;
        return c;
    }
    if(c >= '0' && c <= '9')
        return c;
    if(c >= 'A' && c <= 'Z')
        return c;
    if(c == '!' || c == '#' || c == '$' || c == '%' || c == '&' || c == '\'' ||
       c == '(' || c == ')' || c == '-' || c == '@' || c == '^' || c == '_' || c == '{' ||
       c == '}' || c == '~')
        return c;
    if(mode == DOSNAME_DBCS && check_dbcs_1st(c))
    {
        *dos_valid_char_in_dbcs = 1;
        return c;
    }
    if(c >= 'a' && c <= 'z')
        return c - 'a' + 'A';
    if(mode == DOSNAME_8BIT || mode == DOSNAME_DBCS)
    {
        if((unsigned char)c >= 0xa0 && (unsigned char)c < 0xff)
            return c;
    }
    return 0;
}

// Converts the Unix filename "u" to a Dos filename "d".
static int unix_to_dos(uint8_t *d, const char *u)
{
    int dot;
    int k;
    int in_dbcs = 0;

    uint8_t *buffer = malloc(strlen(u) * 2 + 1);
    if(buffer)
    {
        uint8_t *dst = buffer;
        while(*u)
        {
            uint8_t c = *(uint8_t *)u;
            if(c < 128)
                *dst++ = *u++;
            else if(c >= 0xf0)
            {
                *dst++ = '~';
                for(int i = 0; i < 4 && *u; u++)
                    /*NOP*/;
            }
            else
            {
                uint16_t unicode = utf8_to_unicode((const uint8_t **)&u);
                if(unicode)
                {
                    int n, c1, c2;
                    n = get_dos_char(unicode, &c1, &c2);
                    if(n == 1)
                        *dst++ = c1;
                    else if(n == 2)
                    {
                        *dst++ = c1;
                        *dst++ = c2;
                    }
                }
                else
                {
                    *dst++ = '~';
                    break;
                }
            }
        }
        *dst = '\0';
        u = (char *)buffer;
    }

    for(k = 0; *u && *u != '.' && k < 8; k++, u++, d++)
    {
        char c = dos_valid_char(*u, &in_dbcs);
        if(c)
            *d = c;
        else
            *d = '~';
    }
    in_dbcs = 0;
    dot = k;
    // Search dot
    while(*u && *u != '.')
        u++;
    if(*u && *(u + 1))
    {
        *d = '.';
        d++;
        u++;
        for(k = 0; *u && k < 3; k++, u++, d++)
        {
            char c = dos_valid_char(*u, &in_dbcs);
            if(c)
                *d = c;
            else
                *d = '~';
        }
    }
    if(buffer)
        free(buffer);
    return dot;
}

// Search a name in the current directory list
static int dos_search_name(const struct dos_file_list *dl, const uint8_t *name)
{
    for(; dl->unixname; dl++)
    {
        if(!strncmp((const char *)dl->dosname, (const char *)name, 13))
            return 1;
    }
    return 0;
}

static const struct dos_file_list *dos_search_unix_name(const struct dos_file_list *dl,
                                                        const char *name)
{
    for(; dl && dl->unixname; dl++)
    {
        if(!strcmp(dl->unixname, name))
            return dl;
    }
    return 0;
}

// Sort unix entries so that '~' and '.' comes before other chars
static int dos_unix_sort(const struct dirent **s1, const struct dirent **s2)
{
    const char *n1 = (*s1)->d_name;
    const char *n2 = (*s2)->d_name;
    int in_dbcs1 = 0;
    int in_dbcs2 = 0;
    for(;; n1++, n2++)
    {
        char c1 = dos_valid_char(*n1, &in_dbcs1);
        char c2 = dos_valid_char(*n2, &in_dbcs2);
        if(c1 && c1 == c2)
            continue;
        if(*n1 && *n1 == *n2)
            continue;
        if(!*n1 && !*n2)
        {
            // The two DOS-visible names are equal,
            // sort by Unix name.
            return strcmp((*s1)->d_name, (*s2)->d_name);
        }
        if(!*n1)
            return -1;
        if(!*n2)
            return 1;
        if(*n1 == '.')
            return -1;
        if(*n2 == '.')
            return 1;
        if(*n1 == '~')
            return -1;
        if(*n2 == '~')
            return 1;
        if(!c1 && !c2)
            return *n1 - *n2;
        if(!c1)
            return 1;
        if(!c2)
            return -1;
        return c1 - c2;
    }
}

// GLOB
static int dos_glob(const uint8_t *n, const char *g)
{
    int in_dbcs_g = 0;
    int in_dbcs_n = 0;
    while(*n && *g)
    {
        char cg = *g, cn = *n;
        // An '*' consumes any letter, except the dot
        if(!in_dbcs_g && cg == '*')
        {
            // Special case "." and ".."
            if(!in_dbcs_n && cn == '.' && (n[1] != '.' && n[1] != 0))
                g++;
            else
                n++;
            continue;
        }
        // An '?' consumes one letter, except the dot
        if(!in_dbcs_g && cg == '?')
        {
            g++;
            if(!in_dbcs_n && cn != '.')
                n++;
            continue;
        }
        // Convert letters to uppercase
        if(!in_dbcs_g && cg >= 'a' && cg <= 'z')
            cg = cg - 'a' + 'A';
        if(!in_dbcs_n && cn >= 'a' && cn <= 'z')
            cn = cn - 'a' + 'A';
        // DBCS check
        if(in_dbcs_g)
            in_dbcs_g = 0;
        else if(mode == DOSNAME_DBCS && check_dbcs_1st(cg))
            in_dbcs_g = 1;
        if(in_dbcs_n)
            in_dbcs_n = 0;
        else if(mode == DOSNAME_DBCS && check_dbcs_1st(cn))
            in_dbcs_n = 1;
        // Consume equal letters or '?'
        if(cg == cn)
        {
            g++;
            n++;
            continue;
        }
        return 0;
    }
    if(mode == DOSNAME_DBCS && in_dbcs_g)
        return 0;
    // Consume extra '*', '?' and '.'
    while(*g == '*' || *g == '?' || *g == '.')
        g++;
    if(*n || *g)
        return 0;
    return 1;
}
// DOS files are 8 chars name, 3 chars extension, uppercase only.
// We read the full directory and convert filenames to dos names,
// then we can search the correct ones. 'path' is the Unix path,
// returning all the files matching with glob.
static struct dos_file_list *dos_read_dir(const char *path, const char *glob, int label,
                                          int dirs)
{
    struct dirent **dir;
    struct dos_file_list *ret;

    int n = scandir(path, &dir, 0, dos_unix_sort);
    if(n < 0 || !(n || label))
        return 0;

    // Always allocate two extra items: the drive label and the terminating element
    ret = calloc(n + 2, sizeof(struct dos_file_list));
    if(!ret)
    {
        for(int i = 0; i < n; free(dir[i]), i++)
            ;
        free(dir);
        return 0;
    }
    struct dos_file_list *dirp = ret;

    // Adds label
    if(label)
    {
        dirp->unixname = strdup("//");
        memcpy(dirp->dosname, "DISK LABEL", 11);
        dirp++;
    }

    int i;
    for(i = 0; i < n; free(dir[i]), i++)
    {
        char *fpath = malloc(strlen(path) + strlen(dir[i]->d_name) + 2);
        if(!fpath || -1 == sprintf(fpath, "%s/%s", path, dir[i]->d_name))
        {
            free(fpath);
            continue;
        }
        // Special case "." and "..", only on "full directory" mode
        if(!strcmp(dir[i]->d_name, ".") || !strcmp(dir[i]->d_name, ".."))
        {
            if(dirs != 2)
            {
                free(fpath);
            }
            else
            {
                memcpy(dirp->dosname, dir[i]->d_name, strlen(dir[i]->d_name) + 1);
                dirp->unixname = fpath;
                dirp++;
            }
            continue;
        }
        // Check if it is a folder
        if(!dirs)
        {
            struct stat st;
            if((0 == stat(fpath, &st)) && S_ISDIR(st.st_mode))
            {
                free(fpath);
                continue;
            }
        }
        // Ok, add to list
        int dot = unix_to_dos(dirp->dosname, dir[i]->d_name);
        if(!dot)
        {
            free(fpath);
            continue;
        }
        // Search new DOS name in the list so far
        int pos = dot;
        int n = 0, max = 0;
        while(pos && dos_search_name(ret, dirp->dosname))
        {
            // Change the name... append "~" before dot.
            if(n >= max)
            {
                pos--;
                max *= 10;
                if(!max)
                    max = 1;
                n = 0;
                dirp->dosname[pos] = '~';
            }
            int k = pos + 1, d = max / 10;
            while(d)
            {
                dirp->dosname[k] = '0' + ((n / d) % 10);
                d /= 10;
                k++;
            }
            n++;
        }
        if(!pos)
        {
            free(fpath);
            continue;
        }
        // Ok add to the list
        dirp->unixname = fpath;
        dirp++;
    }
    free(dir);

    // Now, filter the list with the glob pattern
    struct dos_file_list *d;
    for(dirp = ret, d = ret; dirp->unixname; dirp++)
    {
        if(dos_glob(dirp->dosname, glob))
        {
            *d = *dirp;
            d++;
        }
        else
        {
            free(dirp->unixname);
            dirp->unixname = 0;
        }
    }
    d->unixname = 0;
    return ret;
}

void dos_free_file_list(struct dos_file_list *dl)
{
    struct dos_file_list *d = dl;
    if(!d)
        return;
    while(d->unixname)
    {
        free(d->unixname);
        d++;
    }
    free(dl);
}

// Transforms a string to uppercase
static void str_ucase(char *str)
{
    for(; *str; str++)
        if(*str >= 'a' && *str <= 'z')
            *str = *str - ('a' - 'A');
}

// Transforms a string to lowercase
static void str_lcase(char *str)
{
    for(; *str; str++)
        if(*str >= 'A' && *str <= 'Z')
            *str = *str + ('a' - 'A');
}

////////////////////////////////////////////////////////////////////
// Converts a DOS filename to Unix filename, at the given path
static char *dos_unix_name(const char *path, const char *dosN, int force)
{
    // First, try the name as given:
    struct stat st;
    const char *bpath = strcmp(path, "/") ? path : "";
    // Allocate a buffer big enough
    char *ret = malloc(strlen(bpath) + strlen(dosN) * 3 + 2);
    if(!ret)
        return 0;
    if(-1 == sprintf(ret, "%s/", bpath))
    {
        free(ret);
        return 0;
    }
    {
        const uint8_t *src = (const uint8_t *)dosN;
        uint8_t *dst = (uint8_t *)(ret + strlen(ret));
        while(*src)
        {
            int uc = get_unicode(*src++, NULL);
            if(uc == 0)
                /*NOP*/;
            else
                unicode_to_utf8(&dst, uc);
        }
        *dst = '\0';
    }
    if(0 == stat(ret, &st))
        return ret;
    // See if 'dosN' has glob patterns, and exists in that case,
    // so we don't expand path if it contains '*' or '?' chars
    const char *s;
    int in_dbcs = 0;
    for(s = dosN; *s; s++)
    {
        if(in_dbcs)
            in_dbcs = 0;
        else if(check_dbcs_1st(*s))
            in_dbcs = 1;
        else if(*s == '?' || *s == '*')
            return ret;
    }
    // Try converting to uppercase...
    str_ucase(ret + strlen(bpath));
    if(0 == stat(ret, &st))
        return ret;
    // Try converting to lowercase...
    str_lcase(ret + strlen(bpath));
    if(0 == stat(ret, &st))
        return ret;
    // Finally, do a full directory search
    struct dos_file_list *dl = dos_read_dir(bpath, dosN, 0, 1);
    if(!dl || !dl->unixname)
    {
        // The filename does not exists, returns the lowercase version
        dos_free_file_list(dl);
        if(force)
            return ret;
        else
        {
            free(ret);
            return 0;
        }
    }
    free(ret);
    ret = strdup(dl->unixname);
    dos_free_file_list(dl);
    return ret;
}

static const char *get_last_separator(const char *path)
{
    const char *ret = 0;
    int in_dbcs = 0;
    while(*path)
    {
        if(in_dbcs)
        {
            path++;
            in_dbcs = 0;
            continue;
        }
        if(mode == DOSNAME_DBCS && check_dbcs_1st(*path))
            in_dbcs = 1;
        else if(*path == '\\' || *path == '/')
            ret = path;
        path++;
    }
    return ret;
}

// Recursive conversion, convert the first component and adds to the
// already converted.
static char *dos_unix_path_rec(const char *upath, const char *dospath, int force)
{
    // Search for last '\' or '/'
    const char *p = get_last_separator(dospath);
    if(!p)
    {
        // No more subdirs, convert filename
        return dos_unix_name(upath, dospath, force);
    }
    char *part1 = strndup(dospath, p - dospath);
    char *part2 = strdup(p + 1);
    char *path = dos_unix_path_rec(upath, part1, force);
    char *ret = 0;
    if(path)
    {
        ret = dos_unix_name(path, part2, force);
        free(path);
    }
    free(part1);
    free(part2);
    return ret;
}

// CWD for all drives - 'A' to 'Z'
static uint8_t dos_cwd[26][64];
static int dos_default_drive = 2; // C:

void dos_set_default_drive(int drive)
{
    if(drive < 26)
        dos_default_drive = drive;
}

int dos_get_default_drive(void)
{
    return dos_default_drive;
}

// Checks if char is a valid path name character
static int char_valid(unsigned char c)
{
    if(c < 33 || c == '/' || c == '\\')
        return 0;
    else
        return 1;
}

// Checks if char is a valid path separator
static int char_pathsep(unsigned char c)
{
    if(c == '/' || c == '\\')
        return 1;
    else
        return 0;
}

// Normalizes DOS path, removing relative items and adding base
// Modifies the passed string and returns the drive as integer.
int dos_path_normalize(char *path, unsigned max)
{
    int drive = dos_default_drive;

    // Force nul terminated
    path[max] = 0;

    if(path[0] && path[1] == ':')
    {
        drive = path[0];
        if(drive >= 'A' && drive <= 'Z')
            drive = drive - 'A';
        else if(drive >= 'a' && drive <= 'z')
            drive = drive - 'a';
        else
            drive = dos_default_drive;
        memmove(path, path + 2, max - 1);
        path[max - 1] = path[max] = 0;
    }

    // Copy CWD to base
    char base[max + 1];
    memcpy(base, dos_cwd[drive], max + 1);
    // Test for absolute path
    if(path[0] == '\\' || path[0] == '/')
        memset(base, 0, max + 1);

    // Process each component of path
    int beg, end = 0;
    while(end < max && path[end])
    {
        beg = end;
        if(mode == DOSNAME_DBCS)
        {
            int in_dbcs = 0;
            while(path[end])
            {
                if(in_dbcs)
                    in_dbcs = 0;
                else if(check_dbcs_1st(path[end]))
                    in_dbcs = 1;
                else if(!char_valid(path[end]))
                    break;
                end++;
            }
        }
        else
            while(char_valid(path[end]))
                end++;

        if(path[end] && !char_pathsep(path[end]))
            path[end] = 0;
        if(!path[end] && end < max)
            path[end + 1] = 0;

        // Test path
        path[end] = 0;
        if(!strcmp(&path[beg], ".."))
        {
            if(mode == DOSNAME_DBCS)
            {
                // parse start of begin, for dbcs check
                int e1 = 0;
                int e2 = 0;
                int in_dbcs = 0;
                int prev_pathsep = 0;
                while(base[e1])
                {
                    if(in_dbcs)
                    {
                        in_dbcs = 0;
                        prev_pathsep = 0;
                    }
                    else if(check_dbcs_1st(base[e1]))
                    {
                        in_dbcs = 1;
                        prev_pathsep = 0;
                    }
                    else if(char_pathsep(base[e1]))
                    {
                        if(!prev_pathsep)
                            e2 = e1;
                        prev_pathsep = 1;
                    }
                    else
                        prev_pathsep = 0;
                    e1++;
                }
                base[e2] = 0;
            }
            else
            {
                // Up a directory
                int e = strlen(base) - 1;
                while(e >= 0 && !char_pathsep(base[e]))
                    e--;
                while(e >= 0 && char_pathsep(base[e]))
                    e--;
                base[e + 1] = 0;
            }
        }
        else if(path[beg] && strcmp(&path[beg], "."))
        {
            // Standard path, add to base
            int e = strlen(base);
            if(e < max)
            {
                if(e)
                {
                    base[e] = '\\';
                    e++;
                }
                while(e < max - 1 && path[beg])
                {
                    base[e] = path[beg];
                    e++;
                    beg++;
                }
                base[e] = 0;
            }
        }
        end++;
    }
    // Copy result
    memcpy(path, base, max + 1);
    return drive;
}

void make_fcbname(char *dos_shortname, const char *path)
{
    int p = 0;
    int s = 0;
    int in_dbcs = 0;
    if(path[0] && path[1] == ':')
        p = 2;
    while(path[p])
    {
        if(in_dbcs)
            in_dbcs = 0;
        else if(mode == DOSNAME_DBCS && check_dbcs_1st(path[p]))
            in_dbcs = 1;
        else if(char_pathsep(path[p]))
            s = p + 1;
        p++;
    }

    if(s == 0)
    {
        memset(dos_shortname, ' ', 11);
        return;
    }

    in_dbcs = 0;
    int pos;
    for(pos = 0; path[s] && pos < 8; pos++)
    {
        if(path[s] == '.')
            while(pos < 8)
                dos_shortname[pos++] = ' ';
        else
            dos_shortname[pos] = dos_valid_char(path[s], &in_dbcs);
        ;
        s++;
    }
    in_dbcs = 0;
    for(; path[s] && pos < 11; pos++)
        dos_shortname[pos] = dos_valid_char(path[s++], &in_dbcs);
    for(; pos < 11; pos++)
        dos_shortname[pos] = ' ';
}

// Get UNIX base path:
static const char *get_base_path(int drive)
{
    char env[15] = ENV_DRIVE "\0\0";
    env[strlen(env)] = drive + 'A';
    char *base = getenv(env);
    if(!base)
        return ".";
    return base;
}

const uint8_t *dos_get_cwd(int drive)
{
    drive = drive ? drive - 1 : dos_default_drive;
    return dos_cwd[drive];
}

// changes CWD
int dos_change_cwd(char *path)
{
    debug(debug_dos, "\tchdir '%s'\n", path);
    int drive = dos_path_normalize(path, 63);
    // Check if path exists
    char *fname = dos_unix_path_rec(get_base_path(drive), path, 0);
    if(!fname)
        return 1;
    struct stat st;
    int e = stat(fname, &st);
    free(fname);
    if(0 != e || !S_ISDIR(st.st_mode))
        return 1;
    // Ok, change current path
    memcpy(dos_cwd[drive], path, 64);
    return 0;
}

// changes CWD
int dos_change_dir(int addr)
{
    return dos_change_cwd(getstr(addr, 63));
}

static char *dos_unix_path_base(char *path, int force)
{
    // Normalize
    int drive = dos_path_normalize(path, 63);
    // Get UNIX base path:
    const char *base = get_base_path(drive);
    // Adds CWD if path is not absolute
    return dos_unix_path_rec(base, path, force);
}

// Search a file in each possible "append" path component
static char *search_append_path(char *path, const char *append)
{
    // Now we concatenate each of the append paths:
    while(*append)
    {
        // Skip separators
        while(*append == ';')
            append++;
        if(*append)
        {
            // Find the end of token
            const char *p = append;
            while(*append != ';' && *append)
                append++;

            // Construct new path:
            char full_path[64];
            if(snprintf(full_path, 64, "%.*s\\%s", (int)(append - p), p, path) < 64)
            {
                debug(debug_dos, "\tconvert dos path '%s'\n", full_path);
                char *result = dos_unix_path_base(full_path, 0);
                if(result)
                    return result;
            }
        }
    }
    return 0;
}

// Converts a DOS full path to equivalent Unix filename
char *dos_unix_path(int addr, int force, const char *append)
{
    char *path = getstr(addr, 63);
    debug(debug_dos, "\tconvert dos path '%s'\n", path);
    // Check for standard paths:
    if(*path && (!strcasecmp(path, "NUL") || !strcasecmp(path + 1, ":NUL")))
        return strdup("/dev/null");
    if(*path && (!strcasecmp(path, "CON") || !strcasecmp(path + 1, ":CON")))
        return strdup("/dev/tty");
#ifdef EMS_SUPPORT
    if(use_ems && *path &&
       (!strcasecmp(path, "EMMXXXX0") || !strcasecmp(path + 1, ":EMMXXXX0")))
        return strdup("/dev/null");
#endif
    // Try to convert
    char *result = dos_unix_path_base(path, force);
    // Be done if the path is found, or no append.
    if(result || !append)
        return result;
    // Restore original path, and see if path is absolute, so we don't append
    path = getstr(addr, 63);
    if(!char_valid(path[0]) || (path[1] == ':' && !char_valid(path[2])))
        return result;
    return search_append_path(path, append);
}

// Converts a FCB path to equivalent Unix filename
char *dos_unix_path_fcb(int addr, int force, const char *append)
{
    // Copy drive number from the FCB structure:
    int drive = get8(addr) & 0xFF;
    if(!drive)
        drive = dos_default_drive;
    else
    {
        drive = drive - 1;
        // Don't append if drive is specified.
        append = 0;
    }
    // And copy file name
    char *fcb_name = getstr(addr + 1, 11);
    debug(debug_dos, "\tconvert dos fcb name %c:'%s'\n", drive + 'A', fcb_name);

    // Build filename from FCB
    char filename[13];
    int opos = 0;
    int in_dbcs = 0;

    for(int pos = 0; pos < 8 && opos < 13; pos++, opos++)
        if(in_dbcs == 0 && fcb_name[pos] == '?')
            filename[opos] = '?';
        else if(0 == (filename[opos] = dos_valid_char(fcb_name[pos], &in_dbcs)))
            break;
    if(opos < 63 && (dos_valid_char(fcb_name[8], &in_dbcs) || fcb_name[8] == '?'))
        filename[opos++] = '.';
    in_dbcs = 0;
    for(int pos = 8; pos < 11 && opos < 63; pos++, opos++)
        if(in_dbcs == 0 && fcb_name[pos] == '?')
            filename[opos] = '?';
        else if(0 == (filename[opos] = dos_valid_char(fcb_name[pos], &in_dbcs)))
            break;
    filename[opos] = 0;

    // Build complete path, copy current directory and add FCB file name
    char path[64];
    memcpy(path, dos_cwd[drive], 64);

    if(snprintf(path, 64, "%s\\%s", dos_cwd[drive], filename) >= 64)
        return 0; // Path too long

    debug(debug_dos, "\ttemp name '%s'\n", path);
    // Get UNIX base path:
    const char *base = get_base_path(drive);
    // Adds CWD if path is not absolute
    char *result = dos_unix_path_rec(base, path, force);
    // Be done if the path is found, or no append.
    if(result || !append)
        return result;

    // Now, try append paths - using full path resolution
    return search_append_path(filename, append);
}

////////////////////////////////////////////////////////////////////
// Implements FindFirstFile
// NOTE: this frees fspec before return
static struct dos_file_list *find_first_file(char *fspec, int label, int dirs)
{
    // Now, separate the path to the spec
    char *glob, *unixpath, *p = strrchr(fspec, '/');
    char *buffer = NULL;
    if(!p)
    {
        glob = fspec;
        unixpath = ".";
    }
    else
    {
        *p = 0;
        glob = p + 1;
        unixpath = fspec;
    }
    if(mode == DOSNAME_DBCS || mode == DOSNAME_8BIT)
    {
        buffer = malloc(strlen(glob) * 3 + 1);
        const uint8_t *src = (const uint8_t *)glob;
        uint8_t *dst = (uint8_t *)buffer;
        while(*src)
        {
            int unicode = utf8_to_unicode(&src);
            if(unicode)
            {
                int n, c1, c2;
                n = get_dos_char(unicode, &c1, &c2);
                if(n == 1)
                    *dst++ = c1;
                else if(n == 2)
                {
                    *dst++ = c1;
                    *dst++ = c2;
                }
            }
            else
            {
                *dst++ = '*';
            }
        }
        *dst = '\0';
        glob = buffer;
    }
    debug(debug_dos, "\tfind_first '%s' at '%s'\n", glob, unixpath);

    // Read the directory using the given GLOB
    struct dos_file_list *dirEntries = dos_read_dir(unixpath, glob, label, dirs);
    free(fspec);
    if(buffer)
        free(buffer);
    return dirEntries;
}

struct dos_file_list *dos_find_first_file(int addr, int label, int dirs)
{
    return find_first_file(dos_unix_path(addr, 1, 0), label, dirs ? 2 : 0);
}

struct dos_file_list *dos_find_first_file_fcb(int addr, int label)
{
    return find_first_file(dos_unix_path_fcb(addr, 1, 0), label, 1);
}

static char *dos_check_in_drive(int drive, const char *path)
{
    // Get normalized base drive path
    char *base = realpath(get_base_path(drive), 0);
    if(!base)
        return 0;
    // Now, see if the path is actually a descendent of base
    size_t l = strlen(base), k = strlen(path);
    if(strncmp(base, path, l) || (path[l] != '/' && path[l]))
    {
        debug(debug_dos, "dos_real_path: no common base for drive %c\n", drive + 'A');
        free(base);
        return 0;
    }
    else if(k - l > 62)
    {
        debug(debug_dos, "dos_real_path: path too long for DOS\n");
        free(base);
        return 0;
    }
    return base;
}

char *dos_real_path(const char *unix_path)
{
    // Normalize unix path:
    char *path = realpath(unix_path, 0);
    if(!path)
        return 0;
    debug(debug_dos, "dos_real_path: path='%s'\n", path);
    // Check default drive first:
    int drive = dos_get_default_drive();
    char *base = dos_check_in_drive(drive, path);
    if(!base)
    {
        // Now, check all other drives
        for(drive = 0; drive < 26; drive++)
        {
            base = dos_check_in_drive(drive, path);
            if(base)
                break;
        }
        if(!base)
        {
            debug(debug_dos, "dos_real_path: no common base\n");
            free(path);
            return 0;
        }
    }
    // Convert remaining components
    size_t l = strlen(base), k = strlen(path);
    char *ret = calloc(1, 65);
    if(!ret)
    {
        free(base);
        free(path);
        return 0;
    }
    ret[0] = drive + 'A';
    ret[1] = ':';

    path[l] = 0;
    while(++l < k)
    {
        // Extract one component
        char *sep = strchr(path + l, '/');
        // Cut string there
        if(!sep)
            sep = path + k;
        else
            *sep = 0;
        // And search the DOS path name
        struct dos_file_list *fl = dos_read_dir(path, "*.*", 0, 1);
        path[l - 1] = '/';
        const struct dos_file_list *sl = dos_search_unix_name(fl, path);
        if(!sl)
        {
            dos_free_file_list(fl);
            debug(debug_dos, "dos_real_path: path not found: '%s' in '%s'\n", path + l,
                  path);
            free(base);
            free(path);
            free(ret);
            return 0;
        }
        strncat(ret, "\\", 64);
        strncat(ret, (const char *)sl->dosname, 64);
        dos_free_file_list(fl);
        l = sep - path;
    }
    debug(debug_dos, "dos_real_path: located as '%s'\n", ret);
    free(base);
    free(path);
    return ret;
}
