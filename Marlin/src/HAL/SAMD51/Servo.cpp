/**
 * Marlin 3D Printer Firmware
 * Copyright (c) 2020 MarlinFirmware [https://github.com/MarlinFirmware/Marlin]
 *
 * Based on Sprinter and grbl.
 * Copyright (c) 2011 Camiel Gubbels / Erik van der Zalm
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 */

/**
 * SAMD51 HAL developed by Giuliano Zaro (AKA GMagician)
 */

/**
 * This comes from Arduino library which at the moment is buggy and uncompilable
 */

#ifdef __SAMD51__

#include "../../inc/MarlinConfig.h"

#if HAS_SERVOS

#include "../shared/servo.h"
#include "../shared/servo_private.h"
#include "SAMD51.h"

#define __TC_GCLK_ID(t)         TC##t##_GCLK_ID
#define _TC_GCLK_ID(t)          __TC_GCLK_ID(t)
#define TC_GCLK_ID              _TC_GCLK_ID(SERVO_TC)

#define _TC_PRESCALER(d)        TC_CTRLA_PRESCALER_DIV##d##_Val
#define TC_PRESCALER(d)         _TC_PRESCALER(d)

#define __SERVO_IRQn(t)         TC##t##_IRQn
#define _SERVO_IRQn(t)          __SERVO_IRQn(t)
#define SERVO_IRQn              _SERVO_IRQn(SERVO_TC)

#define HAL_SERVO_TIMER_ISR()   TC_HANDLER(SERVO_TC)

#define TIMER_TCCHANNEL(t)      ((t) & 1)
#define TC_COUNTER_START_VAL    0xFFFF

static volatile int8_t currentServoIndex[_Nbr_16timers];    // index for the servo being pulsed for each timer (or -1 if refresh interval)

FORCE_INLINE static uint16_t getTimerCount() {
  Tc * const tc = timer_config[SERVO_TC].pTc;

  tc->COUNT16.CTRLBSET.reg = TC_CTRLBCLR_CMD_READSYNC;
  SYNC(tc->COUNT16.SYNCBUSY.bit.CTRLB || tc->COUNT16.SYNCBUSY.bit.COUNT);

  return tc->COUNT16.COUNT.reg;
}

// ----------------------------
// Interrupt handler for the TC
// ----------------------------
HAL_SERVO_TIMER_ISR() {
  Tc * const tc = timer_config[SERVO_TC].pTc;
  const timer16_Sequence_t timer =
    #ifndef _useTimer1
      _timer2
    #elif !defined(_useTimer2)
      _timer1
    #else
      (tc->COUNT16.INTFLAG.reg & tc->COUNT16.INTENSET.reg & TC_INTFLAG_MC0) ? _timer1 : _timer2
    #endif
  ;
  const uint8_t tcChannel = TIMER_TCCHANNEL(timer);

  int8_t cho = currentServoIndex[timer];                // Handle the prior servo first
  if (cho < 0) {                                        // Servo -1 indicates the refresh interval completed...
    #if defined(_useTimer1) && defined(_useTimer2)
      if (currentServoIndex[timer ^ 1] >= 0) {
        // Wait for both channels
        // Clear the interrupt
        tc->COUNT16.INTFLAG.reg = (tcChannel == 0) ? TC_INTFLAG_MC0 : TC_INTFLAG_MC1;
        return;
      }
    #endif
    tc->COUNT16.COUNT.reg = TC_COUNTER_START_VAL;       // ...so reset the timer
    SYNC(tc->COUNT16.SYNCBUSY.bit.COUNT);
  }
  else if (SERVO_INDEX(timer, cho) < ServoCount)        // prior channel handled?
    digitalWrite(SERVO(timer, cho).Pin.nbr, LOW);       // pulse the prior channel LOW

  currentServoIndex[timer] = ++cho;                     // go to the next channel (or 0)
  if (cho < SERVOS_PER_TIMER && SERVO_INDEX(timer, cho) < ServoCount) {
    if (SERVO(timer, cho).Pin.isActive)                 // activated?
      digitalWrite(SERVO(timer, cho).Pin.nbr, HIGH);    // yes: pulse HIGH

    tc->COUNT16.CC[tcChannel].reg = getTimerCount() - (uint16_t)SERVO(timer, cho).ticks;
  }
  else {
    // finished all channels so wait for the refresh period to expire before starting over
    currentServoIndex[timer] = -1;                                          // reset the timer COUNT.reg on the next call
    const uint16_t cval = getTimerCount() - 256 / (SERVO_TIMER_PRESCALER),  // allow 256 cycles to ensure the next CV not missed
                   ival = (TC_COUNTER_START_VAL) - (uint16_t)usToTicks(REFRESH_INTERVAL); // at least REFRESH_INTERVAL has elapsed
    tc->COUNT16.CC[tcChannel].reg = min(cval, ival);
  }
  if (tcChannel == 0) {
    SYNC(tc->COUNT16.SYNCBUSY.bit.CC0);
    tc->COUNT16.INTFLAG.reg = TC_INTFLAG_MC0; // Clear the interrupt
  }
  else {
    SYNC(tc->COUNT16.SYNCBUSY.bit.CC1);
    tc->COUNT16.INTFLAG.reg = TC_INTFLAG_MC1; // Clear the interrupt
  }
}

