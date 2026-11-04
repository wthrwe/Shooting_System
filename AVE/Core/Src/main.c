/* USER CODE BEGIN Header */
/**
  **********íñíñíñíñíñíñíñíñíñíñíñíñíñíñíñíñíñíñíñíñíñíñíñíñíñíñíñíñíñíñíñíñíñíñíñíñíñíñíñíñíñíñíñíñíñíñíñíñíñíñíñíñíñíñíñíñíñíñíñíñíñíñíñ********************************************************************
  * @file           : main.c
  * @brief          : Main program body
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "cmsis_os.h"
#include "tim.h"
#include "usart.h"
#include "gpio.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "AVE_app.h"
/* Error_Handler() Φªüσ╛Çµ¥┐τ║ºΦ«░µòàΘÜ£τáü∩╝îµëÇΣ╗ÑΦ┐ÖΘçîµÿ╛σ╝ÅσîàσÉ½µ¥┐τ║ºσñ┤µûçΣ╗╢πÇé
   Σ╕ìΘ¥á AVE_app.h Θù┤µÄÑσ╕ªΦ┐¢µ¥Ñ ΓÇöΓÇö Θéúµÿ»"τó░σ╖ºΦâ╜τ╝ûΦ»æ"∩╝îσô¬σñ⌐ AVE_app.h
   Σ╕ìσåìσîàσÉ½ AVE_board.h∩╝îµ£¼µûçΣ╗╢σ░▒Σ╝ÜΣ╗Ñ"µ£¬σú░µÿÄµáçΦ»åτ¼ª"τÜäσ╜óσ╝ÅµèÑΘöÖ∩╝î
   ΦÇîµèÑΘöÖτÜäΣ╜ìτ╜«τª╗τ£ƒµ¡úτÜäσÄƒσ¢áΘÜöτ¥ÇΣ╕ñσ▒éπÇé */
#include "AVE_board.h"
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

/* USER CODE BEGIN PV */

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
void MX_FREERTOS_Init(void);
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */
/* µùºτÜä M1.3 UartDev µ╡ïΦ»òΣ╗úτáüσ╖▓Σ║Ä 2026-09-25 µ╕àΘÖñ∩╝êAPP/ τ¢«σ╜òσ╖▓σêáΘÖñ∩╝îΦ»ÑσÑù API Σ╕ìσñìσ¡ÿσ£¿∩╝ëπÇé
   σ║öτö¿Σ╗úτáüΣ╕Çσ╛ïσåÖσê░ Core/Src/AVE/ Σ╕ï∩╝îΣ╕ìΦªüσåÖσ£¿µ£¼µûçΣ╗╢ΘçîπÇé
   M1.5 τÜä AVE_App_Init() Φ░âτö¿µö╛σ£¿Σ╕ïµû╣τÜäπÇîτö¿µê╖Σ╗úτáüσ¥ù 2πÇìσåàπÇé
   ΓÜá∩╕Å µ£¼µûçΣ╗╢ΘçîτÜäΣ┐¥µèñµáçΦ«░σ┐àΘí╗µêÉσ»╣Σ╕öσö»Σ╕Ç ΓÇöΓÇö Φ┐₧µ│¿ΘçèΘçîΘâ╜Σ╕ìΦªüσç║τÄ░µáçΦ«░σÄƒµûç∩╝î
      σÉªσêÖ CubeMX Θçìµû░τöƒµêÉµù╢σÅ»Φâ╜Φºúµ₧ÉΘöÖΣ╣▒πÇé */
