/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    history.h
  * @brief   历史数据环形存储 + 断网数据暂存/补传
  *
  * 一套结构同时干两件事:
  *   1. 历史数据存储 : 采集到的温湿度/光照按固定间隔写成记录, 存在 W25Q64 里
  *   2. 断网暂存补传 : 每条记录有两个指针 —— "写到哪了(write_seq)" 和
  *                     "传到哪了(acked_seq)"。上传失败指针就不动, 数据留在
  *                     FLASH 里; 网络恢复后从 acked_seq 开始逐条补传,
  *                     所以断网期间的数据一条都不会丢。
  *
  * 环形区: 写在分区表给的固定区域里顺序推进, 写满一圈后回到开头覆盖最老的
  *         (写入前会先擦除该 4KB 扇区 —— FLASH 只能把 1 写成 0)
  ******************************************************************************
  */
/* USER CODE END Header */

#ifndef __HISTORY_H
#define __HISTORY_H

#ifdef __cplusplus
extern "C" {
#endif

#include "main.h"
#include "w25q64.h"

/**
  * @brief 一条历史记录 (16 字节)
  * @note  温度/湿度都放大 10 倍存成整数, 这样整数运算就够了, 不用在记录里存浮点
  */
typedef struct
{
  uint32_t seq;       /* 递增序号: 用来判断这个槽位里的记录是不是"我要的那一条" */
  uint32_t ts;        /* 时间戳: 从 2000-01-01 00:00:00 起算的秒数 */
  int16_t  temp_x10;  /* 温度 x10 (253 表示 25.3 摄氏度, 负数表示零下) */
  uint16_t humi_x10;  /* 湿度 x10 (602 表示 60.2 %) */
  uint16_t lux;       /* 光照 (lx) */
  uint16_t crc;       /* 前 14 字节的 CRC16 */
} __attribute__((packed)) HistRecord_t;

#define HIST_REC_SIZE    ((uint32_t)sizeof(HistRecord_t))                    /* 16 字节 */
#define HIST_CAPACITY    (W25Q_HISTORY_SIZE / HIST_REC_SIZE)                 /* 490752 条 */
#define HIST_INTERVAL_MS 10000U   /* 每 10 秒存一条 -> 整圈可存约 56 天 */

/* History_Peek() 的返回值 */
#define HIST_EMPTY   0   /* 没有待补传的数据了 */
#define HIST_OK      1   /* 成功取到一条有效记录 */
#define HIST_BAD     2   /* 这一条损坏或已被环形区覆盖, 调用方应直接 History_Ack() 跳过 */

/**
  * @brief  上电初始化: 从环形区尾部找回"最后一条记录的时间戳"
  * @note   必须在 Param_Init() 之后调用
  * @retval 1=成功
  */
uint8_t  History_Init(void);

/**
  * @brief  追加一条记录 (自动打时间戳), 会让"写指针"前移
  * @param  temp_x10: 温度 x10; humi_x10: 湿度 x10; lux: 光照
  * @retval 1=成功, 0=失败
  */
uint8_t  History_Append(int16_t temp_x10, uint16_t humi_x10, uint16_t lux);

/**
  * @brief  从"最老的待补传记录"往后数 offset 条, 取出来看看
  * @param  offset: 0=队头(最老的那条)
  * @retval HIST_OK / HIST_EMPTY / HIST_BAD
  */
uint8_t  History_Peek(uint32_t offset, HistRecord_t *out);

/**
  * @brief  确认队头那条已经上传成功, "读指针"前移一条
  * @retval 1=成功, 0=已经没有待传数据
  */
uint8_t  History_Ack(void);

/**
  * @brief  还有多少条没补传 (write_seq - acked_seq)
  */
uint32_t History_PendingCount(void);

/**
  * @brief  环形区里现存多少条记录 (最多 HIST_CAPACITY)
  */
uint32_t History_StoredCount(void);

/**
  * @brief  读 RTC 当前时间, 换算成"从 2000-01-01 起算的秒数"
  */
uint32_t History_NowSeconds(void);

#ifdef __cplusplus
}
#endif

#endif /* __HISTORY_H */
