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
static hal_exti_handle_t hEXTI4;
static hal_exti_handle_t hEXTI5;
static hal_exti_handle_t hEXTI12;
static hal_exti_handle_t hEXTI14;
static hal_exti_handle_t hEXTI13;
static hal_exti_handle_t hEXTI15;
static hal_exti_handle_t hEXTI2;

/******************************************************************************/
/* Exported functions for GPIO in HAL layer                                   */
/******************************************************************************/
system_status_t mx_gpio_default_init(void)
{
  hal_gpio_config_t  gpio_config;

  HAL_RCC_GPIOA_EnableClock();

  HAL_RCC_GPIOB_EnableClock();

  HAL_RCC_GPIOC_EnableClock();

  HAL_RCC_GPIOE_EnableClock();

  /*
    GPIO pin labels :
    PA4   ---------> EXT_PWR_FAULT
    */
  /* Configure PA4 GPIO pin in input mode */
  gpio_config.mode            = HAL_GPIO_MODE_INPUT;
  gpio_config.pull            = HAL_GPIO_PULL_UP;
  if (HAL_GPIO_Init(EXT_PWR_FAULT_PORT, EXT_PWR_FAULT_PIN, &gpio_config) != HAL_OK)
  {
    return SYSTEM_PERIPHERAL_ERROR;
  }

  hal_exti_config_t exti_config;

  /* Initialize the EXTI for line 4 */
  HAL_EXTI_Init(&hEXTI4, HAL_EXTI_LINE_4);

  /* Set the trigger as FALLING for the GPIOA */
  exti_config.trigger   = HAL_EXTI_TRIGGER_FALLING;
  exti_config.gpio_port = HAL_EXTI_GPIOA;
  HAL_EXTI_SetConfig(&hEXTI4, &exti_config);

  /* Enable the INTERRUPT mode */
  HAL_EXTI_Enable(&hEXTI4, HAL_EXTI_MODE_INTERRUPT);

  /* Set line 4 Interrupt priority */
  HAL_CORTEX_NVIC_SetPriority(EXTI4_IRQn, HAL_CORTEX_NVIC_PREEMP_PRIORITY_0, HAL_CORTEX_NVIC_SUB_PRIORITY_0);
  HAL_CORTEX_NVIC_EnableIRQ(EXTI4_IRQn);

  /*
    GPIO pin labels :
    PA5   ---------> EXT_PWR_EN
    PA6   ---------> LANE_PWR_EN
    PA10  ---------> CHAIN_OUT
    */
  /* Configure PA5, PA6, PA10 GPIO pins in output mode */
  gpio_config.mode            = HAL_GPIO_MODE_OUTPUT;
  gpio_config.speed           = HAL_GPIO_SPEED_FREQ_LOW;
  gpio_config.pull            = HAL_GPIO_PULL_NO;
  gpio_config.output_type     = HAL_GPIO_OUTPUT_PUSHPULL;
  gpio_config.init_state      = HAL_GPIO_PIN_RESET;
  if (HAL_GPIO_Init(HAL_GPIOA, EXT_PWR_EN_PIN | LANE_PWR_EN_PIN | CHAIN_OUT_PIN, &gpio_config) != HAL_OK)
  {
    return SYSTEM_PERIPHERAL_ERROR;
  }

  /*
    GPIO pin labels :
    PA15  ---------> BLE_GPIO2
    */
  /* Configure PA15 GPIO pin in input mode */
  gpio_config.mode            = HAL_GPIO_MODE_INPUT;
  gpio_config.pull            = HAL_GPIO_PULL_DOWN;
  if (HAL_GPIO_Init(BLE_GPIO2_PORT, BLE_GPIO2_PIN, &gpio_config) != HAL_OK)
  {
    return SYSTEM_PERIPHERAL_ERROR;
  }

  /*
    GPIO pin labels :
    PB0   ---------> SPI_CS2
    PB1   ---------> SPI_CS1
    */
  /* Configure PB0, PB1 GPIO pins in output mode */
  gpio_config.mode            = HAL_GPIO_MODE_OUTPUT;
  gpio_config.speed           = HAL_GPIO_SPEED_FREQ_LOW;
  gpio_config.pull            = HAL_GPIO_PULL_NO;
  gpio_config.output_type     = HAL_GPIO_OUTPUT_PUSHPULL;
  gpio_config.init_state      = HAL_GPIO_PIN_SET;
  if (HAL_GPIO_Init(HAL_GPIOB, SPI_CS2_PIN | SPI_CS1_PIN, &gpio_config) != HAL_OK)
  {
    return SYSTEM_PERIPHERAL_ERROR;
  }

  /*
    GPIO pin labels :
    PB4   ---------> BLE_GPIO1
    PB12  ---------> USB_FAULT
    */
  /* Configure PB4, PB12 GPIO pins in input mode */
  gpio_config.mode            = HAL_GPIO_MODE_INPUT;
  gpio_config.pull            = HAL_GPIO_PULL_UP;
  if (HAL_GPIO_Init(HAL_GPIOB, BLE_GPIO1_PIN | USB_FAULT_PIN, &gpio_config) != HAL_OK)
  {
    return SYSTEM_PERIPHERAL_ERROR;
  }

  /* Initialize the EXTI for line 12 */
  HAL_EXTI_Init(&hEXTI12, HAL_EXTI_LINE_12);

  /* Set the trigger as FALLING for the GPIOB */
  exti_config.trigger   = HAL_EXTI_TRIGGER_FALLING;
  exti_config.gpio_port = HAL_EXTI_GPIOB;
  HAL_EXTI_SetConfig(&hEXTI12, &exti_config);

  /* Enable the INTERRUPT mode */
  HAL_EXTI_Enable(&hEXTI12, HAL_EXTI_MODE_INTERRUPT);

  /* Set line 12 Interrupt priority */
  HAL_CORTEX_NVIC_SetPriority(EXTI12_IRQn, HAL_CORTEX_NVIC_PREEMP_PRIORITY_0, HAL_CORTEX_NVIC_SUB_PRIORITY_0);
  HAL_CORTEX_NVIC_EnableIRQ(EXTI12_IRQn);

  /*
    GPIO pin labels :
    PB5   ---------> BLE_IRQ
    */
  /* Configure PB5 GPIO pin in input mode */
  gpio_config.mode            = HAL_GPIO_MODE_INPUT;
  gpio_config.pull            = HAL_GPIO_PULL_DOWN;
  if (HAL_GPIO_Init(BLE_IRQ_PORT, BLE_IRQ_PIN, &gpio_config) != HAL_OK)
  {
    return SYSTEM_PERIPHERAL_ERROR;
  }

  /* Initialize the EXTI for line 5 */
  HAL_EXTI_Init(&hEXTI5, HAL_EXTI_LINE_5);

  /* Set the trigger as RISING for the GPIOB */
  exti_config.trigger   = HAL_EXTI_TRIGGER_RISING;
  exti_config.gpio_port = HAL_EXTI_GPIOB;
  HAL_EXTI_SetConfig(&hEXTI5, &exti_config);

  /* Enable the INTERRUPT mode */
  HAL_EXTI_Enable(&hEXTI5, HAL_EXTI_MODE_INTERRUPT);

  /* Set line 5 Interrupt priority */
  HAL_CORTEX_NVIC_SetPriority(EXTI5_IRQn, HAL_CORTEX_NVIC_PREEMP_PRIORITY_6, HAL_CORTEX_NVIC_SUB_PRIORITY_0);
  HAL_CORTEX_NVIC_EnableIRQ(EXTI5_IRQn);

  /*
    GPIO pin labels :
    PB14  ---------> BUS_INT
    */
  /* Configure PB14 GPIO pin in input mode */
  gpio_config.mode            = HAL_GPIO_MODE_INPUT;
  gpio_config.pull            = HAL_GPIO_PULL_NO;
  if (HAL_GPIO_Init(BUS_INT_PORT, BUS_INT_PIN, &gpio_config) != HAL_OK)
  {
    return SYSTEM_PERIPHERAL_ERROR;
  }

  /* Initialize the EXTI for line 14 */
  HAL_EXTI_Init(&hEXTI14, HAL_EXTI_LINE_14);

  /* Set the trigger as FALLING for the GPIOB */
  exti_config.trigger   = HAL_EXTI_TRIGGER_FALLING;
  exti_config.gpio_port = HAL_EXTI_GPIOB;
  HAL_EXTI_SetConfig(&hEXTI14, &exti_config);

  /* Enable the INTERRUPT mode */
  HAL_EXTI_Enable(&hEXTI14, HAL_EXTI_MODE_INTERRUPT);

  /* Set line 14 Interrupt priority */
  HAL_CORTEX_NVIC_SetPriority(EXTI14_IRQn, HAL_CORTEX_NVIC_PREEMP_PRIORITY_3, HAL_CORTEX_NVIC_SUB_PRIORITY_0);
  HAL_CORTEX_NVIC_EnableIRQ(EXTI14_IRQn);

  /*
    GPIO pin labels :
    PC13  ---------> BTN2
    PC15  ---------> BTN1
    */
  /* Configure PC13, PC15 GPIO pins in input mode */
  gpio_config.mode            = HAL_GPIO_MODE_INPUT;
  gpio_config.pull            = HAL_GPIO_PULL_UP;
  if (HAL_GPIO_Init(HAL_GPIOC, BTN2_PIN | BTN1_PIN, &gpio_config) != HAL_OK)
  {
    return SYSTEM_PERIPHERAL_ERROR;
  }

  /* Initialize the EXTI for line 13 */
  HAL_EXTI_Init(&hEXTI13, HAL_EXTI_LINE_13);

  /* Set the trigger as FALLING for the GPIOC */
  exti_config.trigger   = HAL_EXTI_TRIGGER_FALLING;
  exti_config.gpio_port = HAL_EXTI_GPIOC;
  HAL_EXTI_SetConfig(&hEXTI13, &exti_config);

  /* Enable the INTERRUPT mode */
  HAL_EXTI_Enable(&hEXTI13, HAL_EXTI_MODE_INTERRUPT);

  /* Set line 13 Interrupt priority */
  HAL_CORTEX_NVIC_SetPriority(EXTI13_IRQn, HAL_CORTEX_NVIC_PREEMP_PRIORITY_9, HAL_CORTEX_NVIC_SUB_PRIORITY_0);
  HAL_CORTEX_NVIC_EnableIRQ(EXTI13_IRQn);

  /* Initialize the EXTI for line 15 */
  HAL_EXTI_Init(&hEXTI15, HAL_EXTI_LINE_15);

  /* Set the trigger as FALLING for the GPIOC */
  exti_config.trigger   = HAL_EXTI_TRIGGER_FALLING;
  exti_config.gpio_port = HAL_EXTI_GPIOC;
  HAL_EXTI_SetConfig(&hEXTI15, &exti_config);

  /* Enable the INTERRUPT mode */
  HAL_EXTI_Enable(&hEXTI15, HAL_EXTI_MODE_INTERRUPT);

  /* Set line 15 Interrupt priority */
  HAL_CORTEX_NVIC_SetPriority(EXTI15_IRQn, HAL_CORTEX_NVIC_PREEMP_PRIORITY_9, HAL_CORTEX_NVIC_SUB_PRIORITY_0);
  HAL_CORTEX_NVIC_EnableIRQ(EXTI15_IRQn);

  /*
    GPIO pin labels :
    PE2   ---------> BTN3
    */
  /* Configure PE2 GPIO pin in input mode */
  gpio_config.mode            = HAL_GPIO_MODE_INPUT;
  gpio_config.pull            = HAL_GPIO_PULL_UP;
  if (HAL_GPIO_Init(BTN3_PORT, BTN3_PIN, &gpio_config) != HAL_OK)
  {
    return SYSTEM_PERIPHERAL_ERROR;
  }

  /* Initialize the EXTI for line 2 */
  HAL_EXTI_Init(&hEXTI2, HAL_EXTI_LINE_2);

  /* Set the trigger as FALLING for the GPIOE */
  exti_config.trigger   = HAL_EXTI_TRIGGER_FALLING;
  exti_config.gpio_port = HAL_EXTI_GPIOE;
  HAL_EXTI_SetConfig(&hEXTI2, &exti_config);

  /* Enable the INTERRUPT mode */
  HAL_EXTI_Enable(&hEXTI2, HAL_EXTI_MODE_INTERRUPT);

  /* Set line 2 Interrupt priority */
  HAL_CORTEX_NVIC_SetPriority(EXTI2_IRQn, HAL_CORTEX_NVIC_PREEMP_PRIORITY_9, HAL_CORTEX_NVIC_SUB_PRIORITY_0);
  HAL_CORTEX_NVIC_EnableIRQ(EXTI2_IRQn);

  return SYSTEM_OK;
}

