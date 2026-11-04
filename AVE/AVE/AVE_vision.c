//视觉串口报文解析模块

//MaixCAM（串口输出 `cx,cy,flag`） → AVE_vision 解析 
//→ 得到 err_x err_y → PID 计算目标 rpm → AVE_gimbal → AVE_stepper → 云台电机

#include "AVE_vision.h"

#include <stdio.h>      
/**
  * 一、编译期约束
  * 注意事项：数字位数上限钉死为 9，消除有符号累加溢出这类未定义行为；改大 1 位就可能把畸形行当正常帧。
  */
#define AVE_VISION_INT_MAX_DIGITS       9  

typedef char ave_vision_chk_digits[(AVE_VISION_INT_MAX_DIGITS <= 9) ? 1 : -1];

/* ============ 二、私有状态 ============ */
static AVE_VisionState s_state;    
static AVE_VisionFrame s_latest;   
static uint32_t        s_seq;         
static uint8_t         s_have_frame;  

#define AVE_VISION_PRINT_CAP           99999999UL

/**
  * 三、纯解析
  *
  * 函    数：跳过空白（连回车一起）
  * 参    数：p 输入指针
  * 返 回 值：跳过空白后的位置
  * 注意事项：行尾可能残留一个回车（ReadLine 只剥紧邻换行符的那一个），一并吞掉。
  */
static const char *skip_ws(const char *p)
{
  
  while ((*p == ' ') || (*p == '\t') || (*p == '\r'))
  {
    p++;
  }
  return p;
}

/**
  * 函    数：解析带可选符号的十进制整数
  * 参    数：p 起点；out 成功时写入
  * 返 回 值：数字之后的位置；失败返回 0
  * 注意事项：无数字、孤立正负号、位数超上限都算失败；上限 9 位由编译期约束钉死。
  */
static const char *parse_int(const char *p, int *out)
{
  int      sign   = 1;
  uint32_t mag    = 0U;
  int      digits = 0;

  p = skip_ws(p);   //跳过空格/回车/tab
  
  if ((*p == '+') || (*p == '-'))   //处理正负号
  {
    if (*p == '-')
    {
      sign = -1;
    }
    p++;  //指针跳过符号位
  }
  
  while ((*p >= '0') && (*p <= '9'))   //循环读取连续数字字符
  {
    digits++;
    if (digits > AVE_VISION_INT_MAX_DIGITS)   //位数>9位，返回
    {
      return 0;     
    }
    mag = (mag * 10U) + (uint32_t)(*p - '0');
    p++;
  }

  if (digits == 0)   //没有读到任何数字
  {
    return 0;       
  }

  *out = (sign < 0) ? -(int)mag : (int)mag;   //正负赋值输出
  return p;
}

/**
  * 函    数：坐标是否落在画面内
  * 参    数：x 横坐标；y 纵坐标
  * 返 回 值：1 在画内，0 越界
  * 注意事项：只对 flag==1 的目标帧使用，无目标帧的 -1 不查。
  */
static int in_range(int x, int y)
{
  return (x >= 0) && (x < AVE_VISION_SCREEN_W) && (y >= 0) && (y < AVE_VISION_SCREEN_H);
}

/**
  * 函    数：解析一行串口报文
  * 参    数：s 输入行；out 输出的帧结构
  * 返 回 值：AVE_VISION_PARSE_* 各类结果
  * 注意事项：flag==1 时坐标必须落在画面内；flag==0 则坐标归一为 -1,-1 且越界仍算错。
  */
