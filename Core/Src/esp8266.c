/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    esp8266.c
  * @brief   ESP8266 WiFi 模块 AT 指令封装 (USART1)
  *
  * 接收方式: USART1 中断接收, 存入线性缓冲区, 发送指令后轮询查找期望响应
  * 注意:
  *   - AT 指令必须以 "\r\n" 结尾
  *   - MQTT AT 指令需要 AT 固件 v1.6.0+
  *   - AT 指令参数用双引号包裹, 参数内部的 " 和 \ 必须转义成 \" 和 \\
  ******************************************************************************
  */
/* USER CODE END Header */

#include "esp8266.h"
#include "cmsis_os.h"
#include <string.h>
#include <stdio.h>

extern UART_HandleTypeDef huart1;
extern void DebugPrint(const char *fmt, ...);  /* main.c 中的调试打印 */

/* 接收缓冲区 */
#define AT_RX_BUF_SIZE   512
static uint8_t  at_rx_buf[AT_RX_BUF_SIZE];
static volatile uint16_t at_rx_len = 0;
static uint8_t  at_rx_byte;    /* USART1 单字节接收缓存 */

/* 期望响应的最大等待时间 */
#define AT_CMD_TIMEOUT   2000

/* 清空接收缓冲并恢复中断接收
   注意: 若上次缓冲满导致 RXNE 被禁用, 这里必须重新使能, 否则永远收不到响应 */
static void ESP_ClearRx(void)
{
  __disable_irq();
  at_rx_len = 0;
  /* 强制重新使能 RXNE 中断 (清除缓冲满后被禁用的残留状态) */
  __HAL_UART_ENABLE_IT(&huart1, UART_IT_RXNE);
  __enable_irq();
  /* 若接收状态已不是 BUSY_RX (例如 ORE 错误导致接收停止), 重新启动中断接收 */
  if (huart1.RxState != HAL_UART_STATE_BUSY_RX)
  {
    HAL_UART_Receive_IT(&huart1, &at_rx_byte, 1);
  }
}

/* 在接收缓冲中查找目标字符串 (子串匹配) */
static uint8_t ESP_FindStr(const char *expect)
{
  uint8_t found = 0;
  __disable_irq();
  if (at_rx_len > 0)
  {
    /* 缓冲内容不是 C 字符串, 手动逐位置匹配子串 */
    uint16_t i, j;
    uint16_t elen = (uint16_t)strlen(expect);
    for (i = 0; i + elen <= at_rx_len; i++)
    {
      for (j = 0; j < elen; j++)
      {
        if (at_rx_buf[i + j] != (uint8_t)expect[j])
        {
          break;
        }
      }
      if (j == elen)
      {
        found = 1;
        break;
      }
    }
  }
  __enable_irq();
  return found;
}

/**
  * @brief USART1 接收完成回调 (HAL 库中断调用, 覆盖 weak 版本)
  *        逐字节接收 AT 响应
  */
void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
  if (huart->Instance == USART1)
  {
    if (at_rx_len < AT_RX_BUF_SIZE)
    {
      at_rx_buf[at_rx_len++] = at_rx_byte;
      /* 缓冲未满才继续接收, 防止悬空引脚噪声导致中断风暴 */
      HAL_UART_Receive_IT(&huart1, &at_rx_byte, 1);
    }
    else
    {
      /* 缓冲已满, 暂停接收直到下次发送指令前清空 */
      __HAL_UART_DISABLE_IT(&huart1, UART_IT_RXNE);
    }
  }
}

/**
  * @brief ESP8266 初始化: 启动中断接收
  * @note  上电后 ESP8266 启动需要约 1~2 秒, 由调用方等待
  */
void ESP_Init(void)
{
  at_rx_len = 0;
  /* 启动单字节中断接收 */
  HAL_UART_Receive_IT(&huart1, &at_rx_byte, 1);
}

/**
  * @brief 发送 AT 指令并等待期望响应
  * @param cmd: 完整 AT 指令 (含 \r\n)
  * @param expect: 期望出现的响应子串, 例如 "OK"
  * @param timeout_ms: 超时时间
  */
ESP_Status_t ESP_SendCmd(const char *cmd, const char *expect, uint32_t timeout_ms)
{
  uint32_t start;

  ESP_ClearRx();
  /* 发送指令 */
  if (HAL_UART_Transmit(&huart1, (uint8_t *)cmd, (uint16_t)strlen(cmd),
                        AT_CMD_TIMEOUT) != HAL_OK)
  {
    return ESP_FAIL;
  }

  /* 轮询等待期望响应 */
  start = HAL_GetTick();
  while ((HAL_GetTick() - start) < timeout_ms)
  {
    if (ESP_FindStr(expect))
    {
      return ESP_OK;
    }
    osDelay(10);
  }
  return ESP_TIMEOUT;
}

/**
  * @brief 连接 WiFi (Station 模式)
  */
