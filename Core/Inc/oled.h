/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    oled.h
  * @brief   0.96" SSD1306 OLED (I2C) 驱动头文件
  *          挂在 I2C1 总线: 128x64, 6x8 小字一行 8 像素高, 共 8 行
  ******************************************************************************
  */
/* USER CODE END Header */

#ifndef __OLED_H
#define __OLED_H

#ifdef __cplusplus
extern "C" {
#endif

#include "main.h"

/* OLED I2C 地址 (0x3C << 1 = 0x78) */
#define OLED_ADDR       (0x3C << 1)
#define OLED_I2C_TIMEOUT  100   /* ms */

/* 屏幕参数 */
#define OLED_WIDTH      128
#define OLED_HEIGHT     64
#define OLED_PAGES      8      /* 64 / 8: 整个屏幕分成 8 个页, 每页 8 像素高 */
#define OLED_CHAR_W     6      /* 小字宽 6px */
#define OLED_CHAR_H     8      /* 小字高 8px */

/* 大字参数: 由 6x8 小字库 2 倍放大而来 */
#define OLED_BIG_W      12                          /* 大字宽 12px */
#define OLED_BIG_H      16                          /* 大字高 16px (跨 2 个页) */
#define OLED_BIG_LINES  (OLED_HEIGHT / OLED_BIG_H)  /* 大字只有 4 行, 正好铺满屏幕 */

void OLED_Init(void);
void OLED_Clear(void);
void OLED_SetCursor(uint8_t page, uint8_t column);
void OLED_ShowChar(uint8_t x, uint8_t y, char ch);
void OLED_ShowString(uint8_t x, uint8_t y, const char *str);
void OLED_ShowNum(uint8_t x, uint8_t y, uint32_t num, uint8_t len);
void OLED_ShowSignedNum(uint8_t x, uint8_t y, int32_t num, uint8_t len);

/* 大字版接口: 第 2 个参数是"行号 0~3", 不是页号 (每个大字占 2 个页) */
void OLED_ShowBigChar(uint8_t x, uint8_t line, char ch);
void OLED_ShowBigString(uint8_t x, uint8_t line, const char *str);
void OLED_ShowBigNum(uint8_t x, uint8_t line, uint32_t num, uint8_t len);

uint8_t OLED_IsOnline(void);   /* 1=OLED 在线, 0=未连接 */

#ifdef __cplusplus
}
#endif

#endif /* __OLED_H */