system_status_t mx_gpio_default_deinit(void)
{
  /* De-initialize the EXTI for GPIOA line4 */
  HAL_EXTI_DeInit(&hEXTI4);

  /* set line 4 Interrupt priority */
  HAL_CORTEX_NVIC_DisableIRQ(EXTI4_IRQn);

  /* De-initialize the EXTI for GPIOB line12 */
  HAL_EXTI_DeInit(&hEXTI12);

  /* set line 12 Interrupt priority */
  HAL_CORTEX_NVIC_DisableIRQ(EXTI12_IRQn);

  /* De-initialize the EXTI for GPIOB line5 */
  HAL_EXTI_DeInit(&hEXTI5);

  /* set line 5 Interrupt priority */
  HAL_CORTEX_NVIC_DisableIRQ(EXTI5_IRQn);

  /* De-initialize the EXTI for GPIOB line14 */
  HAL_EXTI_DeInit(&hEXTI14);

  /* set line 14 Interrupt priority */
  HAL_CORTEX_NVIC_DisableIRQ(EXTI14_IRQn);

  /* De-initialize the EXTI for GPIOC line13 */
  HAL_EXTI_DeInit(&hEXTI13);

  /* set line 13 Interrupt priority */
  HAL_CORTEX_NVIC_DisableIRQ(EXTI13_IRQn);
  /* De-initialize the EXTI for GPIOC line15 */
  HAL_EXTI_DeInit(&hEXTI15);

  /* set line 15 Interrupt priority */
  HAL_CORTEX_NVIC_DisableIRQ(EXTI15_IRQn);

  /* De-initialize the EXTI for GPIOE line2 */
  HAL_EXTI_DeInit(&hEXTI2);

  /* set line 2 Interrupt priority */
  HAL_CORTEX_NVIC_DisableIRQ(EXTI2_IRQn);

  /* De-initialize pins of GPIOA port */
  HAL_GPIO_DeInit(HAL_GPIOA, EXT_PWR_FAULT_PIN | EXT_PWR_EN_PIN | LANE_PWR_EN_PIN | CHAIN_OUT_PIN | BLE_GPIO2_PIN);

  /* De-initialize pins of GPIOB port */
  HAL_GPIO_DeInit(HAL_GPIOB, SPI_CS2_PIN | SPI_CS1_PIN | BLE_GPIO1_PIN | BLE_IRQ_PIN | USB_FAULT_PIN | BUS_INT_PIN);

  /* De-initialize pins of GPIOC port */
  HAL_GPIO_DeInit(HAL_GPIOC, BTN2_PIN | BTN1_PIN);

  /* De-initialize pins of GPIOE port */
  HAL_GPIO_DeInit(BTN3_PORT, BTN3_PIN);

  return SYSTEM_OK;
}

