                                                      /* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    bh1750.h
  * @brief   BH1750 光照传感器驱动头文件
  *          挂在 I2C1 总线, 与 OLED 共享 (地址不同不冲突)
  ******************************************************************************
  */
/* USER CODE END Header */

#ifndef __BH1750_H
#define __BH1750_H

#ifdef __cplusplus
extern "C" {
#endif

#include "main.h"

/* I2C 地址: 模块 ADDR 脚接低 = 0x23, 接高 = 0x5C */
#define BH1750_ADDR         0x23

/* 常用指令 (值查 BH1750 数据手册) */
#define BH1750_POWER_DOWN   0x00
#define BH1750_POWER_ON     0x01
#define BH1750_RESET        0x07
#define BH1750_CONT_H_RES   0x10   /* 连续 H 分辨率, 1lx, 测量约120ms */
#define BH1750_CONT_H_RES2  0x11   /* 连续 H 分辨率2, 0.5lx, 测量约120ms */
#define BH1750_CONT_L_RES   0x13   /* 连续 L 分辨率, 4lx, 测量约16ms */

/* 注意容易混的一组: 0x20/0x21/0x23 是"单次"模式, 测完自动回到掉电状态,
 * 想要连续测量必须用上面的 0x10/0x11/0x13 */
#define BH1750_ONE_H_RES    0x20   /* 单次 H 分辨率, 1lx */
#define BH1750_ONE_H_RES2   0x21   /* 单次 H 分辨率2, 0.5lx */
#define BH1750_ONE_L_RES    0x23   /* 单次 L 分辨率, 4lx */

uint8_t  BH1750_Init(void);
uint16_t BH1750_ReadLux(void);
uint8_t  BH1750_IsOnline(void);   /* 1=在线, 0=未连接 */

#ifdef __cplusplus
}
#endif

#endif /* __BH1750_H */
