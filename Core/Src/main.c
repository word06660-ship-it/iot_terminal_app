/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
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
/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "cmsis_os.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include "oled.h"
#include "dht11.h"
#include "bh1750.h"
#include "esp8266.h"
#include "control.h"
#include "w25q64.h"
#include "param_store.h"
#include "history.h"
#include "ota.h"

/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* WiFiTask 每轮最多补传几条: 一次发太多会让这一个循环占用过久 */
#define HIST_RESEND_BATCH   4U

/* SensorTask 的采样周期 (与循环末尾的 osDelay 保持一致) */
#define SENSOR_PERIOD_MS    2000U

/* 断网后重连失败时的重试间隔 */
#define NET_RETRY_PERIOD_MS 10000U

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
I2C_HandleTypeDef hi2c1;

IWDG_HandleTypeDef hiwdg;

RTC_HandleTypeDef hrtc;

SPI_HandleTypeDef hspi1;

UART_HandleTypeDef huart1;
UART_HandleTypeDef huart2;
DMA_HandleTypeDef hdma_usart1_rx;

/* Definitions for StatusTask */
osThreadId_t StatusTaskHandle;
const osThreadAttr_t StatusTask_attributes = {
  .name = "StatusTask",
  /* 栈大小: DebugPrint 局部 buf[192] + vsnprintf 调用链, 512B 可能栈溢出, 加大到 1KB */
  .stack_size = 256 * 4,
  /* 高优先级: 保证心跳 LED 与喂狗不被 WiFiTask 长时间阻塞影响 */
  .priority = (osPriority_t) osPriorityHigh,
};
/* Definitions for SensorTask */
osThreadId_t SensorTaskHandle;
const osThreadAttr_t SensorTask_attributes = {
  .name = "SensorTask",
  .stack_size = 512 * 4,
  .priority = (osPriority_t) osPriorityNormal,
};
/* Definitions for WiFiTask */
osThreadId_t WiFiTaskHandle;
const osThreadAttr_t WiFiTask_attributes = {
  .name = "WiFiTask",
  .stack_size = 512 * 4,
  .priority = (osPriority_t) osPriorityNormal,
};
/* USER CODE BEGIN PV */

/* I2C 总线互斥锁 (BH1750 与 OLED 共用 I2C1, 防止并发访问) */
osMutexId_t I2CMutex;

/* 串口打印互斥锁 (printf 多任务输出会字符交错, 加锁保证整行输出) */
osMutexId_t PrintMutex;

/* 存储互斥锁 (SensorTask 写历史记录与 WiFiTask 补传读记录会同时访问 W25Q64) */
osMutexId_t StorageMutex;

/* W25Q64 是否在线: 不在线就跳过所有参数/历史数据存取, 系统照常采集上报 */
static uint8_t g_w25q_ok = 0;

/* 传感器数据消息队列 (SensorTask 发送到 WiFiTask) */
typedef struct
{
  uint8_t temp_int;
  uint8_t humi_int;
  uint16_t lux;
} SensorData_t;
osMessageQueueId_t SensorQueue;

/* 网络状态标志 (WiFiTask 更新, SensorTask 读取并显示到 OLED) */
typedef enum
{
  NET_IDLE = 0,
  NET_WIFI_CONNECTING,
  NET_WIFI_OK,
  NET_MQTT_CONNECTING,
  NET_MQTT_OK,
  NET_FAIL,
} NetState_t;
volatile NetState_t g_net_state = NET_IDLE;

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_DMA_Init(void);
static void MX_I2C1_Init(void);
static void MX_SPI1_Init(void);
static void MX_USART1_UART_Init(void);
static void MX_USART2_UART_Init(void);
static void MX_IWDG_Init(void);
static void MX_RTC_Init(void);
void StartStatusTask(void *argument);
void StartSensorTask(void *argument);
void StartWiFiTask(void *argument);

/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */
/* printf 重定向到调试串口 USART2 */
int __io_putchar(int ch)
{
  HAL_UART_Transmit(&huart2, (uint8_t *)&ch, 1, 100);
  return ch;
}

/**
  * @brief 线程安全的调试打印 (整行缓冲后一次发送)
  * @note  用 DebugPrint 替代 printf, 防止多任务同时打印导致字符交错
  */
