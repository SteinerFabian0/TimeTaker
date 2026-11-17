/**
  ******************************************************************************
  * @file           : mx_gpio_default.c
  * @brief          : gpio_default Peripheral initialization
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
#include "mx_gpio_default.h"

/* Exported types ------------------------------------------------------------*/
/* Exported constants --------------------------------------------------------*/
/* Private define ------------------------------------------------------------*/
/* Private functions ---------------------------------------------------------*/
/* Exported variables by reference -------------------------------------------*/
static hal_exti_handle_t hEXTI0;
static hal_exti_handle_t hEXTI12;

/******************************************************************************/
/* Exported functions for GPIO in HAL layer                                   */
/******************************************************************************/
system_status_t mx_gpio_default_init(void)
{
  hal_gpio_config_t  gpio_config;

  HAL_RCC_GPIOA_EnableClock();

  HAL_RCC_GPIOB_EnableClock();

  /*
    GPIO pin labels :
    PA0   ---------> SENS_INT
    */
  /* Configure PA0 GPIO pin in input mode */
  gpio_config.mode            = HAL_GPIO_MODE_INPUT;
  gpio_config.pull            = HAL_GPIO_PULL_NO;
  if (HAL_GPIO_Init(SENS_INT_PORT, SENS_INT_PIN, &gpio_config) != HAL_OK)
  {
    return SYSTEM_PERIPHERAL_ERROR;
  }

  hal_exti_config_t exti_config;

  /* Initialize the EXTI for line 0 */
  HAL_EXTI_Init(&hEXTI0, HAL_EXTI_LINE_0);

  /* Set the trigger as FALLING for the GPIOA */
  exti_config.trigger   = HAL_EXTI_TRIGGER_FALLING;
  exti_config.gpio_port = HAL_EXTI_GPIOA;
  HAL_EXTI_SetConfig(&hEXTI0, &exti_config);

  /* Enable the INTERRUPT mode */
  HAL_EXTI_Enable(&hEXTI0, HAL_EXTI_MODE_INTERRUPT);

  /* Set line 0 Interrupt priority */
  HAL_CORTEX_NVIC_SetPriority(EXTI0_IRQn, HAL_CORTEX_NVIC_PREEMP_PRIORITY_0, HAL_CORTEX_NVIC_SUB_PRIORITY_0);
  HAL_CORTEX_NVIC_EnableIRQ(EXTI0_IRQn);

  /*
    GPIO pin labels :
    PA1   ---------> CHAIN_OUT
    */
  /* Configure PA1 GPIO pin in output mode */
  gpio_config.mode            = HAL_GPIO_MODE_OUTPUT;
  gpio_config.speed           = HAL_GPIO_SPEED_FREQ_LOW;
  gpio_config.pull            = HAL_GPIO_PULL_NO;
  gpio_config.output_type     = HAL_GPIO_OUTPUT_PUSHPULL;
  gpio_config.init_state      = CHAIN_OUT_INIT_STATE;
  if (HAL_GPIO_Init(CHAIN_OUT_PORT, CHAIN_OUT_PIN, &gpio_config) != HAL_OK)
  {
    return SYSTEM_PERIPHERAL_ERROR;
  }

  /*
    GPIO pin labels :
    PA2   ---------> BUS_INT
    */
  /* Configure PA2 GPIO pin in output mode */
  gpio_config.mode            = HAL_GPIO_MODE_OUTPUT;
  gpio_config.speed           = HAL_GPIO_SPEED_FREQ_LOW;
  gpio_config.pull            = HAL_GPIO_PULL_NO;
  gpio_config.output_type     = HAL_GPIO_OUTPUT_OPENDRAIN;
  gpio_config.init_state      = BUS_INT_INIT_STATE;
  if (HAL_GPIO_Init(BUS_INT_PORT, BUS_INT_PIN, &gpio_config) != HAL_OK)
  {
    return SYSTEM_PERIPHERAL_ERROR;
  }

  /*
    GPIO pin labels :
    PB4   ---------> CS
    */
  /* Configure PB4 GPIO pin in output mode */
  gpio_config.mode            = HAL_GPIO_MODE_OUTPUT;
  gpio_config.speed           = HAL_GPIO_SPEED_FREQ_MEDIUM;
  gpio_config.pull            = HAL_GPIO_PULL_NO;
  gpio_config.output_type     = HAL_GPIO_OUTPUT_PUSHPULL;
  gpio_config.init_state      = CS_INIT_STATE;
  if (HAL_GPIO_Init(CS_PORT, CS_PIN, &gpio_config) != HAL_OK)
  {
    return SYSTEM_PERIPHERAL_ERROR;
  }

  /*
    GPIO pin labels :
    PB12  ---------> CHAIN_IN
    */
  /* Configure PB12 GPIO pin in input mode */
  gpio_config.mode            = HAL_GPIO_MODE_INPUT;
  gpio_config.pull            = HAL_GPIO_PULL_DOWN;
  if (HAL_GPIO_Init(CHAIN_IN_PORT, CHAIN_IN_PIN, &gpio_config) != HAL_OK)
  {
    return SYSTEM_PERIPHERAL_ERROR;
  }

  /* Initialize the EXTI for line 12 */
  HAL_EXTI_Init(&hEXTI12, HAL_EXTI_LINE_12);

  /* Set the trigger as RISING_FALLING for the GPIOB */
  exti_config.trigger   = HAL_EXTI_TRIGGER_RISING_FALLING;
  exti_config.gpio_port = HAL_EXTI_GPIOB;
  HAL_EXTI_SetConfig(&hEXTI12, &exti_config);

  /* Enable the INTERRUPT mode */
  HAL_EXTI_Enable(&hEXTI12, HAL_EXTI_MODE_INTERRUPT);

  /* Set line 12 Interrupt priority */
  HAL_CORTEX_NVIC_SetPriority(EXTI12_IRQn, HAL_CORTEX_NVIC_PREEMP_PRIORITY_6, HAL_CORTEX_NVIC_SUB_PRIORITY_0);
  HAL_CORTEX_NVIC_EnableIRQ(EXTI12_IRQn);

  return SYSTEM_OK;
}