hal_exti_handle_t *mx_gpio_default_exti4_gethandle(void)
{
  return &hEXTI4;
}

hal_exti_handle_t *mx_gpio_default_exti5_gethandle(void)
{
  return &hEXTI5;
}

hal_exti_handle_t *mx_gpio_default_exti12_gethandle(void)
{
  return &hEXTI12;
}

hal_exti_handle_t *mx_gpio_default_exti14_gethandle(void)
{
  return &hEXTI14;
}

hal_exti_handle_t *mx_gpio_default_exti13_gethandle(void)
{
  return &hEXTI13;
}

hal_exti_handle_t *mx_gpio_default_exti15_gethandle(void)
{
  return &hEXTI15;
}

hal_exti_handle_t *mx_gpio_default_exti2_gethandle(void)
{
  return &hEXTI2;
}

/******************************************************************************/
/*                            EXTI Line4 interrupt                            */
/******************************************************************************/
void EXTI4_IRQHandler(void)
{
  HAL_EXTI_IRQHandler(&hEXTI4);
}

/******************************************************************************/
/*                           EXTI Line12 interrupt                            */
/******************************************************************************/
void EXTI12_IRQHandler(void)
{
  HAL_EXTI_IRQHandler(&hEXTI12);
}

/******************************************************************************/
/*                            EXTI Line5 interrupt                            */
/******************************************************************************/
void EXTI5_IRQHandler(void)
{
  HAL_EXTI_IRQHandler(&hEXTI5);
}

/******************************************************************************/
/*                           EXTI Line14 interrupt                            */
/******************************************************************************/
void EXTI14_IRQHandler(void)
{
  HAL_EXTI_IRQHandler(&hEXTI14);
}

/******************************************************************************/
/*                           EXTI Line13 interrupt                            */
/******************************************************************************/
void EXTI13_IRQHandler(void)
{
  HAL_EXTI_IRQHandler(&hEXTI13);
}

/******************************************************************************/
/*                           EXTI Line15 interrupt                            */
/******************************************************************************/
void EXTI15_IRQHandler(void)
{
  HAL_EXTI_IRQHandler(&hEXTI15);
}

/******************************************************************************/
/*                            EXTI Line2 interrupt                            */
/******************************************************************************/
void EXTI2_IRQHandler(void)
{
  HAL_EXTI_IRQHandler(&hEXTI2);
}
