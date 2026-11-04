/**
 * 事件轨迹的实现 —— 见 host_trace.h 的说明。
 *
 * 刻意做得极简：一个固定数组 + 一个下标，不分配、不回收。
 * 它的使用者是**已经出问题的那次运行**，所以它自己不能有失败路径。
 *
 * 临时验证脚手架，不进版本库、不进 Keil 工程。
 */

#include "host_trace.h"
#include <stdio.h>

static char s_tag[HOST_TRACE_MAX][HOST_TRACE_TAG_MAX];
static int  s_n;
static int  s_dropped;

void host_trace(const char *tag)
{
  int i;

  if (s_n >= HOST_TRACE_MAX)
  {
    s_dropped++;
    return;
  }
  if (tag == NULL)
  {
    tag = "?";
  }

  for (i = 0; (i < (HOST_TRACE_TAG_MAX - 1)) && (tag[i] != '\0'); i++)
  {
    s_tag[s_n][i] = tag[i];
  }
  s_tag[s_n][i] = '\0';
  s_n++;
}

void host_trace_ms(const char *tag, uint32_t ms)
{
  char buf[HOST_TRACE_TAG_MAX];

  if (tag == NULL)
  {
    tag = "?";
  }
  (void)snprintf(buf, sizeof buf, "%s:%u", tag, (unsigned)ms);
  host_trace(buf);
}

int host_trace_count(void)   { return s_n; }
int host_trace_dropped(void) { return s_dropped; }

const char *host_trace_at(int i)
{
  if ((i < 0) || (i >= s_n))
  {
    return NULL;
  }
  return s_tag[i];
}

void host_trace_reset(void)
{
  s_n       = 0;
  s_dropped = 0;
}

int host_trace_join(int from, int to, char *out, int cap)
{
  int i;
  int n = 0;

  if ((out == NULL) || (cap <= 0))
  {
    return 0;
  }
  out[0] = '\0';

  if (from < 0)
  {
    from = 0;
  }
  if (to > s_n)
  {
    to = s_n;
  }

  for (i = from; i < to; i++)
  {
    const char *t = s_tag[i];
    int j;

    if (n > 0)
    {
      if (n >= (cap - 1)) { break; }
      out[n++] = ',';
    }
    for (j = 0; (t[j] != '\0') && (n < (cap - 1)); j++)
    {
      out[n++] = t[j];
    }
  }
  out[n] = '\0';
  return n;
}
