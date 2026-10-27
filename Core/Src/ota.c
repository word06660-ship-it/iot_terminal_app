/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    ota.c
  * @brief   OTA 固件缓冲区 + 升级元数据链路 (存储侧)
  *          详见 ota.h 开头的链路说明
  ******************************************************************************
  */
/* USER CODE END Header */

#include "ota.h"
#include "param_store.h"
#include "w25q64.h"
#include <stddef.h>   /* NULL */

/* 本次下载的状态, 只在 "Begin -> Write... -> Commit" 这一串调用之间有效 */
static uint32_t s_fw_size    = 0;   /* Begin 时声明的固件总长度 */
static uint16_t s_fw_version = 0;   /* Begin 时声明的目标版本 */
static uint32_t s_written    = 0;   /* 已写进缓冲区的字节数 (也是下次的写偏移) */

/* 回读校验的中转缓冲: 512KB 装不进 RAM, 只能分段读出来累加 CRC。
 * 放文件作用域而不是栈上, 免得占用调用任务的栈 (调用方持 StorageMutex, 
 * 不会有第二个任务同时用这块缓冲) */
static uint8_t s_crc_buf[256];

/**
  * @brief 回读 W25Q64 里的一整份固件, 算出 CRC16
  */
static uint16_t Ota_CrcOfImage(uint32_t fw_size)
{
  uint16_t crc = 0xFFFF;
  uint32_t off = 0;

  while (off < fw_size)
  {
    uint32_t n = fw_size - off;
    if (n > sizeof(s_crc_buf))
    {
      n = sizeof(s_crc_buf);
    }

    W25Q_Read(W25Q_OTA_BUFFER_ADDR + off, s_crc_buf, n);
    crc = Crc16Update(crc, s_crc_buf, n);
    off += n;
  }
  return crc;
}

uint8_t OTA_BufferBegin(uint32_t fw_size, uint16_t fw_version)
{
  ParamBlock_t *p;

  if ((fw_size == 0U) || (fw_size > OTA_IMAGE_MAX_SIZE))
  {
    DebugPrint("[OTA] bad size %lu (max %lu)\r\n",
               (unsigned long)fw_size, (unsigned long)OTA_IMAGE_MAX_SIZE);
    return OTA_ERR_PARAM;
  }

  /* 本次下载会覆盖缓冲区: 里面原来那份"已就绪"的固件必然被冲掉,
   * 先把标志清掉, 免得后面 Commit 失败时元数据还指着一份不完整的固件 */
  if (OTA_ImageReady())
  {
    p = Param_Get();
    p->upgrade_flag  = 0;
    p->upgrade_valid = 0;
    Param_Save();
    DebugPrint("[OTA] previous image dropped\r\n");
  }

  s_fw_size    = fw_size;
  s_fw_version = fw_version;
  s_written    = 0;

  DebugPrint("[OTA] begin: %lu bytes, target v%u.%u\r\n",
             (unsigned long)fw_size,
             (unsigned)(fw_version >> 8), (unsigned)(fw_version & 0xFF));
  return OTA_OK;
}

uint8_t OTA_BufferWrite(const uint8_t *data, uint32_t len)
{
  if ((data == NULL) || (len == 0U))
  {
    return OTA_ERR_PARAM;
  }
  if ((s_written + len) > s_fw_size)
  {
    DebugPrint("[OTA] write overflow: %lu + %lu > %lu\r\n",
               (unsigned long)s_written, (unsigned long)len,
               (unsigned long)s_fw_size);
    return OTA_ERR_PARAM;
  }

  while (len > 0U)
  {
    uint32_t addr     = W25Q_OTA_BUFFER_ADDR + s_written;
    uint32_t in_sect  = s_written % W25Q64_SECTOR_SIZE;   /* 在本扇区内的偏移 */
    uint32_t chunk    = W25Q64_SECTOR_SIZE - in_sect;     /* 最多写到本扇区末尾 */

    /* 刚进一个新扇区: 必须先擦, FLASH 只能把 1 写成 0, 不擦就是新旧数据按位与 */
    if (in_sect == 0U)
    {
      if (!W25Q_SectorErase(addr))
      {
        DebugPrint("[OTA] erase FAIL @0x%06lX\r\n", (unsigned long)addr);
        return OTA_ERR_FLASH;
      }
    }

    if (chunk > len)
    {
      chunk = len;
    }

    /* W25Q_Write 内部会按 256 字节分页, 但不会帮我们跨扇区擦除, 所以这里自己切 */
    if (!W25Q_Write(addr, data, chunk))
    {
      DebugPrint("[OTA] write FAIL @0x%06lX\r\n", (unsigned long)addr);
      return OTA_ERR_FLASH;
    }

    s_written += chunk;
    data      += chunk;
    len       -= chunk;
  }
  return OTA_OK;
}

