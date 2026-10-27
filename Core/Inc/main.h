/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.h
  * @brief          : Header for main.c file.
  *                   This file contains the common defines of the application.
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */

/* Define to prevent recursive inclusion -------------------------------------*/
#ifndef __MAIN_H
#define __MAIN_H

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include "stm32f4xx_hal.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */

/* USER CODE END Includes */

/* Exported types ------------------------------------------------------------*/
/* USER CODE BEGIN ET */
/* 阈值配置: 温度/湿度/光照的上下限, 供"本地控制闭环"判断使用
 * 上电时由 control.c 填入默认值, 后续可被云端下发的配置覆盖 */
typedef struct
{
  float temp_max;   /* 温度上限 (C) */
  float temp_min;   /* 温度下限 (C) */
  float humi_max;   /* 湿度上限 (%) */
  float humi_min;   /* 湿度下限 (%) */
  float lux_min;    /* 光照下限 (lx) */
  float lux_max;    /* 光照上限 (lx) */
  float temp_rate_max; /* 温度变化率上限 (C/min): 5.0 约等于"10 秒变化 1C"就报警 */
} ThresholdConfig_t;

/* USER CODE END ET */

/* Exported constants --------------------------------------------------------*/
/* USER CODE BEGIN EC */

/* USER CODE END EC */

/* Exported macro ------------------------------------------------------------*/
/* USER CODE BEGIN EM */

/* USER CODE END EM */

/* Exported functions prototypes ---------------------------------------------*/
void Error_Handler(void);

/* USER CODE BEGIN EFP */
/* 线程安全的串口打印, 替代 printf 防止多任务字符交错 */
void DebugPrint(const char *fmt, ...);

/* 全局阈值配置: 变量本体定义在 control.c, 其它文件通过这个声明访问它 */
extern volatile ThresholdConfig_t g_threshold;

/* USER CODE END EFP */

/* Private defines -----------------------------------------------------------*/
#define W25Q_CS_Pin GPIO_PIN_4
#define W25Q_CS_GPIO_Port GPIOA
#define DHT11_DATA_Pin GPIO_PIN_0
#define DHT11_DATA_GPIO_Port GPIOD

/* USER CODE BEGIN Private defines */
/* 板载 LED：PC13 低电平点亮（CubeMX Label 未生效，手动定义） */
#define LED_Pin GPIO_PIN_13
#define LED_GPIO_Port GPIOC

/* 执行器引脚 (4 路推挽输出, 接继电器/蜂鸣器模块):
 *   PE0 = 风扇继电器   PE1 = 水泵继电器
 *   PE2 = 补光灯继电器 PE3 = 蜂鸣器 */
#define FAN_RELAY_Pin         GPIO_PIN_0
#define FAN_RELAY_GPIO_Port   GPIOE
#define PUMP_RELAY_Pin        GPIO_PIN_1
#define PUMP_RELAY_GPIO_Port  GPIOE
#define LIGHT_RELAY_Pin       GPIO_PIN_2
#define LIGHT_RELAY_GPIO_Port GPIOE
#define BUZZER_Pin            GPIO_PIN_3
#define BUZZER_GPIO_Port      GPIOE

/* USER CODE END Private defines */

#ifdef __cplusplus
}
#endif

#endif /* __MAIN_H */
