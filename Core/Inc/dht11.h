/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    dht11.h
  * @brief   DHT11 温湿度传感器驱动头文件 (单总线协议)
  *          数据引脚: PD0
  ******************************************************************************
  */
/* USER CODE END Header */

#ifndef __DHT11_H
#define __DHT11_H

#ifdef __cplusplus
extern "C" {
#endif

#include "main.h"

typedef struct
{
  uint8_t temp_int;   /* 温度整数部分 */
  uint8_t temp_dec;   /* 温度小数部分 */
  uint8_t humi_int;   /* 湿度整数部分 */
  uint8_t humi_dec;   /* 湿度小数部分 */
} DHT11_Data_t;

void DHT11_Init(void);
uint8_t DHT11_Read(DHT11_Data_t *data);   /* 返回 1=成功, 0=失败 */

/* 最近一次读取的错误状态 (供调试): 0=正常 1=无应答 2=位超时 3=校验和错误 */
extern volatile uint8_t g_dht11_err;

#ifdef __cplusplus
}
#endif

#endif /* __DHT11_H */
