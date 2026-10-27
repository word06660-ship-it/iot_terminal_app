/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    param_store.h
  * @brief   用 W25Q64 模拟 EEPROM: 保存"掉电不能丢"的少量参数
  *          存放内容: OTA 元数据 (当前槽位/版本号/升级标志) + 重启次数
  *                    + 历史数据区的读写指针 (见 history.c)
  *
  * 掉电安全原理 (双槽 A/B 轮换):
  *   W25Q64 有 8KB 专门留给参数区, 分成两个 4KB 扇区 (槽 A / 槽 B)。
  *   每次保存都写到"当前没在用"的那个槽, 两个槽轮流当备份:
  *     槽A 有效, 槽B 是旧备份 -> 保存写槽B -> 写成功后槽B 变成最新
  *   这样即使"擦除到一半掉电", 另一个槽里的旧参数依然完好, 上电时
  *   靠 magic + CRC16 就能识别出哪一份是好的, 自动回退。
  ******************************************************************************
  */
/* USER CODE END Header */

#ifndef __PARAM_STORE_H
#define __PARAM_STORE_H

#ifdef __cplusplus
extern "C" {
#endif

#include "main.h"

/* 参数块标识: 只有这个值对得上, 才认为这一槽是我们写过的数据 */
#define PARAM_MAGIC   0x494F5431UL   /* ASCII "IOT1" */

/* 固件运行槽位 (对应片内 Flash 的 Slot A / Slot B) */
#define FW_SLOT_A     0
#define FW_SLOT_B     1

/* 当前固件版本 (主版本<<8 | 次版本), 上报报文里的 fw 字段用它 */
#define FW_VERSION_CURRENT   0x0100U   /* v1.0 */

/**
  * @brief 参数块: 一次性把"掉电不能丢"的元数据都放进来
  * @note  packed 表示不做字节对齐填充, 保证结构体在内存和 FLASH 里是同一份字节序
  *        (改字段顺序/类型会让老数据 CRC 不匹配, 上电后自动重新初始化)
  */
typedef struct
{
  uint32_t magic;         /* 固定为 PARAM_MAGIC */
  uint32_t seq;           /* 写入序号: 上电时取两个槽里序号大的那一份 */
  uint32_t boot_count;    /* 上电次数 (每次上电 +1) */
  uint32_t hist_write_seq;/* 历史数据: 下一条要写的序号 */
  uint32_t hist_acked_seq;/* 历史数据: 下一条待补传的序号 (已上传的都比它小) */
  uint32_t fw_size;       /* 新固件字节数 (OTA 用) */
  uint16_t fw_version;    /* 目标固件版本 (主版本<<8 | 次版本) */
  uint16_t fw_crc;        /* 新固件 CRC16 (OTA 用) */
  uint16_t crc;           /* 本结构体前 31 字节的 CRC16 (计算时本字段先置 0) */
  uint8_t  active_slot;   /* 当前运行槽: FW_SLOT_A / FW_SLOT_B */
  uint8_t  upgrade_flag;  /* 1=W25Q64 里有待安装的新固件 */
  uint8_t  upgrade_valid; /* 1=新固件已下载并通过 CRC 校验 */
} __attribute__((packed)) ParamBlock_t;

/**
  * @brief  上电初始化: 从双槽里挑出有效且最新的那份载入内存, 然后把重启次数 +1 存回去
  * @note   必须在 W25Q_Init() 成功之后调用
  */
void          Param_Init(void);

/**
  * @brief  取内存中的参数 (可以直接改字段, 改完调 Param_Save() 落盘)
  */
ParamBlock_t *Param_Get(void);

/**
  * @brief  把内存中的参数保存到"当前没在用"的那个槽 (擦除 + 写入 + 回读校验)
  * @retval 1=成功, 0=失败
  */
uint8_t       Param_Save(void);

/**
  * @brief  恢复成出厂默认值 (只改内存, 不落盘)
  */
void          Param_SetDefaults(void);

/**
  * @brief  通用 CRC16 (Modbus 多项式 0xA001, 初值 0xFFFF)
  * @note   参数块和历史记录的完整性校验都用它
  */
uint16_t      Crc16(const uint8_t *buf, uint32_t len);

/**
  * @brief  增量式 CRC16: 把一整块数据拆成多段, 一段一段接着算
  * @param  crc: 上一段算出来的结果; 第一段传 0xFFFF
  * @retval 累加到本段为止的 CRC16
  * @note   给"数据太大、RAM 装不下"的场景用 (例如校验 W25Q64 里 512KB 的
  *         OTA 固件缓冲, 只能分段读出来累加)
  */
uint16_t      Crc16Update(uint16_t crc, const uint8_t *buf, uint32_t len);

#ifdef __cplusplus
}
#endif

#endif /* __PARAM_STORE_H */
