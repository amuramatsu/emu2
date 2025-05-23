#include "dbg.h"
#include "env.h"
#include "os.h"
#include "version.h"

#include <fcntl.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

char *prog_name;

void print_version(void)
{
    printf("EMU2 - Simple x86 + DOS Emulator, version " EMU2_VERSION
#ifdef __DATE__
           "  (Compiled " __DATE__ ")"
#endif
           "\n");
}

NORETURN void print_usage(void)
{
    print_version();
    printf("\n"
           "Usage: %s [options] <prog.exe> [args...] [-- environment vars]\n"
           "\n"
           "Options (processed before program name):\n"
           "  -h            Show this help.\n"
           "  -b <addr>     Load header-less binary at address.\n"
           "  -r <seg>:<ip> Specify a run address to start execution.\n"
           "                (only for binary loaded data).\n"
           "\n"
           "Environment variables:\n"
           "  %-18s  Base name of a file to write the debug log, defaults to\n"
           "\t\t      the exe name if not given.\n"
           "  %-18s  List of debug options to activate, from the following:\n"
           "\t\t      'cpu', 'int', 'port', 'dos', 'video'.\n"
           "  %-18s  DOS program name, if not given use the unix name.\n"
           "  %-18s  DOS default (current) drive letter, if not given use 'C:'\n"
           "  %-18s  DOS current working directory, use 'C:\\' if not given.\n"
           "  %-18s  Set unix path as root of drive 'n', by default all drives\n"
           "\t\t      point to the unix working directory.\n"
           "  %-18s  Set DOS code-page. Set to '?' to show list of code-pages.\n"
           "  %-18s  Limit DOS memory to 512KB, fixes some old buggy programs.\n"
           "  %-18s  Memory initialization flags.\n"
           "                      - bit0: limit DOS memory to 512KB. (same as %s)\n"
           "                      - bit1: limit DOS memory to 640KB.\n"
           "                      - bit2: load after first 64kB.\n"
           "                        (for resolve 'Packed file corrupt' error)\n"
           "                      - bit3: A20 gate enabled by default.\n"
           "  %-18s  Specifies a DOS append paths, separated by ';'.\n"
           "  %-18s  Set version of DOS to emulate, e.g. '2.11', '3.20', etc.\n"
           "  %-18s  Set version of Windows to emulate, e.g. '3.0',\n"
           "                      '4.0'(Win95), etc.\n"
           "  %-18s  Setup text mode with given number of rows, from 12 to 50.\n"
#ifdef IA32
           "  %-18s  Whole memory size[MB], power of 2 up to 1024, default 64.\n"
#else
           "  %-18s  Whole memory size[MB], power of 2 up to 16, default 16.\n"
#endif
#ifdef EMS_SUPPORT
           "  %-18s  Use LIM-EMS 4.0. Set this variable as available pages.\n"
#endif
           "  %-18s  Filename mode (7bit, 8bit or DBCS).\n"
           "  %-18s  Exec child process in same emulator process.\n",
           prog_name, ENV_DBG_NAME, ENV_DBG_OPT, ENV_PROGNAME, ENV_DEF_DRIVE, ENV_CWD,
           ENV_DRIVE "n", ENV_CODEPAGE, ENV_LOWMEM, ENV_MEMFLAG, ENV_LOWMEM, ENV_APPEND,
           ENV_DOSVER, ENV_WINVER, ENV_ROWS, ENV_MEMSIZE,
#ifdef EMS_SUPPORT
           ENV_EMSMEM,
#endif
           ENV_FILENAME, ENV_EXEC_SAME);
    exit(EXIT_SUCCESS);
}

NORETURN void print_usage_error(const char *format, ...)
{
    va_list ap;
    fprintf(stderr, "%s: ", prog_name);
    va_start(ap, format);
    vfprintf(stderr, format, ap);
    va_end(ap);
    fprintf(stderr, "\nTry '%s -h' for more information.\n", prog_name);
    exit(EXIT_FAILURE);
}

NORETURN void print_error(const char *format, ...)
{
    va_list ap;
    fprintf(stderr, "%s: ", prog_name);
    va_start(ap, format);
    vfprintf(stderr, format, ap);
    va_end(ap);
    exit(EXIT_FAILURE);
}

static FILE *debug_files[debug_MAX];
static const char *debug_names[debug_MAX] = {"cpu", "int", "port", "dos", "video"};

static FILE *open_log_file(const char *base, const char *type)
{
    char log_name[64 + strlen(base) + strlen(type)];
    int fd = -1;
    for(int i = 0; fd == -1 && i < 1000; i++)
    {
        sprintf(log_name, "%s-%s.%d.log", base, type, i);
        fd = open(log_name, O_CREAT | O_EXCL | O_WRONLY, 0666);
    }
    if(fd == -1)
        print_error("can't open debug log '%s'\n", log_name);
    fprintf(stderr, "%s: %s debug log on file '%s'.\n", prog_name, type, log_name);
    return fdopen(fd, "w");
}

static void close_log_files(void)
{
    for(int i = 0; i < debug_MAX; i++)
        if(debug_files[i] != 0)
        {
            fclose(debug_files[i]);
            debug_files[i] = 0;
        }
}

void init_debug(const char *base)
{
    if(getenv(ENV_DBG_NAME))
        base = getenv(ENV_DBG_NAME);
    if(getenv(ENV_DBG_OPT))
    {
        // Parse debug types:
        const char *spec = getenv(ENV_DBG_OPT);
        for(int i = 0; i < debug_MAX; i++)
        {
            if(strstr(spec, debug_names[i]))
                debug_files[i] = open_log_file(base, debug_names[i]);
        }
        atexit(close_log_files);
    }
}

int debug_active(enum debug_type dt)
{
    if(dt < debug_MAX)
        return debug_files[dt] != 0;
    else
        return 0;
}

void debug(enum debug_type dt, const char *format, ...)
{
    va_list ap;
    if(debug_active(dt))
    {
        va_start(ap, format);
        vfprintf(debug_files[dt], format, ap);
        va_end(ap);
        fflush(debug_files[dt]);
    }
}
