/* 导读.md 第 1.9 节的那个最小验证程序 —— 用来核对我在文档里写的预期是不是真的 */
#include <stdio.h>
#include "AVE_ringbuf.h"

int main(void)
{
  AVE_RingBuf rb;
  uint8_t     storage[8];        /* size = 8，必须是 2 的幂 */
  int         i;
  uint8_t     b;

  if (AVE_RbInit(&rb, storage, 8) != 0) { printf("init 失败\n"); return 1; }

  /* 故意放 11 个字节 —— 超过容量 8，看它怎么绕 */
  for (i = 0; i < 11; i++)
  {
    int r = AVE_RbPush(&rb, (uint8_t)('A' + i));
    printf("push %c -> %-8s head=%lu tail=%lu used=%lu\n",
           'A' + i, (r == 0) ? "OK" : "满!丢弃",
           (unsigned long)rb.head, (unsigned long)rb.tail,
           (unsigned long)AVE_RbUsed(&rb));
  }

  /* 全部取出来，验证取到的还是 ABCDEFGH（先进先出） */
  printf("取出：");
  while (AVE_RbPop(&rb, &b) == 0) { printf("%c", b); }
  printf("\n");

  /*
   * 第二轮：上面那轮只能证明"满了会丢"，【证明不了绕回】——
   * 因为 used 到 8 就拒绝写入，head 永远涨不过 8。
   * 要看到 head & mask 真的绕回开头，必须【边放边取】，让 head 超过 size。
   */
  printf("\n=== 第二轮：边放边取，逼 head 超过 8 ===\n");
  /* 重置（而不是 DropAll）—— DropAll 只动 tail，head 会从 8 继续涨，
     数字太大不好数。重置后 head/tail 都归零，绕回看得清楚。 */
  (void)AVE_RbInit(&rb, storage, 8);

  for (i = 0; i < 5; i++) { (void)AVE_RbPush(&rb, (uint8_t)('a' + i)); }
  printf("放进 a~e 5 个： head=%lu tail=%lu used=%lu\n",
         (unsigned long)rb.head, (unsigned long)rb.tail, (unsigned long)AVE_RbUsed(&rb));

  for (i = 0; i < 3; i++) { (void)AVE_RbPop(&rb, &b); printf("  取走 %c\n", b); }
  printf("取走 3 个后：   head=%lu tail=%lu used=%lu\n",
         (unsigned long)rb.head, (unsigned long)rb.tail, (unsigned long)AVE_RbUsed(&rb));

  /* 再放 5 个。head 会从 5 涨到 10 —— 超过 size=8，绕回必然发生 */
  for (i = 0; i < 5; i++)
  {
    int r = AVE_RbPush(&rb, (uint8_t)('f' + i));
    printf("push %c -> %-4s head=%2lu  存进的数组下标 = (head-1) & 7 = %lu\n",
           'f' + i, (r == 0) ? "OK" : "满",
           (unsigned long)rb.head, (unsigned long)((rb.head - 1U) & rb.mask));
  }

  printf("最后：          head=%lu tail=%lu used=%lu  ← head 超过 8 了，但 used 正常\n",
         (unsigned long)rb.head, (unsigned long)rb.tail, (unsigned long)AVE_RbUsed(&rb));

  printf("取出：");
  while (AVE_RbPop(&rb, &b) == 0) { printf("%c", b); }
  printf("   ← 仍然是先进先出：d e f g h i j\n");

  return 0;
}
