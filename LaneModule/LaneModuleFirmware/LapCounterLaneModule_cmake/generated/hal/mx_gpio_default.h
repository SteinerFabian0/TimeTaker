/**
  ******************************************************************************
  * @file           : mx_gpio_default.h
  * @brief          : Header for mx_gpio_default.c file.
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

/* Define to prevent recursive inclusion -------------------------------------*/
#ifndef MX_GPIO_DEFAULT_H
#define MX_GPIO_DEFAULT_H

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

/* Includes ------------------------------------------------------------------*/
#include "stm32_hal.h"
#include "mx_def.h"

/* Exported types ------------------------------------------------------------*/
/* Exported constants --------------------------------------------------------*/
/******************************************************************************/
/* Exported defines for gpio_default in HAL layer                             */
/******************************************************************************/

/* Primary aliases for GPIO PA0 pin */
#define SENS_INT_PORT                                   HAL_GPIOA
#define SENS_INT_PIN                                    HAL_GPIO_PIN_0

/* Primary aliases for GPIO PA1 pin */
#define CHAIN_OUT_PORT                                  HAL_GPIOA
#define CHAIN_OUT_PIN                                   HAL_GPIO_PIN_1
#define CHAIN_OUT_INIT_STATE                            HAL_GPIO_PIN_RESET
#define CHAIN_OUT_ACTIVE_STATE                          HAL_GPIO_PIN_SET
#define CHAIN_OUT_INACTIVE_STATE                        HAL_GPIO_PIN_RESET

/* Primary aliases for GPIO PA2 pin */
#define BUS_INT_PORT                                    HAL_GPIOA
#define BUS_INT_PIN                                     HAL_GPIO_PIN_2
#define BUS_INT_INIT_STATE                              HAL_GPIO_PIN_SET
#define BUS_INT_ACTIVE_STATE                            HAL_GPIO_PIN_RESET
#define BUS_INT_INACTIVE_STATE                          HAL_GPIO_PIN_SET

/* Primary aliases for GPIO PB4(NJTRST) pin */
#define CS_PORT                                         HAL_GPIOB
#define CS_PIN                                          HAL_GPIO_PIN_4
#define CS_INIT_STATE                                   HAL_GPIO_PIN_SET
#define CS_ACTIVE_STATE                                 HAL_GPIO_PIN_RESET
#define CS_INACTIVE_STATE                               HAL_GPIO_PIN_SET

/* Primary aliases for GPIO PB12 pin */
#define CHAIN_IN_PORT                                   HAL_GPIOB
#define CHAIN_IN_PIN                                    HAL_GPIO_PIN_12

/* Exported macros -----------------------------------------------------------*/
/* Exported variables --------------------------------------------------------*/
/* Exported functions ------------------------------------------------------- */
/******************************************************************************/
/* Exported functions for gpio_default in HAL layer                           */
/******************************************************************************/
/**
  * @brief mx_gpio_default init function
  * This function configures the hardware resources used in this example
  * @retval 0  GPIO group correctly initialized
  * @retval -1 Issue detected during GPIO group initialization
  */
system_status_t mx_gpio_default_init(void);

/**
  * @brief  De-initialize gpio_default instance.
  */
system_status_t mx_gpio_default_deinit(void);

/**
  * @brief  Get the EXTI0 object.
  * @retval Pointer on the EXTI0 Handle
  */
hal_exti_handle_t *mx_gpio_default_exti0_gethandle(void);

/**
  * @brief  Get the EXTI12 object.
  * @retval Pointer on the EXTI12 Handle
  */
hal_exti_handle_t *mx_gpio_default_exti12_gethandle(void);

/******************************************************************************/
/*                            EXTI Line0 interrupt                            */
/******************************************************************************/
void EXTI0_IRQHandler(void);

/******************************************************************************/
/*                           EXTI Line12 interrupt                            */
/******************************************************************************/
void EXTI12_IRQHandler(void);

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* MX_GPIO_DEFAULT_H */
