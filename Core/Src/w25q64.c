/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    w25q64.c
  * @brief   W25Q64 (8MB SPI NOR Flash) 驱动
  *
  * 接口:    SPI1 (PA5=SCK/PA6=MISO/PA7=MOSI), CS=PA4 (软件控制, 低有效)
  * 通信格式: 拉低 CS -> 发指令(1字节) -> 发地址(3字节) -> 收发数据 -> 拉高 CS
  *
  * 两个必须记住的硬件限制:
  *   1. 写入前该区域必须已擦除 (擦除最小单位是 4KB 扇区)
  *   2. 一次"页编程"不能跨页, 最多 256 字节 (本驱动已自动分段)
  ******************************************************************************
  */
/* USER CODE END Header */

#include "w25q64.h"
#include <string.h>              /* memcmp */

extern SPI_HandleTypeDef hspi1;   /* main.c 中的 SPI1 句柄 */

/* ---- 指令码 (来自 W25Q64 数据手册) ---- */
#define CMD_WRITE_ENABLE   0x06   /* 写使能: 每次写/擦之前都要先发它 */
#define CMD_READ_STATUS1   0x05   /* 读状态寄存器1: bit0=BUSY */
#define CMD_READ_DATA      0x03   /* 读数据 */
#define CMD_PAGE_PROGRAM   0x02   /* 页编程 (写) */
#define CMD_SECTOR_ERASE   0x20   /* 扇区擦除 (4KB) */
#define CMD_JEDEC_ID       0x9F   /* 读 JEDEC ID (3 字节) */

/* 超时设置 */
#define W25Q_SPI_TIMEOUT    100    /* ms, 单次 SPI 收发 */
#define W25Q_PROG_TIMEOUT   100    /* ms, 页写入完成 (手册典型 0.7ms) */
#define W25Q_ERASE_TIMEOUT  5000   /* ms, 扇区擦除完成 (手册最大 400ms) */

#define W25Q_STATUS_BUSY    0x01   /* 状态寄存器 bit0: 1=芯片正在忙 */

/* 单次 HAL_SPI 收发的最大字节数 (HAL 的长度参数是 uint16_t) */
#define W25Q_IO_CHUNK       4096

/* ---- 片选控制 (低有效) ---- */
static void W25Q_CS_Low(void)
{
  HAL_GPIO_WritePin(W25Q_CS_GPIO_Port, W25Q_CS_Pin, GPIO_PIN_RESET);
}

static void W25Q_CS_High(void)
{
  HAL_GPIO_WritePin(W25Q_CS_GPIO_Port, W25Q_CS_Pin, GPIO_PIN_SET);
}

/* 写使能: 任何"写/擦"操作之前都必须先发一次 */
static uint8_t W25Q_WriteEnable(void)
{
  uint8_t cmd = CMD_WRITE_ENABLE;

  W25Q_CS_Low();
  if (HAL_SPI_Transmit(&hspi1, &cmd, 1, W25Q_SPI_TIMEOUT) != HAL_OK)
  {
    W25Q_CS_High();
    return 0;
  }
  W25Q_CS_High();
  return 1;
}

/**
  * @brief 等待芯片空闲 (轮询状态寄存器 BUSY 位)
  * @param timeout_ms: 最长等待时间
  * @retval 1=已空闲, 0=超时或通信失败
  */
static uint8_t W25Q_WaitBusy(uint32_t timeout_ms)
{
  uint8_t cmd = CMD_READ_STATUS1;
  uint8_t sr = W25Q_STATUS_BUSY;
  uint32_t start = HAL_GetTick();

  W25Q_CS_Low();
  if (HAL_SPI_Transmit(&hspi1, &cmd, 1, W25Q_SPI_TIMEOUT) != HAL_OK)
  {
    W25Q_CS_High();
    return 0;
  }

  /* CS 保持低电平时可连续读状态寄存器, 不必每轮重新发指令 */
  do
  {
    if (HAL_SPI_Receive(&hspi1, &sr, 1, W25Q_SPI_TIMEOUT) != HAL_OK)
    {
      W25Q_CS_High();
      return 0;
    }
  } while (((sr & W25Q_STATUS_BUSY) != 0) && ((HAL_GetTick() - start) < timeout_ms));

  W25Q_CS_High();
  return ((sr & W25Q_STATUS_BUSY) == 0) ? 1 : 0;
}

