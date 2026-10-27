/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    w25q64.h
  * @brief   W25Q64 (8MB SPI NOR Flash) 驱动头文件
  *
  * 接口:    SPI1 (PA5=SCK / PA6=MISO / PA7=MOSI), CS=PA4 (软件控制, 低有效)
  * 用途:    OTA 固件缓冲、历史数据环形存储、模拟 EEPROM (存升级元数据)
  ******************************************************************************
  */
/* USER CODE END Header */

#ifndef __W25Q64_H
#define __W25Q64_H

#ifdef __cplusplus
extern "C" {
#endif

#include "main.h"

/* JEDEC ID: 厂商 0xEF (华邦 Winbond) + 型号 0x4017 (W25Q64, 8MB) */
#define W25Q64_JEDEC_ID      0xEF4017

/* 擦除/写入粒度 (查数据手册):
 *   擦除最小单位是"扇区"4KB, 写入最小单位是"页"256 字节
 *   注意: 写入前必须先擦除, 而且只能把 1 写成 0, 擦除才能把 0 变回 1 */
#define W25Q64_PAGE_SIZE     256
#define W25Q64_SECTOR_SIZE   4096
#define W25Q64_TOTAL_SIZE    (8UL * 1024 * 1024)   /* 8MB */

/* ==================== 8MB 空间分区规划 ====================
 * 地址范围              大小      用途
 * 0x000000-0x07FFFF     512KB     OTA 固件下载缓冲区 (先下到这里, 校验通过再写片内 Flash)
 * 0x080000-0x081FFF     8KB       模拟 EEPROM: 双槽 A/B, 每槽 4KB (OTA 元数据等)
 * 0x082000-0x7FEFFF    7.49MB    历史数据环形区 (16 字节/条, 490752 条)
 * 0x7FF000-0x7FFFFF     4KB       保留给 W25Q_SelfTest 读写自测, 不存放任何数据
 * ======================================================== */
#define W25Q_OTA_BUFFER_ADDR   0x000000UL                          /* OTA 缓冲起始 */
#define W25Q_OTA_BUFFER_SIZE   (512UL * 1024)                      /* 512KB */
#define W25Q_EEPROM_ADDR       (W25Q_OTA_BUFFER_ADDR + W25Q_OTA_BUFFER_SIZE)
#define W25Q_EEPROM_SIZE       (8UL * 1024)                        /* 8KB = 双槽 x 4KB */
#define W25Q_HISTORY_ADDR      (W25Q_EEPROM_ADDR + W25Q_EEPROM_SIZE)
#define W25Q_SELFTEST_ADDR     (W25Q64_TOTAL_SIZE - W25Q64_SECTOR_SIZE)
#define W25Q_HISTORY_SIZE      (W25Q_SELFTEST_ADDR - W25Q_HISTORY_ADDR)

/**
  * @brief  上电自检: 读 JEDEC ID 并与期望值比对, 结果打印到调试串口
  * @retval 1=芯片在线且型号正确, 0=通信失败或型号不符
  */
uint8_t  W25Q_Init(void);

/**
  * @brief  读写擦回环自测: 擦除最后一个扇区 -> 写入测试数据 -> 读回比对
  * @note   只建议上电调试时调用一次; 会破坏该扇区原有内容
  *         (自测用的是 8MB 的最后一个 4KB 扇区, 目前未存放任何数据)
  * @retval 1=通过, 0=失败(原因会打印到调试串口)
  */
uint8_t  W25Q_SelfTest(void);

/**
  * @brief  读 JEDEC ID (不含校验, 读不到时返回 0)
  */
uint32_t W25Q_ReadID(void);

/**
  * @brief  任意地址读 (无长度上限, 内部自动分段)
  */
void     W25Q_Read(uint32_t addr, uint8_t *buf, uint32_t len);

/**
  * @brief  任意地址写 (内部自动按页分段, 不会跨页)
  * @note   写入区域必须已擦除, 否则写出来的是新旧数据的"按位与"
  * @retval 1=成功, 0=失败
  */
uint8_t  W25Q_Write(uint32_t addr, const uint8_t *buf, uint32_t len);

/**
  * @brief  擦除一个扇区 (4KB, 地址会向下对齐到扇区起点)
  * @retval 1=成功, 0=失败
  */
uint8_t  W25Q_SectorErase(uint32_t addr);

#ifdef __cplusplus
}
#endif

#endif /* __W25Q64_H */
