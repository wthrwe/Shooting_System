/* [JOG] 行宽实测 —— 宿主侧，不链接任何产品代码。
 *
 * 为什么单独做这个小程序：AVE_Log 的缓冲是 AVE_LOG_LINE_MAX = 100 字节，
 * 超了会被截断（尾部字段**静默消失**，而日志看起来是"正常的一行"）。
 * 本工程为"行宽算错"栽过一次（把 0/1 标志当成计数，见 CLAUDE.md 规则 3 的风格基准）。
 * 所以这里不估：把**真实的格式串**原样抄过来，用**最坏取值**跑一遍，数实际字节数。
 *
 * 🔴 用法局限（诚实边界）：它验的是"格式串 + 最坏取值下的宽度"，
 *    **不验** AVE_Log 内部的截断行为，也**不验**真机栈。
 *    真机上的 AVE_Log 会返回 cut 标志，但本行**没有读**它 —— 宽度超了不会有人告诉你。
 */
#include <stdio.h>
#include <string.h>

/* 与 AVE_tuning.h 保持一致。改那边就要改这里 —— 两处不一致时本程序给出的是假判据。 */
#define AVE_LOG_LINE_MAX 100

/* 两个待验配置：默认（关）不可能出现，但把"用户会填的那个"和"最坏"都数一遍 */
struct case_ { const char *name; long ax, n, hz, rc_g, rc_m, rc_c, f; int b; unsigned long sp, mv; };

int main(void)
{
    static const struct case_ cases[] = {
        /* 用户这次要用的：200 Hz / 20 步 / 水平轴。
           b=0 sp=19999999 mv=1 = "被接受、走完了"（走完就提前退出自旋，sp 剩很多） */
        { "本次配置：走完", 0, 20, 200, 1, 0, 1, 200, 0, 19999999UL, 1 },

        /* 同一次点动的另两种结局 —— 格式串必须能把它们和上面区分开 */
        { "被拒（门没开）", 0, 20, 200, 1, -3, 1, 0, 0, 20000000UL, 0 },
        { "没走完（等超时）", 0, 20, 200, 1, 0, 1, 200, 1, 0, 1 },

        /* 最坏取值：每个字段取各自最大宽度。不取这个的话，
           将来有人把 n 填成 2000000、或 rc 全是负的，就会截断而且没人知道。 */
        { "最坏取值", 1, -2000000L, 40000, -1, -4, -1, 40000, -1, 20000000UL, 4294967295UL },
    };

    int bad = 0;
    size_t i;

    printf("AVE_LOG_LINE_MAX = %d（正文上限 = %d）\n\n", AVE_LOG_LINE_MAX, AVE_LOG_LINE_MAX - 2);

    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        char body[512];
        int n;

        n = snprintf(body, sizeof(body),
            "[JOG] ax=%lu n=%ld hz=%lu rc=%d,%d,%d f=%ld b=%d sp=%lu mv=%lu",
            cases[i].ax, cases[i].n, cases[i].hz,
            (int)cases[i].rc_g, (int)cases[i].rc_m, (int)cases[i].rc_c,
            cases[i].f, cases[i].b, cases[i].sp, cases[i].mv);

        printf("%-22s 长度 %3d  %s\n", cases[i].name, n,
               (n < AVE_LOG_LINE_MAX - 2) ? "OK" : "**超宽**");
        printf("    |%s|\n", body);

        if (n >= AVE_LOG_LINE_MAX - 2) { bad++; }
    }

    printf("\n%s\n", bad ? "**有配置会截断，必须缩短格式串**" : "全部配置都在正文上限之内");
    return bad ? 1 : 0;
}