/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_USART1_UART_Init();
  MX_USART3_UART_Init();
  MX_TIM2_Init();
  MX_TIM5_Init();
  /* USER CODE BEGIN 2 */
  /* σ║öτö¿σ▒éσàÑσÅú ΓÇöΓÇö σÅ¬σüÜτ║»σñûΦ«╛σê¥σºïσîû: µ¥┐τ║º(µ┐Çσàëτ╜«σà│) + Σ╕ëΦ╖»Σ╕▓σÅúσ╝Çσºïµö╢πÇé
     ≡ƒö┤ Φ┐ÖΘçî**Σ╕ìΦâ╜**σê¢σ╗║ RTOS σ»╣Φ▒í: Σ╕ïΘ¥óτÜä osKernelInitialize() Φ┐ÿµ▓íµëºΦíî,
        σåàµá╕σ»╣Φ▒íΦí¿Φ┐ÿΣ╕ìσ¡ÿσ£¿πÇéΣ╗╗σèíΣ╕ÄΣ║ÆµûÑΘçÅσ£¿ freertos.c Θçîσê¢σ╗║πÇé
     Θí║σ║ÅΣ╣ƒΣ╕ìΦâ╜Φ░â: Σ╕▓σÅúσ┐àΘí╗σ£¿Φ┐ÖΘçîσ░▒σ╝Çσºïµö╢, σÉªσêÖΣ╗Ä"ΦºåΦºëµ¿íσ¥ùΣ╕èτö╡"σê░
        "Σ╗╗σèíσ╝Çσºïµ╢êΦ┤╣"Σ╣ïΘù┤τÜäµò░µì«Σ╝Üµ░╕Σ╣àΣ╕óσñ▒πÇé */
  AVE_App_Init();
  /* USER CODE END 2 */

  /* Init scheduler */
  osKernelInitialize();  /* Call init function for freertos objects (in cmsis_os2.c) */
  MX_FREERTOS_Init();

  /* Start scheduler */
  osKernelStart();

  /* We should never get here as control is now taken by the scheduler */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
  }
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Configure the main internal regulator output voltage
  */
  __HAL_RCC_PWR_CLK_ENABLE();
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSI;
  RCC_OscInitStruct.PLL.PLLM = 8;
  RCC_OscInitStruct.PLL.PLLN = 168;
  RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV2;
  RCC_OscInitStruct.PLL.PLLQ = 4;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV4;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV2;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_5) != HAL_OK)
  {
    Error_Handler();
  }
}

/* USER CODE BEGIN 4 */

/* USER CODE END 4 */

/**
  * @brief  Period elapsed callback in non blocking mode
  * @note   This function is called  when TIM7 interrupt took place, inside
  * HAL_TIM_IRQHandler(). It makes a direct call to HAL_IncTick() to increment
  * a global variable "uwTick" used as application time base.
  * @param  htim : TIM handle
  * @retval None
  */
