/*
Some flash handling cgi routines. Used for reading the existing flash and updating the ESPFS image.
*/

/*
 * ----------------------------------------------------------------------------
 * "THE BEER-WARE LICENSE" (Revision 42):
 * Jeroen Domburg <jeroen@spritesmods.com> wrote this file. As long as you retain
 * this notice you can do whatever you want with this stuff. If we meet some day,
 * and you think this stuff is worth it, you can buy me a beer in return.
 * ----------------------------------------------------------------------------
 * Heavily modified and enhanced by Thorsten von Eicken in 2015
 * ----------------------------------------------------------------------------
 */


#include <esp8266.h>
#include <osapi.h>
#include "cgi.h"
#include "cgiflash.h"
#include "espfs.h"
#include "safeupgrade.h"

#define SPI_FLASH_MEM_EMU_START_ADDR    0x40200000
#define USER1_BIN_SPI_FLASH_ADDR        4*1024                                      // either start after 4KB boot partition

#ifndef USER2_BIN_SPI_FLASH_ADDR
#define USER2_BIN_SPI_FLASH_ADDR        4*1024 + FIRMWARE_SIZE + 16*1024 + 4*1024   // 4KB boot, fw1, 16KB user param, 4KB reserved
#endif

#ifdef CGIFLASH_DBG
#define DBG(format, ...) do { os_printf(format, ## __VA_ARGS__); } while(0)
#else
#define DBG(format, ...) do { } while(0)
#endif

// Check that the header of the firmware blob looks like actual firmware...
static char* ICACHE_FLASH_ATTR check_header(void *buf) {
  uint8_t *cd = (uint8_t *)buf;
#ifdef CGIFLASH_DBG
  uint32_t *buf32 = buf;
  os_printf("%p: %08lX %08lX %08lX %08lX\n", buf, buf32[0], buf32[1], buf32[2], buf32[3]);
#endif
  if (cd[0] != 0xEA) return "IROM magic missing";
  if (cd[1] != 4 || cd[2] > 3 || (cd[3]>>4) > 6) return "bad flash header";
  if (((uint16_t *)buf)[3] != 0x4010) return "Invalid entry addr";
  if (((uint32_t *)buf)[2] != 0) return "Invalid start offset";
  return NULL;
}

// check whether the flash map/size we have allows for OTA upgrade
static bool canOTA(void) {
        enum flash_size_map map = system_get_flash_size_map();
        return map >= FLASH_SIZE_4M_MAP_256_256;
}

static char *flash_too_small = "Flash too small for OTA update";

extern uint32 _irom0_text_start;

static uint8 ICACHE_FLASH_ATTR system_upgrade_enhance_userbin_check()
{
    const uint32* const user2_bin_start = (uint32*)(SPI_FLASH_MEM_EMU_START_ADDR + USER2_BIN_SPI_FLASH_ADDR);
    if (&_irom0_text_start >= user2_bin_start) {
        /* The currently used IROM section lies behind the user 2 start.
         * So the current ROM is in user 2
         */
        return 1;
    } else {
        return 0;
    }
}

static uint32 ICACHE_FLASH_ATTR getNextSPIFlashAddr(void) {
    const uint8 id = system_upgrade_enhance_userbin_check();
    const uint32 address = ( (id == 1) ? USER1_BIN_SPI_FLASH_ADDR : USER2_BIN_SPI_FLASH_ADDR );

    return address;
}

const char* const ICACHE_FLASH_ATTR checkUpgradedFirmware()
{
    // sanity-check that the 'next' partition actually contains something that looks like
    // valid firmware
    const uint32 address = getNextSPIFlashAddr();
    uint32 buf[8];
    DBG("Checking %p\n", (void *)address);
    spi_flash_read(address, buf, sizeof(buf));
    char *err = check_header(buf);

    return err;
}

uint32* const ICACHE_FLASH_ATTR getNextFlashAddr(void) {
    const uint32 addr = SPI_FLASH_MEM_EMU_START_ADDR + getNextSPIFlashAddr();

    /* cast as a pointer, because it is the real address in this system,
     * for accessing the SPI flash position through the mem emu
     */
    return (uint32*)addr;
}

//===== Cgi to query which firmware needs to be uploaded next
int ICACHE_FLASH_ATTR cgiGetFirmwareNext(HttpdConnData *connData) {
  if (connData->conn==NULL) return HTTPD_CGI_DONE; // Connection aborted. Clean up.

        if (!canOTA()) {
          errorResponse(connData, 400, flash_too_small);
          return HTTPD_CGI_DONE;
        }

  const uint8 id = system_upgrade_enhance_userbin_check();
  httpdStartResponse(connData, 200);
  httpdHeader(connData, "Content-Type", "text/plain");
  httpdHeader(connData, "Content-Length", "9");
  httpdEndHeaders(connData);
  char *next = id == 1 ? "user1.bin" : "user2.bin";
  httpdSend(connData, next, -1);
  DBG("Next firmware: %s (got %d)\n", next, id);

  /* the httpd works and a firmeware upgrade would be possible.
   * So the last upgrade was successful
   */
  cgiFlashSetUpgradeSuccessful();

  return HTTPD_CGI_DONE;
}

