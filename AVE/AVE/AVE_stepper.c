//STM32 定时器 PWM 驱动两路步进电机（云台 YAW 水平、PITCH 俯仰）的底层驱动库
#include "AVE_stepper.h"

/* 每轴配置表。const 放 flash，不占 RAM */
typedef struct
{
	TIM_HandleTypeDef *htim;			//AVE_HTIM_YAW_STEP 或 AVE_HTIM_PITCH_STEP
	uint32_t channel;					//TIM_CHANNEL_3 或 TIM_CHANNEL_4
	uint32_t cc_it;						//TIM_IT_CC3 或 TIM_IT_CC4，比较中断在 DIER 里的位
	uint32_t ccxe;						//TIM_CCER_CC3E 或 CC4E，通道输出使能位
	uint32_t ocpe;						//TIM_CCMR2_OC3PE 或 OC4PE，比较值预装载位
	uint32_t ocm_msk;					//OCxM[2:0] 掩码，自检用它对 PWM 模式 1
	uint32_t ocm_pos;					//OCxM 在 CCMR2 里的起始位
	uint32_t ccs_msk;					//CCxS[1:0] 掩码，应为 00（输出，不是输入捕获）
	uint32_t ccxp;						//CCxP 极性位，共阳接法应为 **1**（STM32 里 1 才是低有效）
	GPIO_TypeDef *dir_port;				//AVE_YAW_DIR_PORT 或 AVE_PITCH_DIR_PORT
	uint16_t dir_pin;					//AVE_YAW_DIR_PIN 或 AVE_PITCH_DIR_PIN
	uint8_t pos_dir_level;				//正方向对应的 Dir 电平
} AVE_StepperAxisCfg;

/* ⚠️ 本表只覆盖 CH3/CH4 —— 本工程两条轴恰好都在 CCMR2 里。
   哪天换成 CH1/CH2，必须把上面 ocpe / ocm_msk / ocm_pos / ccs_msk 四项的来源
   一起改成 CCMR1，否则自检会去读一个没人配过的寄存器、并一直报"配置不符"。 */
typedef char ave_stepper_cfg_table_is_ccmr2_only
[
	(AVE_STEP_YAW_CHANNEL == TIM_CHANNEL_4) && (AVE_STEP_PITCH_CHANNEL == TIM_CHANNEL_3) ? 1 : -1
];

/* 下标必须与 AVE_StepperAxis 的枚举顺序一一对应。
   用指名初始化而不是按位置排：这 12 列里有 5 列是相互只差几个字母的 uint32_t
   掩码（ocpe / ocm_msk / ocm_pos / ccs_msk / ccxp），排错顺序**不会有任何编译
   警告**，只会让自检从此对着一个错位读出来的值下结论。 */
static const AVE_StepperAxisCfg s_cfg[AVE_STEPPER_AXIS_NUM] =
{
	/* 水平轴 YAW：TIM5_CH4 → PA3，方向 PA5 */
	{
		.htim          = AVE_HTIM_YAW_STEP,
		.channel       = AVE_STEP_YAW_CHANNEL,
		.cc_it         = TIM_IT_CC4,
		.ccxe          = TIM_CCER_CC4E,
		.ocpe          = TIM_CCMR2_OC4PE,
		.ocm_msk       = TIM_CCMR2_OC4M,
		.ocm_pos       = TIM_CCMR2_OC4M_Pos,
		.ccs_msk       = TIM_CCMR2_CC4S,
		.ccxp          = TIM_CCER_CC4P,
		.dir_port      = AVE_YAW_DIR_PORT,
		.dir_pin       = AVE_YAW_DIR_PIN,
		.pos_dir_level = AVE_YAW_POS_DIR_LEVEL,
	},
	/* 俯仰轴 PITCH：TIM2_CH3 → PB10，方向 PA4 */
	{
		.htim          = AVE_HTIM_PITCH_STEP,
		.channel       = AVE_STEP_PITCH_CHANNEL,
		.cc_it         = TIM_IT_CC3,
		.ccxe          = TIM_CCER_CC3E,
		.ocpe          = TIM_CCMR2_OC3PE,
		.ocm_msk       = TIM_CCMR2_OC3M,
		.ocm_pos       = TIM_CCMR2_OC3M_Pos,
		.ccs_msk       = TIM_CCMR2_CC3S,
		.ccxp          = TIM_CCER_CC3P,
		.dir_port      = AVE_PITCH_DIR_PORT,
		.dir_pin       = AVE_PITCH_DIR_PIN,
		.pos_dir_level = AVE_PITCH_POS_DIR_LEVEL,
	}
};

