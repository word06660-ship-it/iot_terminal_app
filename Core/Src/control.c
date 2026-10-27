/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    control.c
  * @brief   执行器驱动 + 本地控制闭环 (不依赖云端)
  *
  * 控制策略 (谁管谁):
  *   温度 > temp_max              -> 开风扇 (降不到 temp_max-HYST 以下不关)
  *   湿度 < humi_min              -> 开水泵 (升不到 humi_min+HYST 以上不关)
  *   光照 < lux_min               -> 开补光灯 (升不到 lux_min+HYST 以上不关)
  *   任一参数越限 (含上限/下限)   -> 蜂鸣器报警
  *   温度变化过快 (升温/降温都算) -> 蜂鸣器报警
  ******************************************************************************
  */
/* USER CODE END Header */

#include "control.h"

/* 全局阈值配置: 变量本体在这里定义 (main.h 里只有 extern 声明)
 * 上电先用这套本地默认值, 以后云端下发配置时直接改这个变量的成员即可 */
volatile ThresholdConfig_t g_threshold =
{
  .temp_max      = 30.0f,  /* 超过 30C 开风扇 */
  .temp_min      = 10.0f,  /* 低于 10C 报警 */
  .humi_max      = 80.0f,  /* 高于 80% 报警 */
  .humi_min      = 30.0f,  /* 低于 30% 开水泵 */
  .lux_min       = 300.0f, /* 低于 300lx 开补光灯 */
  .lux_max       = 3000.0f,/* 高于 3000lx 报警 */
  .temp_rate_max = 5.0f,   /* 温度变化率超过 5C/min 报警 (约等于 10 秒变化 1C) */
};

/* 各执行器"上一次"的开关状态, 用于滞回判断: 落在死区里就保持原状态 */
static uint8_t s_fan_on      = 0;
static uint8_t s_pump_on     = 0;
static uint8_t s_light_on    = 0;
static uint8_t s_limit_alarm = 0;   /* 越限报警 (温度/湿度/光照超上下限) */
static uint8_t s_rate_alarm  = 0;   /* 温度变化过快报警 */
static uint8_t s_buzzer_on   = 0;   /* 蜂鸣器上一次的实际状态 */

/* 变化率检测用: 记住"窗口起点"那一刻的温度和时间 */
static uint8_t  s_rate_valid    = 0;      /* 是否已经记录过窗口起点 */
static float    s_rate_ref_temp = 0.0f;   /* 窗口起点温度 */
static uint32_t s_rate_ref_tick = 0;      /* 窗口起点时刻 (ms) */

/* 下面 4 个是执行器封装: 调用者只关心"开还是关",
 * 由 ACTUATOR_ON_LEVEL/OFF_LEVEL 负责翻译成实际的高低电平 */
void Relay_Fan_Set(uint8_t on)
{
  HAL_GPIO_WritePin(FAN_RELAY_GPIO_Port, FAN_RELAY_Pin,
                    on ? ACTUATOR_ON_LEVEL : ACTUATOR_OFF_LEVEL);
}

void Relay_Pump_Set(uint8_t on)
{
  HAL_GPIO_WritePin(PUMP_RELAY_GPIO_Port, PUMP_RELAY_Pin,
                    on ? ACTUATOR_ON_LEVEL : ACTUATOR_OFF_LEVEL);
}

void Relay_Light_Set(uint8_t on)
{
  HAL_GPIO_WritePin(LIGHT_RELAY_GPIO_Port, LIGHT_RELAY_Pin,
                    on ? ACTUATOR_ON_LEVEL : ACTUATOR_OFF_LEVEL);
}

void Buzzer_Set(uint8_t on)
{
  HAL_GPIO_WritePin(BUZZER_GPIO_Port, BUZZER_Pin,
                    on ? ACTUATOR_ON_LEVEL : ACTUATOR_OFF_LEVEL);
}

/**
  * @brief  执行器初始化: 上电先把 4 路全部置为"关", 并清零内部状态
  * @note   引脚的模式配置在 main.c 的 MX_GPIO_Init() 里完成, 这里只负责"安全默认状态"
  */
void Control_Init(void)
{
  s_fan_on      = 0;
  s_pump_on     = 0;
  s_light_on    = 0;
  s_limit_alarm = 0;
  s_rate_alarm  = 0;
  s_buzzer_on   = 0;

  /* 变化率检测状态也要清零: 否则重启后可能拿"重启前"的温度当窗口起点 */
  s_rate_valid    = 0;
  s_rate_ref_temp = 0.0f;
  s_rate_ref_tick = 0;

  Relay_Fan_Set(0);
  Relay_Pump_Set(0);
  Relay_Light_Set(0);
  Buzzer_Set(0);
}

/**
  * @brief  本地闭环核心: 用一组实测值比对阈值, 超限立即驱动执行器
  * @param  t: 温度 (C)    h: 湿度 (%)    l: 光照 (lx)
  * @note   带滞回, 避免数值在阈值附近抖动导致执行器反复开关
  */
