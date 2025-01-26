Internal memory map
===================


    - 000000h ------------------------------------
    Realmode interrupt vectors
    - 000400h ------------------------------------
    Pseudo IBM PC BIOS work
    - 000500h ------------------------------------
    Pseudo DOS work
    - 000800h ------------------------------------
    DOS user memory
    
    . 080000h .. LOWMEM mode limit ..........
    
    . 0A0000h .. DOS640K mode limit .........

    - 0B8000h ------------------------------------
    
    CGA text VRAM
    
    - 0C0000h ------------------------------------
    
    TopView pseudo VRAM (for DOS/V support)
    
    - 0C8000h ------------------------------------
    
    unused (RAM is mapped)
    
    - 0D0000h ------------------------------------
    
    EMS pageframe or unused (RAM is mapped)
    
    - 0E0000h ------------------------------------
    
    unused (RAM is mapped)
    
    - 0F8000h ------------------------------------
    Mimic of DOS system file table
    - 0FC000h ------------------------------------
    DOS static memory
    - 0FF000h ------------------------------------
    - 0FFFF0h ------------------------------------
    Pseudo BIOS ROM
    - 100000h ------------------------------------
    
    XMS HMA
    
    - 10FFF0h ------------------------------------
    - 110000h ------------------------------------
    
    XMS EMB area
     .
     .
     .
    - END OF MEMORY -------------------------------
