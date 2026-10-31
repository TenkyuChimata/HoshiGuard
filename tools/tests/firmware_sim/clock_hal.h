#pragma once

// 時間・電気動作ではなく、HAL 呼出しと読戻し不一致を検証する境界。
#ifndef HSE_VALUE
#define HSE_VALUE 8000000U
#endif
#ifndef F_CPU
#define F_CPU 72000000UL
#endif
#define WEAK __attribute__((weak))
#define __IO volatile
#define RCC_CR_HSION 1U
#define RCC_CR_HSEON (1U<<16)
#define RCC_CR_HSEBYP (1U<<18)
#define RCC_CR_CSSON (1U<<19)
#define RCC_CR_PLLON (1U<<24)
#define RCC_CFGR_SW_PLL 2U
#define RCC_CFGR_SWS_PLL 8U
#define RCC_CFGR_HPRE (15U<<4)
#define RCC_CFGR_PPRE1 (7U<<8)
#define RCC_CFGR_PPRE1_DIV2 (4U<<8)
#define RCC_CFGR_ADCPRE (3U<<14)
#define RCC_CFGR_ADCPRE_DIV6 (2U<<14)
#define RCC_CFGR_PLLXTPRE (1U<<17)
#define RCC_CFGR_PLLMULL (15U<<18)
#define RCC_CFGR_PLLMULL9 (7U<<18)
#define RCC_CFGR_USBPRE (1U<<22)
#define RCC_APB2ENR_USART1EN (1U<<14)
#define RCC_APB1ENR_USART2EN (1U<<17)
#define RCC_APB1ENR_I2C1EN (1U<<21)
#define FLASH_ACR_LATENCY 7U
#define FLASH_ACR_HLFCYA 8U
#define FLASH_ACR_PRFTBE 16U
#define FLASH_LATENCY_2 2U
#define FLASH_LATENCY_1 1U
#define RCC_OSCILLATORTYPE_HSE 1U
#define RCC_OSCILLATORTYPE_HSI 2U
#define RCC_HSE_ON RCC_CR_HSEON
#define RCC_HSE_BYPASS (RCC_CR_HSEON|RCC_CR_HSEBYP)
#define RCC_HSI_ON 1U
#define RCC_HSE_PREDIV_DIV1 0U
#define RCC_HSICALIBRATION_DEFAULT 16U
#define RCC_PLL_ON 2U
#define RCC_PLLSOURCE_HSE RCC_CFGR_PLLSRC
#define RCC_PLLSOURCE_HSI_DIV2 0U
#define RCC_PLL_MUL9 RCC_CFGR_PLLMULL9
#define RCC_PLL_MUL12 (10U<<18)
#define RCC_PLL_MUL16 (14U<<18)
#define RCC_CLOCKTYPE_SYSCLK 1U
#define RCC_CLOCKTYPE_HCLK 2U
#define RCC_CLOCKTYPE_PCLK1 4U
#define RCC_CLOCKTYPE_PCLK2 8U
#define RCC_SYSCLKSOURCE_PLLCLK 2U
#define RCC_SYSCLK_DIV1 0U
#define RCC_HCLK_DIV2 RCC_CFGR_PPRE1_DIV2
#define RCC_HCLK_DIV1 0U
#define RCC_PERIPHCLK_ADC 1U
#define RCC_PERIPHCLK_USB 2U
#define RCC_ADCPCLK2_DIV6 RCC_CFGR_ADCPRE_DIV6
#define RCC_USBCLKSOURCE_PLL_DIV1_5 0U
#define RCC_USBCLKSOURCE_PLL RCC_CFGR_USBPRE
#define RCC_IT_CSS (1U<<7)
struct RCC_OscInitTypeDef {
  uint32_t OscillatorType,HSEState,HSEPredivValue,HSIState,HSICalibrationValue,LSIState,LSEState;
  struct {uint32_t PLLState,PLLSource,PLLMUL;} PLL;
};
struct RCC_ClkInitTypeDef {uint32_t ClockType,SYSCLKSource,AHBCLKDivider,APB1CLKDivider,APB2CLKDivider;};
struct RCC_PeriphCLKInitTypeDef {uint32_t PeriphClockSelection,AdcClockSelection,UsbClockSelection;};
struct SimFlash { uint32_t ACR=0U; };
struct SimSysTick { uint32_t CTRL=7U,LOAD=47999U,VAL=0U; };
struct SimUsart { uint32_t CR1=0U; };
struct SimI2c { uint32_t CR1=0U,DR=0U,SR1=0U,SR2=0U,CR2=0U,CCR=0U,TRISE=0U,OAR1=0U; };
extern uint32_t SystemCoreClock;
enum HAL_StatusTypeDef {HAL_OK=0,HAL_ERROR=1};
namespace sim {
extern SimFlash flash;
extern SimSysTick systick;
extern SimUsart usart1,usart2;
extern SimI2c i2c1;
extern RCC_OscInitTypeDef oscillatorRequest;
extern RCC_ClkInitTypeDef clockRequest;
extern RCC_PeriphCLKInitTypeDef peripheralRequest;
extern unsigned oscillatorCalls,clockCalls,peripheralCalls;
extern const char *clockFault;
extern bool irqDisabled;
extern unsigned i2cBegins;
struct ClockStopped {};
}
#define FLASH (&sim::flash)
#define SysTick (&sim::systick)
#define USART1 (&sim::usart1)
#define USART2 (&sim::usart2)
#define I2C1 (&sim::i2c1)
#define GPIOB (&sim::ports[1])
#define GPIO_PIN_6 (1U<<6)
#define GPIO_PIN_7 (1U<<7)
#define GPIO_MODE_AF_OD 5U
#define GPIO_SPEED_FREQ_HIGH 3U
#define I2C_CR1_START (1U<<8)
#define I2C_CR1_STOP (1U<<9)
#define I2C_CR1_PE 1U
#define I2C_CR1_SWRST (1U<<15)
#define I2C_SR1_SB 1U
#define I2C_SR1_ADDR (1U<<1)
#define I2C_SR1_BTF (1U<<2)
#define I2C_SR1_TXE (1U<<7)
#define I2C_SR1_BERR (1U<<8)
#define I2C_SR1_ARLO (1U<<9)
#define I2C_SR1_AF (1U<<10)
#define I2C_SR1_OVR (1U<<11)
#define I2C_SR1_TIMEOUT (1U<<14)
#define I2C_SR2_BUSY (1U<<1)
#define I2C_CCR_FS (1U<<15)
#define __HAL_RCC_GPIOB_CLK_ENABLE() ((void)0)
#define __HAL_RCC_I2C1_CLK_ENABLE() ((RCC->APB1ENR |= RCC_APB1ENR_I2C1EN),++sim::i2cBegins)
#define __HAL_RCC_I2C1_CLK_DISABLE() (RCC->APB1ENR &= ~RCC_APB1ENR_I2C1EN)
#define __HAL_AFIO_REMAP_I2C1_DISABLE() ((void)0)
#define __HAL_RCC_I2C1_FORCE_RESET() (sim::i2c1=SimI2c())
#define __HAL_RCC_I2C1_RELEASE_RESET() ((void)0)
inline void HAL_GPIO_DeInit(GPIO_TypeDef *,uint32_t) {}
#define __HAL_FLASH_PREFETCH_BUFFER_ENABLE() (FLASH->ACR |= FLASH_ACR_PRFTBE)
#define __HAL_RCC_GET_IT(flag) (RCC->CIR & (flag))
#define __HAL_RCC_CLEAR_IT(flag) (RCC->CIR &= ~(flag))
inline void __disable_irq() { sim::irqDisabled=true; }
inline void __WFI() { throw sim::ClockStopped(); }
inline uint32_t HAL_RCC_GetPCLK1Freq() {return sim::hclk/2U;}
HAL_StatusTypeDef HAL_RCC_OscConfig(RCC_OscInitTypeDef *request);
HAL_StatusTypeDef HAL_RCC_ClockConfig(RCC_ClkInitTypeDef *request,uint32_t latency);
HAL_StatusTypeDef HAL_RCCEx_PeriphCLKConfig(RCC_PeriphCLKInitTypeDef *request);
void HAL_RCC_GetClockConfig(RCC_ClkInitTypeDef *out,uint32_t *latency);
inline void HAL_RCC_EnableCSS() {RCC->CR |= RCC_CR_CSSON;}
inline void Error_Handler() {throw sim::ClockStopped();}
extern "C" void SystemClock_Config(void);
extern "C" void NMI_Handler(void);