/* 静态断言：表行数必须等于轴数。AVE_STEPPER_AXIS_NUM 是枚举值，不能用 #if
   ——预处理器不认识枚举，会把它一律当 0，护栏就变成"永远报错" */
typedef char ave_stepper_cfg_row_must_match_axis_num
[
	((sizeof(s_cfg) / sizeof(s_cfg[0])) == (unsigned)AVE_STEPPER_AXIS_NUM) ? 1 : -1
];

/* 每轴运行状态 */
typedef struct
{
	int32_t want_hz;				//最后一次被接受的请求频率，仅诊断用

	volatile int32_t cur_hz;		//实际输出频率，由分频比反算；停 = 0
	volatile int32_t remain;		//位置模式剩余脉冲数；速度模式恒 0
	volatile uint8_t pos_active;	//1 = 位置模式在跑，IsBusy 问的就是它
	volatile uint8_t running;		//1 = 计数器在跑（CEN = 1）

	uint8_t gate;					//1 = 软件门控开
	uint8_t dir_is_pos;				//Dir 引脚当前是否等于正方向电平
} AVE_StepperAxisState;

static AVE_StepperAxisState s_state[AVE_STEPPER_AXIS_NUM];

static volatile AVE_StepperStats s_stats;	//计数被中断改写，整个结构体声明 volatile
static uint8_t s_dwt_ok;					//1 = DWT 周期计数器可用，是自检过的

/**
  * 函    数：判断轴号是否合法
  * 参    数：axis 轴号
  * 返 回 值：1 合法，0 越界
  * 注意事项：所有对外函数的第一句都该是它。
  */
static int axis_valid(AVE_StepperAxis axis)
{
	return ((int)axis >= 0) && ((int)axis < (int)AVE_STEPPER_AXIS_NUM);
}

/**
  * 函    数：从 HAL 句柄反查轴号
  * 参    数：htim HAL 定时器句柄
  * 返 回 值：轴号；认不出来返回 -1
  */
static int axis_of_htim(const TIM_HandleTypeDef *htim)
{
	int i;

	if (htim == NULL) { return -1; }

	for (i = 0; i < (int)AVE_STEPPER_AXIS_NUM; i++)
	{
		if (s_cfg[i].htim->Instance == htim->Instance) { return i; }
	}
	return -1;
}

/**
  * 函    数：写某一路的比较寄存器 CCRx
  * 参    数：inst 定时器寄存器基址；channel 通道号；value 要写入的值
  * 返 回 值：无
  */
static void ccr_write(TIM_TypeDef *inst, uint32_t channel, uint32_t value)
{
	switch (channel)
	{
		case TIM_CHANNEL_1: inst->CCR1 = value; break;
		case TIM_CHANNEL_2: inst->CCR2 = value; break;
		case TIM_CHANNEL_3: inst->CCR3 = value; break;
		default:            inst->CCR4 = value; break;	//只剩 CH4，不需要再判
	}
}

/**
  * 函    数：把 Dir 引脚设成正方向或反方向电平
  * 参    数：axis 轴号；pos 1 = 正方向，0 = 反方向
  * 返 回 值：无
  */
static void dir_apply(AVE_StepperAxis axis, uint8_t pos)
{
	const AVE_StepperAxisCfg *cfg = &s_cfg[axis];
	uint8_t level;
	
	//正向 DIR 高电平：pos = 1 level = 1 dir = 高电平
	//正向 DIR 低电平：pos = 0 level = 0 dir = 低电平
	if (pos)
    {
        level = cfg->pos_dir_level;  
    }
    else
    {
        level = !cfg->pos_dir_level;
    }

	/* 直接写 BSRR：低 16 位置位、高 16 位复位，一次 32 位写是原子的（RM0090 §8.4.5） */
	if (level) { cfg->dir_port->BSRR = (uint32_t)cfg->dir_pin; }
	else       { cfg->dir_port->BSRR = (uint32_t)cfg->dir_pin << 16U; }
}

/**
  * 函    数：打开 DWT 周期计数器
  * 参    数：无
  * 返 回 值：无
  */
static void dwt_init(void)
{
	uint32_t a;
	uint32_t b;
	volatile uint32_t spin;

	CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;		//打开跟踪与调试模块的时钟
	DWT->CYCCNT = 0U;                                   // 清零计数
	DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;				// 启用周期计数

	a = DWT->CYCCNT;
	for (spin = 0U; spin < 64U; spin++) { }				//空转几十个周期
	b = DWT->CYCCNT;

	s_dwt_ok = (b != a) ? 1U : 0U;    //判断 b != a：计数器在增长 → DWT 正常，标记s_dwt_ok=1
}