void Control_Evaluate(float t, float h, float l)
{
  uint32_t now = HAL_GetTick();     /* 当前系统毫秒数, 用来算"过了多久" */
  uint8_t fan, pump, light, limit_alarm, buzzer;

  /* ---------- 1. 温度过高 -> 开风扇 ---------- */
  /* 先取上一次的状态; 只有明确"越上限"才开, 明确"退到上限-滞回以下"才关,
   * 两者都不满足(落在死区)就维持原状态, 这就是滞回 */
  fan = s_fan_on;
  if (t > g_threshold.temp_max)
  {
    fan = 1;
  }
  else if (t < (g_threshold.temp_max - HYST_TEMP))
  {
    fan = 0;
  }

  /* ---------- 2. 湿度过低 -> 开水泵 ---------- */
  pump = s_pump_on;
  if (h < g_threshold.humi_min)
  {
    pump = 1;
  }
  else if (h > (g_threshold.humi_min + HYST_HUMI))
  {
    pump = 0;
  }

  /* ---------- 3. 光照过低 -> 开补光灯 ---------- */
  light = s_light_on;
  if (l < g_threshold.lux_min)
  {
    light = 1;
  }
  else if (l > (g_threshold.lux_min + HYST_LUX))
  {
    light = 0;
  }

  /* ---------- 4. 越限报警: 任一参数超出上下限 ---------- */
  limit_alarm = s_limit_alarm;
  if ((t > g_threshold.temp_max) || (t < g_threshold.temp_min) ||
      (h > g_threshold.humi_max) || (h < g_threshold.humi_min) ||
      (l > g_threshold.lux_max)  || (l < g_threshold.lux_min))
  {
    limit_alarm = 1;   /* 只要有一条越限, 立刻报警 */
  }
  else if ((t <= (g_threshold.temp_max - HYST_TEMP)) && (t >= (g_threshold.temp_min + HYST_TEMP)) &&
           (h <= (g_threshold.humi_max - HYST_HUMI)) && (h >= (g_threshold.humi_min + HYST_HUMI)) &&
           (l <= (g_threshold.lux_max  - HYST_LUX))  && (l >= (g_threshold.lux_min  + HYST_LUX)))
  {
    limit_alarm = 0;   /* 所有参数都退回安全区才解除报警 */
  }

  /* ---------- 5. 温度变化率报警: 变化太快说明环境突变(冷风/暴晒)或传感器异常 ----------
   * 做法: 每过 RATE_WINDOW_MS 算一次,
   *       速率 = |当前温度 - 窗口起点温度| / 经过的分钟数     (单位 C/min) */
  if (s_rate_valid == 0)
  {
    /* 第一个样本只当"窗口起点", 数据太少没法算变化率 */
    s_rate_valid    = 1;
    s_rate_ref_temp = t;
    s_rate_ref_tick = now;
  }
  else if ((now - s_rate_ref_tick) >= RATE_WINDOW_MS)
  {
    /* 注意: now 和 tick 都是无符号数, 直接相减在"计数器回绕"时也是对的 */
    float elapsed_min = (float)(now - s_rate_ref_tick) / 60000.0f;  /* ms -> 分钟 */
    float delta       = t - s_rate_ref_temp;                       /* 正=升温, 负=降温 */
    float mag         = (delta < 0.0f) ? -delta : delta;           /* 取绝对值, 升降温都算 */
    float rate        = mag / elapsed_min;                         /* C/min */

    if (rate >= g_threshold.temp_rate_max)
    {
      if (s_rate_alarm == 0)
      {
        /* 学个坑: 工程用 --specs=nano.specs (newlib-nano), printf 默认不支持 %f,
         * 直接打印浮点会是空的。这里把数值放大 10 倍转成整数, 自己拼出小数 */
        int x10 = (int)(rate * 10.0f);
        DebugPrint("[Ctrl] Temp rate %s %d.%d C/min (limit %d C/min)\r\n",
                   (delta < 0.0f) ? "down" : "up",
                   x10 / 10, x10 % 10,
                   (int)g_threshold.temp_rate_max);
      }
      s_rate_alarm = 1;
    }
    else
    {
      s_rate_alarm = 0;
    }

    /* 窗口结束: 把起点推进到当前样本, 开始算下一个窗口 */
    s_rate_ref_temp = t;
    s_rate_ref_tick = now;
  }

  /* ---------- 6. 蜂鸣器: 越限 或 温变过快, 有任意一条就响 ---------- */
  buzzer = (limit_alarm || s_rate_alarm) ? 1 : 0;

  /* ---------- 7. 只在状态发生变化时才操作引脚 + 打印日志 ----------
   * 每 2 秒无脑写一次引脚也行, 但"变化才动作"能减少无谓的总线操作,
   * 打印也只打变化, 串口日志才看得清 */
  if (fan != s_fan_on)
  {
    s_fan_on = fan;
    Relay_Fan_Set(fan);
    DebugPrint("[Ctrl] Fan   -> %s\r\n", fan ? "ON" : "OFF");
  }
  if (pump != s_pump_on)
  {
    s_pump_on = pump;
    Relay_Pump_Set(pump);
    DebugPrint("[Ctrl] Pump  -> %s\r\n", pump ? "ON" : "OFF");
  }
  if (light != s_light_on)
  {
    s_light_on = light;
    Relay_Light_Set(light);
    DebugPrint("[Ctrl] Light -> %s\r\n", light ? "ON" : "OFF");
  }
  if (buzzer != s_buzzer_on)
  {
    s_buzzer_on = buzzer;
    Buzzer_Set(buzzer);
    DebugPrint("[Ctrl] Buzzer-> %s\r\n", buzzer ? "ON" : "OFF");
  }

  /* 记住本次的越限报警状态, 下次进来作为"上一次状态"参与滞回判断 */
  s_limit_alarm = limit_alarm;
}
