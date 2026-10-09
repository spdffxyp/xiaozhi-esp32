# ZL38063 bring-up — what (a) 0x40203350, (b) 0x40207ae8, (c) 0x40124ce8 do
Method: byte-exact stepping (tools\zl_step.py) + objdump on backup\disasm\b220_full.elf (its .symtab
carries the ROM names); all addresses below are app addresses in IROM/IRAM2.
## (a) 0x40203350 = xTimerStop(t1,0); xTimerStop(t2,0)   [no hardware access]
40203353 a8=&0x3ffc5558; 4020335e a10=*(0x3ffc5558); 4020335c a11=3; 40203356 a14=0 (xTicksToWait);
40203358 a13=0 (pWoken=NULL); 4020335a a12=0 (value); 40203360/63 callx8 0x400928d8;
4020336a..40203376 same with a10=*(0x3ffc5554); 40203379 retw.
0x400928d8 = FreeRTOS xTimerGenericCommand: reads timer queue *(0x3ffc3544), builds
{xMessageID=a3, xMessageValue=a4, xTimerHandle=a2} on the stack (400928e4-e8); cmd<6 ->
xQueueGenericSend 0x400906b8 (assert "xQueueGenericSend"@0x3f406935, queue.c:0x2cf), else
xQueueGenericSendFromISR 0x4009088c ("xQueueGenericSendFromISR"@0x3f406901, :0x485); timeout
zeroed unless xTaskGetSchedulerState()==2 (400928ee-f8). Queue+task created at 0x40092860
(xTimerCreateTimerTask: "Tmr Svc"@0x3f406d33, entry 0x40092988=prvTimerTask, prio 6, stack 0xa00;
timers.c strings 0x3f406cef-0x3f406db4). cmd 3 = tmrCOMMAND_STOP  =>  two xTimerStop calls.
Handles are LCD_SERVICE timers: the same globals are START/RESET (cmd 1/2) at 0x4020337c and
DELETE (cmd 5) at 0x40202e2e/0x40202e41 ("LCDDeactive"); tag "LCD_SERVICE"@0x3f4bd760.
Zero GPIO/I2C/SPI/MMIO writes in (a) or in its callees (all FreeRTOS queue code).
## (b) 0x40207ae8 = audio-PA enable via AW9106 reg 0x02; board_sub_init calls it with 0 (PA OFF)
arg a2=a10&0xff (40207af9); log "...: audio_pa_on=%d"@0x3f5b4673, tag "aw9106"@0x3f5b4620.
40207b10 bnei a2,1: ==1 -> byte=0x02 (40207b13-15); !=1 -> byte=0x00 (40207b20-23).
40207b29-b31 aw9106_write(dev=0x5B, reg=0x02, &byte, 1) -> helper 0x40207770: 4020779d
i2c_master_write_byte(0x5B<<1=0xB6), 402077aa reg 0x02, 402077bb i2c_master_write(&byte,1) on
I2C0 (SDA18/SCL23, 100 kHz) => START 0xB6 0x02 <byte> STOP. Site 4012445d sets a10=0 -> 0x02<-0x00.
PA ON (0x02<-0x02) only in the playback path: 401253ed (movi a10,1) before audio start, also 4012c21e.
Init 402078dc already leaves GPMD_A(0x12)=0x02 and GPIO_OUTPUT_A(0x02)=0x00.
## (c) 0x40124ce8 = gpio_set_intr_type(38, GPIO_INTR_DISABLE)   [no level/rail change]
40124ceb a11=0 (type); 40124cee a10=38 (gpio_num); 40124cf1 call8 0x40176028.
0x40176028 = gpio_set_intr_type: __FUNCTION__ "gpio_set_intr_type"@0x3f46ae90, "GPIO interrupt
type error"@0x3f46a46d, type<6=GPIO_INTR_MAX @40176050, GPIO_PIN_MUX_REG[pin] test @40176030.
Write 40176086-9d: GPIO_PINn_REG (0x3ff44088+4n; n=38 -> 0x3ff44120) &= 0xfffffc7f, |= (0&7)<<7
=> INT_TYPE[9:7]=0. No output level, no interrupt enable, no I2C/SPI.
## VERDICT: no rail/enable is set by (a)(b)(c) — neither audio nor LCD
(a) is pure FreeRTOS timer bookkeeping (touches no hardware).
(b) is called with 0 and drives the one group-A GPIO bit LOW = exactly our current 0x02<-0x00.
(c) only clears an interrupt-type field on GPIO38, which is input-only on ESP32 (an interrupt/
HINT-style input line, not a power enable).
The only DSP-affecting step of board_sub_init is the reset pulse 0x40124418: memset cfg;
pin_bit_mask=0x80000 (1<<19); mode=2 (GPIO_MODE_OUTPUT); gpio_config 0x4017643c; gpio_set_level(19,0)
0x401761c0; vTaskDelay(10) 0x40091774; gpio_set_level(19,1). Then vTaskDelay(500) and up to 6 calls
of 0x40126240(0). => A silent DSP on HBI is NOT explained by these three calls: re-check the GPIO19
pulse, HBI SPI mode/CS/pins, and the DSP reset/enable pin carried in the DSP bus config (below).
For sound only, our FW must add: I2C0 write AW9106 0x5B reg 0x02 <- 0x02 before playback (0x00=mute);
no ordering/delay dependency on the DSP reset.
## Group-A bits driven (item 4)
Only (b) writes group A; it writes the whole 8-bit reg 0x02: bit1=0x02 = PA enable, other bits 0.
From board_sub_init: reg 0x02 <- 0x00 (OUT_A1 LOW). Playback: reg 0x02 <- 0x02 (OUT_A1 HIGH).
(a) and (c) write no AW9106 registers at all.
## Uncertainty
HIGH: (a)=xTimerGenericCommand with cmd 3 (tmrCOMMAND_STOP); (b) byte=2 iff arg==1 and the call args;
(c)=gpio_set_intr_type(38,0). MEDIUM-HIGH: the two handles are LCD_SERVICE timers.
MEDIUM: OUT_A1 is the PA EN and is active-high (an active-low amp EN would invert, unlikely vs the
log text); GPIO38 is an input line. LOW-MEDIUM: a DSP enable/reset hides in the bus-config GPIO fields.
## DSP HBI pins: not resolved (lead)
The only 400 kHz SPI device init (0x40234f90; spi_bus_add_device clock 0x61a80 at 40234fe4) is reached
from device_register 0x401fd894 only when descriptor+0==8; its sole call site (0x40125208, function at
0x401251ac) passes descriptor 0x3f400f54 patched to +0=1 with pins {+0 miso:-1,+4 mosi:-1,+8 sclk:1,
+12 rst:0,+16/+20 out GPIO0|GPIO5}, i.e. that registration is not the DSP. The layout the DSP path
would consume: +0 miso, +4 mosi, +8 sclk, +12 reset/enable (made an output at 4023501c-40235036 via
__ashldi3 0x4000c818 + gpio_config 0x4017643c), +16/+20 two more outputs (4023507e-402350e7), +24 DMA.
MEDIUM-LOW confidence that one of +12/+16/+20 is the enable our firmware is missing.
