/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    history.c
  * @brief   历史数据环形存储 + 断网数据暂存/补传
  ******************************************************************************
  */
/* USER CODE END Header */

#include "history.h"
#include "param_store.h"

extern RTC_HandleTypeDef hrtc;   /* main.c 中的 RTC 句柄 */

/* 指针落盘节流: 每条记录都存一次指针会把 FLASH 擦坏, 所以攒够一批再存
 * (掉电最多导致这一批记录被重复上传一次, 不会丢数据) */
#define HIST_CHECKPOINT_WRITES   64U    /* 每写入 64 条(约 10 分钟)存一次写指针 */
#define HIST_CHECKPOINT_ACKS     256U   /* 每确认 256 条(约 40 分钟数据)存一次读指针 */

/* 上一条记录的时间戳: 用来保证时间戳只增不减 (RTC 上电会被重置) */
static uint32_t s_last_ts = 0;

/* 序号 -> 在 W25Q64 里的物理地址 (取模实现"环形") */
static uint32_t Hist_Addr(uint32_t seq)
{
  return W25Q_HISTORY_ADDR + ((seq % HIST_CAPACITY) * HIST_REC_SIZE);
}

static uint8_t Hist_IsLeap(uint16_t year)
{
  if ((year % 4U) != 0U)
  {
    return 0;
  }
  if ((year % 100U) != 0U)
  {
    return 1;
  }
  return ((year % 400U) == 0U) ? 1 : 0;
}

/**
  * @brief 年月日 -> 从 2000-01-01 起算的天数 (RTC 年份 0~99 对应 2000~2099)
  */
static uint32_t Hist_DaysSince2000(uint16_t year, uint8_t month, uint8_t day)
{
  static const uint8_t mdays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  uint32_t days = 0;
  uint16_t y;
  uint8_t m;

  for (y = 0; y < year; y++)
  {
    days += Hist_IsLeap((uint16_t)(2000U + y)) ? 366UL : 365UL;
  }
  for (m = 0; (uint8_t)(m + 1) < month; m++)
  {
    days += mdays[m];
    if ((m == 1U) && Hist_IsLeap((uint16_t)(2000U + year)))
    {
      days++;   /* 闰年的二月多一天 */
    }
  }
  if (day > 0)
  {
    days += (uint32_t)(day - 1U);
  }
  return days;
}

uint32_t History_NowSeconds(void)
{
  RTC_TimeTypeDef t = {0};
  RTC_DateTypeDef d = {0};

  HAL_RTC_GetTime(&hrtc, &t, RTC_FORMAT_BIN);
  /* 必须紧接着再读一次日期: 这样才能解锁 RTC 影子寄存器, 否则时间不再刷新 */
  HAL_RTC_GetDate(&hrtc, &d, RTC_FORMAT_BIN);

  return (Hist_DaysSince2000(d.Year, d.Month, d.Date) * 86400UL)
       + ((uint32_t)t.Hours * 3600UL)
       + ((uint32_t)t.Minutes * 60UL)
       + (uint32_t)t.Seconds;
}

uint8_t History_Init(void)
{
  ParamBlock_t *p = Param_Get();
  HistRecord_t rec;
  uint32_t last_ts = 0;
  uint32_t i;

  /* 从最近写下的几条里往前找, 找到第一条能通过 CRC 校验的记录, 取它的时间戳。
   * 作用: RTC 每次上电都被重置成 2000-01-01, 靠这个值保证新记录的时间戳不会倒退 */
  for (i = 1; i <= 8U; i++)
  {
    uint32_t seq;
    uint32_t addr;

    if (p->hist_write_seq < i)
    {
      break;   /* 还没写过这么多条 */
    }
    seq = p->hist_write_seq - i;
    addr = Hist_Addr(seq);
    W25Q_Read(addr, (uint8_t *)&rec, HIST_REC_SIZE);
    if ((rec.seq == seq) && (rec.crc == Crc16((const uint8_t *)&rec, HIST_REC_SIZE - 2U)))
    {
      last_ts = rec.ts;
      break;
    }
  }
  s_last_ts = last_ts;

  DebugPrint("[HIST] stored=%lu/%lu, pending=%lu, last_ts=%lu\r\n",
             (unsigned long)History_StoredCount(), (unsigned long)HIST_CAPACITY,
             (unsigned long)History_PendingCount(), (unsigned long)s_last_ts);
  return 1;
}