void DebugPrint(const char *fmt, ...)
{
  char buf[192];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);

  if (PrintMutex != NULL)
  {
    osMutexAcquire(PrintMutex, osWaitForever);
    HAL_UART_Transmit(&huart2, (uint8_t *)buf, (uint16_t)strlen(buf), 500);
    osMutexRelease(PrintMutex);
  }
  else
  {
    /* 互斥锁尚未创建(调度器启动前), 直接发送 */
    HAL_UART_Transmit(&huart2, (uint8_t *)buf, (uint16_t)strlen(buf), 500);
  }
}

/**
  * @brief 建立网络连接: 探测模块 → 连 WiFi → 连 MQTT
  * @note  开机首次连接和运行中重连都调用它, 全程带超时不会永久卡死
  * @retval 1=MQTT 已连上, 0=任一步失败(状态已置 NET_FAIL)
  */
static uint8_t WiFi_Connect(void)
{
  uint8_t try_cnt = 0;
  ESP_Status_t st;

  g_net_state = NET_WIFI_CONNECTING;

  /* 1. 与 ESP8266 做 AT 握手, 确认模块在线 (最多 5 次) */
  do
  {
    st = ESP_SendCmd("AT\r\n", "OK", 1000);
    try_cnt++;
  } while ((st != ESP_OK) && (try_cnt < 5));

  if (st != ESP_OK)
  {
    DebugPrint("[WiFi] ESP8266 no response!\r\n");
    g_net_state = NET_FAIL;
    return 0;
  }
  DebugPrint("[WiFi] ESP8266 ready\r\n");

  /* 2. 连接 WiFi (内部等待分配 IP) */
  if (ESP_ConnectWiFi(WIFI_SSID, WIFI_PASSWORD) != ESP_OK)
  {
    DebugPrint("[WiFi] connect FAIL\r\n");
    g_net_state = NET_FAIL;
    return 0;
  }
  DebugPrint("[WiFi] connected\r\n");
  g_net_state = NET_WIFI_OK;

  /* 3. 连接 MQTT */
  g_net_state = NET_MQTT_CONNECTING;
  if (ESP_MQTTConnect(MQTT_BROKER, MQTT_PORT, MQTT_CLIENTID, "", "") != ESP_OK)
  {
    DebugPrint("[MQTT] connect FAIL\r\n");
    g_net_state = NET_FAIL;
    return 0;
  }
  DebugPrint("[MQTT] connected to %s:%d\r\n", MQTT_BROKER, MQTT_PORT);
  g_net_state = NET_MQTT_OK;
  return 1;
}