/**
  * 函    数：忙等若干微秒，换向建立时间用
  * 参    数：us 要等的微秒数
  * 返 回 值：无
  */
static void stepper_delay_us(uint32_t us)
{
	//DWT 正常工作（主路径，高精度）
	if (s_dwt_ok)
	{
		uint32_t start  = DWT->CYCCNT;  //记录延时开始时刻的内核周期计数值
		uint32_t cycles = us * (SystemCoreClock / 1000000U);  //需要等待的 CPU 内核时钟周期
		while ((uint32_t)(DWT->CYCCNT - start) < cycles) { }	//忙等，故意不让出 CPU
	}
	//DWT 初始化失败（备用降级路径，低精度）
	else
	{
		volatile uint32_t spin = us * (SystemCoreClock / 4000000U);

		s_stats.dwt_fallbacks++;
		while (spin != 0U) { spin--; }
	}
}

/**
  * 函    数：把计数器停干净，并把输出钉在空闲高电平
  * 参    数：axis 轴号
  * 返 回 值：无
  */
static void pwm_stop(AVE_StepperAxis axis)
{
	const AVE_StepperAxisCfg *cfg  = &s_cfg[axis];
	AVE_StepperAxisState     *st   = &s_state[axis];
	TIM_TypeDef              *inst = cfg->htim->Instance;

	inst->DIER &= ~cfg->cc_it;		// 先关中断源，否则下面的 EGR 会产生新的中断请求
	inst->CR1 &= ~TIM_CR1_CEN;		// CNT 计数器立刻停止计数，保持当前计数值不动

	ccr_write(inst, cfg->channel, 0U); //把 捕获比较寄存器 CCRx 写入 0
	inst->EGR = TIM_EGR_UG;			//软件产生更新事件
	inst->SR = ~(TIM_SR_UIF | cfg->cc_it);

	st->running = 0U;  //标记该轴脉冲输出停止
	st->cur_hz  = 0;   //当前脉冲频率置 0
}

/**
  * 函    数：配置定时器，启动 PWM 脉冲输出，用来产生步进电机 STEP 脉冲
  * 参    数：axis 轴号；hz 频率；count_it 1 = 打开比较中断
  * 返 回 值：无
  */
static void pwm_run(AVE_StepperAxis axis, uint32_t hz, uint8_t count_it)
{
	const AVE_StepperAxisCfg *cfg  = &s_cfg[axis];
	AVE_StepperAxisState     *st   = &s_state[axis];
	TIM_TypeDef              *inst = cfg->htim->Instance;
	uint32_t                  div  = AVE_STEPPER_TIMEBASE_HZ / hz;
	uint32_t                  arr;
	uint32_t                  ccr;

	/* 除零护栏。正常路径进不来，但 MAX_HZ 是宏，万一被调到大于 TIMEBASE，挡住比崩溃强 */
	if (div == 0U) { return; }

	arr = div - 1U;					//f = TIMEBASE / (ARR + 1)
	ccr = div / 2U;					//占空比约 50%，低位周期允许差 1 个计数

	inst->CR1 &= ~TIM_CR1_CEN;      //先关闭定时器计数器

	inst->ARR = arr;
	ccr_write(inst, cfg->channel, ccr);
	inst->EGR = TIM_EGR_UG;         //软件触发更新事件
	inst->SR = ~(TIM_SR_UIF | cfg->cc_it);
 
	if (count_it) { inst->DIER |= cfg->cc_it; }  //开启捕获比较中断

	inst->CR1 |= TIM_CR1_CEN;      //开启定时器计数器
	st->running = 1U;
	{
		/* 记实际频率，不是请求频率：分频比是整数，一般除不尽。
		   例：请求 999 Hz → div = 840 → 实际 1000 Hz */
		int32_t sign = st->dir_is_pos ? 1 : -1;
		st->cur_hz = sign * (int32_t)(AVE_STEPPER_TIMEBASE_HZ / div);
	}
}

/**
  * 函    数：定时器正在运行的时候，在线修改 PWM 脉冲频率（动态调速），不需要停定时器
  * 参    数：axis 轴号；hz 新的频率
  * 返 回 值：无
  */