int AVE_Vision_ParseLine(const char *s, AVE_VisionFrame *out)
{
  const char *p;
  int         v[3];  //解析出来的3个数字：v[0]=cx, v[1]=cy, v[2]=flag
  int         i;

  if (s == 0)  //空指针
  {
    return AVE_VISION_PARSE_FORMAT;   //语法错
  }

  p = skip_ws(s);
  if (*p == '\0') //空白行
  {
    return AVE_VISION_PARSE_BLANK;      //空白帧
  }

  for (i = 0; i < 3; i++)  //循环3个数字，用逗号分开
  {
    p = parse_int(p, &v[i]);  //解析一个整数
    if (p == 0)   
    {
      return AVE_VISION_PARSE_FORMAT;
    }

    p = skip_ws(p);
    if (i < 2)
    {
      if (*p != ',')
      {
        return AVE_VISION_PARSE_FORMAT; 
      }
      p++;
    }
  }

  p = skip_ws(p);
  if (*p != '\0')
  {
    return AVE_VISION_PARSE_FORMAT;
  }
 
  if ((v[2] != 0) && (v[2] != 1))     //flag只能是0或1
  {
    return AVE_VISION_PARSE_FORMAT;  //语法格式错误
  }

  if (out == 0)  //空指针保护
  {
    
    return AVE_VISION_PARSE_FORMAT;
  }

  out->flag            = (uint8_t)v[2];
  out->flag0_bad_coords = 0U;

  if (v[2] == 1)   //检测到靶标
  {
    
    if (!in_range(v[0], v[1]))
    {
      return AVE_VISION_PARSE_RANGE;  //强制校验坐标必须在画面内
    }

    out->cx   = v[0];
    out->cy   = v[1];
    out->kind = AVE_VISION_KIND_OK;  //成功识别到目标
    return AVE_VISION_PARSE_OK;
  }

  out->cx   = -1;
  out->cy   = -1;
  out->kind = AVE_VISION_KIND_NO_TARGET;

  if ((v[0] == -1) && (v[1] == -1))    //没有检测到靶标
  {
    return AVE_VISION_PARSE_NO_TARGET;   
  }

  if (!in_range(v[0], v[1]))
  {
    return AVE_VISION_PARSE_RANGE;     //坐标越界
  }

  out->flag0_bad_coords = 1U;
  return AVE_VISION_PARSE_NO_TARGET;   //报文合法，但没有目标
}

/**
  * 函    数：由目标中心求与画面中心的像素偏差
  * 参    数：f 帧数据；err_x 输出偏差；err_y 输出偏差
  * 返 回 值：1 有目标，0 无（偏差置 0）
  * 注意事项：纵轴取反使目标在下方即云台下压方向正确，符号写错会导致云台追即逃。
  */
int AVE_Vision_Deviation(const AVE_VisionFrame *f, int *err_x, int *err_y)
{
  if ((f == 0) || (err_x == 0) || (err_y == 0))
  {
    return 0;
  }

  if (f->kind != AVE_VISION_KIND_OK) //判断帧种类，是否识别到目标物体
  {
    *err_x = 0;
    *err_y = 0;  // 误差清零，PID输入置0，云台停止跟踪
    return 0;
  }
  *err_x = f->cx - AVE_VISION_CENTER_X;   //靶标在画面下方，云台向下转动
  *err_y = AVE_VISION_CENTER_Y - f->cy;   //靶标在画面上方，云台向上转动

  return 1;
}


/**
  * 函    数：发布最新一帧并刷新断流计时
  * 参    数：f 帧数据；now_ms 当前毫秒
  * 返 回 值：无
  * 注意事项：只保留最后一帧，防止队列积压。
  */
/* ============ 四、状态机 ============ */
static void publish(const AVE_VisionFrame *f, uint32_t now_ms)
{
  s_seq++;                       //帧序列号
  s_latest = *f;                 //最新帧存储
  s_latest.seq     = s_seq;      //保留最新一帧
  s_latest.tick_ms = now_ms;
  s_have_frame     = 1U;
}

/**
  * 函    数：处理一行已切好的文本
  * 参    数：line 一行文本；now_ms 当前毫秒
  * 返 回 值：无
  * 注意事项：任何非法输入都不刷新 last_frame_ms，200ms 后自然断流，用单一机制兜底。
  */