void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
  /* USER CODE BEGIN Callback 0 */

  /* USER CODE END Callback 0 */
  if (htim->Instance == TIM7)
  {
    HAL_IncTick();
  }
  /* USER CODE BEGIN Callback 1 */

  /* USER CODE END Callback 1 */
}

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /*
   * ≡ƒö┤ **τ╗¥Σ╕ìσ£¿Φ┐ÖΘçîµ¡╗σ╛¬τÄ»πÇé**∩╝ê2026-09-26 Σ┐«Φ«ó∩╝îσÄƒσ«₧τÄ░µÿ» `__disable_irq(); while(1){}`∩╝ë
   *
   * ΓöÇΓöÇ σÄƒµ¥ÑτÜäσåÖµ│òµ£ëΣ╗ÇΣ╣êΘù«Θóÿ ΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇ
   *   Σ╕ìµÿ»"σü£µ£║"µ£¼Φ║½∩╝îΦÇîµÿ»"σü£µ£║**Σ╕öΣ╕ìτòÖΣ╗╗Σ╜òτùòΦ┐╣**"∩╝Ü
   *   Σ╕▓σÅúΣ╕ÇΦíîµ▓íµ£ëπÇüσ┐âΦ╖│Σ╕ìµëôπÇüτü»Σ╕ìΘù¬∩╝îΦ░âΦ»òσÖ¿µîéΣ╕èσÄ╗τ£ïσê░τÜäµÿ»σü£σ£¿ `while(1)` Θçî ΓÇöΓÇö
   *   σÆî"τ¿ïσ║Åσìíµ¡╗""Σ╛¢τö╡Σ╕ìΦ╢│""Σ╕▓σÅúτ║┐µÄÑσÅìΣ║å"**τ£ïΦ╡╖µ¥Ñσ«îσà¿Σ╕Çµá╖**πÇé
   *   µ£¼σ╖Ñτ¿ïΣ╕║Φ┐ÖΣ╕¬τÄ░Φ▒íµ╡¬Φ┤╣Φ┐çΣ╕Çµò┤Φ╜«µÄÆΘÜ£
   *   ∩╝êΦºü verify.md **τ¼¼σìüΣ║îµ¼í┬╖τ¼¼σ¢¢Φèé**∩╝îµö╣σè¿µ£¼Φ║½Φºü**τ¼¼σìüΣ╕ëµ¼í**∩╝ëπÇé
   *
   * ΓöÇΓöÇ µö╣µêÉΣ╗ÇΣ╣ê ΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇ
   *   Φ«░Σ╕ïΣ╕ÇΣ╕¬µòàΘÜ£τáüτä╢σÉÄ**Φ┐öσ¢₧**∩╝îΦ«⌐ `main()` τ╗ºτ╗¡σ╛ÇΣ╕ïΦ╡░∩╝îΦ┐¢σàÑΘÖìτ║ºΦ┐ÉΦíî∩╝Ü
   *
   *     SystemClock_Config() Φ┐öσ¢₧ ΓåÆ MX_GPIO_Init() ΓåÆ MX_USART*_UART_Init()
   *       ΓåÆ AVE_App_Init() ΓåÆ osKernelInitialize() ΓåÆ osKernelStart()
   *
   *   Σ║Äµÿ»Φ░âσ║ªσÖ¿τàºσ╕╕Φ╡╖µ¥Ñ∩╝îσ┐âΦ╖│τàºσ╕╕µ»Å 2 τºÆσÉÉΣ╕ÇΦíî∩╝îΦÇîΘéúΣ╕ÇΦíîΘçîτÜä `bf=1` σ░▒µÿ»
   *   **Φ┐ÖσÅ░µ£║σÖ¿Σ╕èτö╡µù╢σñ▒Φ┤ÑΦ┐çτÜäΦ»üµì«**πÇéµ£ëµò░σ¡ùσÅ»τ£ï∩╝îµëìµ£ëσ╛ùµƒÑπÇé
   *
   * ΓöÇΓöÇ Σ╕║Σ╗ÇΣ╣êΘÖìτ║ºΣ╣ïσÉÄΣ╕▓σÅúΦ┐ÿΦâ╜τö¿∩╝êΦ┐Öµÿ»µ£¼µö╣µ│òΦâ╜µêÉτ½ïτÜäσëìµÅÉ∩╝ëΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇ
   *   HAL τ«ùµ│óτë╣τÄçτö¿τÜäµÿ» `HAL_RCC_GetPCLK2Freq()`∩╝îσ«âΦ»╗τÜäµÿ»**σ«₧ΘÖà**τÜä
   *   `SystemCoreClock` σÅÿΘçÅΣ╕Ä RCC σêåΘóæσ»äσ¡ÿσÖ¿∩╝îΦÇîΣ╕ìµÿ»"µêæΣ╗¼µëôτ«ùΘàìµêÉ 168 σàå"Φ┐ÖΣ╕¬µäÅσ¢╛πÇé
   *   PLL µ▓íΦ╡╖µ¥Ñµù╢ SYSCLK σü£σ£¿ HSI 16 σàå∩╝î`SystemCoreClock` Σ╣ƒΦ┐ÿµÿ» 16 σàå
   *   ∩╝ê`system_stm32f4xx.c:137` τÜäσê¥σÇ╝∩╝îΣ╕öσñ▒Φ┤ÑτÜäΦ╖»σ╛äΣ╕ìΣ╝ÜσÄ╗µö╣σ«â∩╝ë∩╝î
   *   Σ╕ñΦÇàΣ╕ÇΦç┤ ΓåÆ **τ«ùσç║µ¥ÑτÜäµ│óτë╣τÄçµÿ»σ»╣τÜä** ΓåÆ Σ╕▓σÅúτàºµá╖Φâ╜µëôσ¡ùπÇé
   *
   *   ΓÜá∩╕Å Φ»Üσ«₧Φ╛╣τòî∩╝ÜΣ╗ÑΣ╕èµÿ»Φ»╗ HAL µ║Éτáüσ╛ùσê░τÜäτ╗ôΦ«║∩╝ê`stm32f4xx_hal_rcc.c:943-970`πÇü
   *      `HAL_RCC_ClockConfig` τ╗ôσ░╛τÜä `HAL_InitTick`∩╝ë∩╝î**µ▓íµ£ëσ£¿τ£ƒµ¥┐Σ╕èσê╢ΘÇáΦ┐çµù╢ΘÆƒµòàΘÜ£**πÇé
   *      Σ╕èµ¥┐Θ¬îσê░τÜäσÅ¬µ£ë"µ¡úσ╕╕σÉ»σè¿µù╢ `bf=0`"Φ┐ÖΣ╕ÇΣ╛ºπÇé
   *
   *   ΓÜá∩╕Å ΘÖìτ║ºτè╢µÇüΣ╕ï HAL τÜäµ»½τºÆµù╢σƒ║σÅ»Φâ╜σüÅµàó∩╝Ü`HAL_InitTick()` σÅ¬σ£¿ `ClockConfig`
   *      **µêÉσèƒ**Φ╖»σ╛äτÜäτ╗ôσ░╛Φó½Φ░âτö¿∩╝îµëÇΣ╗Ñ tick Σ╗ìµîë HSI 16 σàåΘàìτ╜« ΓÇöΓÇö Φ┐ÖΣ╕Äσ«₧ΘÖàΣ╕╗Θóæ
   *      Σ╕ÇΦç┤∩╝îµÿ»**σ»╣τÜä**∩╝¢Σ╜å `HAL_Delay()` τÜäΦ╢àµù╢σÇ╝πÇüΣ╗ÑσÅèσ░åµ¥ÑΣ╛¥Φ╡û tick Φ«íµù╢τÜä
   *      µÄºσê╢τÄ»∩╝îΘâ╜Σ╕ìΦâ╜σåìΣ┐íΣ╗╗"168 σàå"Φ┐ÖΣ╕¬σëìµÅÉπÇé**ΘÖìτ║ºΦ┐ÉΦíî = Φâ╜τ£ïπÇüΦâ╜µèÑ∩╝îΣ╕ìΦâ╜σ╣▓µ┤╗πÇé**
   *
   * ΓöÇΓöÇ Σ╕║Σ╗ÇΣ╣êµòàΘÜ£τáüΣ║ñτ╗Öµ¥┐τ║ºΦ«░∩╝îΦÇîΣ╕ìµÿ»σ£¿Φ┐ÖΘçîσ¡ÿΣ╕¬Θ¥ÖµÇüσÅÿΘçÅ ΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇΓöÇ
   *   σ¢áΣ╕║**µ┐ÇσàëΦüöΘöüΦªüσ£¿µ¥┐τ║ºΦ»╗σ«â**∩╝ê`AVE_Laser_Set()`∩╝ÜσÉ»σè¿σñ▒Φ┤Ñσ░▒Σ╕ìσç║σàë∩╝ëπÇé
   *   τè╢µÇüσ¡ÿσ£¿Σ╕ñΣ╕¬σ£░µû╣σ░▒Σ╝Üµ£ëΣ╕ñΣ╗╜Σ╕ìΣ╕ÇΦç┤∩╝îµëÇΣ╗ÑσÅ¬µ£ëΣ╕ÇΣ╕¬σç║σñäπÇé
   */
  AVE_Board_NoteBootFail((uint32_t)AVE_BOARD_BOOT_FAIL_HAL);

  /*
   * σà│Σ║Ä `__disable_irq()`∩╝Ü**σÄ╗µÄë**πÇé
   *
   * σ«âσÄƒµ£¼τÜäµäÅσ¢╛µÿ»"σç║Σ║åΣ║ïσ░▒µèèΣ╕¡µû¡σà│µÄë∩╝îΘÿ▓µ¡óτ╗ºτ╗¡Σ╣▒Φ╖æ"πÇé
   * Σ╜åσ£¿ΘÖìτ║ºΦ┐ÉΦíîτÜäΦ╖»σ¡ÉΣ╕è∩╝îσà│Σ╕¡µû¡τ¡ëΣ║Äµèè HAL τÜä tick Σ╣ƒσü£Σ║å ΓÇöΓÇö
   * `HAL_UART_Transmit()` Φ┐Öτ▒╗σ╕ªΦ╢àµù╢τÜäΦ░âτö¿Σ╝Ü**µ░╕Σ╣àσìíµ¡╗τ¡ëσ╛àΣ╕ÇΣ╕¬Σ╕ìΣ╝Üσê░µ¥ÑτÜä tick**∩╝î
   * Σ║Äµÿ»σ┐âΦ╖│ΘéúΣ╕ÇΦíîµ░╕Φ┐£σÅæΣ╕ìσç║σÄ╗∩╝Ü**σÅêσÅÿσ¢₧Θ¥ÖΘ╗ÿσü£µ£║∩╝îτÖ╜µö╣**πÇé
   *
   * σà│Σ╕¡µû¡σÆî"ΦªüΦâ╜µèÑσç║µòàΘÜ£"µÿ»τ¢┤µÄÑσå▓τ¬üτÜä∩╝îΦ┐ÖΘçîΘÇëµï⌐σÉÄΦÇàπÇé
   */
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