uint8_t OTA_BufferCommit(uint16_t expect_crc)
{
  ParamBlock_t *p;
  uint16_t crc;

  if (s_written != s_fw_size)
  {
    DebugPrint("[OTA] commit too early: %lu/%lu bytes\r\n",
               (unsigned long)s_written, (unsigned long)s_fw_size);
    return OTA_ERR_STATE;
  }

  /* 回读校验: 只有从 FLASH 里读回来的每个字节都算得对, 才算真的下好了 */
  crc = Ota_CrcOfImage(s_fw_size);
  if (crc != expect_crc)
  {
    DebugPrint("[OTA] crc mismatch: flash=0x%04X expect=0x%04X\r\n",
               (unsigned)crc, (unsigned)expect_crc);
    return OTA_ERR_CRC;
  }

  /* 校验通过: 把固件信息和"待安装"标志一起写进模拟 EEPROM (双槽 A/B 掉电安全) */
  p = Param_Get();
  p->fw_size       = s_fw_size;
  p->fw_version    = s_fw_version;
  p->fw_crc        = crc;
  p->upgrade_valid = 1;
  p->upgrade_flag  = 1;

  if (!Param_Save())
  {
    /* 元数据没落盘: 把两个标志收回内存原样, 否则内存里留着"可安装"的假象,
     * 下一次 Param_Save (比如存历史指针) 会把它一起写进 FLASH */
    p->upgrade_valid = 0;
    p->upgrade_flag  = 0;
    return OTA_ERR_FLASH;
  }

  DebugPrint("[OTA] image ready: v%u.%u, %lu bytes, crc=0x%04X\r\n",
             (unsigned)(p->fw_version >> 8), (unsigned)(p->fw_version & 0xFF),
             (unsigned long)p->fw_size, (unsigned)crc);
  return OTA_OK;
}

uint8_t OTA_ImageReady(void)
{
  ParamBlock_t *p = Param_Get();

  if ((p->upgrade_flag == 0U) || (p->upgrade_valid == 0U))
  {
    return 0;
  }
  return 1;
}

void OTA_ImageConsumed(void)
{
  ParamBlock_t *p = Param_Get();

  p->upgrade_flag  = 0;
  p->upgrade_valid = 0;
  Param_Save();

  DebugPrint("[OTA] image flag cleared (active slot %c)\r\n",
             (p->active_slot == FW_SLOT_A) ? 'A' : 'B');
}

void OTA_PrintStatus(void)
{
  ParamBlock_t *p = Param_Get();

  if (OTA_ImageReady())
  {
    DebugPrint("[OTA] pending image: v%u.%u, %lu bytes, crc=0x%04X, active slot %c\r\n",
               (unsigned)(p->fw_version >> 8), (unsigned)(p->fw_version & 0xFF),
               (unsigned long)p->fw_size, (unsigned)p->fw_crc,
               (p->active_slot == FW_SLOT_A) ? 'A' : 'B');
  }
  else
  {
    DebugPrint("[OTA] no pending image, active slot %c\r\n",
               (p->active_slot == FW_SLOT_A) ? 'A' : 'B');
  }
}