static void pwm_reprogram(AVE_StepperAxis axis, uint32_t hz)
{
	const AVE_StepperAxisCfg *cfg  = &s_cfg[axis];
	AVE_StepperAxisState     *st   = &s_state[axis];
	TIM_TypeDef              *inst = cfg->htim->Instance;
	uint32_t                  div  = AVE_STEPPER_TIMEBASE_HZ / hz;

	if (div == 0U) { return; }

	inst->ARR = div - 1U;
	ccr_write(inst, cfg->channel, div / 2U);

	{
		int32_t sign = st->dir_is_pos ? 1 : -1;  //电机转向
		st->cur_hz = sign * (int32_t)(AVE_STEPPER_TIMEBASE_HZ / div); //脉冲频率
	}
}

/**
  * 函    数：单轴步进电机硬件 + 状态初始化
  * 参    数：axis 轴号
  * 返 回 值：无
  */
static void init_one_axis(AVE_StepperAxis axis)
{
	const AVE_StepperAxisCfg *cfg  = &s_cfg[axis];
	AVE_StepperAxisState     *st   = &s_state[axis];
	TIM_TypeDef              *inst = cfg->htim->Instance;

	st->want_hz    = 0;   //期望目标脉冲频率
	st->cur_hz     = 0;   //当前实际脉冲频率
	st->remain     = 0;   //剩余脉冲数
	st->pos_active = 0U;  //方向标记
	st->running    = 0U;  //脉冲输出是否正在运行
	st->gate       = 0U;  //禁止输出脉冲

	inst->DIER &= ~cfg->cc_it;		//比较中断先关掉
	inst->CR1  &= ~TIM_CR1_CEN;		//计数器先停掉

	inst->CCMR2 |= cfg->ocpe;

	inst->CCER |= cfg->ccxe;		//通道输出使能，一直开着，理由见函数头

	ccr_write(inst, cfg->channel, 0U);
	inst->EGR = TIM_EGR_UG;			//让上面的 CCR=0 立刻生效，并把 CNT 归零
	inst->SR  = ~(TIM_SR_UIF | cfg->cc_it);

	/* 上电时把 Dir 摆在正方向电平上：电源接通那一瞬方向是确定的，
	   不会因为引脚复位值 0 与正方向不一致而先听到一声反向抖动 */
	dir_apply(axis, 1U);
	st->dir_is_pos = 1U;
}

/**
  * 函    数：步进模块初始化
  * 参    数：无
  * 返 回 值：无
  * 注意事项：在 MX_TIM2_Init() / MX_TIM5_Init() 之后、osKernelStart() 之前调用。
  */
void AVE_Stepper_Init(void)
{
	int i;

	//清零所有统计计数器
	s_stats.freq_updates  = 0U;
	s_stats.dir_changes   = 0U;
	s_stats.move_started  = 0U;
	s_stats.move_done     = 0U;
	s_stats.move_aborted  = 0U;
	s_stats.gate_rejects  = 0U;
	s_stats.range_rejects = 0U;
	s_stats.busy_rejects  = 0U;
	s_stats.stray_irq     = 0U;
	s_stats.dwt_fallbacks = 0U;

	for (i = 0; i < (int)AVE_STEPPER_AXIS_NUM; i++)
	{
		init_one_axis((AVE_StepperAxis)i);
	}

	dwt_init();
}

/**
  * 函    数：安全门控开关，是步进驱动的故障保护开关。用来允许 / 禁止该轴输出脉冲
  * 参    数：axis 轴号；on 1 = 允许出脉冲，0 = 禁止
  * 返 回 值：AVE_STEPPER_EOK 或 AVE_STEPPER_EBADARG
  * 注意事项：关闸必须立即停脉冲——只拦新指令的门控不叫门控。
  */
int AVE_Stepper_SetGate(AVE_StepperAxis axis, uint8_t on)
{
	if (!axis_valid(axis)) { return AVE_STEPPER_EBADARG; }  //校验抽号

	//关门，禁止脉冲
	if (on == 0U)
	{
		(void)AVE_Stepper_Stop(axis);
	}
	s_state[axis].gate = (on != 0U) ? 1U : 0U;  //更新门控状态

	return AVE_STEPPER_EOK;
}

/**
  * 函    数：读一条轴的门控状态
  * 参    数：axis 轴号
  * 返 回 值：1 门控开，0 门控关；轴号越界返回 AVE_STEPPER_EBADARG
  */
int AVE_Stepper_IsGateOpen(AVE_StepperAxis axis)
{
	if (!axis_valid(axis)) { return AVE_STEPPER_EBADARG; }
	return (int)s_state[axis].gate;
}

