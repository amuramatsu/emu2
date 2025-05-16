
// DOS file names to Unix file names conversions

#ifndef DOSNAMES_H
#define DOSNAMES_H

#include <stdint.h>

// Set dosnames mode
enum DOSNAME_MODE
{
    DOSNAME_7BIT,
    DOSNAME_8BIT,
    DOSNAME_DBCS
};
void dosname_mode(enum DOSNAME_MODE m);

// make 8+3 with space filename from dos path
void make_fcbname(char *dos_shortname, const char *path);

// Converts a DOS full path to equivalent Unix filename
// If the file exists, returns the name of the file.
// If the file does not exists, and "force" is true, returns the possible lowercase name.
// If the file does not exists, and "force" is false, returns null, but if append is not
// null the search is retried searching inside the append paths.
char *dos_unix_path(int addr, int force, const char *append, int lfn);

// Converts a DOS FCB file name to equivalent Unix filename
// If the file exists, returns the name of the file.
// If the file does not exists, and "force" is true, returns the possible lowercase name.
// If the file does not exists, and "force" is false, returns null, but if append is not
// null the search is retried searching inside the append paths.
char *dos_unix_path_fcb(int addr, int force, const char *append);

// Changes current working directory
int dos_change_cwd(char *path, int lfn);
int dos_change_dir(int addr, int lfn);

// Returns a DOS path representing given Unix path in drive
char *dos_real_path(const char *unix_path, int lfn);

// Gets current working directory
const uint8_t *dos_get_cwd(int drive);
const uint8_t *lfn_get_cwd(int drive);

// Sets/gets default drive
void dos_set_default_drive(int drive);
int dos_get_default_drive(void);

// Struct used as return to dosFindFirstFile
struct dos_file_list
{
    uint8_t dosname[13];
#ifdef LFN_SUPPORT
    uint8_t *lfnname;
#endif
    char *unixname;
};

// Implements "find first file" and "next file" DOS functions.
// Returns a list with filenames compatibles with the DOS filespec, as pairs
// The list will be deleted at the next call
struct dos_file_list *dos_find_first_file(int addr, int label, int dirs, int lfn);
struct dos_file_list *dos_find_first_file_fcb(int addr, int label);

// Frees a fileList.
void dos_free_file_list(struct dos_file_list *dl);

// Normalizes DOS path, removing relative items and adding base
// Modifies the passed string and returns the drive as integer.
int dos_path_normalize(char *path, unsigned max);
#endif // DOSNAMES_H