/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_DMA_Init();
  MX_I2C1_Init();
  MX_SPI1_Init();
  MX_USART1_UART_Init();
  MX_USART2_UART_Init();
  MX_IWDG_Init();
  MX_RTC_Init();
  /* USER CODE BEGIN 2 */
  /* Reset reason diagnosis: IWDG watchdog reset? (PrintMutex is NULL here, DebugPrint falls back to direct send) */
  if (__HAL_RCC_GET_FLAG(RCC_FLAG_IWDGRST))
  {
    DebugPrint("[RST] IWDG watchdog reset!\r\n");
  }
  __HAL_RCC_CLEAR_RESET_FLAGS();

  /* 执行器初始化: 上电先把 4 路输出置为"关", 保证安全默认状态
   * (引脚模式已在 MX_GPIO_Init() 里配好, 这里只负责电平) */
  Control_Init();

  /* W25Q64: 先确认芯片在线, 再挂上"模拟 EEPROM"参数区和历史数据区 */
  if (W25Q_Init())
  {
    g_w25q_ok = 1;

    /* 读写擦回环自测: 用分区表里预留的自测扇区, 不碰历史数据 (约几十 ms) */
    W25Q_SelfTest();

    /* 模拟 EEPROM: 从双槽 A/B 里载入 OTA 元数据与历史指针, 并把重启次数 +1 */
    Param_Init();

    /* 历史数据区: 找回最后一条记录的时间戳, 防止 RTC 复位后时间戳倒退 */
    History_Init();

    /* OTA 元数据: 打印模拟 EEPROM 里存着的升级标志与固件信息 */
    OTA_PrintStatus();
  }
  else
  {
    DebugPrint("[W25Q] offline! 历史存储/断网补传已停用\r\n");
  }
  /* USER CODE END 2 */

  /* Init scheduler */
  osKernelInitialize();

  /* USER CODE BEGIN RTOS_MUTEX */
  /* add mutexes, ... */
  I2CMutex = osMutexNew(NULL);
  PrintMutex = osMutexNew(NULL);
  StorageMutex = osMutexNew(NULL);
  /* USER CODE END RTOS_MUTEX */

  /* USER CODE BEGIN RTOS_SEMAPHORES */
  /* add semaphores, ... */
  /* USER CODE END RTOS_SEMAPHORES */

  /* USER CODE BEGIN RTOS_TIMERS */
  /* start timers, add new ones, ... */
  /* USER CODE END RTOS_TIMERS */

  /* USER CODE BEGIN RTOS_QUEUES */
  /* add queues, ... */
  SensorQueue = osMessageQueueNew(4, sizeof(SensorData_t), NULL);
  /* USER CODE END RTOS_QUEUES */

  /* Create the thread(s) */
  /* creation of StatusTask */
  StatusTaskHandle = osThreadNew(StartStatusTask, NULL, &StatusTask_attributes);

  /* creation of SensorTask */
  SensorTaskHandle = osThreadNew(StartSensorTask, NULL, &SensorTask_attributes);

  /* creation of WiFiTask */
  WiFiTaskHandle = osThreadNew(StartWiFiTask, NULL, &WiFiTask_attributes);

  /* USER CODE BEGIN RTOS_THREADS */
  /* add threads, ... */
  /* USER CODE END RTOS_THREADS */

  /* USER CODE BEGIN RTOS_EVENTS */
  /* add events, ... */
  /* USER CODE END RTOS_EVENTS */

  /* Start scheduler */
  osKernelStart();

  /* We should never get here as control is now taken by the scheduler */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
  }
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Configure the main internal regulator output voltage
  */
  __HAL_RCC_PWR_CLK_ENABLE();
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_LSI|RCC_OSCILLATORTYPE_HSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_ON;
  RCC_OscInitStruct.LSIState = RCC_LSI_ON;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLM = 8;
  RCC_OscInitStruct.PLL.PLLN = 336;
  RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV2;
  RCC_OscInitStruct.PLL.PLLQ = 4;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV4;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV2;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_5) != HAL_OK)
  {
    Error_Handler();
  }
}

/**
  * @brief I2C1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_I2C1_Init(void)
{

  /* USER CODE BEGIN I2C1_Init 0 */

  /* USER CODE END I2C1_Init 0 */

  /* USER CODE BEGIN I2C1_Init 1 */

  /* USER CODE END I2C1_Init 1 */
  hi2c1.Instance = I2C1;
  hi2c1.Init.ClockSpeed = 100000;
  hi2c1.Init.DutyCycle = I2C_DUTYCYCLE_2;
  hi2c1.Init.OwnAddress1 = 0;
  hi2c1.Init.AddressingMode = I2C_ADDRESSINGMODE_7BIT;
  hi2c1.Init.DualAddressMode = I2C_DUALADDRESS_DISABLE;
  hi2c1.Init.OwnAddress2 = 0;
  hi2c1.Init.GeneralCallMode = I2C_GENERALCALL_DISABLE;
  hi2c1.Init.NoStretchMode = I2C_NOSTRETCH_DISABLE;
  if (HAL_I2C_Init(&hi2c1) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN I2C1_Init 2 */

  /* USER CODE END I2C1_Init 2 */

}

/**
  * @brief IWDG Initialization Function
  * @param None
  * @retval None
  */