system_status_t mx_gpio_default_deinit(void)
{
  /* De-initialize the EXTI for GPIOA line0 */
  HAL_EXTI_DeInit(&hEXTI0);

  /* set line 0 Interrupt priority */
  HAL_CORTEX_NVIC_DisableIRQ(EXTI0_IRQn);

  /* De-initialize the EXTI for GPIOB line12 */
  HAL_EXTI_DeInit(&hEXTI12);

  /* set line 12 Interrupt priority */
  HAL_CORTEX_NVIC_DisableIRQ(EXTI12_IRQn);

  /* De-initialize pins of GPIOA port */
  HAL_GPIO_DeInit(HAL_GPIOA, SENS_INT_PIN | CHAIN_OUT_PIN | BUS_INT_PIN);

  /* De-initialize pins of GPIOB port */
  HAL_GPIO_DeInit(HAL_GPIOB, CS_PIN | CHAIN_IN_PIN);

  return SYSTEM_OK;
}

hal_exti_handle_t *mx_gpio_default_exti0_gethandle(void)
{
  return &hEXTI0;
}

hal_exti_handle_t *mx_gpio_default_exti12_gethandle(void)
{
  return &hEXTI12;
}

/******************************************************************************/
/*                            EXTI Line0 interrupt                            */
/******************************************************************************/
void EXTI0_IRQHandler(void)
{
  HAL_EXTI_IRQHandler(&hEXTI0);
}

/******************************************************************************/
/*                           EXTI Line12 interrupt                            */
/******************************************************************************/
void EXTI12_IRQHandler(void)
{
  HAL_EXTI_IRQHandler(&hEXTI12);
}