ESP_Status_t ESP_ConnectWiFi(const char *ssid, const char *password)
{
  char cmd[160];

  /* 设置为 Station 模式 */
  if (ESP_SendCmd("AT+CWMODE=1\r\n", "OK", 3000) != ESP_OK)
  {
    return ESP_FAIL;
  }
  /* 连接 WiFi, 等待获取 IP (可能需要 5~15 秒) */
  snprintf(cmd, sizeof(cmd), "AT+CWJAP=\"%s\",\"%s\"\r\n", ssid, password);
  if (ESP_SendCmd(cmd, "WIFI GOT IP", 20000) != ESP_OK)
  {
    return ESP_FAIL;
  }
  return ESP_OK;
}

/* 调试用: 打印 AT 响应缓冲区内容 (截断到 140 字符, 防止 DebugPrint 溢出) */
static void ESP_DumpRx(const char *tag)
{
  char tmp[160];
  uint16_t len;

  __disable_irq();
  len = at_rx_len;
  if (len > 140) len = 140;
  if (len > 0)
  {
    memcpy(tmp, (const char *)at_rx_buf, len);
  }
  tmp[len] = '\0';
  __enable_irq();

  DebugPrint("[ESP] %s resp: %s\r\n", tag, tmp);
}

/**
  * @brief 配置并连接 MQTT
  * @note  需要 AT 固件 v1.6.0+ 支持 MQTT 指令
  */
ESP_Status_t ESP_MQTTConnect(const char *broker, uint16_t port,
                             const char *clientid, const char *user, const char *pass)
{
  char cmd[192];

  /* 配置 MQTT 用户参数: AT+MQTTUSERCFG=<LinkID>,<scheme>,<client_id>,<user>,<pass>,<cert>,<ca>,<path> */
  snprintf(cmd, sizeof(cmd),
           "AT+MQTTUSERCFG=0,1,\"%s\",\"%s\",\"%s\",0,0,\"\"\r\n",
           clientid, user, pass);
  if (ESP_SendCmd(cmd, "OK", 3000) != ESP_OK)
  {
    ESP_DumpRx("MQTTUSERCFG");
    return ESP_FAIL;
  }

  /* 连接 MQTT 服务器 */
  snprintf(cmd, sizeof(cmd), "AT+MQTTCONN=0,\"%s\",%u,1\r\n", broker, port);
  if (ESP_SendCmd(cmd, "OK", 8000) != ESP_OK)
  {
    ESP_DumpRx("MQTTCONN");
    return ESP_FAIL;
  }
  return ESP_OK;
}

/**
  * @brief  把一个字符串转义成可以放进 AT 指令双引号参数里的形式
  * @note   ESP-AT 用双引号包裹参数, 参数内部的 " 和 \ 必须写成 \" 和 \\
  *         例: {"temp":25}  ->  {\"temp\":25}
  * @retval 0=成功, -1=目标缓冲区不够
  */
static int ESP_EscapeATParam(const char *src, char *dst, size_t dst_size)
{
  size_t out = 0;

  while (*src != '\0')
  {
    char c = *src++;

    if ((c == '"') || (c == '\\'))
    {
      /* 一个原字符要占两个位置(反斜杠 + 原字符), 还要给结尾 '\0' 留位置 */
      if ((out + 2U) >= dst_size)
      {
        return -1;
      }
      dst[out++] = '\\';
      dst[out++] = c;
    }
    else
    {
      if ((out + 1U) >= dst_size)
      {
        return -1;
      }
      dst[out++] = c;
    }
  }

  dst[out] = '\0';
  return 0;
}

/**
  * @brief  MQTT 发布消息
  * @param  topic: 主题
  * @param  payload: 消息内容 (JSON)
  * @note   JSON 里的 " 必须转义, 否则模块在第一个引号处就把参数截断了。
  *         三个缓冲区用 static: WiFiTask 栈只有 2KB, 放栈上会溢出;
  *         该函数只由 WiFiTask 调用, 不存在重入问题。
  */
ESP_Status_t ESP_MQTTPublish(const char *topic, const char *payload)
{
  static char topic_esc[64];
  static char payload_esc[320];
  static char cmd[384];
  int n;

  if (ESP_EscapeATParam(topic, topic_esc, sizeof(topic_esc)) != 0)
  {
    return ESP_FAIL;
  }
  if (ESP_EscapeATParam(payload, payload_esc, sizeof(payload_esc)) != 0)
  {
    return ESP_FAIL;
  }

  /* AT+MQTTPUB=<LinkID>,<topic>,<data>,<qos>,<retain> */
  n = snprintf(cmd, sizeof(cmd), "AT+MQTTPUB=0,\"%s\",\"%s\",0,0\r\n",
               topic_esc, payload_esc);
  if ((n < 0) || ((size_t)n >= sizeof(cmd)))
  {
    return ESP_FAIL;   /* 被截断: 宁可失败也不发一条残缺指令 */
  }

  return ESP_SendCmd(cmd, "OK", 5000);
}
