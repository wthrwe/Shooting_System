/* 替身。数值抄自真实 AVE_tuning.h（行号见下方注释），改动会破坏本测试的有效性。 */
#ifndef STUB_AVE_TUNING_H
#define STUB_AVE_TUNING_H
#define AVE_STEPPER_TIMEBASE_HZ     840000UL   /* 真值 AVE_tuning.h:165 */
#define AVE_STEPPER_MIN_HZ          1U         /* :171 */
#define AVE_STEPPER_MAX_HZ          40000U     /* :172 */
#define AVE_STEPPER_DIR_SETUP_US    10U        /* :177 */
#define AVE_STEPPER_MAX_MOVE_PULSES 2000000L   /* :182 */
#define AVE_PAN_POS_DIR_LEVEL       1          /* :47 */
#define AVE_TILT_POS_DIR_LEVEL      1
#endif
