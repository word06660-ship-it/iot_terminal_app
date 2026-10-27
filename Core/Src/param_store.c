/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    param_store.c
  * @brief   用 W25Q64 模拟 EEPROM (双槽 A/B 轮换 + CRC 校验)
  ******************************************************************************
  */
/* USER CODE END Header */

#include "param_store.h"
#include "w25q64.h"
#include <stddef.h>   /* offsetof */
#include <string.h>   /* memcpy, memset */

/* 槽 A 在 W25Q_EEPROM_ADDR, 槽 B 紧跟在后面 (每个槽占一个 4KB 扇区) */
#define PARAM_SLOT_ADDR(n)   (W25Q_EEPROM_ADDR + ((uint32_t)(n) * W25Q64_SECTOR_SIZE))

/* 内存里的当前参数 */
static ParamBlock_t s_param;

/* 最新有效参数所在的槽号 (0=槽A, 1=槽B); 保存时写到"另一个"槽 */
static uint8_t s_slot_in_use = 1;

uint16_t Crc16Update(uint16_t crc, const uint8_t *buf, uint32_t len)
{
  uint32_t i;
  uint8_t bit;

  for (i = 0; i < len; i++)
  {
    crc ^= buf[i];
    for (bit = 0; bit < 8; bit++)
    {
      if (crc & 0x0001)
      {
        crc = (uint16_t)((crc >> 1) ^ 0xA001);
      }
      else
      {
        crc >>= 1;
      }
    }
  }
  return crc;
}

uint16_t Crc16(const uint8_t *buf, uint32_t len)
{
  /* 一次算完 = 从初值开始, 只"累加"这一段 */
  return Crc16Update(0xFFFF, buf, len);
}

ParamBlock_t *Param_Get(void)
{
  return &s_param;
}

void Param_SetDefaults(void)
{
  memset(&s_param, 0, sizeof(s_param));
  s_param.magic = PARAM_MAGIC;
  s_param.active_slot = FW_SLOT_A;          /* 出厂固件在 Slot A */
  s_param.fw_version = FW_VERSION_CURRENT;
}

/**
  * @brief 读一个槽, 校验 magic 与 CRC
  * @retval 1=这一槽的数据有效, 0=无效(没写过 / 被擦坏了)
  */
static uint8_t Param_LoadSlot(uint8_t slot, ParamBlock_t *out)
{
  uint8_t raw[sizeof(ParamBlock_t)];

  W25Q_Read(PARAM_SLOT_ADDR(slot), raw, sizeof(raw));
  memcpy(out, raw, sizeof(ParamBlock_t));

  if (out->magic != PARAM_MAGIC)
  {
    return 0;
  }
  /* CRC 只覆盖 crc 字段之前的部分 */
  if (out->crc != Crc16(raw, (uint32_t)offsetof(ParamBlock_t, crc)))
  {
    return 0;
  }
  return 1;
}

uint8_t Param_Save(void)
{
  uint8_t target = (uint8_t)(s_slot_in_use ^ 1U);   /* 换另一个槽写 */
  uint32_t addr = PARAM_SLOT_ADDR(target);
  ParamBlock_t check = {0};

  s_param.magic = PARAM_MAGIC;
  s_param.seq++;                                    /* 序号递增: 上电时靠它判断谁更新 */
  s_param.crc = 0;                                  /* 先置 0 再算 */
  s_param.crc = Crc16((const uint8_t *)&s_param, (uint32_t)offsetof(ParamBlock_t, crc));

  if (!W25Q_SectorErase(addr))
  {
    DebugPrint("[PARAM] erase FAIL @0x%06lX\r\n", (unsigned long)addr);
    return 0;
  }
  if (!W25Q_Write(addr, (const uint8_t *)&s_param, sizeof(ParamBlock_t)))
  {
    DebugPrint("[PARAM] write FAIL @0x%06lX\r\n", (unsigned long)addr);
    return 0;
  }

  /* 回读校验: 确认真的落盘了, 才敢把刚写坏的槽当备份、把老槽替换掉 */
  if (!Param_LoadSlot(target, &check) || (check.seq != s_param.seq))
  {
    DebugPrint("[PARAM] verify FAIL @0x%06lX\r\n", (unsigned long)addr);
    return 0;
  }

  s_slot_in_use = target;
  return 1;
}

void Param_Init(void)
{
  ParamBlock_t a = {0};
  ParamBlock_t b = {0};
  uint8_t ok_a = Param_LoadSlot(0, &a);
  uint8_t ok_b = Param_LoadSlot(1, &b);

  if (ok_a && ok_b)
  {
    /* 两槽都有效: 取写入序号大的那一份 (序号大 = 后写的 = 更新) */
    if (b.seq > a.seq)
    {
      s_param = b;
      s_slot_in_use = 1;
    }
    else
    {
      s_param = a;
      s_slot_in_use = 0;
    }
  }
  else if (ok_a)
  {
    s_param = a;
    s_slot_in_use = 0;
  }
  else if (ok_b)
  {
    s_param = b;
    s_slot_in_use = 1;
  }
  else
  {
    /* 两槽都无效: 首次上电或参数被破坏, 用默认值重建。
     * s_slot_in_use 先设成 1, 这样下面第一次保存会写进槽 A */
    DebugPrint("[PARAM] no valid block, use defaults\r\n");
    s_slot_in_use = 1;
    Param_SetDefaults();
    Param_Save();
  }

  /* 重启次数 +1 (每次上电都存一次, W25Q64 扇区可擦写 10 万次, 够用) */
  s_param.boot_count++;
  Param_Save();

  DebugPrint("[PARAM] boot #%lu, slot=%c, fw=%u.%u, upgrade=%u\r\n",
             (unsigned long)s_param.boot_count,
             (s_param.active_slot == FW_SLOT_A) ? 'A' : 'B',
             (unsigned)(s_param.fw_version >> 8),
             (unsigned)(s_param.fw_version & 0xFF),
             (unsigned)s_param.upgrade_flag);
}
