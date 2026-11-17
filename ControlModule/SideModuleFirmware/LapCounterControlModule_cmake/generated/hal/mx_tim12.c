/**
  ******************************************************************************
  * @file           : mx_tim12.c
  * @brief          : Peripheral initialization
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the mx_stm32c5xx_hal_drivers_license.md file
  * in the same directory as the generated code.
  * If no mx_stm32c5xx_hal_drivers_license.md file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */

/* Includes ------------------------------------------------------------------*/
#include "mx_tim12.h"

/* Private typedef -----------------------------------------------------------*/
/* Private define ------------------------------------------------------------*/
/* Private macro -------------------------------------------------------------*/
/* Private variables ---------------------------------------------------------*/
/* Private functions prototype------------------------------------------------*/
/* Exported variables by reference--------------------------------------------*/
static hal_tim_handle_t hTIM12;

/* Exported function definition ----------------------------------------------*/
/******************************************************************************/
/* Exported functions for TIM12 in HAL layer */
/******************************************************************************/
hal_tim_handle_t *mx_tim12_init(void)
{
  if (HAL_TIM_Init(&hTIM12, HAL_TIM12) != HAL_OK)
  {
    return NULL;
  }

  HAL_RCC_TIM12_EnableClock();

  /* Timer configuration to reach the output frequency at 1 kHz */
  hal_tim_config_t config;
  config.prescaler              = 143;
  config.counter_mode           = HAL_TIM_COUNTER_UP;
  config.period                 = 0x3E7;
  config.repetition_counter     = 0;
  config.clock_sel.clock_source = HAL_TIM_CLK_INTERNAL;
  if (HAL_TIM_SetConfig(&hTIM12, &config) != HAL_OK)
  {
    return NULL;
  }

  /* Sampling Clock */
  if (HAL_TIM_SetDTSPrescaler(&hTIM12, HAL_TIM_DTS_DIV1) != HAL_OK)
  {
    return NULL;
  }
  if (HAL_TIM_SetDTS2Prescaler(&hTIM12, HAL_TIM_DTS2_DIV1) != HAL_OK)
  {
    return NULL;
  }

  if (HAL_TIM_EnableAutoReloadPreload(&hTIM12) != HAL_OK)
  {
    return NULL;
  }

  hal_tim_oc_channel_config_t oc_config;

  oc_config.polarity       = HAL_TIM_OC_HIGH;
  if (HAL_TIM_OC_SetConfigChannel(&hTIM12, HAL_TIM_CHANNEL_2, &oc_config) != HAL_OK)
  {
    return NULL;
  }
  hal_tim_oc_compare_unit_config_t oc_compare_unit_config;

  oc_compare_unit_config.mode  = HAL_TIM_OC_PWM1;
  oc_compare_unit_config.pulse = 0x1F4;
  if (HAL_TIM_OC_SetConfigCompareUnit(&hTIM12, HAL_TIM_OC_COMPARE_UNIT_2,
                                      &oc_compare_unit_config) != HAL_OK)
  {
    return NULL;
  }

  if (HAL_TIM_OC_EnableComparePreload(&hTIM12, HAL_TIM_OC_COMPARE_UNIT_2) != HAL_OK)
  {
    return NULL;
  }

  /* Update Event Management */
  if (HAL_TIM_SetUpdateSource(&hTIM12, HAL_TIM_UPDATE_REGULAR) != HAL_OK)
  {
    return NULL;
  }
  if (HAL_TIM_EnableUpdateGeneration(&hTIM12) != HAL_OK)
  {
    return NULL;
  }
  /* Master Mode Configuration */
  /* ### TIM12 GPIO Configuration ########################### */
  /* GPIO Clocks activation */
  HAL_RCC_GPIOB_EnableClock();

  hal_gpio_config_t  gpio_config;

  /**
    [GPIO Pin] ------> [Signal Name] ------> [Labels]

       PB15    ------>   TIM12_CH2   ------>  SYNC
    **/
  gpio_config.mode        = HAL_GPIO_MODE_ALTERNATE;
  gpio_config.output_type = HAL_GPIO_OUTPUT_PUSHPULL;
  gpio_config.pull        = HAL_GPIO_PULL_NO;
  gpio_config.speed       = HAL_GPIO_SPEED_FREQ_LOW;
  gpio_config.alternate   = HAL_GPIO_AF_2;
  HAL_GPIO_Init(SYNC_PORT, SYNC_PIN, &gpio_config);

  /* Enable the Timer global interrupt */
  HAL_CORTEX_NVIC_SetPriority(TIM12_IRQn, HAL_CORTEX_NVIC_PREEMP_PRIORITY_1, HAL_CORTEX_NVIC_SUB_PRIORITY_0);
  HAL_CORTEX_NVIC_EnableIRQ(TIM12_IRQn);

  return &hTIM12;
}

void mx_tim12_deinit(void)
{
  (void)HAL_TIM_DeInit(&hTIM12);

  HAL_RCC_TIM12_DisableClock();

  HAL_RCC_TIM12_Reset();

  /* De-initialize all GPIOB pins associated with TIM12 */
  HAL_GPIO_DeInit(SYNC_PORT, SYNC_PIN);

  /* Disable Timer global interrupt */
  HAL_CORTEX_NVIC_DisableIRQ(TIM12_IRQn);
}

hal_tim_handle_t *mx_tim12_gethandle(void)
{
  return &hTIM12;
}

/******************************************************************************/
/*                           TIM12 global interrupt                           */
/******************************************************************************/
void TIM12_IRQHandler(void)
{
  HAL_TIM_IRQHandler(&hTIM12);
}