static void MX_IWDG_Init(void)
{

  /* USER CODE BEGIN IWDG_Init 0 */

  /* USER CODE END IWDG_Init 0 */

  /* USER CODE BEGIN IWDG_Init 1 */

  /* USER CODE END IWDG_Init 1 */
  hiwdg.Instance = IWDG;
  hiwdg.Init.Prescaler = IWDG_PRESCALER_128;
  hiwdg.Init.Reload = 1000;
  if (HAL_IWDG_Init(&hiwdg) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN IWDG_Init 2 */

  /* USER CODE END IWDG_Init 2 */

}

/**
  * @brief RTC Initialization Function
  * @param None
  * @retval None
  */
static void MX_RTC_Init(void)
{

  /* USER CODE BEGIN RTC_Init 0 */

  /* USER CODE END RTC_Init 0 */

  RTC_TimeTypeDef sTime = {0};
  RTC_DateTypeDef sDate = {0};

  /* USER CODE BEGIN RTC_Init 1 */

  /* USER CODE END RTC_Init 1 */

  /** Initialize RTC Only
  */
  hrtc.Instance = RTC;
  hrtc.Init.HourFormat = RTC_HOURFORMAT_24;
  hrtc.Init.AsynchPrediv = 127;
  hrtc.Init.SynchPrediv = 255;
  hrtc.Init.OutPut = RTC_OUTPUT_DISABLE;
  hrtc.Init.OutPutPolarity = RTC_OUTPUT_POLARITY_HIGH;
  hrtc.Init.OutPutType = RTC_OUTPUT_TYPE_OPENDRAIN;
  if (HAL_RTC_Init(&hrtc) != HAL_OK)
  {
    Error_Handler();
  }

  /* USER CODE BEGIN Check_RTC_BKUP */

  /* USER CODE END Check_RTC_BKUP */

  /** Initialize RTC and set the Time and Date
  */
  sTime.Hours = 0x0;
  sTime.Minutes = 0x0;
  sTime.Seconds = 0x0;
  sTime.DayLightSaving = RTC_DAYLIGHTSAVING_NONE;
  sTime.StoreOperation = RTC_STOREOPERATION_RESET;
  if (HAL_RTC_SetTime(&hrtc, &sTime, RTC_FORMAT_BCD) != HAL_OK)
  {
    Error_Handler();
  }
  sDate.WeekDay = RTC_WEEKDAY_MONDAY;
  sDate.Month = RTC_MONTH_JANUARY;
  sDate.Date = 0x1;
  sDate.Year = 0x0;

  if (HAL_RTC_SetDate(&hrtc, &sDate, RTC_FORMAT_BCD) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN RTC_Init 2 */

  /* USER CODE END RTC_Init 2 */

}

/**
  * @brief SPI1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_SPI1_Init(void)
{

  /* USER CODE BEGIN SPI1_Init 0 */

  /* USER CODE END SPI1_Init 0 */

  /* USER CODE BEGIN SPI1_Init 1 */

  /* USER CODE END SPI1_Init 1 */
  /* SPI1 parameter configuration*/
  hspi1.Instance = SPI1;
  hspi1.Init.Mode = SPI_MODE_MASTER;
  hspi1.Init.Direction = SPI_DIRECTION_2LINES;
  hspi1.Init.DataSize = SPI_DATASIZE_8BIT;
  hspi1.Init.CLKPolarity = SPI_POLARITY_LOW;
  hspi1.Init.CLKPhase = SPI_PHASE_1EDGE;
  hspi1.Init.NSS = SPI_NSS_SOFT;
  hspi1.Init.BaudRatePrescaler = SPI_BAUDRATEPRESCALER_16;
  hspi1.Init.FirstBit = SPI_FIRSTBIT_MSB;
  hspi1.Init.TIMode = SPI_TIMODE_DISABLE;
  hspi1.Init.CRCCalculation = SPI_CRCCALCULATION_DISABLE;
  hspi1.Init.CRCPolynomial = 10;
  if (HAL_SPI_Init(&hspi1) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN SPI1_Init 2 */

  /* USER CODE END SPI1_Init 2 */

}

/**
  * @brief USART1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_USART1_UART_Init(void)
{

  /* USER CODE BEGIN USART1_Init 0 */

  /* USER CODE END USART1_Init 0 */

  /* USER CODE BEGIN USART1_Init 1 */

  /* USER CODE END USART1_Init 1 */
  huart1.Instance = USART1;
  huart1.Init.BaudRate = 115200;
  huart1.Init.WordLength = UART_WORDLENGTH_8B;
  huart1.Init.StopBits = UART_STOPBITS_1;
  huart1.Init.Parity = UART_PARITY_NONE;
  huart1.Init.Mode = UART_MODE_TX_RX;
  huart1.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  huart1.Init.OverSampling = UART_OVERSAMPLING_16;
  if (HAL_UART_Init(&huart1) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN USART1_Init 2 */

  /* USER CODE END USART1_Init 2 */

}

/**
  * @brief USART2 Initialization Function
  * @param None
  * @retval None
  */
static void MX_USART2_UART_Init(void)
{

  /* USER CODE BEGIN USART2_Init 0 */

  /* USER CODE END USART2_Init 0 */

  /* USER CODE BEGIN USART2_Init 1 */

  /* USER CODE END USART2_Init 1 */
  huart2.Instance = USART2;
  huart2.Init.BaudRate = 115200;
  huart2.Init.WordLength = UART_WORDLENGTH_8B;
  huart2.Init.StopBits = UART_STOPBITS_1;
  huart2.Init.Parity = UART_PARITY_NONE;
  huart2.Init.Mode = UART_MODE_TX_RX;
  huart2.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  huart2.Init.OverSampling = UART_OVERSAMPLING_16;
  if (HAL_UART_Init(&huart2) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN USART2_Init 2 */

  /* USER CODE END USART2_Init 2 */

}

/**
  * Enable DMA controller clock
  */
static void MX_DMA_Init(void)
{

  /* DMA controller clock enable */
  __HAL_RCC_DMA2_CLK_ENABLE();

  /* DMA interrupt init */
  /* DMA2_Stream2_IRQn interrupt configuration */
  HAL_NVIC_SetPriority(DMA2_Stream2_IRQn, 5, 0);
  HAL_NVIC_EnableIRQ(DMA2_Stream2_IRQn);

}

/**
  * @brief GPIO Initialization Function
  * @param None
  * @retval None
  */
static void MX_GPIO_Init(void)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};
  /* USER CODE BEGIN MX_GPIO_Init_1 */

  /* USER CODE END MX_GPIO_Init_1 */

  /* GPIO Ports Clock Enable */
  __HAL_RCC_GPIOC_CLK_ENABLE();
  __HAL_RCC_GPIOH_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOD_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(GPIOC, GPIO_PIN_13, GPIO_PIN_RESET);

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(W25Q_CS_GPIO_Port, W25Q_CS_Pin, GPIO_PIN_SET);

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(DHT11_DATA_GPIO_Port, DHT11_DATA_Pin, GPIO_PIN_RESET);

  /*Configure GPIO pin : PC13 */
  GPIO_InitStruct.Pin = GPIO_PIN_13;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOC, &GPIO_InitStruct);

  /*Configure GPIO pin : PA0 */
  GPIO_InitStruct.Pin = GPIO_PIN_0;
  GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
  GPIO_InitStruct.Pull = GPIO_PULLUP;
  HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

  /*Configure GPIO pin : W25Q_CS_Pin */
  GPIO_InitStruct.Pin = W25Q_CS_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(W25Q_CS_GPIO_Port, &GPIO_InitStruct);

  /*Configure GPIO pin : DHT11_DATA_Pin */
  GPIO_InitStruct.Pin = DHT11_DATA_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(DHT11_DATA_GPIO_Port, &GPIO_InitStruct);

  /* USER CODE BEGIN MX_GPIO_Init_2 */
  /* ---- 执行器引脚初始化: PE0~PE3, 4 路推挽输出 ----
   * 放在 USER CODE 区里, CubeMX 重新生成代码时不会被覆盖 */
  __HAL_RCC_GPIOE_CLK_ENABLE();   /* 关键: 必须使能 GPIOE 端口时钟, 否则下面的配置全部无效 */

  /* 先把输出寄存器写成"关"电平, 再切成输出模式, 避免上电瞬间抖动误动作 */
  HAL_GPIO_WritePin(GPIOE,
                    FAN_RELAY_Pin | PUMP_RELAY_Pin | LIGHT_RELAY_Pin | BUZZER_Pin,
                    ACTUATOR_OFF_LEVEL);

  GPIO_InitStruct.Pin   = FAN_RELAY_Pin | PUMP_RELAY_Pin | LIGHT_RELAY_Pin | BUZZER_Pin;
  GPIO_InitStruct.Mode  = GPIO_MODE_OUTPUT_PP;   /* 推挽输出 */
  GPIO_InitStruct.Pull  = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;   /* 只是开关继电器, 低速足够, 还能减小干扰 */
  HAL_GPIO_Init(GPIOE, &GPIO_InitStruct);

  /* USER CODE END MX_GPIO_Init_2 */
}

