/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    control.h
  * @brief   执行器驱动 + 本地控制闭环 (不依赖云端)
  *
  * 引脚:    PE0=风扇继电器  PE1=水泵继电器  PE2=补光灯继电器  PE3=蜂鸣器
  *
  * 思路:    SensorTask 每次采完数据就调用 Control_Evaluate(),
  *          由本模块把实测值与 g_threshold 里的阈值比对, 超限立刻拉引脚,
  *          整个过程不发网络请求, 所以断网也能保护。
  ******************************************************************************
  */
/* USER CODE END Header */

#ifndef __CONTROL_H
#define __CONTROL_H

#ifdef __cplusplus
extern "C" {
#endif

#include "main.h"

/* ==================== 用户配置区: 按你的硬件改这里 ==================== */
/* 执行器触发极性:
 *   1 = 高电平导通 (给高电平继电器吸合/蜂鸣器响)
 *   0 = 低电平导通 (常见蓝色继电器模块多为低触发)
 * 判断方法: 烧录后若上电继电器就"啪"一声常吸合, 把这里改成 0 */
#define ACTUATOR_ACTIVE_LEVEL   1

/* 滞回死区: "触发阈值" 与 "释放阈值" 之间的差值。
 * 没有它的话, 数值在阈值附近抖动时执行器会疯狂开-关-开-关;
 * 有了它, 超过阈值才开, 要退回到"阈值-滞回"以下才关, 中间区域保持原状态。 */
#define HYST_TEMP   1.0f    /* 温度滞回 (C) */
#define HYST_HUMI   2.0f    /* 湿度滞回 (%) */
#define HYST_LUX    50.0f   /* 光照滞回 (lx) */

/* 温度"变化率"的观察窗口 (ms):
 * 每隔这么久, 用"当前温度"和"窗口起点温度"算一次变化速率,
 * 再和 g_threshold.temp_rate_max (C/min) 比较。
 * 默认 10 秒, 对应你说的"10 秒内温度变化多少度" */
#define RATE_WINDOW_MS   10000
/* ===================================================================== */

/* 由"逻辑开关"换算出的实际电平: 统一在这里换算, 换硬件只改上面一个宏 */
#define ACTUATOR_ON_LEVEL   ((ACTUATOR_ACTIVE_LEVEL) ? GPIO_PIN_SET : GPIO_PIN_RESET)
#define ACTUATOR_OFF_LEVEL  ((ACTUATOR_ACTIVE_LEVEL) ? GPIO_PIN_RESET : GPIO_PIN_SET)

void Control_Init(void);
void Control_Evaluate(float t, float h, float l);

void Relay_Fan_Set(uint8_t on);
void Relay_Pump_Set(uint8_t on);
void Relay_Light_Set(uint8_t on);
void Buzzer_Set(uint8_t on);

#ifdef __cplusplus
}
#endif

#endif /* __CONTROL_H */
