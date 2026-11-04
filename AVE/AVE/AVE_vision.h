

#ifndef AVE_VISION_H
#define AVE_VISION_H

#include "AVE_config.h"
#include "AVE_uart.h"



/**
  * 一、协议判定的结果码
  * 注意事项：只有 OK 与 NO_TARGET 是合法帧，其余不刷新断流计时。
  */
typedef enum
{
  AVE_VISION_PARSE_BLANK     = -3,//空行/纯空白，忽略，不算错误
  AVE_VISION_PARSE_RANGE     = -2,//取值/范围错：flag==1 而坐标越界
  AVE_VISION_PARSE_FORMAT    = -1,//语法错，整行丢弃
  AVE_VISION_PARSE_NO_TARGET =  1,//合法"无目标"帧 -1,-1,0
  AVE_VISION_PARSE_OK        =  0,//合法目标帧：flag==1 且在画面内
} AVE_VisionParse;



/**
  * 二、帧与状态
  *
  * 一帧的性质：NONE 与 NO_TARGET 是两件不同的事，不能混。
  */
typedef enum
{
  AVE_VISION_KIND_NONE      = 0,  //从未收到过任何帧
  AVE_VISION_KIND_NO_TARGET = 1,  //收到合法无目标帧
  AVE_VISION_KIND_OK        = 2,  //收到合法目标帧
} AVE_VisionKind;

/**
  * 一帧视觉数据。cx/cy 只在 kind==OK 时有意义，NO_TARGET 时归一为 -1,-1。
  */
typedef struct
{
  int            cx;      //目标中心 x（向右为正）
  int            cy;      //目标中心 y（向下为正）
  uint8_t        flag;    //线上原始 flag（0 或 1）

  
  uint8_t        flag0_bad_coords;//flag=0 却带坐标的越规帧

  AVE_VisionKind kind;    //这一帧的性质
  uint32_t       seq;     //已发布帧序号
  uint32_t       tick_ms; //这一帧被接受的时刻
} AVE_VisionFrame;

/**
  * 三、视觉层全部计数与状态，全是真实值，打印时才封顶。
  */
typedef struct
{
  uint32_t ok;            //合法目标帧数
  uint32_t no_target;     //合法无目标帧数（含越规无害的）
  uint32_t flag0_coords;  //其中 flag=0 却带坐标的越规数
  uint32_t proto_err;     //语法错+范围错的总数
  uint32_t range_err;//其中属于范围错的部分
  uint32_t line_spilled;//行放不下被整行丢弃
  uint32_t ring_flushed;//无换行整段被强制丢弃
  uint32_t backlog;//单次 Poll 触上限次数
  uint32_t blank;//空行条数  
  uint32_t stale;//当前是否断流(1=断流)
  uint32_t stale_events;//由通变断的跳变次数
  uint32_t last_frame_ms;//最后一次收到合法帧的时刻
} AVE_VisionState;



/**
  * 函    数：清空全部状态与计数
  * 参    数：无
  * 返 回 值：无
  * 注意事项：只能在调度器启动前调用；开机约 201ms 后未收帧即进入断流。
  */
void AVE_Vision_Init(void);

/**
  * 函    数：排空视觉串口所有完整行，只把最新一帧发布出去
  * 参    数：now_ms 当前毫秒
  * 返 回 值：无
  * 注意事项：每轮最多处理 MAX_LINES_PER_POLL 行，触顶计 backlog，防止任务被饿死。
  */
void AVE_Vision_Poll(uint32_t now_ms);

/**
  * 函    数：取最新一帧的副本
  * 参    数：out 输出缓冲
  * 返 回 值：1 有帧可读，0 从未收到过帧（out 被写默认值）
  * 注意事项：拷出而非返回内部指针，避免跨任务读到撕裂的组合。
  */
int AVE_Vision_Latest(AVE_VisionFrame *out);

/**
  * 函    数：取全部计数与状态的只读指针
  * 参    数：无
  * 返 回 值：静态存储只读指针，不要存也不要改
  */
const AVE_VisionState *AVE_Vision_State(void);

/**
  * 函    数：判断视觉是否断流（距上次合法帧超时）
  * 参    数：now_ms 当前毫秒
  * 返 回 值：1 断流，0 正常
  */
int AVE_Vision_IsStale(uint32_t now_ms);

/**
  * 函    数：解析一行（纯函数，不碰静态状态）
  * 参    数：s 输入行；out 输出帧
  * 返 回 值：AVE_VISION_PARSE_* 之一
  * 注意事项：前后容忍空白，数字最多 9 位（防符号溢出 UB），第三个整数之后只允许空白。
  */
int AVE_Vision_ParseLine(const char *s, AVE_VisionFrame *out);

/**
  * 函    数：求与画面中心的像素偏差
  * 参    数：f 帧；err_x；err_y
  * 返 回 值：1 偏差可用，0 无目标（输出置 0）
  * 注意事项：纵轴取反只在此发生一次，符号写反云台一追就逃。
  */
int AVE_Vision_Deviation(const AVE_VisionFrame *f, int *err_x, int *err_y);

/**
  * 函    数：把状态渲染成 [VS] 行正文（不含 \\r\\n）
  * 参    数：out；cap；now_ms
  * 返 回 值：写入字符数，负数表示 cap 不够
  * 注意事项：会无限增长的计数做 8 位封顶，保证行宽有限不超预算。
  */
int AVE_Vision_Report(char *out, int cap, uint32_t now_ms);

/**
  * 函    数：渲染用的纯函数版本（值从参数进，不读静态状态）
  * 参    数：out；cap；now_ms；st；f
  * 返 回 值：写入字符数，负数表示 cap 不够
  * 注意事项：st 与 f 必须非空；存在的唯一理由让最坏宽度可被测试。
  */
int AVE_Vision_Format(char *out, int cap, uint32_t now_ms,
                      const AVE_VisionState *st, const AVE_VisionFrame *f);

#endif