//===== Cgi that allows the firmware to be replaced via http POST
int ICACHE_FLASH_ATTR cgiUploadFirmware(HttpdConnData *connData) {
  if (connData->conn==NULL) return HTTPD_CGI_DONE; // Connection aborted. Clean up.

        if (!canOTA()) {
          errorResponse(connData, 400, flash_too_small);
          return HTTPD_CGI_DONE;
        }

  int offset = connData->post->received - connData->post->buffLen;
  if (offset == 0) {
    connData->cgiPrivData = NULL;
  } else if (connData->cgiPrivData != NULL) {
    // we have an error condition, do nothing
    return HTTPD_CGI_DONE;
  }

  // assume no error yet...
  char *err = NULL;
  int code = 400;

  // check overall size
  //os_printf("FW: %d (max %d)\n", connData->post->len, FIRMWARE_SIZE);
  if (connData->post->len > FIRMWARE_SIZE) err = "Firmware image too large";
  if (connData->post->buff == NULL || connData->requestType != HTTPD_METHOD_POST ||
      connData->post->len < 1024) err = "Invalid request";

  // check that data starts with an appropriate header
  if (err == NULL && offset == 0) {
      err = check_header(connData->post->buff);

      /* update anyway, if it is an ESP FS image */
      if (err != NULL && espFsIsImage(connData->post->buff)) {
          err = NULL;
      }
  }

  // make sure we're buffering in 1024 byte chunks
  if (err == NULL && offset % 1024 != 0) {
    err = "Buffering problem";
    code = 500;
  }

  // return an error if there is one
  if (err != NULL) {
    DBG("Error %d: %s\n", code, err);
    httpdStartResponse(connData, code);
    httpdHeader(connData, "Content-Type", "text/plain");
    //httpdHeader(connData, "Content-Length", strlen(err)+2);
    httpdEndHeaders(connData);
    httpdSend(connData, err, -1);
    httpdSend(connData, "\r\n", -1);
    connData->cgiPrivData = (void *)1;
    return HTTPD_CGI_DONE;
  }

  // let's see which partition we need to flash and what flash address that puts us at
  uint32 address = getNextSPIFlashAddr();
  address += offset;

  // erase next flash block if necessary
  if (address % SPI_FLASH_SEC_SIZE == 0){
    const uint8 id = system_upgrade_enhance_userbin_check();
    DBG("Flashing 0x%05x (id=%d)\n", address, 2 - id);
    spi_flash_erase_sector(address/SPI_FLASH_SEC_SIZE);
  }

  // Write the data
  //DBG("Writing %d bytes at 0x%05x (%d of %d)\n", connData->post->buffSize, address,
  //		connData->post->received, connData->post->len);
  spi_flash_write(address, (uint32 *)connData->post->buff, connData->post->buffLen);

  if (connData->post->received == connData->post->len){
    httpdStartResponse(connData, 200);
    httpdEndHeaders(connData);
    return HTTPD_CGI_DONE;
  } else {
    return HTTPD_CGI_MORE;
  }
}


static ETSTimer flash_reboot_timer;

static void cgiRebootFirmwareTimer(void *timer_arg)
{
    if ( !system_restart_enhance(SYS_BOOT_NORMAL_BIN, getNextSPIFlashAddr())) {
        DBG("Enhanced reboot failed.\n");
    }
}

// Handle request to reboot into the new firmware
int ICACHE_FLASH_ATTR cgiRebootFirmware(HttpdConnData *connData) {
  if (connData->conn==NULL) return HTTPD_CGI_DONE; // Connection aborted. Clean up.

  if (!canOTA()) {
    errorResponse(connData, 400, flash_too_small);
    return HTTPD_CGI_DONE;
  }

  // sanity-check that the 'next' partition actually contains something that looks like
  // valid firmware
  const char* const err = checkUpgradedFirmware();
  if (err != NULL) {
    DBG("Error %d: %s\n", 400, err);
    httpdStartResponse(connData, 400);
    httpdHeader(connData, "Content-Type", "text/plain");
    //httpdHeader(connData, "Content-Length", strlen(err)+2);
    httpdEndHeaders(connData);
    httpdSend(connData, err, -1);
    httpdSend(connData, "\r\n", -1);
    return HTTPD_CGI_DONE;
  }

  httpdStartResponse(connData, 200);
  httpdHeader(connData, "Content-Length", "0");
  httpdEndHeaders(connData);

  // Schedule a reboot
  system_upgrade_flag_set(UPGRADE_FLAG_FINISH);
  os_timer_disarm(&flash_reboot_timer);
  os_timer_setfn(&flash_reboot_timer, cgiRebootFirmwareTimer, NULL);
  os_timer_arm(&flash_reboot_timer, 2000, 1);
  return HTTPD_CGI_DONE;
}

int ICACHE_FLASH_ATTR cgiReset(HttpdConnData *connData) {
  if (connData->conn == NULL) return HTTPD_CGI_DONE; // Connection aborted. Clean up.

  httpdStartResponse(connData, 200);
  httpdHeader(connData, "Content-Length", "0");
  httpdEndHeaders(connData);

  // Schedule a reboot
  os_timer_disarm(&flash_reboot_timer);
  os_timer_setfn(&flash_reboot_timer, (os_timer_func_t *)system_restart, NULL);
  os_timer_arm(&flash_reboot_timer, 2000, 1);
  return HTTPD_CGI_DONE;
}