uint32_t W25Q_ReadID(void)
{
  uint8_t cmd = CMD_JEDEC_ID;
  uint8_t id[3] = {0};

  W25Q_CS_Low();
  if (HAL_SPI_Transmit(&hspi1, &cmd, 1, W25Q_SPI_TIMEOUT) != HAL_OK)
  {
    W25Q_CS_High();
    return 0;
  }
  if (HAL_SPI_Receive(&hspi1, id, 3, W25Q_SPI_TIMEOUT) != HAL_OK)
  {
    W25Q_CS_High();
    return 0;
  }
  W25Q_CS_High();

  return (((uint32_t)id[0] << 16) | ((uint32_t)id[1] << 8) | (uint32_t)id[2]);
}

uint8_t W25Q_Init(void)
{
  uint32_t id;

  W25Q_CS_High();               /* 空闲时片选拉高 */
  id = W25Q_ReadID();

  if (id == W25Q64_JEDEC_ID)
  {
    DebugPrint("[W25Q] online, ID=0x%06lX (W25Q64, 8MB)\r\n", (unsigned long)id);
    return 1;
  }

  DebugPrint("[W25Q] NOT found! ID=0x%06lX (expect 0x%06lX)\r\n",
             (unsigned long)id, (unsigned long)W25Q64_JEDEC_ID);
  return 0;
}

void W25Q_Read(uint32_t addr, uint8_t *buf, uint32_t len)
{
  uint8_t cmd[4];

  if ((buf == NULL) || (len == 0) || (addr >= W25Q64_TOTAL_SIZE))
  {
    return;
  }

  cmd[0] = CMD_READ_DATA;
  cmd[1] = (uint8_t)(addr >> 16);
  cmd[2] = (uint8_t)(addr >> 8);
  cmd[3] = (uint8_t)addr;

  W25Q_CS_Low();
  if (HAL_SPI_Transmit(&hspi1, cmd, 4, W25Q_SPI_TIMEOUT) == HAL_OK)
  {
    /* 一次最多收 4096 字节, 长数据分段读 */
    while (len > 0)
    {
      uint16_t chunk = (len > W25Q_IO_CHUNK) ? (uint16_t)W25Q_IO_CHUNK : (uint16_t)len;

      if (HAL_SPI_Receive(&hspi1, buf, chunk, W25Q_SPI_TIMEOUT) != HAL_OK)
      {
        break;
      }
      buf += chunk;
      len -= chunk;
    }
  }
  W25Q_CS_High();
}

uint8_t W25Q_Write(uint32_t addr, const uint8_t *buf, uint32_t len)
{
  uint8_t cmd[4];

  if ((buf == NULL) || (len == 0))
  {
    return 1;
  }
  /* 越界保护 */
  if (addr >= W25Q64_TOTAL_SIZE || (addr + len) > W25Q64_TOTAL_SIZE)
  {
    return 0;
  }

  while (len > 0)
  {
    /* 本次最多写到本页末尾: 页编程不能跨页 */
    uint16_t chunk = (uint16_t)(W25Q64_PAGE_SIZE - (addr % W25Q64_PAGE_SIZE));
    if (chunk > len)
    {
      chunk = (uint16_t)len;
    }

    /* 每一页都要重新"写使能" */
    if (!W25Q_WriteEnable())
    {
      return 0;
    }

    cmd[0] = CMD_PAGE_PROGRAM;
    cmd[1] = (uint8_t)(addr >> 16);
    cmd[2] = (uint8_t)(addr >> 8);
    cmd[3] = (uint8_t)addr;

    W25Q_CS_Low();
    if ((HAL_SPI_Transmit(&hspi1, cmd, 4, W25Q_SPI_TIMEOUT) != HAL_OK) ||
        (HAL_SPI_Transmit(&hspi1, (uint8_t *)buf, chunk, W25Q_SPI_TIMEOUT) != HAL_OK))
    {
      W25Q_CS_High();
      return 0;
    }
    W25Q_CS_High();

    /* 等这一页真正写完, 再进行下一页 */
    if (!W25Q_WaitBusy(W25Q_PROG_TIMEOUT))
    {
      return 0;
    }

    addr += chunk;
    buf  += chunk;
    len  -= chunk;
  }

  return 1;
}