void initISR(const timer16_Sequence_t timer) {
  Tc * const tc = timer_config[SERVO_TC].pTc;
  const uint8_t tcChannel = TIMER_TCCHANNEL(timer);

  static bool initialized = false;  // Servo TC has been initialized
  if (!initialized) {
    NVIC_DisableIRQ(SERVO_IRQn);

    // Disable the timer
    tc->COUNT16.CTRLA.bit.ENABLE = false;
    SYNC(tc->COUNT16.SYNCBUSY.bit.ENABLE);

    // Select GCLK0 as timer/counter input clock source
    GCLK->PCHCTRL[TC_GCLK_ID].bit.CHEN = false;
    SYNC(GCLK->PCHCTRL[TC_GCLK_ID].bit.CHEN);
    GCLK->PCHCTRL[TC_GCLK_ID].reg = GCLK_PCHCTRL_GEN_GCLK0 | GCLK_PCHCTRL_CHEN;   // 120MHz startup code programmed
    SYNC(!GCLK->PCHCTRL[TC_GCLK_ID].bit.CHEN);

    // Reset the timer
    tc->COUNT16.CTRLA.bit.SWRST = true;
    SYNC(tc->COUNT16.SYNCBUSY.bit.SWRST);
    SYNC(tc->COUNT16.CTRLA.bit.SWRST);

    // Set timer counter mode to 16 bits
    tc->COUNT16.CTRLA.reg = TC_CTRLA_MODE_COUNT16;

    // Set timer counter mode as normal PWM
    tc->COUNT16.WAVE.bit.WAVEGEN = TCC_WAVE_WAVEGEN_NPWM_Val;

    // Set the prescaler factor
    tc->COUNT16.CTRLA.bit.PRESCALER = TC_PRESCALER(SERVO_TIMER_PRESCALER);

    // Count down
    tc->COUNT16.CTRLBSET.reg = TC_CTRLBCLR_DIR;
    SYNC(tc->COUNT16.SYNCBUSY.bit.CTRLB);

    // Reset all servo indexes
    memset((void *)currentServoIndex, 0xFF, sizeof(currentServoIndex));

    // Configure interrupt request
    NVIC_ClearPendingIRQ(SERVO_IRQn);
    NVIC_SetPriority(SERVO_IRQn, 5);
    NVIC_EnableIRQ(SERVO_IRQn);

    initialized = true;
  }

  if (!tc->COUNT16.CTRLA.bit.ENABLE) {
    // Reset the timer counter
    tc->COUNT16.COUNT.reg = TC_COUNTER_START_VAL;
    SYNC(tc->COUNT16.SYNCBUSY.bit.COUNT);

    // Enable the timer and start it
    tc->COUNT16.CTRLA.bit.ENABLE = true;
    SYNC(tc->COUNT16.SYNCBUSY.bit.ENABLE);
  }
  // First interrupt request after 1 ms
  tc->COUNT16.CC[tcChannel].reg = getTimerCount() - (uint16_t)usToTicks(1000UL);

  if (tcChannel == 0 ) {
    SYNC(tc->COUNT16.SYNCBUSY.bit.CC0);

    // Clear pending match interrupt
    tc->COUNT16.INTFLAG.reg = TC_INTENSET_MC0;
    // Enable the match channel interrupt request
    tc->COUNT16.INTENSET.reg = TC_INTENSET_MC0;
  }
  else {
    SYNC(tc->COUNT16.SYNCBUSY.bit.CC1);

    // Clear pending match interrupt
    tc->COUNT16.INTFLAG.reg = TC_INTENSET_MC1;
    // Enable the match channel interrupt request
    tc->COUNT16.INTENSET.reg = TC_INTENSET_MC1;
  }
}

void finISR(const timer16_Sequence_t timer_index) {
  Tc * const tc = timer_config[SERVO_TC].pTc;
  const uint8_t tcChannel = TIMER_TCCHANNEL(timer_index);

  // Disable the match channel interrupt request
  tc->COUNT16.INTENCLR.reg = (tcChannel == 0) ? TC_INTENCLR_MC0 : TC_INTENCLR_MC1;

  if (true
    #if defined(_useTimer1) && defined(_useTimer2)
      && (tc->COUNT16.INTENCLR.reg & (TC_INTENCLR_MC0|TC_INTENCLR_MC1)) == 0
    #endif
  ) {
    // Disable the timer if not used
    tc->COUNT16.CTRLA.bit.ENABLE = false;
    SYNC(tc->COUNT16.SYNCBUSY.bit.ENABLE);
  }
}

#endif // HAS_SERVOS

#endif // __SAMD51__
