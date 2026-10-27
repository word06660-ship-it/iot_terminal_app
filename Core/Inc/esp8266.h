/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    esp8266.h
  * @brief   ESP8266 WiFi 模块 AT 指令封装 (USART1)
  *
  * 固件要求: AT 固件 v1.6.0 及以上 (支持 MQTT AT 指令)
  * 检查命令: AT+GMR
  ******************************************************************************
  */
/* USER CODE END Header */

#ifndef __ESP8266_H
#define __ESP8266_H

#ifdef __cplusplus
extern "C" {
#endif

#include "main.h"

/* ================= 用户配置区: 修改这里 ================= */
#define WIFI_SSID       "WIFI"      /* 你的 WiFi 名 */
#define WIFI_PASSWORD   "2282403765"      /* 你的 WiFi 密码 */
#define MQTT_BROKER     "broker-cn.emqx.io"      /* MQTT 服务器 (EMQX 国内节点) */
#define MQTT_PORT       1883
#define MQTT_CLIENTID   "iot_terminal_01"     /* 设备ID */
#define MQTT_TOPIC      "iot_terminal/data"   /* 上报主题 */
/* ======================================================= */

typedef enum
{
  ESP_OK = 0,
  ESP_FAIL,
  ESP_TIMEOUT,
} ESP_Status_t;

void          ESP_Init(void);
ESP_Status_t  ESP_SendCmd(const char *cmd, const char *expect, uint32_t timeout_ms);
ESP_Status_t  ESP_ConnectWiFi(const char *ssid, const char *password);
ESP_Status_t  ESP_MQTTConnect(const char *broker, uint16_t port,
                              const char *clientid, const char *user, const char *pass);
ESP_Status_t  ESP_MQTTPublish(const char *topic, const char *payload);

#ifdef __cplusplus
}
#endif

#endif /* __ESP8266_H */
