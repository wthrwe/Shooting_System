/* 单生产者/单消费者环形缓冲（SPSC），纯 C99、不包含任何 HAL/CMSIS，可脱离硬件单测。
   无锁依据：生产者只写 head、消费者只写 tail，无写写冲突；Cortex-M4 对齐 32 位
   读写在单次总线访问内不撕裂，故不需开中断/进临界区。
   注意：本证明只在「一读一写」时成立，两个任务同时读或两个中断写就必须加锁。
   head/tail 只增不取模，used=head-tail 在 32 位无符号回绕下天然正确 —— 前提是
   size 为 2 的幂（取下标时才与掩码），该约束已在 AVE_tuning.h 用编译期检查拦住。 */


//基线：rb.buf → storage 数组，size=8，mask=7，head=0，tail=0
//第1次 Push 写 0x11：head=0，下标 0&7=0，rb.buf[0]=0x11，head=1
//storage：[0x11,_,_,_,_,_,_,_]
//第2次 Push 写 0x22：head=1，下标 1&7=1，rb.buf[1]=0x22，head=2
//storage：[0x11,0x22,_,_,_,_,_,_]


#ifndef AVE_RINGBUF_H
#define AVE_RINGBUF_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 环形缓冲控制块。存储区由调用方提供（静态数组，不从堆上分配）。 */
typedef struct
{
  uint8_t          *buf;    //存储区，由调用方提供，长度 >= size
  uint32_t          size;   //必须是 2 的幂
  uint32_t          mask;   //size - 1，取下标用
  volatile uint32_t head;   //写指针，只有生产者可以写
  volatile uint32_t tail;   //读指针，只有消费者可以写
} AVE_RingBuf;

/**
  * 函    数：初始化环形缓冲区
  * 参    数：rb 环形缓冲控制块；storage 存储区；size 容量，必须为 2 的幂
  * 返 回 值：0 成功；-1 表示 size 不是 2 的幂，属编程错误，调用方应立刻发现
  * 注意事项：size 为 2 的幂由调用方负责保证，配置项里有编译期检查。
  */
//初始化环形缓冲区
static inline int AVE_RbInit(AVE_RingBuf *rb, uint8_t *storage, uint32_t size)
{
  if ((rb == 0) || (storage == 0) || (size < 2U) || ((size & (size - 1U)) != 0U))
  {
    return -1;
  }
  rb->buf  = storage;
  rb->size = size;
  rb->mask = size - 1U;
  rb->head = 0U;
  rb->tail = 0U;
  return 0;
}

/**
  * 函    数：查询当前已存字节数
  * 参    数：rb 环形缓冲控制块
  * 返 回 值：head - tail，即已写入未读的字节数
  * 注意事项：只读，生产者与消费者都可安全调用。
  */
static inline uint32_t AVE_RbUsed(const AVE_RingBuf *rb)
{
  return (uint32_t)(rb->head - rb->tail);
}

/**
  * 函    数：查询剩余可写空间
  * 参    数：rb 环形缓冲控制块
  * 返 回 值：size - 已用字节数
  */
static inline uint32_t AVE_RbFree(const AVE_RingBuf *rb)
{
  return (uint32_t)(rb->size - (rb->head - rb->tail));
}

/**
  * 函    数：判断缓冲区是否为空
  * 参    数：rb 环形缓冲控制块
  * 返 回 值：1 空，0 非空
  */
static inline int AVE_RbIsEmpty(const AVE_RingBuf *rb)
{
  return (rb->head == rb->tail) ? 1 : 0;
}

/**
  * 函    数：生产者侧写入一个字节
  * 参    数：rb 环形缓冲控制块；b 待写入的字节
  * 返 回 值：0 成功；-1 缓冲已满，本字节被丢弃（调用方负责计数上报）
  * 注意事项：满时绝不覆盖最旧数据——否则正在读的那一行会被撕成半截，
  *           表现为偶尔收到一条错乱的行，比丢一个新字节难查得多。
  */
//Push 时先写 buf 数据，最后才更新 head
static inline int AVE_RbPush(AVE_RingBuf *rb, uint8_t b)
{
  uint32_t head = rb->head;

  if ((uint32_t)(head - rb->tail) >= rb->size)
  {
    return -1;                       /* 满 */
  }
  rb->buf[head & rb->mask] = b;
  rb->head = head + 1U;              /* 最后才更新 head —— 保证消费者看到 head 时数据已就位 */
  return 0;
}

/**
  * 函    数：消费者侧取一个字节
  * 参    数：rb 环形缓冲控制块；out 接收读出的字节
  * 返 回 值：0 成功；-1 缓冲为空
  * 注意事项：先读 buf 数据，最后才更新 tail。
  */
//Pop 时先读 buf 数据，最后才更新 tail
static inline int AVE_RbPop(AVE_RingBuf *rb, uint8_t *out)
{
  uint32_t tail = rb->tail;

  if (tail == rb->head)
  {
    return -1;                       /* 空 */
  }
  *out = rb->buf[tail & rb->mask];
  rb->tail = tail + 1U;
  return 0;
}

/**
  * 函    数：丢弃全部未读数据
  * 参    数：rb 环形缓冲控制块
  * 返 回 值：无
  * 注意事项：消费者侧逃生路径用，把 tail 直接推进到 head。
  */
static inline void AVE_RbDropAll(AVE_RingBuf *rb)
{
  rb->tail = rb->head;
}

/**
  * 函    数：从最旧数据起查找第一个等于 c 的字节
  * 参    数：rb 环形缓冲控制块；c 待查找的字节；offset 出参，命中时写它距 tail 的偏移
  * 返 回 值：0 找到；-1 未找到
  * 注意事项：本函数不消费、不移动 tail，确认要用后由调用方自己推进；
  *           这样调用方能在「找到了但放不下」时决定丢还是留。
  */
static inline int AVE_RbFind(const AVE_RingBuf *rb, uint8_t c, uint32_t *offset)
{
  uint32_t used = (uint32_t)(rb->head - rb->tail);
  uint32_t i;
  uint32_t tail = rb->tail;

  for (i = 0U; i < used; i++)
  {
    if (rb->buf[(tail + i) & rb->mask] == c)
    {
      *offset = i;
      return 0;
    }
  }
  return -1;
}

/**
  * 函    数：把最旧的 n 个字节拷出并消费掉
  * 参    数：rb 环形缓冲控制块；out 输出缓冲；n 拷贝字节数，必须 <= AVE_RbUsed
  * 返 回 值：无
  * 注意事项：语义上等价于 n 次 AVE_RbPop，但只更新一次 tail。
  */
static inline void AVE_RbRead(AVE_RingBuf *rb, uint8_t *out, uint32_t n)
{
  uint32_t tail = rb->tail;
  uint32_t i;

  for (i = 0U; i < n; i++)
  {
    out[i] = rb->buf[(tail + i) & rb->mask];
  }
  rb->tail = tail + n;
}

/**
  * 函    数：不拷贝、直接丢掉最旧的 n 个字节
  * 参    数：rb 环形缓冲控制块；n 丢弃字节数，必须 <= AVE_RbUsed
  * 返 回 值：无
  * 注意事项：数据放不下或不需要时只推进 tail 扔掉；
  *           绝不能写成「拷进调用方缓冲再不管」，那样会溢出调用方缓冲。
  */
static inline void AVE_RbSkip(AVE_RingBuf *rb, uint32_t n)
{
  rb->tail = rb->tail + n;
}

#ifdef __cplusplus
}
#endif

#endif /* AVE_RINGBUF_H */