/* USER CODE BEGIN 4 */

/* USER CODE END 4 */

/* USER CODE BEGIN Header_StartStatusTask */
/**
  * @brief  Function implementing the StatusTask thread.
  * @param  argument: Not used
  * @retval None
  */
/* USER CODE END Header_StartStatusTask */
void StartStatusTask(void *argument)
{
  /* USER CODE BEGIN 5 */
  uint32_t log_cnt = 0;
  DebugPrint("\r\n===== System Start =====\r\n");
  DebugPrint("FreeRTOS running, CPU: 168MHz\r\n");
  for(;;)
  {
    /* PC13 板载 LED 心跳闪烁, 指示系统正常运行 (500ms 翻转一次) */
    HAL_GPIO_TogglePin(LED_GPIO_Port, LED_Pin);

    /* 喂独立看门狗, 超时约 4 秒 (128 分频 x 1000 重装) */
    HAL_IWDG_Refresh(&hiwdg);

    /* 每 20 次循环(约 10 秒)打印一次存活日志 */
    log_cnt++;
    if (log_cnt >= 20)
    {
      log_cnt = 0;
      DebugPrint("[StatusTask] alive, tick=%lu\r\n", (unsigned long)HAL_GetTick());
    }

    osDelay(500);
  }
  /* USER CODE END 5 */
}