uint8_t W25Q_SectorErase(uint32_t addr)
{
  uint8_t cmd[4];

  if (addr >= W25Q64_TOTAL_SIZE)
  {
    return 0;
  }
  /* 地址向下对齐到 4KB 扇区起点 */
  addr &= ~((uint32_t)W25Q64_SECTOR_SIZE - 1U);

  if (!W25Q_WriteEnable())
  {
    return 0;
  }

  cmd[0] = CMD_SECTOR_ERASE;
  cmd[1] = (uint8_t)(addr >> 16);
  cmd[2] = (uint8_t)(addr >> 8);
  cmd[3] = (uint8_t)addr;

  W25Q_CS_Low();
  if (HAL_SPI_Transmit(&hspi1, cmd, 4, W25Q_SPI_TIMEOUT) != HAL_OK)
  {
    W25Q_CS_High();
    return 0;
  }
  W25Q_CS_High();

  /* 擦除是"慢操作"(典型 45ms), 必须等 BUSY 清零才能干别的 */
  return W25Q_WaitBusy(W25Q_ERASE_TIMEOUT);
}

uint8_t W25Q_SelfTest(void)
{
  /* 用分区表里预留的自测扇区 (8MB 的最后一个 4KB), 不碰历史数据区 */
  const uint32_t addr = W25Q_SELFTEST_ADDR;
  static uint8_t tx[64];
  static uint8_t rx[64];
  uint32_t i;
  uint8_t fail = 0;

  /* 1. 造测试数据: 填一段有规律的字节, 便于肉眼看出错在哪 */
  for (i = 0; i < sizeof(tx); i++)
  {
    tx[i] = (uint8_t)(i + 1);
  }

  /* 2. 擦除整个扇区 (擦完该扇区应全是 0xFF) */
  if (!W25Q_SectorErase(addr))
  {
    DebugPrint("[W25Q] selftest: erase FAILED (addr=0x%06lX)\r\n", (unsigned long)addr);
    return 0;
  }

  /* 3. 擦除后先读一小段, 确认确实被擦成 0xFF (空片校验) */
  memset(rx, 0, sizeof(rx));
  W25Q_Read(addr, rx, sizeof(rx));
  for (i = 0; i < sizeof(rx); i++)
  {
    if (rx[i] != 0xFF)
    {
      DebugPrint("[W25Q] selftest: after-erase not 0xFF at +%lu (got 0x%02X)\r\n",
                 (unsigned long)i, rx[i]);
      fail = 1;
      break;
    }
  }

  /* 4. 写入测试数据 */
  if (!fail && !W25Q_Write(addr, tx, sizeof(tx)))
  {
    DebugPrint("[W25Q] selftest: write FAILED\r\n");
    fail = 1;
  }

  /* 5. 读回比对 (比对前先把 rx 清 0, 才能区分"读失败"和"数据恰好相同") */
  if (!fail)
  {
    memset(rx, 0, sizeof(rx));
    W25Q_Read(addr, rx, sizeof(rx));

    for (i = 0; i < sizeof(tx); i++)
    {
      if (rx[i] != tx[i])
      {
        DebugPrint("[W25Q] selftest: data MISMATCH at +%lu (wr 0x%02X, rd 0x%02X)\r\n",
                   (unsigned long)i, tx[i], rx[i]);
        fail = 1;
        break;
      }
    }
  }

  if (fail)
  {
    DebugPrint("[W25Q] selftest: FAILED\r\n");
    return 0;
  }

  DebugPrint("[W25Q] selftest: PASS (erase/write/read OK, sector @0x%06lX)\r\n",
             (unsigned long)addr);
  return 1;
}
