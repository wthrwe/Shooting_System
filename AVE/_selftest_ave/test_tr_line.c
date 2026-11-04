/* [TR] 行格式自测：把 AVE_app.c 里那条 AVE_Log 的格式串与实参类型原样搬过来，
   用 -Wformat 编译 + 跑一遍，确认
     ① 类型不匹配会在这里编译报错（-Werror=format）；
     ② 典型长度和"跑几小时后"的最坏长度都 < AVE_LOG_LINE_MAX=100。
   第七十二次起字段变了：撤掉 r= 和 dir=，加入 age=。 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>

typedef struct { uint32_t range_rejects, gate_rejects, busy_rejects, dir_changes; } AVE_StepperStats;

static int render(char *buf, size_t cap,
                  unsigned long t, int nv, int ex, int ey,
                  long hz_yaw, long hz_pitch,
                  const AVE_StepperStats *stp,
                  int rc_yaw, int rc_pitch, unsigned long age)
{
  return snprintf(buf, cap,
      "[TR] t=%lu nv=%d ex=%d ey=%d hz=%ld,%ld rj=%lu,%lu,%lu rc=%d,%d dc=%lu age=%lu",
      t, nv, ex, ey, hz_yaw, hz_pitch,
      (unsigned long)stp->range_rejects, (unsigned long)stp->gate_rejects,
      (unsigned long)stp->busy_rejects,
      rc_yaw, rc_pitch, (unsigned long)stp->dir_changes, age);
}

int main(void)
{
  char buf[256];
  int  n;
  int  fail = 0;

  {   /* 典型：刚上电一分钟，靶子偏右 53 px，2 rpm 限速下 hz 最大 ±107 */
    AVE_StepperStats st = { 0u, 0u, 0u, 2u };
    n = render(buf, sizeof buf, 11000UL, 1, 53, -3, -107, 0, &st, 0, 0, 37UL);
    printf("%s\nlen=%d\n", buf, n);
    if (n >= 100) { printf("FAIL: 典型行超过 100\n"); fail = 1; }
    if (strncmp(buf, "[TR] t=11000 nv=1 ex=53 ey=-3 hz=-107,0", 38) != 0) {
      printf("FAIL: 字段顺序不符\n"); fail = 1;
    }
    if (strstr(buf, "age=37") == NULL) { printf("FAIL: age 字段缺失\n"); fail = 1; }
    if (strstr(buf, "dir=") != NULL)  { printf("FAIL: dir 应已撤掉\n"); fail = 1; }
    if (strstr(buf, " r=") != NULL)   { printf("FAIL: r 应已撤掉\n"); fail = 1; }
  }

  {   /* 最坏：跑一小时 + 拒收计数五位数 + 偏差三位 + 无目标长时 age。
        这一档必须仍 < 100，否则被截掉的就是 age —— 而 age 正是要看的字段。 */
    AVE_StepperStats st = { 99999u, 99999u, 99999u, 999u };
    n = render(buf, sizeof buf, 3600000UL, 0, -110, -110, -4000, -4000,
               &st, 0, 0, 999UL);
    printf("%s\nlen=%d\n", buf, n);
    if (n >= 100) { printf("FAIL: 最坏行超过 100，age 会被截掉\n"); fail = 1; }
  }

  printf("%s\n", fail ? "FAIL" : "PASS");
  return fail;
}
