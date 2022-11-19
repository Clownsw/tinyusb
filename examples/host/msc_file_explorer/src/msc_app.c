/* 
 * The MIT License (MIT)
 *
 * Copyright (c) 2019 Ha Thach (tinyusb.org)
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 */

#include "tusb.h"

#include "ff.h"
#include "diskio.h"

// lib/embedded-cli
#define EMBEDDED_CLI_IMPL
#include "embedded_cli.h"

//--------------------------------------------------------------------+
// MACRO TYPEDEF CONSTANT ENUM DECLARATION
//--------------------------------------------------------------------+

//------------- embedded-cli -------------//
#define CLI_BUFFER_SIZE     256
#define CLI_RX_BUFFER_SIZE  16
#define CLI_CMD_BUFFER_SIZE 32
#define CLI_HISTORY_SIZE    32
#define CLI_BINDING_COUNT   3

static EmbeddedCli *_cli;
static CLI_UINT cli_buffer[BYTES_TO_CLI_UINTS(CLI_BUFFER_SIZE)];

//------------- Elm Chan FatFS -------------//
static FATFS fatfs[CFG_TUH_DEVICE_MAX]; // for simplicity only support 1 LUN per device
static volatile bool _disk_busy[CFG_TUH_DEVICE_MAX];

static scsi_inquiry_resp_t inquiry_resp;

//--------------------------------------------------------------------+
//
//--------------------------------------------------------------------+

void cli_cmd_ls(EmbeddedCli *cli, char *args, void *context);
void cli_cmd_cd(EmbeddedCli *cli, char *args, void *context);

void cli_write_char(EmbeddedCli *cli, char c)
{
  (void) cli;
  putchar((int) c);
}

void cli_cmd_unknown(EmbeddedCli *cli, CliCommand *command)
{
  (void) cli;
  printf("%s: command not found\r\n", command->name);
}

bool msc_app_init(void)
{
  for(size_t i=0; i<CFG_TUH_DEVICE_MAX; i++) _disk_busy[i] = false;

  // disable stdout buffered for echoing typing command
  setbuf(stdout, NULL);

  EmbeddedCliConfig *config = embeddedCliDefaultConfig();
  config->cliBuffer         = cli_buffer;
  config->cliBufferSize     = CLI_BUFFER_SIZE;
  config->rxBufferSize      = CLI_RX_BUFFER_SIZE;
  config->cmdBufferSize     = CLI_CMD_BUFFER_SIZE;
  config->historyBufferSize = CLI_HISTORY_SIZE;
  config->maxBindingCount   = CLI_BINDING_COUNT;

  _cli = embeddedCliNew(config);
  TU_ASSERT(_cli != NULL);

  _cli->writeChar = cli_write_char;

  embeddedCliAddBinding(_cli, (CliCommandBinding) {
    "cd",
    "Usage: cd [DIR]...\r\n\tChange the current directory to DIR.",
    true,
    NULL,
    cli_cmd_cd
  });

  embeddedCliAddBinding(_cli, (CliCommandBinding) {
    "ls",
    "Usage: ls [DIR]...\r\n\tList information about the FILEs (the current directory by default).",
    true,
    NULL,
    cli_cmd_ls
  });


  return true;
}

void msc_app_task(void)
{
  if (!_cli) return;

  int ch;
  while( (ch = getchar()) > 0 )
  {
    embeddedCliReceiveChar(_cli, (char) ch);
  }

  embeddedCliProcess(_cli);
}

//--------------------------------------------------------------------+
//
//--------------------------------------------------------------------+


bool inquiry_complete_cb(uint8_t dev_addr, msc_cbw_t const* cbw, msc_csw_t const* csw)
{
  if (csw->status != 0)
  {
    printf("Inquiry failed\r\n");
    return false;
  }

  // Print out Vendor ID, Product ID and Rev
  printf("%.8s %.16s rev %.4s\r\n", inquiry_resp.vendor_id, inquiry_resp.product_id, inquiry_resp.product_rev);

  // Get capacity of device
  uint32_t const block_count = tuh_msc_get_block_count(dev_addr, cbw->lun);
  uint32_t const block_size = tuh_msc_get_block_size(dev_addr, cbw->lun);

  printf("Disk Size: %lu MB\r\n", block_count / ((1024*1024)/block_size));
  printf("Block Count = %lu, Block Size: %lu\r\n", block_count, block_size);

  // For simplicity: we only mount 1 LUN per device
  uint8_t const drive_num = dev_addr-1;
  char drive_path[3] = "0:";
  drive_path[0] += drive_num;

  if ( f_mount(&fatfs[drive_num], drive_path, 1) != FR_OK )
  {
    puts("mount failed");
  }

  f_chdrive(drive_path); // change to newly mounted drive
  f_chdir("/"); // root as current dir

  return true;
}

//------------- IMPLEMENTATION -------------//
void tuh_msc_mount_cb(uint8_t dev_addr)
{
  printf("A MassStorage device is mounted\r\n");

  uint8_t const lun = 0;
  tuh_msc_inquiry(dev_addr, lun, &inquiry_resp, inquiry_complete_cb);
}

