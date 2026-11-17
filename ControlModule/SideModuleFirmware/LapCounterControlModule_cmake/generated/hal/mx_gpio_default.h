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

/* Primary aliases for GPIO PA4 pin */
#define EXT_PWR_FAULT_PORT                              HAL_GPIOA
#define EXT_PWR_FAULT_PIN                               HAL_GPIO_PIN_4

/* Primary aliases for GPIO PA5 pin */
#define EXT_PWR_EN_PORT                                 HAL_GPIOA
#define EXT_PWR_EN_PIN                                  HAL_GPIO_PIN_5
#define EXT_PWR_EN_INIT_STATE                           HAL_GPIO_PIN_RESET
#define EXT_PWR_EN_ACTIVE_STATE                         HAL_GPIO_PIN_SET
#define EXT_PWR_EN_INACTIVE_STATE                       HAL_GPIO_PIN_RESET

/* Primary aliases for GPIO PA6 pin */
#define LANE_PWR_EN_PORT                                HAL_GPIOA
#define LANE_PWR_EN_PIN                                 HAL_GPIO_PIN_6
#define LANE_PWR_EN_INIT_STATE                          HAL_GPIO_PIN_RESET
#define LANE_PWR_EN_ACTIVE_STATE                        HAL_GPIO_PIN_SET
#define LANE_PWR_EN_INACTIVE_STATE                      HAL_GPIO_PIN_RESET

/* Primary aliases for GPIO PA10 pin */
#define CHAIN_OUT_PORT                                  HAL_GPIOA
#define CHAIN_OUT_PIN                                   HAL_GPIO_PIN_10
#define CHAIN_OUT_INIT_STATE                            HAL_GPIO_PIN_RESET
#define CHAIN_OUT_ACTIVE_STATE                          HAL_GPIO_PIN_SET
#define CHAIN_OUT_INACTIVE_STATE                        HAL_GPIO_PIN_RESET

/* Primary aliases for GPIO PA15(JTDI) pin */
#define BLE_GPIO2_PORT                                  HAL_GPIOA
#define BLE_GPIO2_PIN                                   HAL_GPIO_PIN_15

/* Primary aliases for GPIO PB0 pin */
#define SPI_CS2_PORT                                    HAL_GPIOB
#define SPI_CS2_PIN                                     HAL_GPIO_PIN_0
#define SPI_CS2_INIT_STATE                              HAL_GPIO_PIN_SET
#define SPI_CS2_ACTIVE_STATE                            HAL_GPIO_PIN_RESET
#define SPI_CS2_INACTIVE_STATE                          HAL_GPIO_PIN_SET

/* Primary aliases for GPIO PB1 pin */
#define SPI_CS1_PORT                                    HAL_GPIOB
#define SPI_CS1_PIN                                     HAL_GPIO_PIN_1
#define SPI_CS1_INIT_STATE                              HAL_GPIO_PIN_SET
#define SPI_CS1_ACTIVE_STATE                            HAL_GPIO_PIN_RESET
#define SPI_CS1_INACTIVE_STATE                          HAL_GPIO_PIN_SET

/* Primary aliases for GPIO PB4(NJTRST) pin */
#define BLE_GPIO1_PORT                                  HAL_GPIOB
#define BLE_GPIO1_PIN                                   HAL_GPIO_PIN_4

/* Primary aliases for GPIO PB5 pin */
#define BLE_IRQ_PORT                                    HAL_GPIOB
#define BLE_IRQ_PIN                                     HAL_GPIO_PIN_5

/* Primary aliases for GPIO PB12 pin */
#define USB_FAULT_PORT                                  HAL_GPIOB
#define USB_FAULT_PIN                                   HAL_GPIO_PIN_12

/* Primary aliases for GPIO PB14 pin */
#define BUS_INT_PORT                                    HAL_GPIOB
#define BUS_INT_PIN                                     HAL_GPIO_PIN_14

/* Primary aliases for GPIO PC13 pin */
#define BTN2_PORT                                       HAL_GPIOC
#define BTN2_PIN                                        HAL_GPIO_PIN_13

/* Primary aliases for GPIO PC15-OSC32_OUT(OSC32_OUT) pin */
#define BTN1_PORT                                       HAL_GPIOC
#define BTN1_PIN                                        HAL_GPIO_PIN_15

/* Primary aliases for GPIO PE2 pin */
#define BTN3_PORT                                       HAL_GPIOE
#define BTN3_PIN                                        HAL_GPIO_PIN_2

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
  * @brief  Get the EXTI4 object.
  * @retval Pointer on the EXTI4 Handle
  */
hal_exti_handle_t *mx_gpio_default_exti4_gethandle(void);

/**
  * @brief  Get the EXTI5 object.
  * @retval Pointer on the EXTI5 Handle
  */
hal_exti_handle_t *mx_gpio_default_exti5_gethandle(void);

/**
  * @brief  Get the EXTI12 object.
  * @retval Pointer on the EXTI12 Handle
  */
hal_exti_handle_t *mx_gpio_default_exti12_gethandle(void);

/**
  * @brief  Get the EXTI14 object.
  * @retval Pointer on the EXTI14 Handle
  */
hal_exti_handle_t *mx_gpio_default_exti14_gethandle(void);

/**
  * @brief  Get the EXTI13 object.
  * @retval Pointer on the EXTI13 Handle
  */
hal_exti_handle_t *mx_gpio_default_exti13_gethandle(void);

/**
  * @brief  Get the EXTI15 object.
  * @retval Pointer on the EXTI15 Handle
  */
hal_exti_handle_t *mx_gpio_default_exti15_gethandle(void);

/**
  * @brief  Get the EXTI2 object.
  * @retval Pointer on the EXTI2 Handle
  */
hal_exti_handle_t *mx_gpio_default_exti2_gethandle(void);

/******************************************************************************/
/*                            EXTI Line4 interrupt                            */
/******************************************************************************/
void EXTI4_IRQHandler(void);

/******************************************************************************/
/*                           EXTI Line12 interrupt                            */
/******************************************************************************/
void EXTI12_IRQHandler(void);

/******************************************************************************/
/*                            EXTI Line5 interrupt                            */
/******************************************************************************/
void EXTI5_IRQHandler(void);

/******************************************************************************/
/*                           EXTI Line14 interrupt                            */
/******************************************************************************/
void EXTI14_IRQHandler(void);

/******************************************************************************/
/*                           EXTI Line13 interrupt                            */
/******************************************************************************/
void EXTI13_IRQHandler(void);

/******************************************************************************/
/*                           EXTI Line15 interrupt                            */
/******************************************************************************/
void EXTI15_IRQHandler(void);

/******************************************************************************/
/*                            EXTI Line2 interrupt                            */
/******************************************************************************/
void EXTI2_IRQHandler(void);

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* MX_GPIO_DEFAULT_H */