/**
  * 函    数：速度模式，设定一条轴的带符号步进频率
  * 参    数：axis 轴号；hz 带符号频率，范围：±[AVE_STEPPER_MIN_HZ, AVE_STEPPER_MAX_HZ]
  * 返 回 值：AVE_STEPPER_EOK；AVE_STEPPER_ERANGE；AVE_STEPPER_EGATE；AVE_STEPPER_EBUSY
  * 注意事项：换向由本函数内部处理（停脉冲→改 DIR→等建立→重启），调用方不用先
  *           Stop。顺序不能反：DIR 在脉冲还在跑的时候变，那一瞬间驱动器可能把
  *           最后半个脉冲当成反向的第一步，丢一步，且只在换向时偶发。
  */
int AVE_Stepper_SetFreqHz(AVE_StepperAxis axis, int32_t hz)
{
	AVE_StepperAxisState *st;
	int64_t               mag64;      
	uint32_t              mag;       //频率绝对值
	uint8_t               want_pos;  //目标方向：1=正向 0=反向

	if (!axis_valid(axis)) { return AVE_STEPPER_EBADARG; }
	st = &s_state[axis];

	if (hz == 0) { return AVE_Stepper_Stop(axis); }		//0 = 停，不再往下走

	if (st->pos_active) { s_stats.busy_rejects++;  return AVE_STEPPER_EBUSY; }
	if (!st->gate)      { s_stats.gate_rejects++;  return AVE_STEPPER_EGATE; }

	//判断频率是否在合法区间
	mag64 = (hz > 0) ? (int64_t)hz : -(int64_t)hz;
	if ((mag64 < (int64_t)AVE_STEPPER_MIN_HZ) || (mag64 > (int64_t)AVE_STEPPER_MAX_HZ))
	{
		s_stats.range_rejects++;
		return AVE_STEPPER_ERANGE;			//越界时原频率原样不动，不会先停一下再报错
	}
	mag      = (uint32_t)mag64;
	want_pos = (hz > 0) ? 1U : 0U;  //DIR输出电平方向

	// 1.电机处于停止状态
	if (!st->running)
	{
		/* 从静止起：先摆方向、等建立时间、再出脉冲 */
		if (st->dir_is_pos != want_pos)
		{
			dir_apply(axis, want_pos);
			st->dir_is_pos = want_pos;
			s_stats.dir_changes++;    //方向切换次数
			stepper_delay_us(AVE_STEPPER_DIR_SETUP_US);
		}
		pwm_run(axis, mag, 0U);				//速度模式不开定时器比较中断
	}
	// 2.电机正在转动，并且需要换向
	else if (st->dir_is_pos != want_pos)
	{
		/* 换向：停脉冲 → 改 DIR → 等建立 → 重启 */
		pwm_stop(axis);
		dir_apply(axis, want_pos);
		st->dir_is_pos = want_pos;
		s_stats.dir_changes++;
		stepper_delay_us(AVE_STEPPER_DIR_SETUP_US);
		pwm_run(axis, mag, 0U);
	}
	// 3.电机正在转动，不需要换向，只需修改速度
	else
	{
		/* 同向改频：走无缝换档，绝不重启计数器 */
		pwm_reprogram(axis, mag);
	}

	st->want_hz = hz;
	s_stats.freq_updates++;

	return AVE_STEPPER_EOK;
}

/**
  * 函    数：读一条轴的实际输出频率
  * 参    数：axis 轴号
  * 返 回 值：带符号实际频率，停下时为 0；轴号越界返回 0
  */
int32_t AVE_Stepper_GetFreqHz(AVE_StepperAxis axis)
{
	if (!axis_valid(axis)) { return 0; }
	return s_state[axis].cur_hz;
}

/**
  * 函    数：读一条轴定时器当前的 CNT 计数值
  * 参    数：axis 轴号
  * 返 回 值：TIMx->CNT 原值；轴号越界返回 0
  */
uint32_t AVE_Stepper_GetCounter(AVE_StepperAxis axis)
{
	if (!axis_valid(axis)) { return 0U; }
	return s_cfg[axis].htim->Instance->CNT;
}

/**
  * 函    数：位置模式接口，让电机走指定脉冲步数（定长运动）
  * 参    数：axis 轴号；pulses 带符号脉冲数；hz 走这一趟的频率，恒为正
  * 返 回 值：AVE_STEPPER_EOK 已启动；其余同 SetFreqHz
  * 注意事项：一次只允许一个动作在跑，正在走时返回 EBUSY 而不是覆盖目标，
  *           覆盖会让"已经走了多少"变得不可知。
  */
