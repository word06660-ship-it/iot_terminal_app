/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    bh1750.c
  * @brief   BH1750 光照传感器驱动
  *          使用 HAL 硬件 I2C1 (PB6/PB7), 阻塞式读写
  *
  * 测量模式: 连续 H 分辨率 (1lx), 测量时间约 120ms
  * 换算公式: 亮度(lx) = 寄存器值 / 1.2
  ******************************************************************************
  */
/* USER CODE END Header */

#include "bh1750.h"

extern I2C_HandleTypeDef hi2c1;   /* main.c 中的 I2C1 句柄 */

#define BH1750_I2C_TIMEOUT  100   /* ms */

/* BH1750 是否在线 (初始化时检测, 未连接则跳过读写避免忙等) */
static uint8_t g_bh1750_online = 0;

uint8_t BH1750_IsOnline(void)
{
  return g_bh1750_online;
}

/* 向传感器发一条命令 (写 1 字节) */
static uint8_t BH1750_WriteCmd(uint8_t cmd)
{
  return (HAL_I2C_Master_Transmit(&hi2c1, BH1750_ADDR << 1, &cmd, 1,
                                  BH1750_I2C_TIMEOUT) == HAL_OK);
}

/**
  * @brief BH1750 初始化: 上电
  * @retval 1=成功, 0=失败
  */
uint8_t BH1750_Init(void)
{
  /* 先检测设备是否在线, 不在线直接返回 (I2C 忙等会阻塞 CPU) */
  if (HAL_I2C_IsDeviceReady(&hi2c1, BH1750_ADDR << 1, 3, BH1750_I2C_TIMEOUT) != HAL_OK)
  {
    g_bh1750_online = 0;
    return 0;
  }
  g_bh1750_online = 1;

  /* 上电 */
  if (!BH1750_WriteCmd(BH1750_POWER_ON))
  {
    return 0;
  }
  /* 复位寄存器 (清除可能残留的测量) */
  BH1750_WriteCmd(BH1750_RESET);
  HAL_Delay(10);
  return 1;
}

/**
  * @brief 读取一次光照强度
  * @retval 光照值 (lx), 失败返回 0
  */
uint16_t BH1750_ReadLux(void)
{
  uint8_t buf[2];
  uint16_t reg;
  uint32_t lux_x10;

  if (!g_bh1750_online)
  {
    return 0;
  }

  /* 启动一次连续 H 分辨率测量 */
  if (!BH1750_WriteCmd(BH1750_CONT_H_RES))
  {
    return 0;
  }

  /* 等待测量完成 (H 模式约 120ms) */
  HAL_Delay(180);

  /* 读取 2 字节结果 (高字节在前) */
  if (HAL_I2C_Master_Receive(&hi2c1, BH1750_ADDR << 1, buf, 2,
                             BH1750_I2C_TIMEOUT) != HAL_OK)
  {
    return 0;
  }

  reg = (uint16_t)((buf[0] << 8) | buf[1]);

  /* lx = reg / 1.2, 用整数运算避免浮点: *10/12 */
  lux_x10 = (uint32_t)reg * 10 / 12;
  if (lux_x10 > 0xFFFF)
  {
    lux_x10 = 0xFFFF;
  }
  return (uint16_t)lux_x10;
}