/* USER CODE BEGIN Header_StartSensorTask */
/**
* @brief Function implementing the SensorTask thread.
* @param argument: Not used
* @retval None
*/
/* USER CODE END Header_StartSensorTask */
__weak void StartSensorTask(void *argument)
{
  /* USER CODE BEGIN StartSensorTask */
  DHT11_Data_t dht = {0};
  SensorData_t sd;
  uint16_t lux;
  uint8_t dht_ok;
  uint8_t hist_div = 0;   /* 采样次数计数器: 攒够 HIST_INTERVAL_MS 才存一条历史记录 */

  DebugPrint("[SensorTask] sensor mode\r\n");

  /* 初始化 DWT 微秒级延时 (DHT11 时序需要) */
  DHT11_Init();

  /* 初始化 OLED */
  OLED_Init();
  OLED_Clear();

  /* 初始化 BH1750 (I2C 访问需加总线互斥锁) */
  osMutexAcquire(I2CMutex, osWaitForever);
  BH1750_Init();
  osMutexRelease(I2CMutex);

  for(;;)
  {
    /* 1. 读取 DHT11 温湿度 (含起止信号时序, 采样间隔 >= 1s) */
    dht_ok = DHT11_Read(&dht);
    if (!dht_ok)
    {
      DebugPrint("[Sensor] DHT11 read FAIL (err=%u: 1=no resp, 2=bit timeout, 3=checksum)\r\n",
                 (unsigned)g_dht11_err);
    }

    /* 2. 读取 BH1750 光照 (I2C 访问需加总线互斥锁) */
    osMutexAcquire(I2CMutex, osWaitForever);
    lux = BH1750_ReadLux();
    osMutexRelease(I2CMutex);

    /* 3. 刷新 OLED 显示 (I2C 访问需加总线互斥锁)
     *    大字宽 12px / 高 16px: 第 2 个参数是行号 0~3, x 按 12 的倍数排布 */
    osMutexAcquire(I2CMutex, osWaitForever);
    OLED_ShowBigString(0, 0, "Temp:");
    OLED_ShowBigNum(60, 0, dht.temp_int, 2);
    OLED_ShowBigChar(84, 0, 'C');

    OLED_ShowBigString(0, 1, "Humi:");
    OLED_ShowBigNum(60, 1, dht.humi_int, 2);
    OLED_ShowBigChar(84, 1, '%');

    OLED_ShowBigString(0, 2, "Lux:");
    OLED_ShowBigNum(36, 2, lux, 5);
    OLED_ShowBigString(96, 2, "lx");

    OLED_ShowBigString(0, 3, "Net:");
    switch (g_net_state)
    {
      /* 状态串固定 6 个字符 (=72px, 48+72=120 < 128), 末尾补空格可覆盖上次的残留 */
      case NET_IDLE:            OLED_ShowBigString(48, 3, "IDLE  "); break;
      case NET_WIFI_CONNECTING: OLED_ShowBigString(48, 3, "WiFi.."); break;
      case NET_WIFI_OK:         OLED_ShowBigString(48, 3, "WiFiOK"); break;
      case NET_MQTT_CONNECTING: OLED_ShowBigString(48, 3, "MQTT.."); break;
      case NET_MQTT_OK:         OLED_ShowBigString(48, 3, "MQTTOK"); break;
      case NET_FAIL:            OLED_ShowBigString(48, 3, "FAIL  "); break;
      default:                  OLED_ShowBigString(48, 3, "??????"); break;
    }
    osMutexRelease(I2CMutex);

    /* 4. 本地闭环控制: 拿刚采到的数据比对阈值, 超限立刻驱动执行器(不等云端)
     *    注意只在 DHT11 读取成功时评估: 读取失败时 dht 里是上一次的旧值,
     *    用陈旧的温湿度去判断会产生误报警/误动作 */
    if (dht_ok)
    {
      float temp_f = (float)dht.temp_int + (float)dht.temp_dec * 0.1f;
      float humi_f = (float)dht.humi_int + (float)dht.humi_dec * 0.1f;
      Control_Evaluate(temp_f, humi_f, (float)lux);
    }

    /* 5. 传感器数据入队发送给 WiFiTask (非阻塞) */
    sd.temp_int = dht.temp_int;
    sd.humi_int = dht.humi_int;
    sd.lux = lux;
    osMessageQueuePut(SensorQueue, &sd, 0, 0);

    /* 6. 每 HIST_INTERVAL_MS 往 W25Q64 存一条历史记录
     *    采样周期 2 秒, 所以攒够 5 次才存一条 = 10 秒一条 (整圈能存约 56 天)。
     *    这条记录同时也充当"断网暂存": MQTT 没连上时 WiFiTask 不会去确认它,
     *    数据就一直留在 FLASH 里, 等联网后再补传出去。
     *    DHT11 读失败时 dht 里是上一次的旧值, 这里照旧存下去 (跟 OLED 显示保持一致),
     *    避免因为一次读失败就在历史曲线里留个空洞 */
    if (g_w25q_ok)
    {
      hist_div++;
      if (hist_div >= (HIST_INTERVAL_MS / SENSOR_PERIOD_MS))
      {
        hist_div = 0;
        osMutexAcquire(StorageMutex, osWaitForever);
        History_Append((int16_t)(((int16_t)dht.temp_int * 10) + (int16_t)dht.temp_dec),
                       (uint16_t)(((uint16_t)dht.humi_int * 10) + dht.humi_dec),
                       lux);
        osMutexRelease(StorageMutex);
      }
    }

    /* 7. 打印调试日志 */
    if (dht_ok)
    {
      DebugPrint("[Sensor] Temp=%u.%uC Humi=%u.%u%% Lux=%u lx\r\n",
                 dht.temp_int, dht.temp_dec, dht.humi_int, dht.humi_dec, lux);
    }
    else
    {
      DebugPrint("[Sensor] Temp=-- Humi=-- Lux=%u lx\r\n", lux);
    }

    /* DHT11 采样间隔不能小于 1 秒 */
    osDelay(SENSOR_PERIOD_MS);
  }
  /* USER CODE END StartSensorTask */
}

