/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    ota.h
  * @brief   OTA 固件缓冲区 + 升级元数据链路 (只做"存储侧")
  *
  * 这一层解决的是: "拿到固件数据后, 怎么安全地放进 W25Q64, 以及怎么在
  * 模拟 EEPROM 里记下'有一份校验通过、等着安装的新固件'"。
  * 不含从网络下载固件, 也不含写片内 Flash / Bootloader 跳转 (那是 OTA 本体)。
  *
  * 完整链路:
  *   OTA_BufferBegin(大小, 版本)    复位写入位置, 准备接收 (会作废上一份待装固件)
  *        |  反复 OTA_BufferWrite() 顺序追加数据, 进新扇区自动擦除
  *        v
  *   OTA_BufferCommit(期望CRC)     回读整个缓冲区算 CRC16 并比对:
  *                                   一致 -> 把 fw_size/fw_crc/fw_version
  *                                           + upgrade_valid=1/upgrade_flag=1
  *                                           写进模拟 EEPROM (双槽 A/B 掉电安全)
  *                                   不一致 -> 元数据原样不动, 当作"没下过"
  *
  *   OTA_ImageReady()    查询: 现在是不是有一份校验通过、等着安装的新固件
  *   OTA_ImageConsumed() 安装完成后清标志 (Bootloader 或 APP 装完调用)
  *   OTA_PrintStatus()   把当前元数据打到调试串口
  *
  * 注意事项:
  *   - 写入必须"从缓冲区开头顺序追加", 不允许跳着写 (跳着写无法判断该扇区
  *     有没有擦过, 而 FLASH 不擦就写 = 新旧数据按位与)
  *   - 回读校验要读完整份固件 (最多 512KB), 是阻塞操作且耗时较长,
  *     调用方任务需要自己保证看门狗被喂到
  *   - 本模块内部不加锁, 调用方用 StorageMutex 包住 (与 history/param 一致)
  ******************************************************************************
  */
/* USER CODE END Header */

#ifndef __OTA_H
#define __OTA_H

#ifdef __cplusplus
extern "C" {
#endif

#include "main.h"
#include "w25q64.h"

/* 单次可接收的固件上限: 就是分区表里划给 OTA 的那 512KB */
#define OTA_IMAGE_MAX_SIZE   W25Q_OTA_BUFFER_SIZE

/* 接口返回值 */
#define OTA_OK         0   /* 成功 */
#define OTA_ERR_PARAM  1   /* 参数不对: 大小超限 / 写入越界 / 长度为 0 */
#define OTA_ERR_FLASH  2   /* W25Q64 擦除、写入或元数据落盘失败 */
#define OTA_ERR_CRC    3   /* 回读算出的 CRC 和期望值对不上 (固件不完整或被改坏) */
#define OTA_ERR_STATE  4   /* 状态不对: 还没 Begin 就 Write, 或没写满就 Commit */

/**
  * @brief  开始接收一份新固件: 复位写入位置, 记住总长度和目标版本
  * @param  fw_size:    固件总字节数 (必须 1 ~ OTA_IMAGE_MAX_SIZE)
  * @param  fw_version: 目标版本号 (主版本<<8 | 次版本), 供上报和回滚判断
  * @retval OTA_OK / OTA_ERR_PARAM
  * @note   如果模拟 EEPROM 里还留着上一份"待安装"标志, 这里会先把它清掉 ——
  *         因为本次下载会覆盖缓冲区, 那份固件已经不再完整了
  */
uint8_t  OTA_BufferBegin(uint32_t fw_size, uint16_t fw_version);

/**
  * @brief  顺序往缓冲区追加一段固件数据
  * @param  data: 数据首地址; len: 本段字节数
  * @retval OTA_OK / OTA_ERR_PARAM / OTA_ERR_FLASH
  * @note   进入新扇区时自动擦除该扇区; 累计长度不能超过 Begin 时声明的大小
  */
uint8_t  OTA_BufferWrite(const uint8_t *data, uint32_t len);

/**
  * @brief  收尾: 回读整个缓冲区校验, 通过才把元数据写进模拟 EEPROM
  * @param  expect_crc: 固件提供方给的 CRC16 (与 W25Q64 里读回来的比对)
  * @retval OTA_OK / OTA_ERR_STATE / OTA_ERR_CRC / OTA_ERR_FLASH
  * @note   校验不通过时元数据不会被改动, 升级标志也不会置起来
  */
uint8_t  OTA_BufferCommit(uint16_t expect_crc);

/**
  * @brief  现在是否有一份"校验通过、等着安装"的新固件
  * @retval 1=有, 0=没有
  */
uint8_t  OTA_ImageReady(void);

/**
  * @brief  安装完成后清掉升级标志 (元数据落盘), 避免下次上电又装一遍
  */
void     OTA_ImageConsumed(void);

/**
  * @brief  把当前的 OTA 元数据打印到调试串口 (上电时调用一次)
  */
void     OTA_PrintStatus(void);

#ifdef __cplusplus
}
#endif

#endif /* __OTA_H */