void tuh_msc_umount_cb(uint8_t dev_addr)
{
  (void) dev_addr;
  printf("A MassStorage device is unmounted\r\n");

  uint8_t const drive_num = dev_addr-1;
  char drive_path[3] = "0:";
  drive_path[0] += drive_num;

  f_unmount(drive_path);

//  if ( phy_disk == f_get_current_drive() )
//  { // active drive is unplugged --> change to other drive
//    for(uint8_t i=0; i<CFG_TUH_DEVICE_MAX; i++)
//    {
//      if ( disk_is_ready(i) )
//      {
//        f_chdrive(i);
//        cli_init(); // refractor, rename
//      }
//    }
//  }
}

//--------------------------------------------------------------------+
// DiskIO
//--------------------------------------------------------------------+

static void wait_for_disk_io(BYTE pdrv)
{
  while(_disk_busy[pdrv])
  {
    tuh_task();
  }
}

static bool disk_io_complete(uint8_t dev_addr, msc_cbw_t const* cbw, msc_csw_t const* csw)
{
  (void) dev_addr; (void) cbw; (void) csw;
  _disk_busy[dev_addr-1] = false;
  return true;
}

DSTATUS disk_status (
	BYTE pdrv		/* Physical drive nmuber to identify the drive */
)
{
  uint8_t dev_addr = pdrv + 1;
  return tuh_msc_mounted(dev_addr) ? 0 : STA_NODISK;
}

DSTATUS disk_initialize (
	BYTE pdrv				/* Physical drive nmuber to identify the drive */
)
{
  (void) pdrv;
	return 0; // nothing to do
}

DRESULT disk_read (
	BYTE pdrv,		/* Physical drive nmuber to identify the drive */
	BYTE *buff,		/* Data buffer to store read data */
	LBA_t sector,	/* Start sector in LBA */
	UINT count		/* Number of sectors to read */
)
{
	uint8_t const dev_addr = pdrv + 1;
	uint8_t const lun = 0;

	_disk_busy[pdrv] = true;
	tuh_msc_read10(dev_addr, lun, buff, sector, count, disk_io_complete);
	wait_for_disk_io(pdrv);

	return RES_OK;
}

#if FF_FS_READONLY == 0

DRESULT disk_write (
	BYTE pdrv,			/* Physical drive nmuber to identify the drive */
	const BYTE *buff,	/* Data to be written */
	LBA_t sector,		/* Start sector in LBA */
	UINT count			/* Number of sectors to write */
)
{
	uint8_t const dev_addr = pdrv + 1;
	uint8_t const lun = 0;

	_disk_busy[pdrv] = true;
	tuh_msc_write10(dev_addr, lun, buff, sector, count, disk_io_complete);
	wait_for_disk_io(pdrv);

	return RES_OK;
}

#endif

DRESULT disk_ioctl (
	BYTE pdrv,		/* Physical drive nmuber (0..) */
	BYTE cmd,		/* Control code */
	void *buff		/* Buffer to send/receive control data */
)
{
  uint8_t const dev_addr = pdrv + 1;
  uint8_t const lun = 0;
  switch ( cmd )
  {
    case CTRL_SYNC:
      // nothing to do since we do blocking
      return RES_OK;

    case GET_SECTOR_COUNT:
      *((DWORD*) buff) = tuh_msc_get_block_count(dev_addr, lun);
      return RES_OK;

    case GET_SECTOR_SIZE:
      *((WORD*) buff) = tuh_msc_get_block_size(dev_addr, lun);
      return RES_OK;

    case GET_BLOCK_SIZE:
      *((DWORD*) buff) = 1;    // erase block size in units of sector size
      return RES_OK;

    default:
      return RES_PARERR;
  }

	return RES_OK;
}

//--------------------------------------------------------------------+
// CLI Commands
//--------------------------------------------------------------------+

void cli_cmd_ls(EmbeddedCli *cli, char *args, void *context)
{
  (void) cli;
  (void) context;

  uint16_t argc = embeddedCliGetTokenCount(args);

  // only support 1 argument
  if ( argc > 1 )
  {
    printf("invalid arguments\r\n");
    return;
  }

  // default is current directory
  const char* dpath = ".";
  if (argc) dpath = args;

  DIR dir;
  if ( FR_OK != f_opendir(&dir, dpath) )
  {
    printf("cannot access '%s': No such file or directory\r\n", dpath);
    return;
  }

  FILINFO fno;
  while( (f_readdir(&dir, &fno) == FR_OK) && (fno.fname[0] != 0) )
  {
    if ( fno.fname[0] != '.' ) // ignore . and .. entry
    {
      if ( fno.fattrib & AM_DIR )
      {
        // directory
        printf("/%s\r\n", fno.fname);
      }else
      {
        printf("%-40s%lu KB\r\n", fno.fname, fno.fsize / 1000);
      }
    }
  }

  f_closedir(&dir);
}

void cli_cmd_cd(EmbeddedCli *cli, char *args, void *context)
{
  (void) cli;
  (void) context;

  uint16_t argc = embeddedCliGetTokenCount(args);

  // only support 1 argument
  if ( argc != 1 )
  {
    printf("invalid arguments\r\n");
    return;
  }

  // default is current directory
  const char* dpath = args;

  if ( FR_OK != f_chdir(dpath) )
  {
    printf("%s: No such file or directory\r\n", dpath);
    return;
  }
}
