#pragma once
#include <Arduino.h>
#include "driver/gpio.h"
#include "soc/gpio_struct.h"

// UGREEN KVM (AiP1617 / TM1617) EMULATOR — Phase 2.
// The ESP is the SOLE chip on the bus (real remote REMOVED). It reads every
// transaction (tracking active input from 0xC0) and DRIVES the 5-byte response
// to the 0x42 key-scan read so it can "press" buttons and switch inputs.
//
// Bus facts (from logic-analyzer decode of this exact KVM):
//   CPOL=1/CPHA=1: data changes just after CLK falling, sampled on CLK rising.
//   => As slave we DRIVE each response bit on the CLK FALLING edge; the KVM
//      samples it on the following RISING edge. LSB-first, 8-bit, STB active-low.
//   0x42 read response = 00 00 <byte3> <byte4> 00, with the key bit set:
//      input1 -> byte3 bit0 (0x01)   input2 -> byte3 bit3 (0x08)
//      input3 -> byte4 bit0 (0x01)   input4 -> byte4 bit3 (0x08)
//   0xC0 <bitmap> tells us the ACTIVE input: 0x10/0x20/0x40/0x80 = in 1/2/3/4.
//
// Pins (Olimex ESP32-PoE UEXT): STB=GPIO14, CLK=GPIO15, DIO=GPIO13.

#define KVM_STB 14
#define KVM_CLK 15
#define KVM_DIO 13
#define DIO_MASK (1u << KVM_DIO)

namespace kvm {

// ---- control (set from the ESPHome main loop) ----
volatile bool emulation_enabled = false;  // false = drive all-zeros (safe, no switching)
volatile int  target_input      = 0;      // 1..4 desired input; 0 = idle/none

// ---- decode + drive state ----
volatile bool     in_tx        = false;
volatile bool     driving      = false;   // in the 0x42 response window?
volatile uint8_t  cur_byte     = 0;
volatile uint8_t  bit_count    = 0;
volatile uint8_t  byte_index   = 0;
volatile bool     is_led_cmd   = false;
volatile uint8_t  last_cmd     = 0;
volatile uint8_t  led_bitmap   = 0;
volatile uint8_t  led_pending  = 0;       // for two-read confirm filter
volatile uint32_t tx_count     = 0;
volatile uint32_t key_tx_count = 0;
volatile uint8_t  resp[5]      = {0,0,0,0,0};

inline int active_input() {
  switch (led_bitmap) {
    case 0x10: return 1; case 0x20: return 2;
    case 0x40: return 3; case 0x80: return 4; default: return 0;
  }
}

// --- direct-register GPIO (IRAM-safe, no flash calls in the ISR) ---
static inline void    dio_input()        { GPIO.enable_w1tc = DIO_MASK; }   // hi-Z (release)
static inline void    dio_output()       { GPIO.enable_w1ts = DIO_MASK; }   // drive
static inline void    dio_write(uint8_t b){ if (b) GPIO.out_w1ts = DIO_MASK; else GPIO.out_w1tc = DIO_MASK; }
static inline uint8_t dio_read()         { return (GPIO.in >> KVM_DIO) & 1u; }
static inline uint8_t clk_hi()           { return (GPIO.in >> KVM_CLK) & 1u; }
static inline uint8_t stb_hi()           { return (GPIO.in >> KVM_STB) & 1u; }

// Build the response for a key-read. All zeros unless we should assert a press.
inline void compute_resp() {
  uint8_t b3 = 0, b4 = 0;
  if (emulation_enabled && target_input >= 1 && target_input <= 4
      && active_input() != target_input) {          // press until KVM confirms
    switch (target_input) {
      case 1: b3 = 0x01; break;
      case 2: b3 = 0x08; break;
      case 3: b4 = 0x01; break;
      case 4: b4 = 0x08; break;
    }
  }
  resp[0] = 0; resp[1] = 0; resp[2] = b3; resp[3] = b4; resp[4] = 0;
}

void IRAM_ATTR on_stb() {
  if (stb_hi() == 0) {                 // falling: transaction start
    in_tx = true; driving = false;
    cur_byte = 0; bit_count = 0; byte_index = 0; is_led_cmd = false;
    dio_input();                       // never drive during the command byte
    compute_resp();                    // latch the response for this frame
  } else {                             // rising: transaction end
    in_tx = false; driving = false;
    dio_input();                       // always release between transactions
    tx_count++;
  }
}

void IRAM_ATTR on_clk() {
  if (!in_tx) return;
  if (clk_hi()) {
    // ---- RISING: sample point ----
    if (!driving) {                    // reading command / data bytes
      if (dio_read()) cur_byte |= (uint8_t)(1u << bit_count);
    }
    if (++bit_count == 8) {
      uint8_t b = cur_byte;
      if (byte_index == 0) {           // command byte just completed
        last_cmd = b;
        is_led_cmd = (b == 0xC0);
        if (b == 0x42) {               // key read -> drive the response next
          key_tx_count++;
          driving = true;              // content is zeros unless emulation_enabled
          dio_output();
        }
      } else if (is_led_cmd && byte_index == 1) {
        // Corruption filter: only accept a VALID single-input bitmap, and only
        // after seeing the same value twice in a row. Garbage reads (bit-slips)
        // produce invalid or one-off values that never commit -> no flapping.
        uint8_t v = b;
        if (v == 0x10 || v == 0x20 || v == 0x40 || v == 0x80) {
          if (v == led_pending) led_bitmap = v;   // confirmed -> commit
          else                  led_pending = v;  // first sighting -> wait
        }
      }
      byte_index++; cur_byte = 0; bit_count = 0;
      if (driving && byte_index >= 6) { // drove all 5 response bytes -> release
        driving = false; dio_input();
      }
    }
  } else {
    // ---- FALLING: drive the next response bit (only in the 0x42 window) ----
    if (driving && byte_index >= 1 && byte_index <= 5) {
      dio_write((resp[byte_index - 1] >> bit_count) & 1u);
    }
  }
}

void begin() {
  pinMode(KVM_STB, INPUT);
  pinMode(KVM_CLK, INPUT);
  // DIO: input buffer + output driver both available; start released (hi-Z).
  gpio_config_t io = {};
  io.pin_bit_mask = 1ULL << KVM_DIO;
  io.mode         = GPIO_MODE_INPUT_OUTPUT;   // readable AND drivable
  io.pull_up_en   = GPIO_PULLUP_DISABLE;
  io.pull_down_en = GPIO_PULLDOWN_DISABLE;
  io.intr_type    = GPIO_INTR_DISABLE;
  gpio_config(&io);
  dio_input();                                 // output driver off to start
  attachInterrupt(digitalPinToInterrupt(KVM_STB), on_stb, CHANGE);
  attachInterrupt(digitalPinToInterrupt(KVM_CLK), on_clk, CHANGE);
}

} // namespace kvm