int AVE_Stepper_MovePulses(AVE_StepperAxis axis, int32_t pulses, uint32_t hz)
{
	AVE_StepperAxisState *st;
	int64_t               mag64;
	uint8_t               want_pos;
	uint8_t               need_setup_delay = 0U;

	if (!axis_valid(axis)) { return AVE_STEPPER_EBADARG; }
	st = &s_state[axis];

	if (pulses == 0) { return AVE_STEPPER_EOK; }		//空动作，合法

	if (st->pos_active) { s_stats.busy_rejects++; return AVE_STEPPER_EBUSY; }
	if (!st->gate)      { s_stats.gate_rejects++; return AVE_STEPPER_EGATE; }

	mag64 = (pulses > 0) ? (int64_t)pulses : -(int64_t)pulses;
	if (mag64 > (int64_t)AVE_STEPPER_MAX_MOVE_PULSES)
	{
		s_stats.range_rejects++;
		return AVE_STEPPER_ERANGE;
	}
	if ((hz < AVE_STEPPER_MIN_HZ) || (hz > AVE_STEPPER_MAX_HZ))
	{
		s_stats.range_rejects++;
		return AVE_STEPPER_ERANGE;
	}

	want_pos = (pulses > 0) ? 1U : 0U;

	/* 若正在速度模式跑着，先停干净——位置模式需要从确定的相位起算 */
	if (st->running) { pwm_stop(axis); }

	if (st->dir_is_pos != want_pos)
	{
		dir_apply(axis, want_pos);
		st->dir_is_pos    = want_pos;
		need_setup_delay  = 1U;
		s_stats.dir_changes++;
	}
	if (need_setup_delay) { stepper_delay_us(AVE_STEPPER_DIR_SETUP_US); }

	/* 先写 remain 和 pos_active，再启动计数器。反过来的话，第一个比较中断
	   可能落在"remain 还是 0"的窗口里，中断里看到 remain <= 0 会立刻停脉冲，
	   结果一步都不走 */
	st->remain     = (int32_t)mag64;
	st->pos_active = 1U;

	pwm_run(axis, hz, 1U);				//这次开比较中断

	s_stats.move_started++;

	return AVE_STEPPER_EOK;
}

/**
  * 函    数：查询轴是否正在执行位置模式定位动作
  * 参    数：axis 轴号
  * 返 回 值：1 正在走，0 空闲；轴号越界返回 AVE_STEPPER_EBADARG
  */
int AVE_Stepper_IsBusy(AVE_StepperAxis axis)
{
	if (!axis_valid(axis)) { return AVE_STEPPER_EBADARG; }
	return (int)s_state[axis].pos_active;
}

/**
  * 函    数：立即停止一条轴
  * 参    数：axis 轴号
  * 返 回 值：AVE_STEPPER_EOK 或 AVE_STEPPER_EBADARG
  */
int AVE_Stepper_Stop(AVE_StepperAxis axis)
{
	AVE_StepperAxisState *st;

	if (!axis_valid(axis)) { return AVE_STEPPER_EBADARG; }
	st = &s_state[axis];

	/* 先记"被打断"，再清标志——顺序反了的话 move_aborted 永远不涨 */
	if (st->pos_active)
	{
		st->pos_active = 0U;
		s_stats.move_aborted++;
	}

	if (st->running) { pwm_stop(axis); }

	st->remain  = 0;
	st->want_hz = 0;

	return AVE_STEPPER_EOK;
}

/**
  * 函    数：HAL 定时器 PWM 回调，全工程唯一一份定义
  * 参    数：htim HAL 定时器句柄
  * 返 回 值：无
  * 注意事项：HAL_TIM_IRQHandler 会把 CCx 事件分发到这里（构建产物
  *           MDK-ARM/AVE/AVE.map:176 有这条引用），而 AVE.map:3553 显示它
  *           当前解析到 HAL 自带的空弱函数，所以这里定义它不会重复符号。
  */
void HAL_TIM_PWM_PulseFinishedCallback(TIM_HandleTypeDef *htim)
{
	AVE_Stepper_PulseFinishedIsr(htim);
}

