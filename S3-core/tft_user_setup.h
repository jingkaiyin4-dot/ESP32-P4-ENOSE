/**
 * tft_user_setup.h — ST7789 240x280 配置 for XIAO ESP32-S3
 * 
 * TFT_eSPI User_Setup 自定义配置
 * 此文件在编译时被 TFT_eSPI 库引用
 * 
 * 屏幕: 1.69寸 IPS ST7789 240x280 SPI
 * 
 * XIAO ESP32-S3 引脚分配:
 *   D8  (GPIO18) → TFT_SCK
 *   D10 (GPIO21) → TFT_MOSI (SDA)
 *   D9  (GPIO17) → TFT_CS
 *   D3  (GPIO6)  → TFT_DC  (RS)
 *   D2  (GPIO5)  → TFT_RST
 *   D4  (GPIO7)  → TFT_BL  (背光)
 */

// 驱动芯片
#define ST7789_DRIVER

// 分辨率 (严格 240x280)
#define TFT_WIDTH  240
#define TFT_HEIGHT 280

// 颜色顺序 (IPS屏通常是BGR)
#define TFT_BGR 1

// SPI引脚
#define TFT_MOSI 21
#define TFT_SCLK 18
#define TFT_CS   17
#define TFT_DC    6
#define TFT_RST   5

// 背光
#define TFT_BL    7

// SPI频率 (ST7789支持最高80MHz, 用40MHz更稳定)
#define SPI_FREQUENCY       40000000
#define SPI_READ_FREQUENCY  20000000
#define SPI_TOUCH_FREQUENCY  2500000

// 不使用触控
#define TOUCH_CS -1

// 加载自定义配置
#define USER_SETUP_LOADED