static void handle_line(const char *line, uint32_t now_ms)
{
  AVE_VisionFrame f;   //定义帧结构体，存储：cx, cy, flag
  int              k;  //解析状态码

  k = AVE_Vision_ParseLine(line, &f);   //调用解析函数，检测到目标

  if (k == AVE_VISION_PARSE_BLANK)  //空行
  {
    
    s_state.blank++;    //收到空行次数
    return;
  }

  if (k == AVE_VISION_PARSE_FORMAT) //格式错误
  {
    s_state.proto_err++; //协议格式错误次数
    return;             
  }

  if (k == AVE_VISION_PARSE_RANGE) //数值越界
  {
    
    s_state.proto_err++;
    s_state.range_err++;  //坐标数值越界错误次数
    return;             
  }
  
  if (k == AVE_VISION_PARSE_OK)   //解析成功
  {
    s_state.ok++;        //识别到靶标的有效帧次数
    s_state.last_frame_ms = now_ms;              
  }
  else
  {
    s_state.no_target++;         //解析成功耽误目标
    if (f.flag0_bad_coords != 0U)
    {
      s_state.flag0_coords++;    //无效坐标帧次数
    }
    s_state.last_frame_ms = now_ms;  
  }

  publish(&f, now_ms);
}

/**
  * 函    数：轮询读串口喂给状态机并刷新断流判定
  * 参    数：now_ms 当前毫秒
  * 返 回 值：无
  * 注意事项：每轮最多读 MAX_LINES_PER_POLL 行，超限记 backlog，防止任务被饿死。
  */
void AVE_Vision_Poll(uint32_t now_ms)
{
  char     line[AVE_VISION_LINE_MAX];  //字符数组缓冲区
  uint32_t stale_now;                  //视觉链路是否断连
  int      n;                          //本轮读取到的行数
  int      r;                          //结果码
  
  for (n = 0; n < AVE_VISION_MAX_LINES_PER_POLL; n++) //限流保护，防止串口积压大量数据
  {
    r = AVE_Uart_ReadLine(AVE_UART_CAM, line, (int)sizeof(line)); //从CAM串口读取一行

    if (r >= 0)
    {
      handle_line(line, now_ms);  //成功读取到一行，交给handle_line解析处理
      continue;
    }

    if (r == AVE_UART_ENOSPC)   //单行溢出
    {
      s_state.line_spilled++;   //溢出事件次数
      continue;
    }

    if (r == AVE_UART_ELINE_TOO_LONG)  //超长脏数据
    {
      s_state.ring_flushed++;  //脏数据事件次数
      break;
    }
    break;      
  }
  
  if (n == AVE_VISION_MAX_LINES_PER_POLL) //缓冲区最大上限
  {
    s_state.backlog++;     //接收积压次数
  }

  stale_now = ((now_ms - s_state.last_frame_ms) > AVE_VISION_RX_TIMEOUT_MS) ? 1U : 0U;
  
  if ((stale_now != 0U) && (s_state.stale == 0U))
  {
    s_state.stale_events++;
  }
  s_state.stale = stale_now;
}

/**
  * 函    数：初始化视觉状态机
  * 参    数：无
  * 返 回 值：无
  * 注意事项：清空全部统计、序号与最近帧。
  */
void AVE_Vision_Init(void)
{
  // ========== 【s_state：链路状态统计结构体，全部清零】 ==========
  s_state.ok            = 0U;         // 成功识别靶标帧数
  s_state.no_target     = 0U;         // 解析成功、但无靶标的帧数
  s_state.flag0_coords  = 0U;         // flag=0时无效坐标计数
  s_state.proto_err     = 0U;         // 协议错误总计数
  s_state.range_err     = 0U;         // 坐标越界错误计数
  s_state.line_spilled  = 0U;         // 单行超长溢出计数
  s_state.ring_flushed  = 0U;         // 串口环形缓存冲刷清空次数
  s_state.backlog       = 0U;         // 接收数据积压计数
  s_state.blank         = 0U;         // 收到空行计数
  s_state.stale         = 0U;         // 链路超时标记：0=正常，1=断连
  s_state.stale_events  = 0U;         // 链路发生断连事件次数
  s_state.last_frame_ms = 0U;         // 上一次收到合法帧的时间戳

  s_seq        = 0U;  // 帧序列号，用于帧序号计数
  s_have_frame = 0U; // 是否存在有效最新帧标记：0=没有有效帧

  // ========== 【s_latest：保存最新收到的那一帧视觉数据】 ==========
  s_latest.cx                = -1;        // 目标X坐标，初始-1代表无效
  s_latest.cy                = -1;        // 目标Y坐标，初始-1代表无效
  s_latest.flag              = 0U;        // 目标标志：0=无目标
  s_latest.flag0_bad_coords  = 0U;        // flag=0时坐标是否为无效标记
  s_latest.kind              = AVE_VISION_KIND_NONE; // 帧类型：无有效视觉数据
  s_latest.seq               = 0U;        // 当前帧序列号
  s_latest.tick_ms           = 0U;        // 该帧对应的时间戳
}