/**
  * 函    数：比较中断服务例程
  * 参    数：htim HAL 定时器句柄
  * 返 回 值：无
  * 注意事项：① 数脉冲必须用比较中断，不能用更新中断。更新事件那一刻输出刚被
  *              拉低，而第 k 步的上升沿早就发生过了，此刻关通道会凭空多出一个
  *              上升沿，位置永远多走一步（0.11°），且不报任何错。
  *              比较中断正好落在上升沿那一刻，此时输出已经是高电平，关掉通道
  *              不产生任何边沿，恰好 N 步。
  *           ② 速度模式没有开比较中断，所以走进 pos_active == 0 的分支说明有
  *              东西不对（残留标志，或别的代码动过 DIER）。只记账，不擅自停脉冲
  *              ——在速度模式下把电机停掉，比多走几步危险得多。
  */
void AVE_Stepper_PulseFinishedIsr(TIM_HandleTypeDef *htim)
{
	int                       a = axis_of_htim(htim);
	const AVE_StepperAxisCfg *cfg;
	AVE_StepperAxisState     *st;

	if (a < 0) { s_stats.stray_irq++; return; }		//异常中断
	cfg = &s_cfg[a];
	st  = &s_state[a];

	//手动清除中断标志位 SR
	cfg->htim->Instance->SR = ~cfg->cc_it;

	/* 一个 STEP 上升沿 = 一次本中断 ⇒ 就在这里计数。**故意放在 pos_active 判断之前**：
	   位置动作走完的那一拍会先被 pwm_stop 清掉 pos_active，计数若挪到判断后面，
	   最后那一拍就漏了。漏计是危险方向（会让人以为“一个脉冲都没发”），多计不是。 */
	s_stats.pulses_total[a]++;

	//不在位置模式
	if (!st->pos_active)
	{
		s_stats.stray_irq++;  //不擅自停电机
		return;
	}

	// 每一个 STEP 脉冲边沿进入中断，剩余步数减 1
	if (st->remain > 0) { st->remain--; }

	//目标步数走完
	if (st->remain <= 0)
	{
		pwm_stop((AVE_StepperAxis)a);
		st->pos_active = 0U;
		s_stats.move_done++;
	}
}

/**
  * 函    数：取诊断计数快照
  * 参    数：无
  * 返 回 值：指向内部存储的只读指针
  */
const AVE_StepperStats *AVE_Stepper_Stats(void)
{
	return (const AVE_StepperStats *)&s_stats;
}

/* ============================================================================
 * 定时器配置自检
 * ==========================================================================*/

/* OCxM[2:0] = 110b = PWM 模式 1。写死这个数是因为 HAL 的 TIM_OCMODE_PWM1 是
   0x60（已经按 CH1 的 bit 6:4 摆好位置），拿它去比 CH4 反而要移位，更容易错 */
#define AVE_STEPPER_OCM_PWM1	(0x6U)

/**
  * 函    数：算 TIM2 / TIM5 的输入时钟（两条轴都在 APB1 上）
  * 参    数：无
  * 返 回 值：Hz
  * 注意事项：① APB 分频不为 1 时，定时器时钟是 PCLK 的 **2 倍**（RM0090 §6.2 时钟树）。
  *              PPRE1 = 0xx 表示不分频，1xx ~ 4xx 依次是 /2 /4 /8 /16，所以判据是
  *              "字段值 < 4"而不是"字段值 != 0"。
  *           ② 刻意不写死 84 兆。写死了就发现不了"PLL 没起来、SystemCoreClock 停在
  *              HSI 16 兆"那一档降级运行——那种情况下所有速度按同一个比例错，
  *              现象和"PID 没调好"一模一样，而 [HB] 的 bf 字段又只在启动失败时非 0。
  */
static uint32_t tim_input_clk_hz(void)
{
	uint32_t ppre1 = (RCC->CFGR & RCC_CFGR_PPRE1) >> RCC_CFGR_PPRE1_Pos;
	uint32_t pclk1 = HAL_RCC_GetPCLK1Freq();

	return (ppre1 < 4U) ? pclk1 : (pclk1 * 2U);
}

/**
  * 函    数：把定时器配置读回来与模块的假设逐条对账
  * 参    数：out 结果快照，不许传 NULL
  * 返 回 值：AVE_STEPPER_EOK / AVE_STEPPER_ECONFIG / AVE_STEPPER_EBADARG
  * 注意事项：① 全程只读，不写任何寄存器，所以随时可调、可重复调；
  *           ② 每一项都是"改错了也不会报错"的那一类，所以这里不做任何自动修正
  *              ——自动改寄存器会把"CubeMX 配置漂了"这个事实盖掉，而那正是要看的。
  */