/* USER CODE BEGIN Header_StartWiFiTask */
/**
* @brief Function implementing the WiFiTask thread.
* @param argument: Not used
* @retval None
*/
/* USER CODE END Header_StartWiFiTask */
void StartWiFiTask(void *argument)
{
  /* USER CODE BEGIN StartWiFiTask */
  SensorData_t sd;
  HistRecord_t rec;
  char payload[192];
  uint8_t sent;       /* 本轮已成功补传的条数 */
  uint8_t peek;       /* History_Peek 的返回值 */
  int16_t temp_x10;
  char tsign;
  uint32_t pending;
  uint16_t fw_ver;    /* 上报报文里的固件版本 (从模拟 EEPROM 里读) */

  DebugPrint("[WiFiTask] start\r\n");

  /* 等待 ESP8266 上电就绪 (NodeMCU USB 串口枚举约需 1~2 秒) */
  osDelay(3000);

  /* 初始化 USART1 与 ESP8266 的通信 */
  ESP_Init();

  /* 首次连接: 失败也不退出, 交给下面的主循环一直重试 */
  WiFi_Connect();

  /* 上报报文里的固件版本: 从模拟 EEPROM 读, 读不到就用编译时的默认版本 */
  fw_ver = (g_w25q_ok != 0U) ? Param_Get()->fw_version : FW_VERSION_CURRENT;

  for(;;)
  {
    /* 1. 把队列取空 (超时 1 秒, 兼作本轮的节拍)。
     *    注意数据不靠这个队列传递了: SensorTask 已经把记录写进 W25Q64,
     *    这里取队列只是别让 SensorTask 的非阻塞入队一直堆积 */
    osMessageQueueGet(SensorQueue, &sd, NULL, 1000);

    /* 2. MQTT 没连上就重连 (开机首次失败、运行中掉线都会走到这里)。
     *    连上之前不发数据, 数据留在 W25Q64 里等联网后补传 */
    if (g_net_state != NET_MQTT_OK)
    {
      if (WiFi_Connect() == 0U)
      {
        DebugPrint("[NET] offline, retry in %u s\r\n",
                   (unsigned)(NET_RETRY_PERIOD_MS / 1000U));
        osDelay(NET_RETRY_PERIOD_MS);
      }
      continue;
    }

    /* 3. 存储不可用: 什么都不发 */
    if (!g_w25q_ok)
    {
      continue;
    }

    /* 4. 从最老的待补传记录开始, 逐条发出去 (在线时这就是常规上报) */
    sent = 0;
    while (sent < HIST_RESEND_BATCH)
    {
      osMutexAcquire(StorageMutex, osWaitForever);
      peek = History_Peek(0, &rec);
      osMutexRelease(StorageMutex);

      if (peek == HIST_EMPTY)
      {
        break;   /* 没有待传数据了 */
      }
      if (peek == HIST_BAD)
      {
        /* 这条记录坏了(写入时掉电)或已被环形区覆盖: 直接跳过,
         * 否则补传会永远卡在这一条上, 后面的数据也发不出去 */
        DebugPrint("[MQTT] skip bad record\r\n");
        osMutexAcquire(StorageMutex, osWaitForever);
        History_Ack();
        osMutexRelease(StorageMutex);
        continue;
      }

      /* 组装 JSON: 温湿度带一位小数, 并带上序号和时间戳方便云端去重 */
      temp_x10 = rec.temp_x10;
      tsign = ' ';
      if (temp_x10 < 0)
      {
        tsign = '-';
        temp_x10 = (int16_t)(-temp_x10);
      }
      snprintf(payload, sizeof(payload),
               "{\"id\":\"%s\",\"fw\":\"%u.%u\",\"seq\":%lu,\"ts\":%lu,"
               "\"temp\":%c%lu.%lu,\"humi\":%lu.%lu,\"lux\":%u}",
               MQTT_CLIENTID,
               (unsigned)(fw_ver >> 8), (unsigned)(fw_ver & 0xFF),
               (unsigned long)rec.seq, (unsigned long)rec.ts,
               tsign,
               (unsigned long)(temp_x10 / 10), (unsigned long)(temp_x10 % 10),
               (unsigned long)(rec.humi_x10 / 10), (unsigned long)(rec.humi_x10 % 10),
               (unsigned)rec.lux);

      if (ESP_MQTTPublish(MQTT_TOPIC, payload) != ESP_OK)
      {
        /* 发送失败: 不确认, 这条记录原样留在 FLASH 里等下一轮重发 */
        DebugPrint("[MQTT] publish FAIL, data kept in W25Q64\r\n");
        g_net_state = NET_FAIL;
        break;
      }

      DebugPrint("[MQTT] PUBLISH OK: %s\r\n", payload);

      /* 上传成功才确认, 让"待补传队列"的队头往后走一条 */
      osMutexAcquire(StorageMutex, osWaitForever);
      History_Ack();
      osMutexRelease(StorageMutex);
      sent++;
    }

    /* 5. 报一下还剩多少条积压 (只在刚清空或一条都发不出去时打印, 避免刷屏) */
    osMutexAcquire(StorageMutex, osWaitForever);
    pending = History_PendingCount();
    osMutexRelease(StorageMutex);

    if (pending == 0U)
    {
      if (sent > 0U)
      {
        DebugPrint("[MQTT] backlog cleared\r\n");
      }
    }
    else if (sent == 0U)
    {
      DebugPrint("[MQTT] pending=%lu, wait for resend\r\n", (unsigned long)pending);
    }

    /* 发送间隔 500ms */
    osDelay(500);
  }
  /* USER CODE END StartWiFiTask */
}

/**
  * @brief  Period elapsed callback in non blocking mode
  * @note   This function is called  when TIM6 interrupt took place, inside
  * HAL_TIM_IRQHandler(). It makes a direct call to HAL_IncTick() to increment
  * a global variable "uwTick" used as application time base.
  * @param  htim : TIM handle
  * @retval None
  */
void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
  /* USER CODE BEGIN Callback 0 */

  /* USER CODE END Callback 0 */
  if (htim->Instance == TIM6)
  {
    HAL_IncTick();
  }
  /* USER CODE BEGIN Callback 1 */

  /* USER CODE END Callback 1 */
}

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