/**
  * 函    数：取最新一帧
  * 参    数：out 输出的帧结构
  * 返 回 值：1 有帧，0 无（out 填默认值）
  */
int AVE_Vision_Latest(AVE_VisionFrame *out) //out 指针，用于存储最新一帧视觉数据
{
  if (out == 0)
  {
    return 0;
  }

  if (s_have_frame == 0U)    //未收到视觉帧
  {
    out->cx               = -1;
    out->cy               = -1;
    out->flag             = 0U;
    out->flag0_bad_coords = 0U;
    out->kind             = AVE_VISION_KIND_NONE;
    out->seq              = 0U;
    out->tick_ms          = 0U;
    return 0;
  }

  *out = s_latest;      
  return 1;
}

/**
  * 函    数：取统计信息只读指针
  * 参    数：无
  * 返 回 值：AVE_VisionState 只读指针
  */
const AVE_VisionState *AVE_Vision_State(void)
{
  return &s_state;
}
/**
  * 函    数：判断视觉数据是否断流
  * 参    数：now_ms 当前毫秒
  * 返 回 值：1 断流，0 未断流
  */
int AVE_Vision_IsStale(uint32_t now_ms)
{
  return ((now_ms - s_state.last_frame_ms) > AVE_VISION_RX_TIMEOUT_MS) ? 1 : 0;
}


/**
  * 函    数：渲染时的 8 位封顶
  * 参    数：v 原始值
  * 返 回 值：封顶后的值
  */
/* ============ 五、日志渲染 ============ */
static unsigned long print_cap(uint32_t v)
{
  if (v > AVE_VISION_PRINT_CAP) { return (unsigned long)AVE_VISION_PRINT_CAP; }
  return (unsigned long)v;
}
/**
  * 函    数：格式化 [VS] 日志行
  * 参    数：out 输出缓冲；cap 容量；now_ms 当前毫秒；st 统计；f 帧
  * 返 回 值：>0 长度，-1 参数错或写不下
  */
int AVE_Vision_Format(char *out, int cap, uint32_t now_ms,
                      const AVE_VisionState *st, const AVE_VisionFrame *f)
{
  int n;

  
  if ((out == 0) || (st == 0) || (f == 0) || (cap <= 0))
  {
    return -1;
  }

  
  n = snprintf(out, (size_t)cap,
               "[VS] t=%lu st=%lu se=%lu ok=%lu dr=%lu sp=%lu f=%d,%d,%d",
               (unsigned long)now_ms,          
               print_cap(st->stale),
               print_cap(st->stale_events),
               print_cap(st->ok),
               print_cap(st->proto_err),
               print_cap(st->line_spilled),
               f->cx, f->cy, (int)f->kind);

  if ((n < 0) || (n >= cap))
  {
    return -1;      
  }

  return n;
}
/**
  * 函    数：把最新一帧格式成 [VS] 日志行
  * 参    数：out 输出缓冲；cap 容量；now_ms 当前毫秒
  * 返 回 值：>0 长度，-1 失败
  */
int AVE_Vision_Report(char *out, int cap, uint32_t now_ms)
{
  AVE_VisionFrame f;

  (void)AVE_Vision_Latest(&f);     
  return AVE_Vision_Format(out, cap, now_ms, &s_state, &f);
}
