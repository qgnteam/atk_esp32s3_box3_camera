"""Run production exFAT formatting against a memory disk; never access devices.

Uses the project's generated FatFs override. Copies SDK files and preserves
licenses in ignored outputs; host-only config replaces RTOS synchronisation.
"""
from pathlib import Path
import re
import shutil
import subprocess

root = Path(__file__).resolve().parents[1]
out = root / "outputs/card-tests"
out.mkdir(parents=True, exist_ok=True)
source = root / "outputs/fatfs_exfat/fatfs"
for name in ("ff.c", "ff.h", "ffunicode.c", "ffconf.h"):
    shutil.copyfile(source / "src" / name, out / name)
shutil.copyfile(source / "src/diskio.h", out / "diskio.h")
# Host uses fixed timestamps, stack LFNs and no RTOS mutexes. File system and
# production formatter implementations execute unchanged on an isolated disk.
conf = (out / "ffconf.h").read_text()
for name, value in (("FF_FS_REENTRANT",0),("FF_FS_NORTC",1)):
    conf = re.sub(r"(#define\s+"+name+r"\s+)\d+",lambda m:m.group(1)+str(value),conf)
conf = conf.replace('#include "freertos/FreeRTOS.h"', '#include <stddef.h>').replace('#include "freertos/semphr.h"', '')
(out / "ffconf.h").write_text(conf)
(out / "sdkconfig.h").write_text("""
#define CONFIG_FATFS_CODEPAGE 437
#define CONFIG_FATFS_LFN_STACK 1
#define CONFIG_FATFS_MAX_LFN 255
#define CONFIG_FATFS_VOLUME_COUNT 1
#define CONFIG_WL_SECTOR_SIZE 512
#define CONFIG_FATFS_PER_FILE_CACHE 1
#define CONFIG_FATFS_FS_LOCK 0
#define CONFIG_FATFS_TIMEOUT_MS 1000
#define CONFIG_FATFS_USE_FASTSEEK 0
#define CONFIG_FATFS_USE_DYN_BUFFERS 0
""")
(out / "test.c").write_text(r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ff.h"
#include "diskio.h"
#include "card_format.h"
void *ff_memalloc(size_t size) { return malloc(size); }
void ff_memfree(void *data) { free(data); }
PARTITION VolToPart[1] = {{0,0}};
static BYTE *disk = NULL;
static LBA_t sectors = 0;
DSTATUS disk_initialize(BYTE drive) { return drive == 0 ? 0 : STA_NOINIT; }
DSTATUS disk_status(BYTE drive) { return disk_initialize(drive); }
DRESULT disk_read(BYTE drive, BYTE *data, LBA_t sector, UINT count) {
    if (drive || sector > sectors || count > sectors-sector) return RES_PARERR;
    memcpy(data,disk+sector*512,(size_t)count*512); return RES_OK;
}
DRESULT disk_write(BYTE drive, const BYTE *data, LBA_t sector, UINT count) {
    if (drive || sector > sectors || count > sectors-sector) return RES_PARERR;
    memcpy(disk+sector*512,data,(size_t)count*512); return RES_OK;
}
DRESULT disk_ioctl(BYTE drive, BYTE command, void *data) {
    if (drive) return RES_PARERR;
    switch (command) {
    case CTRL_SYNC: return RES_OK;
    case GET_SECTOR_COUNT: *(LBA_t*)data=sectors; return RES_OK;
    case GET_SECTOR_SIZE: *(WORD*)data=512; return RES_OK;
    case GET_BLOCK_SIZE: *(DWORD*)data=1; return RES_OK;
    default: return RES_PARERR;
    }
}
int main(void) {
    BYTE work[4096]={0}; FATFS fs={0}; FIL file={0}; UINT bytes=0;
    sectors=131072; disk=calloc(sectors,512); assert(disk);
    assert(f_mount(&fs,"0:",1)==FR_NO_FILESYSTEM);
    assert(!CardFilesystemSupported(0));
    assert(!CardFilesystemSupported(FS_FAT12));
    assert(!CardFilesystemSupported(FS_FAT16));
    assert(CardFilesystemSupported(FS_FAT32));
    assert(CardFilesystemSupported(FS_EXFAT));
    MKFS_PARM fat={.fmt=FM_FAT|FM_SFD,.n_fat=1,.au_size=4096};
    assert(f_mkfs("0:",&fat,work,sizeof(work))==FR_OK);
    assert(f_mount(&fs,"0:",1)==FR_OK && fs.fs_type==FS_FAT16);
    assert(!CardFilesystemSupported(fs.fs_type));
    assert(f_open(&file,"0:/OLD.TXT",FA_CREATE_NEW|FA_WRITE)==FR_OK);
    assert(f_write(&file,"preserve until confirmed",24,&bytes)==FR_OK && bytes==24);
    assert(f_close(&file)==FR_OK);
    assert(CardFormatExfat(NULL,work,sizeof(work))==FR_INVALID_PARAMETER);
    assert(CardFormatExfat("0:",NULL,sizeof(work))==FR_INVALID_PARAMETER);
    assert(CardFormatExfat("0:",work,512)==FR_INVALID_PARAMETER);
    assert(f_stat("0:/OLD.TXT",NULL)==FR_OK); /* Invalid args never erase. */
    assert(f_mount(NULL,"0:",0)==FR_OK);
    assert(CardFormatExfat("0:",work,sizeof(work))==FR_OK);
    assert(f_mount(&fs,"0:",1)==FR_OK && fs.fs_type==FS_EXFAT);
    assert(f_stat("0:/OLD.TXT",NULL)==FR_NO_FILE);
    assert(f_mkdir("0:/DCIM")==FR_OK);
    assert(f_open(&file,"0:/DCIM/IMG00001.BMP",FA_CREATE_NEW|FA_WRITE)==FR_OK);
    assert(f_write(&file,"verified",8,&bytes)==FR_OK && bytes==8);
    assert(f_sync(&file)==FR_OK && f_close(&file)==FR_OK);
    assert(f_mount(NULL,"0:",0)==FR_OK);
    assert(f_mount(&fs,"0:",1)==FR_OK && fs.fs_type==FS_EXFAT);
    assert(f_open(&file,"0:/DCIM/IMG00001.BMP",FA_READ)==FR_OK);
    BYTE readback[8]={0};
    assert(f_read(&file,readback,8,&bytes)==FR_OK && bytes==8);
    assert(memcmp(readback,"verified",8)==0);
    assert(f_close(&file)==FR_OK);
    assert(f_mount(NULL,"0:",0)==FR_OK);
    /* Force FAT32 creation, independently mount and validate supported type. */
    fat.fmt=FM_FAT32|FM_SFD; fat.au_size=512;
    assert(f_mkfs("0:",&fat,work,sizeof(work))==FR_OK);
    assert(f_mount(&fs,"0:",1)==FR_OK && fs.fs_type==FS_FAT32);
    assert(CardFilesystemSupported(fs.fs_type));
    assert(f_mount(NULL,"0:",0)==FR_OK);
    free(disk); disk=NULL;
    puts("PASS: actual FatFs; unknown/FAT16/FAT32/exFAT; invalid args preserve data; format/remount/readback");
    return 0;
}
''')
subprocess.run(["cc","-O1","-Wall","-Wextra","-Werror","-I",str(out),
               "-I",str(root/"main/storage"),str(out/"ff.c"),str(out/"ffunicode.c"),
               str(root/"main/storage/card_format.c"),str(out/"test.c"),
               "-o",str(out/"test_card")],check=True)
subprocess.run([str(out/"test_card")],check=True)
