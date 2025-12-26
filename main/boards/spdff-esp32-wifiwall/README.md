# WifiWall
淘宝电子垃圾WifiWall

## 🛠️ 编译指南
**开发环境**：ESP-IDF v5.4.0

### 编译步骤：
> ⚠️ **提示**：若在编译过程中访问在线库失败，可以尝试切换加速器状态，或修改 [idf_component.yml] 文件，替换为国内镜像源。

1. 使用 VSCode 打开项目文件夹；
2. 清除工程（Clean Project）；
3. 设置 ESP-IDF 版本为 `v5.4.0`；
4. 设置目标设备为 `[esp32] -> [Uart]`；
5. 打开 **SDK Configuration Editor**；
6. 配置自定义分区表路径为：`partitions/v1/4m.csv`；
7. 设置 **Board Type** 为 **WifiWall**；
8. 保存配置并开始编译;
9. 使用microUSB线连接WifiWall至电脑；
10. 选择正确的COM口；
11. Flash Device。

# WIFIWALL IO

## 充电芯片：LHT7

充电检测 --> GPIO33

电压检测 --> ACD1 ADC_CHANNEL_7

## 按键

侧边复位按键 RST --> STM32 pin9(CHIP_PU)

正面左侧按键  --> GPIO34

正面右侧按键  --> GPIO39

## 数字功放：NS4150

pin3  -->  GPIO25

## LED

空焊盘  --> GPIO22

空电阻  --> STM32 VCC

## Flash 25Q32

pin1  --> STM32 pin33

pin2  --> STM32 pin31

pin3  --> STM32 pin28

pin5  --> STM32 pin30

pin6  --> STM32 pin32

pin7  --> STM32 pin29

## TF

pin1  --> GPIO12
pin2  --> GPIO13

pin3  --> GPIO15

pin4  --> VCC

pin5  --> GPIO14

pin6  --> GND

pin7  --> GPIO2

pin8  --> GPIO4

## LCD

pin1  --> GND

pin2  --> GND

pin3 | LEDK  --> GPIO17

pin4 | LEDA  --> VCC

pin5  --> GND

pin6 | RST --> GPIO16

pin7 | DC  --> GPIO23

pin8 | MOSI  --> GPIO19

pin9 | CLK  --> GPIO18

pin10 --> VCC

pin11  --> VCC

pin12 | CS  --> GPIO5

pin13  --> GND

pin14  --> GND

# 硬件改造

## 拆掉TF卡

## 小智接线

#define AUDIO_I2S_MIC_GPIO_WS   GPIO_NUM_14

#define AUDIO_I2S_MIC_GPIO_SCK  GPIO_NUM_2

#define AUDIO_I2S_MIC_GPIO_DIN  GPIO_NUM_4

#define AUDIO_I2S_SPK_GPIO_DOUT GPIO_NUM_12

#define AUDIO_I2S_SPK_GPIO_BCLK GPIO_NUM_13

#define AUDIO_I2S_SPK_GPIO_LRCK GPIO_NUM_15
