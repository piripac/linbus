// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//    http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "serial_printer.h"
#include "avr_util.h"
#include "hardware_clock.h"
#include "system_clock.h"
#include "lin_decoder.h"
#include "io_pins.h"
#include "action_led.h"

// Config pin. Sampled once during initialization does not change value
// after that. Using alternative configuration when pin is low.
static io_pins::ConfigInputPin alt_config_pin(PORTB, 2);

// Auxilary LEDs.
static ActionLed status1_led(PORTD, 6);
static io_pins::OutputPin status2_led(PORTD, 7);

// Action LEDs. Indicates activity by blinking. Require periodic calls to
// update().
static ActionLed frame_led(PORTB, 0);
static ActionLed error_led(PORTB, 1);

// Arduino standard LED.
static io_pins::OutputPin led(PORTB, 5);

// Compute frame checksum. Assuming buffer is not empty.
static uint8 frameChecksum(const lin_decoder::RxFrameBuffer& buffer) {
  // LIN V1 and V2 have slightly different checksum formulas.
  static const boolean kV2Checksum = alt_config_pin.isHigh();

  // LIN V2 includes ID byte in checksum, V1 does not.
  // Per the assumption above, we have at least one byte.
  const uint8 startByte = kV2Checksum ? 0 : 1;
  const uint8* p = &buffer.bytes[startByte];
  // Exclude also the checksum at the end.
  uint8 nBytes = buffer.num_bytes - (startByte + 1);

  // Sum bytes. We should not have 16 bit overflow here since the frame has a limited size.
  uint16 sum = 0;
  while (nBytes-- > 0) {
    sum += *(p++);
  }

  // Keep adding the high and low bytes until no carry.
  for (;;) {
    const uint8 highByte = (uint8)(sum >> 8);
    if (!highByte) {
      break;  
    }
    // NOTE: this can add additional carry.  
    sum = (sum & 0xff) + highByte; 
  }

  return (uint8)(~sum);
}

// Replace the 2 msb bits with checksum of the node ID in the 6 lsb bits per the
// LINBUS spec.
static uint8 setIdChecksumBits(uint8 id) {
  // Algorithm is optimized for CPU time (avoiding individual shifts per id bit).
  // Using registers for the two checksum bits. P1 is computed in bit 7 of p1_at_b7 
  // and p0 is comptuted in bit 6 of p0_at_b6.
  uint8 p1_at_b7 = ~0;
  uint8 p0_at_b6 = 0;
  
  // P1: id5, P0: id4
  uint8 shifter = id << 2;
  p1_at_b7 ^= shifter;
  p0_at_b6 ^= shifter;

  // P1: id4, P0: id3
  shifter += shifter;
  p1_at_b7 ^= shifter;
  
  // P1: id3, P0: id2
  shifter += shifter;
  p1_at_b7 ^= shifter;
  p0_at_b6 ^= shifter;

  // P1: id2, P0: id1
  shifter += shifter;
  p0_at_b6 ^= shifter;

  // P1: id1, P0: id0
  shifter += shifter;
  p1_at_b7 ^= shifter;
  p0_at_b6 ^= shifter;
  
  return (p1_at_b7 & 0b10000000) | (p0_at_b6 & 0b01000000) | (id & 0b00111111);
}

// We consider a single byte frame with ID only and no slave response as valid.
static boolean isFrameValid(const lin_decoder::RxFrameBuffer& buffer) {
  const uint8 n = buffer.num_bytes;
  
  // Check frame size.
  // One ID byte with optional 1-8 data bytes and 1 checksum byte.
  // TODO: should we enforce only 1, 2, 4, or 8 data bytes?  (total size 
  // 1, 3, 4, 6, or 10)
  if (n != 1 && (n < 3 || n > 10)) {
    SerialPrinter.println("x1");
    return false;
  }
  
  // Check ID byte checksum bits.
  const uint8 id_byte = buffer.bytes[0];
  if (id_byte != setIdChecksumBits(id_byte)) {
    // TODO: remove after stabilization.
    // SerialPrinter.printHexByte(id_byte);
    // SerialPrinter.print(" vs. ");
    // SerialPrinter.printHexByte(setIdChecksumBits(id_byte));
    // SerialPrinter.println();
    return false;
  }
   
  // If not an ID only frame, check also the overall checksum.
  if (n > 1) {
    if (buffer.bytes[n - 1] != frameChecksum(buffer)) {
      return false;
    }  
  }
  // TODO: check protected id.
  return true;
}

void setup()
{
  // If alt config pin is pulled low, using alternative config..
  const boolean alt_config = alt_config_pin.isLow();
  
  // We don't want interrupts from timer 2.
  avr_util::timer0_off();

  // Uses Timer1, no interrupts.
  hardware_clock::setup();

  // Hard coded to 115.2k baud. Uses URART0, no interrupts.
  SerialPrinter.setup();

  // Uses Timer2 with interrupts, and a few i/o pins. See code for details.
  lin_decoder::setup(alt_config ? 9600 : 19200);

  // Enable global interrupts. We expect to have only timer1 interrupts by
  // the lin decoder to reduce ISR jitter.
  sei(); 

  SerialPrinter.println(alt_config ? F("Started (alt config).") : F("Started (std config)."));
}

// This is a quick loop that does not use delay() or other busy loops or blocking calls.
// The iterations are are at the order of 60usec.
void loop()
{
  // Having our own loop shaves about 4 usec per iteration. It also eliminate
  // any underlying functionality that we may not want.
  for(;;) {
    // Periodic updates.
    system_clock::loop();
    SerialPrinter.loop();
    status1_led.loop();
    frame_led.loop();
    error_led.loop();

    // Heartbeat LED blink.
    {
      static PassiveTimer heart_beat_timer;
      if (heart_beat_timer.timeMillis() >= 3000) {
        status1_led.action(); 
        heart_beat_timer.restart(); 
      }
    }

    // Generate periodic messages if no activiy.
    static PassiveTimer periodic_watchdog;
    if (periodic_watchdog.timeMillis() >= 5000) {
      SerialPrinter.println(F("waiting..."));
      periodic_watchdog.restart();
    }

    // Handle LIN errors.
    if (lin_decoder::getAndClearErrorFlag()) {
      error_led.action();
    }

    // Handle recieved LIN frames.
    lin_decoder::RxFrameBuffer buffer;
    if (readNextFrame(&buffer)) {
      const boolean frameOk = isFrameValid(buffer);
      if (frameOk) {
        frame_led.action();
      } 
      else {
        error_led.action();
      }
      // Dump frame.
      for (int i = 0; i < buffer.num_bytes; i++) {
        if (i > 0) {
          SerialPrinter.print(' ');  
        }
        SerialPrinter.printHexByte(buffer.bytes[i]);  
      }
      if (!frameOk) {
        SerialPrinter.print(F(" ERR"));
      }
      SerialPrinter.println();  
      periodic_watchdog.restart(); 
    }
  }
}