uint8_t History_Append(int16_t temp_x10, uint16_t humi_x10, uint16_t lux)
{
  ParamBlock_t *p = Param_Get();
  HistRecord_t rec;
  uint32_t addr;
  uint32_t now;

  /* 环形区满了: 最老的那些待传数据马上会被覆盖, 先把读指针推过去,
   * 否则补传时会一直去读已经被覆盖的地址 (数据丢了, 但至少不会卡死) */
  if ((p->hist_write_seq - p->hist_acked_seq) >= HIST_CAPACITY)
  {
    p->hist_acked_seq = p->hist_write_seq - (HIST_CAPACITY - 1U);
    DebugPrint("[HIST] ring FULL, oldest records overwritten\r\n");
    Param_Save();
  }

  addr = Hist_Addr(p->hist_write_seq);

  /* 写进一个新扇区前必须先擦: FLASH 只能把 1 写成 0, 不擦就等于新旧数据按位与 */
  if ((addr % W25Q64_SECTOR_SIZE) == 0U)
  {
    if (!W25Q_SectorErase(addr))
    {
      DebugPrint("[HIST] erase FAIL @0x%06lX\r\n", (unsigned long)addr);
      return 0;
    }
  }

  now = History_NowSeconds();
  if (now <= s_last_ts)
  {
    now = s_last_ts + 1U;   /* 时间戳只增不减 */
  }
  s_last_ts = now;

  rec.seq      = p->hist_write_seq;
  rec.ts       = now;
  rec.temp_x10 = temp_x10;
  rec.humi_x10 = humi_x10;
  rec.lux      = lux;
  rec.crc      = Crc16((const uint8_t *)&rec, HIST_REC_SIZE - 2U);

  if (!W25Q_Write(addr, (const uint8_t *)&rec, HIST_REC_SIZE))
  {
    DebugPrint("[HIST] write FAIL @0x%06lX\r\n", (unsigned long)addr);
    return 0;
  }

  p->hist_write_seq++;

  /* 攒够一批就把"写到哪了"落盘 (指针落后只会导致重复上传, 不会丢数据) */
  if ((p->hist_write_seq % HIST_CHECKPOINT_WRITES) == 0U)
  {
    Param_Save();
  }
  return 1;
}

uint8_t History_Peek(uint32_t offset, HistRecord_t *out)
{
  ParamBlock_t *p = Param_Get();
  uint32_t seq = p->hist_acked_seq + offset;

  if (seq >= p->hist_write_seq)
  {
    return HIST_EMPTY;
  }

  W25Q_Read(Hist_Addr(seq), (uint8_t *)out, HIST_REC_SIZE);

  /* 序号对不上 = 这条已经被环形区绕圈覆盖掉了; CRC 对不上 = 写入时掉电了 */
  if (out->seq != seq)
  {
    return HIST_BAD;
  }
  if (out->crc != Crc16((const uint8_t *)out, HIST_REC_SIZE - 2U))
  {
    return HIST_BAD;
  }
  return HIST_OK;
}

uint8_t History_Ack(void)
{
  ParamBlock_t *p = Param_Get();

  if (p->hist_acked_seq >= p->hist_write_seq)
  {
    return 0;
  }
  p->hist_acked_seq++;

  /* 攒够一批, 或者待传队列刚清空, 就把"传到哪了"落盘 */
  if (((p->hist_acked_seq % HIST_CHECKPOINT_ACKS) == 0U) ||
      (p->hist_acked_seq == p->hist_write_seq))
  {
    Param_Save();
  }
  return 1;
}

uint32_t History_PendingCount(void)
{
  ParamBlock_t *p = Param_Get();

  return (p->hist_write_seq - p->hist_acked_seq);
}

uint32_t History_StoredCount(void)
{
  ParamBlock_t *p = Param_Get();

  return (p->hist_write_seq < HIST_CAPACITY) ? p->hist_write_seq : HIST_CAPACITY;
}