int AVE_Stepper_SelfTest(AVE_StepperSelfTest *out)
{
	uint32_t clk;
	uint32_t all = 0U;
	int      i;

	if (out == NULL) { return AVE_STEPPER_EBADARG; }

	clk = tim_input_clk_hz();

	/* 应然 PSC = 定时器时钟 / 时间基准 − 1。
	   ⚠️ 除不尽时这里会**向下取整**，报出来的是"最接近的整数分频"，不是精确值。
	   当前 84 兆 / 840 千 = 100 整除，所以没有余数；改 TIMEBASE 时要注意这条。 */
	out->timer_clk_hz = clk;
	out->timebase_hz  = (uint32_t)AVE_STEPPER_TIMEBASE_HZ;	//自报假设，免得调用方另引一遍宏、两处漂开

	/* 除零/下溢护栏：clk < TIMEBASE 时 `clk / TIMEBASE` 是 0，减 1 会绕成 0xFFFFFFFF。
	   F407 上 SYSCLK 最低是 HSI 16 MHz、APB1 最多 /16 ⇒ 定时器时钟最低 1 MHz > 840 kHz，
	   正常路径到不了这里；真到了（比如 SystemCoreClock 被改坏）宁可报 0 —— 反正与
	   实际 PSC 不符，CHK_PSC 照样会亮，而报一个天文数字只会让人以为是自己看错了。 */
	out->expected_psc = (clk < (uint32_t)AVE_STEPPER_TIMEBASE_HZ)
	                    ? 0U
	                    : ((clk / (uint32_t)AVE_STEPPER_TIMEBASE_HZ) - 1U);

	for (i = 0; i < (int)AVE_STEPPER_AXIS_NUM; i++)
	{
		const AVE_StepperAxisCfg *cfg  = &s_cfg[i];
		TIM_TypeDef              *inst = cfg->htim->Instance;
		uint32_t                  ccmr = (uint32_t)inst->CCMR2;
		uint32_t                  ccer = (uint32_t)inst->CCER;
		uint32_t                  bad  = 0U;

		/* CR1 的两条：预装载、边沿对齐 */
		if ((inst->CR1 & TIM_CR1_ARPE) == 0U) { bad |= AVE_STEPPER_CHK_ARPE; }
		if ((inst->CR1 & TIM_CR1_CMS)  != 0U) { bad |= AVE_STEPPER_CHK_CMS; }

		/* CCMR2 的三条：波形模式、预装载、通道方向 */
		if (((ccmr & cfg->ocm_msk) >> cfg->ocm_pos) != AVE_STEPPER_OCM_PWM1)
		{
			bad |= AVE_STEPPER_CHK_OCMODE;
		}
		if ((ccmr & cfg->ocpe)    == 0U) { bad |= AVE_STEPPER_CHK_OCPE; }
		if ((ccmr & cfg->ccs_msk) != 0U) { bad |= AVE_STEPPER_CHK_CCS;  }

		/* CCER 的两条：极性（要 1 = 低有效）、输出使能（要 1）
		 * 🔴 这里原本写的是「要 0」——**错的**，且与 CubeMX 正面对撞（第二十八次上板才暴露，
		 *    板上一行 `[TIM]` 报 `bad=10,10`，两轴同一位）。
		 * 实测依据：SWD 直读 `TIM2 CCER=0x0300` / `TIM5 CCER=0x3000`，两轴 `CCxP` 都是 **1**；
		 *    而 `Core/Src/tim.c` 里唯一能置这两位的就是 `OCPolarity = TIM_OCPOLARITY_LOW`
		 *    ⇒「LOW 极性 ⇒ CCxP=1」是**实测定论**，不是照手册抄的。
		 * 反过来：配成 0 才会让共阳光耦常亮（PWM1 + CCR=0 时引脚恒 LOW ⇒ LED 一直通）。 */
		if ((ccer & cfg->ccxp) == 0U) { bad |= AVE_STEPPER_CHK_POL; }
		if ((ccer & cfg->ccxe) == 0U) { bad |= AVE_STEPPER_CHK_CCE; }

		/* 分频：与实测时钟算出的应然值比，不和 CubeMX 里那个字面量 99 比 */
		if ((uint32_t)inst->PSC != out->expected_psc) { bad |= AVE_STEPPER_CHK_PSC; }

		out->psc[i]   = (uint32_t)inst->PSC;
		out->ccmr2[i] = ccmr;
		out->ccer[i]  = ccer;
		out->bad[i]   = bad;
		all          |= bad;
	}

	return (all == 0U) ? AVE_STEPPER_EOK : AVE_STEPPER_ECONFIG;
}
