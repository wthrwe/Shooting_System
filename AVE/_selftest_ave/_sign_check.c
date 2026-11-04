#include "AVE_tuning.h"
_Static_assert(AVE_VISION_YAW_SIGN == 1, "yaw sign must be +1 (第七十三次定案: hz 与 ex 同号)");
_Static_assert(AVE_VISION_PITCH_SIGN == 1, "pitch sign must be +1");
_Static_assert((AVE_VISION_YAW_SIGN == 1 || AVE_VISION_YAW_SIGN == -1), "sign must be unit");
int main(void){ return 0; }
