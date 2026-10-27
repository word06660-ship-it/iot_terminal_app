/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    dht11.c
  * @brief   DHT11 温湿度传感器驱动 (单总线协议)
  *
  * 引脚:    PD0 (DHT11_DATA)
  * 协议:    主机拉低总线 18ms 后释放 -> 等待应答 -> 应答低80us+高80us
  *          传 40bit 数据 (0: 高电平26~28us, 1: 高电平70us)
  * 时序实现: 使用 DWT 周期计数器做微秒延时与超时判断, 不受调度影响
  *
  * 注意:    读取时序为微秒级, 接收阶段需关闭中断/调度;
  *          总线需上拉, 已启用内部上拉, 建议外接 4.7K 上拉电阻
  ******************************************************************************
  */
/* USER CODE END Header */

#include "dht11.h"
#include "cmsis_os.h"
#include "FreeRTOS.h"
#include "task.h"

/* DWT 周期计数器每微秒对应的 tick 数 (168MHz) */
#define DWT_TICKS_PER_US   (SystemCoreClock / 1000000)

/* 最近一次读取的错误状态 (供调试/界面提示): 0=正常 1=无应答 2=位超时 3=校验和错误 */
volatile uint8_t g_dht11_err = 0;

/* 切换引脚方向: DHT11 为单总线, 主机发送时配推挽输出, 接收时配输入 */
static void DHT11_SetOutput(void)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};
  GPIO_InitStruct.Pin = DHT11_DATA_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_HIGH;
  HAL_GPIO_Init(DHT11_DATA_GPIO_Port, &GPIO_InitStruct);
}

static void DHT11_SetInput(void)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};
  GPIO_InitStruct.Pin = DHT11_DATA_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
  GPIO_InitStruct.Pull = GPIO_PULLUP;   /* 启用内部上拉, 建议外接 4.7K 上拉电阻 */
  HAL_GPIO_Init(DHT11_DATA_GPIO_Port, &GPIO_InitStruct);
}

/* 微秒延时 (基于 DWT, 不依赖 SysTick 中断, 时序关键阶段依然精确) */
static void DHT11_DelayUs(uint32_t us)
{
  uint32_t start = DWT->CYCCNT;
  uint32_t ticks = us * DWT_TICKS_PER_US;
  while ((DWT->CYCCNT - start) < ticks) { }
}

/**
  * @brief 等待总线电平变为指定值, 带超时
  * @param level: 期望电平 (GPIO_PIN_SET / GPIO_PIN_RESET)
  * @param timeout_us: 超时时间
  * @retval 1=等到目标电平, 0=超时
  */
static uint8_t DHT11_WaitLevel(uint8_t level, uint32_t timeout_us)
{
  uint32_t start = DWT->CYCCNT;
  while ((DWT->CYCCNT - start) < (timeout_us * DWT_TICKS_PER_US))
  {
    if (HAL_GPIO_ReadPin(DHT11_DATA_GPIO_Port, DHT11_DATA_Pin) == level)
    {
      return 1;
    }
  }
  return 0;
}

/**
  * @brief 初始化 DWT 周期计数器 (供微秒级时序使用)
  */
void DHT11_Init(void)
{
  CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;   /* 使能 DWT 追踪 */
  DWT->CYCCNT = 0;
  DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;              /* 启动周期计数器 */
}

/**
  * @brief 读取 DHT11 数据
  * @param data: 输出数据缓冲区
  * @retval 1=读取成功且校验通过, 0=失败
  */
uint8_t DHT11_Read(DHT11_Data_t *data)
{
  uint8_t bits[40];
  uint8_t buf[5];
  uint8_t i, byte_idx;
  uint8_t bit;

  if (data == NULL)
  {
    return 0;
  }

  /* ---------- 1. 主机起始信号: 拉低后释放 ---------- */
  DHT11_SetOutput();
  HAL_GPIO_WritePin(DHT11_DATA_GPIO_Port, DHT11_DATA_Pin, GPIO_PIN_RESET);
  HAL_Delay(18);                /* 起始低电平 >=18ms */
  HAL_GPIO_WritePin(DHT11_DATA_GPIO_Port, DHT11_DATA_Pin, GPIO_PIN_SET);
  DHT11_DelayUs(30);            /* 释放总线 20~40us */
  DHT11_SetInput();

  /* ---------- 2. 接收 40bit (关调度保护, 时序不可打断) ---------- */
  taskENTER_CRITICAL();

  /* 等待传感器应答: 拉低总线 80us */
  if (!DHT11_WaitLevel(GPIO_PIN_RESET, 100))
  {
    g_dht11_err = 1;   /* 错误1: 无应答/器件未接 */
    taskEXIT_CRITICAL();
    return 0;
  }
  /* 应答低电平 80us */
  if (!DHT11_WaitLevel(GPIO_PIN_SET, 100))
  {
    g_dht11_err = 1;   /* 应答超时 */
    taskEXIT_CRITICAL(); 
    return 0;
  }

  /* 逐位接收: 每位起始低 50us + 高电平宽度判 0/1 */
  for (i = 0; i < 40; i++)
  {
    if (!DHT11_WaitLevel(GPIO_PIN_RESET, 100))   /* 等待位起始低电平 */
    {
      g_dht11_err = 2;   /* 错误2: 位传输超时 */
      taskEXIT_CRITICAL();
      return 0;
    }
    if (!DHT11_WaitLevel(GPIO_PIN_SET, 100))     /* 等待位数据高电平 */
    {
      g_dht11_err = 2;   /* 位超时 */
      taskEXIT_CRITICAL();
      return 0;
    }
    DHT11_DelayUs(40);           /* 高电平持续 40us 后再采样 */
    bit = HAL_GPIO_ReadPin(DHT11_DATA_GPIO_Port, DHT11_DATA_Pin);
    bits[i] = (bit == GPIO_PIN_SET) ? 1 : 0;
    /* 若为 '0', 高电平即将结束, 等待其拉低, 保证下一位从位起始处对齐 */
    if (bits[i] == 0)
    {
      DHT11_WaitLevel(GPIO_PIN_RESET, 100);
    }
  }
  taskEXIT_CRITICAL();

  /* ---------- 3. 按 5 字节组装并校验 ---------- */
  for (i = 0; i < 40; i++)
  {
    byte_idx = i / 8;
    buf[byte_idx] <<= 1;
    buf[byte_idx] |= bits[i];
  }

  /* 校验和: 前4字节之和 == 第5字节 */
  if ((buf[0] + buf[1] + buf[2] + buf[3]) != buf[4])
  {
    g_dht11_err = 3;   /* 错误3: 校验和不符, 数据可能受干扰 */
    return 0;
  }

  /* 湿度: byte0(整数) byte1(小数); 温度: byte2(整数) byte3(小数) */
  data->humi_int = buf[0];
  data->humi_dec = buf[1];
  data->temp_int = buf[2];
  data->temp_dec = buf[3];

  g_dht11_err = 0;
  return 1;
}
