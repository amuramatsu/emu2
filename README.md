EMU2: A simple text-mode x86 + DOS emulator
-------------------------------------------

This is a simple DOS emulator for the Linux text console, supporting basic DOS
system calls and console I/O.

Some extended features are implemented from
[dmsc's original version](https://github.com/dmsc/emu2.git).

* Experimental IA-32 CPU core implementation. This cpu core is came from
  [NekoProject 21/w](https://simk98.github.io/np21w/).  
  Pentium instruction set and x87 FPU, MMX, SSE, SSE2, SSE3, SSSE3, SSE4 and
  SSE4a instructions are supported. Almost protect mode features are also supported.  
  IA-32 CPU core is **4x slower** than original 286-realmode core.
* XMS 3.0.
* LIM-EMS 4.0.
* Some peripherals needed to run DOS extender (a20 gate, 8259 PIC).
* DBCS codepage support.  
  Some Japanese applications ([VZEditor](https://github.com/vcraftjp/VZEditor), 
  FD, MIEL, FILMNT, LHMNT..) can work on this with `EMU2_CODEPAGE=932`.
  DBCS filenames are also supported.
* Load and exec (ah=4B00h) and loading only (ah=4B01h) are implemented as real MS-DOS.   
  Some debuggers like as TurboC++ IDE, [Enhanced DEBUG](https://pcdosretro.gitlab.io/enhdebug.htm)
  are working with debuggee without any options.
  If you want to use batch file which contains some TSRs and applications, you can use 
  [FreeCOM](https://github.com/FDOS/freecom) with environment variable `EMU2_EXEC_SAMEPROC=1`.
* Long File Name support like as Windows 95 DOS prompt.

Installation
------------

    make
    sudo make install

or

    make IA32=1
    sudo make IA32=1 install

The above installs `emu2` or `emu2-ia32` into `$(DESTDIR)${PREFIX}/bin/emu2` or
`$(DESTDIR)${PREFIX}/bin/emu2-ia32`
this is `/usr/bin/emu2` and `/usr/bin/emu2-ia32` by default.

Using the emulator
------------------

To run a DOS `.exe` or `.com` file, simply load it with

    emu2 myprog.exe

or

    emu2-ia32 myprog.exe

The emulator accepts some options in the command line and more options as
environment variables, this allows child process (programs run by your DOS
program) to inherit the configuration.

The full usage is:

    emu2 [options] <prog.exe> [args...] [-- environment vars]

Options (should be placed *before* the DOS program name):
- `-h`        Shows a brief help.

- `-b addr`   Load header-less binary at given address (to load ROMs or test data).

- `-r <seg>:<ip>`  Specify a run address to start execution (only for binary loaded data).

The available environment variables are:
- `EMU2_DEBUG_NAME`    Base name of a file to write the debug log, defaults to
                       the exe name if not given.

- `EMU2_DEBUG`         List of debug options to activate, from the following:
                       `cpu`, `int`, `port`, `dos`, `video`.

- `EMU2_PROGNAME`      DOS program name, if not given try to convert the unix
                       name to an equivalent DOS path.

- `EMU2_DEFAULT_DRIVE` DOS default (current) drive letter, if not given use `C:`

- `EMU2_CWD`           DOS current working directory, if not given tries to convert
                       the current directory to the equivalent DOS path inside the
                       DOS default drive, or `C:\` if not possible.

- `EMU2_DRIVE_`n       Set unix path as root of drive `n`, by default all drives
                       point to the unix working directory.

- `EMU2_APPEND`        Sets a list of paths to search for data files on open,
                       emulating the DOS `APPEND` command. Only files with a
                       relative path are included in the search, and the search
                       is relative to the current working directory if no drive
                       letter is specified in the append path.
                       For example, if set to "`TXT;C:\IN`", when opening the
                       file "`CAT.TXT`" the file is searched as "`CAT.TXT`",
                       "`TXT\CAT.TXT`" and "`C:\IN\CAT.TXT`" in turn.

- `EMU2_CODEPAGE`      Set DOS code-page to the specified string. Set to '?' to
                       show list of included code-pages, multiple aliases
                       separated with commas.  Set to a file name to read the
                       mapping table from a file with the unicode value for
                       each byte.  You can download mapping tables from
                       ftp://ftp.unicode.org/Public/MAPPINGS/VENDORS/MICSFT/PC/
                       The code-page setting affects keyboard input and screen
                       output, but does not change the DOS NLS information.
                       The default code-page is CP437.

- `EMU2_LOWMEM`        Limits main memory to 512KB, this fixes some old DOS
                       programs with a bug that checks available memory using
                       "signed" comparison instructions (JLE instead of JBE).
                       This is needed at least for MASM versions 1.0 and 1.10.

- `EMU2_MEMFLAG`       Memory flags:
    - bit0: limit DOS memory to 512KB. (same as `EMU2_LOWMEM`)
    - bit1: limit DOS memory to 640KB.
    - bit2: load after first 64kB. This is same effect of LOADFIX utilities.
            (for resolve 'Packed file corrupt' error)
    - bit3: A20 gate enabled by default.

- `EMU2_DOSVER`        Changes the reported DOS version, allowing programs that
                       checks this version to run.  You can specify a major
                       version or a major dot minor, for example "3.20", "2.11"
                       or "5".

- `EMU2_WINVER`        Changes the reported Windows version, allowing programs
                       that checks running on Windows or not. You can specify a
                       major version or a major dot minor, for example "3.1" or
                       "4.0" (4.0 is used by Windows 95).

- `EMU2_ROWS`          Setups the VGA text mode to the given number of rows,
                       from 12 to 50 at the program start. Some full-screen DOS
                       programs will retrieve this info and adjust the screen
                       properly, some other will ignore this and setup the text
                       mode again.

- `EMU2_MEMSIZE`       Virtual machines memory size (conventional memory +
                       extended memory).
                       This values must be power of 2, between 2 to 16 (286 CPU
                       core) or 2 to 1024 (ia32 CPU core), MiB unit.
                       The default is 16 (286) or 64 (ia32).

- `EMU2_EMSMEM`        Use LIM-EMS 4.0. Set this variable as available pages
                       between 0 (no use) to 2048 (32MiB).
                       The default is 0 (no use).

- `EMU2_FILENAME`      Filename handling/convertion mode. You can choise from
                       "7bit" (not supported with 8-bit charactors), "8bit"
                       (8-bit charactors are supported, but there are no care
                       about upper/lower cases) or "dbcs" (DBCS file names are
                       supported with 8-bit charactors.) Default is "7bit."

- `EMU2_EXEC_SAMEPROC` In default, intr 21h exec function (ax=4b00) is execute
                       MS-DOS file in other emulator process. If this variable
                       is set, emu2 exec MS-DOS binary in same emulator process
                       like as real MS-DOS.

Simple Example
--------------

For a simple example, we can run the Turbo Pascal 3, available from the antique
software collection as a file `tp302.zip`.

First, make a new directory and unzip the file:

    $ mkdir tp302
    $ cd tp302
    $ unzip ../tp302.zip
    $ ls
    ACCESS3.BOX   CALC.PAS      DEMO-BCD.PAS  GRAPH.P     SUBDIR.PAS    TURBO.COM
    ART.PAS       CMDLINE.PAS   EXTERNAL.DOC  LISTER.PAS  TINST.COM     TURBO.MSG
    CALCDEMO.MCS  COLOR.PAS     GETDATE.PAS   README      TINST.MSG     TURTLE.PAS
    CALC.HLP      DEMO1-87.PAS  GETTIME.PAS   README.COM  TURBO-87.COM  WINDOW.PAS
    CALC.INC      DEMO2-87.PAS  GRAPH.BIN     SOUND.PAS   TURBOBCD.COM
    $ chmod +w *

The last is necessary as you will want to to modify the program files afterwards.

The main file is named "TURBO.COM", there is also a "README.COM" to read further
info, try that:

    $ emu2 README.COM

![Image of README.COM](doc/readme.com.png)

You can press exit to return to the command line. Now, you can configure the compiler,
as the README said, just load `TINST.COM`, press "S" (screen type), "0" (default), "N"
(screen does not blink) and "Q" to quit.

    $ emu2 TINST.COM

                       TURBO Pascal installation menu.
                 Choose installation item from the following:

    [S]creen type |  [C]ommand installation  |  [M]sg file path  |  [Q]uit


                             Enter S, C, M or Q: 

    Choose one of the following displays:

      0)  Default display mode
      1)  Monochrome display
      2)  Color display 80x25
      3)  Color display 40x25
      4)  b/w   display 80x25
      5)  b/w   display 40x25

    Which display? (Enter no. or ^Q to exit):

    Does your screen blink when the text scrolls? (Y/N):

Finally, we are ready to run the program:

    $ emu2 TURBO.COM
    ----------------------------------------
    TURBO Pascal system        Version 3.02A
                                      PC-DOS
                                            
    Copyright (C) 1983,84,85,86 BORLAND Inc.
    ----------------------------------------
                                            
    Default display mode                    
                                            
                                            
                                            
    Include error messages (Y/N)?           

    Logged drive: C                         
    Active directory: \                     
                                            
    Work file:                              
    Main file:                              
                                            
    Edit     Compile  Run   Save            
                                            
    Dir      Quit  compiler Options         
                                            
    Text:     0 bytes                       
    Free: 62024 bytes                       
                                            
    >                                       

Try loading a program, use "e" to edit, type "window.pas", and you are in the editor:

          Line 1    Col 1   Insert    Indent  C:WINDOW.PAS                          
    program TestWindow;                                                             
    {$C-}                                                                           
    {                                                                               
                  WINDOW DEMONSTRATION PROGRAM  Version 1.00A                       
                                                                                    
           This program demonstrates the use of windows on the IBM PC               
           and true compatibles.                                                    
                                                                                    
           PSEUDO CODE                                                              
           1.  MakeWindow        - draws window boxes on the screen                 
           2.  repeat                                                               
                 UpdateWindow 1  - scrolls the window contents up or                
                                   down for each window.                            
                 UpdateWindow 2                                                     
                 UpdateWindow 3                                                     
               until a key is pressed                                               
           3.  Reset to full screen window                                          
                                                                                    
           INSTRUCTIONS                                                             
           1.  Compile this program using the TURBO.COM compiler.                   
           2.  Type any key to exit the program.                                    
    }                                                                               
                                                                                    
                                                                                    

To exit the editor, type "CONTROL+K" and "D", in the prompt you can now type "R" to
compile and run the program.

![Image of WINDOW.PAS running](doc/window.pas.png)

Advanced Example
----------------

For a more advanced example, we can install and run Turbo Pascal 5.5, available from the same
antique software collection as a file `tp55.zip`.

First, make a new directory and unzip the file:

    $ mkdir tp55
    $ cd tp55
    $ unzip ../tp55.zip
    $ ls
    Disk1 Disk2

As you see, the program was distributed in two disks, and must be installed before running.

To install, let's first copy all the contents to one directory:

    $ mkdir all
    $ cp -r Disk1/* Disk2/* all/

And now, run the emulator giving the correct paths to simulate a floppy drive:

    $ EMU2_DEFAULT_DRIVE=A EMU2_DRIVE_A=all emu2 all/INSTALL.EXE

![Image of TP55 INSTALL.EXE](doc/tp55.inst-1.png)

Type enter, enter again to install from drive "A", again to install on a hard-drive, go down
to "Start Installation" and enter again. The install program shows an error, this is because
we copied all the content to one drive. Simply type "S" to skip all errors.

After the installation is finished, we must run the install again, to copy the missing files
from before, with the same command line:

    $ EMU2_DEFAULT_DRIVE=A EMU2_DRIVE_A=all emu2 all/INSTALL.EXE

![Image of TP55 INSTALL.EXE at the end](doc/tp55.inst-2.png)

Again, press enter to the questions and go to "Start Installation", this time will complete
without errors.

You can now compile from the command line, as:

    $ emu2 tp/tpc.exe -- 'PATH=C:\TP'
    Turbo Pascal Version 5.5  Copyright (c) 1983,89 Borland International
    Syntax: TPC [options] filename [options]
    /B	Build all units		/$A-	No word alignment
    /Dxxx	Define conditionals	/$B+	Complete boolean evaluation
    /Exxx	EXE & TPU directory	/$D-	No debug information
    /Fxxx	Find run-time error	/$E-	No 8087 emulation
    /GS	Map file with segments	/$F+	Force FAR calls
    /GP	Map file with publics	/$I-	No I/O checking
    /GD	Detailed map file	/$L-	No local debug symbols
    /Ixxx	Include directories	/$Mxxx	Memory allocation parameters
    /L	Link buffer on disk	/$N+	8087 code generation
    /M	Make modified units	/$O+	Overlays allowed
    /Oxxx	Object directories	/$R+	Range checking
    /Q	Quiet compile		/$S-	No stack checking
    /Txxx	Turbo directories	/$V-	No var-string checking
    /Uxxx	Unit directories
    /V	EXE debug information

    $ emu2 tp/tpc.exe tp\\qsort.pas -- 'PATH=C:\TP'
    Turbo Pascal Version 5.5  Copyright (c) 1983,89 Borland International
    TP\QSORT.PAS(66)
    66 lines, 4384 bytes code, 2668 bytes data.

    $ emu2 tp/qsort.exe
    ....

And for the IDE, you can use:

    emu2 tp/turbo.exe  -- 'PATH=C:\TP'

![Image of TP55 environment](doc/tp55.turbo.png)

