/*
 * Copyright (C) 2022 Fern Lane, ANMAK (AN-Motors Arduino Keyfob) Project v1.0.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 *
 */

#include <Arduino.h>

#include "LowPower.h"

// Keyfob button defines
#define KEYFOB_BUTTON_1 0b0100
#define KEYFOB_BUTTON_2 0b0010
#define KEYFOB_BUTTON_3 0b0001
#define KEYFOB_BUTTON_4 0b1000

// ID of keyfob to send
const uint32_t KEYFOB_ID PROGMEM = 0x0000000;

// Keyfob button to send
const uint8_t KEYFOB_BUTTON PROGMEM = KEYFOB_BUTTON_1;

// Random seed
#define RANDOM_SEED 1234

// Uncomment to enable 1.1V output to AREF pin
//#define REF_CALIBRATION

// Measured real 1.1V reference in mV
const uint32_t VREF_ACTUAL_MV PROGMEM = 1101;

// Button pin (with interrupt)
const uint8_t PIN_BTN PROGMEM = 2;

// Charger connected pin (with interrupt)
const uint8_t PIN_CHG PROGMEM = 3;

// LEDs pins
const uint8_t PIN_LED_RED PROGMEM = 0;
const uint8_t PIN_LED_YELLOW PROGMEM = A4;
const uint8_t PIN_LED_GREEN PROGMEM = A5;

// Transmitter pin
const uint8_t PIN_TRANSMITTER PROGMEM = 1;

// VCC thresholds (mV)
const uint32_t VCC_GREEN_TRH PROGMEM = 3750;
const uint32_t VCC_YELLOW_TRH PROGMEM = 3600;
const uint32_t VCC_RED_TRH PROGMEM = 3450;

// Blink intervals
#define CHARGE_BLINK_LOW_PERIOD 100
#define CHARGE_BLINK_CONNECTED_PERIOD 500

// For how long (ms) show battery charge after sleep before sleep again
#define TIME_AFTER_WAKE_UP 1000

// Packet time constant (in microseconds)
const unsigned int TIME_PE_US PROGMEM = 413;
const unsigned int TIME_PE_2_US PROGMEM = 413 * 2;

// Internal variables
uint32_t vcc;
uint64_t wake_up_timer, show_charge_timer, transmit_timer;
uint64_t show_charge_period;
boolean button_pressed, charger_connected;
boolean show_charge_stage;
boolean tx_allowed;
uint64_t payload;
uint8_t data_bit;

// Voids
void transmit_data(void);
void show_charge(void);
void wake_up(void);
void sleep_begin(void);
void check_charger(void);
void check_button(void);
void vcc_read(void);
void send_data(void);
void send_bit(void);
void send_preamble(void);
void set_tx_high(void);
void set_tx_low(void);

void setup() {
  // Initialize pins
  pinMode(PIN_BTN, INPUT);
  pinMode(PIN_CHG, INPUT);
  pinMode(PIN_LED_RED, OUTPUT);
  pinMode(PIN_LED_YELLOW, OUTPUT);
  pinMode(PIN_LED_GREEN, OUTPUT);
  pinMode(PIN_TRANSMITTER, OUTPUT);

  // Set random seed
  randomSeed(RANDOM_SEED);

  // AREF calibration mode
#ifdef REF_CALIBRATION
  // Enable 1.1V reference
  analogReference(INTERNAL);

  // Get first measurement
  analogRead(A0);

  // Infinite loop
  while (true);;
#endif
}

void loop() {
  // Check if charger is connected
  check_charger();

  // Check if button is pressed
  check_button();

  // Allow transmitting every TIME_AFTER_WAKE_UP ms if charger is connected
  if (charger_connected && millis() - transmit_timer >= TIME_AFTER_WAKE_UP) {
    tx_allowed = true;
    transmit_timer = millis();
  }

  // Show current battery charge
  show_charge();

  // Start RF transmitting if button is pressed
  if (button_pressed)
    transmit_data();

  // Reset sleep timer if charger is connected or button is pressed
  if (charger_connected || button_pressed)
    wake_up_timer = millis();

  // Go to sleep
  if (millis() - wake_up_timer >= TIME_AFTER_WAKE_UP)
    sleep_begin();
}

/**
 * Sends RF packet
*/
void transmit_data(void) {
  if (tx_allowed) {
    // Transmit RF packet
    send_data();

    // Turn off transmitter (just in case)
    digitalWrite(PIN_TRANSMITTER, LOW);

    // Stop sending RF packets
    tx_allowed = false;
  }
}

/**
 * Shows current battery voltage with 3 LEDs
*/
void show_charge(void) {
  if (millis() - show_charge_timer >= show_charge_period) {
    // Measure VCC
    vcc_read();

    // Charging
    if (charger_connected) {
      // Set timer period
      show_charge_period = CHARGE_BLINK_CONNECTED_PERIOD;

      // Stage 2
      if (show_charge_stage) {
        // Initial state RED - ON, other - OFF
        digitalWrite(PIN_LED_RED, HIGH);
        digitalWrite(PIN_LED_YELLOW, LOW);
        digitalWrite(PIN_LED_GREEN, LOW);

        // Turn ON next LEDs
        if (vcc > VCC_RED_TRH)
          digitalWrite(PIN_LED_YELLOW, HIGH);
        if (vcc > VCC_YELLOW_TRH)
          digitalWrite(PIN_LED_GREEN, HIGH);
      }

      // Stage 1
      else {
        // Turn off highest level LED
        if (vcc > VCC_YELLOW_TRH)
          digitalWrite(PIN_LED_GREEN, LOW);
        else if (vcc > VCC_RED_TRH)
          digitalWrite(PIN_LED_YELLOW, LOW);
        else
          digitalWrite(PIN_LED_RED, LOW);
      }

      // Blink
      show_charge_stage = !show_charge_stage;
    }

    // Not charging
    else {
      // Normal battery
      if (vcc >= VCC_RED_TRH) {
        // Initial state RED - ON, other - OFF
        digitalWrite(PIN_LED_RED, HIGH);
        digitalWrite(PIN_LED_YELLOW, LOW);
        digitalWrite(PIN_LED_GREEN, LOW);

        // Turn on other LEDs depending on voltage
        if (vcc >= VCC_YELLOW_TRH)
          digitalWrite(PIN_LED_YELLOW, HIGH);
        if (vcc >= VCC_GREEN_TRH)
          digitalWrite(PIN_LED_GREEN, HIGH);
      }

      // Low battery
      else {
        // Set timer period
        show_charge_period = CHARGE_BLINK_LOW_PERIOD;

        // Blink with red LED
        digitalWrite(PIN_LED_RED, show_charge_stage);
        show_charge_stage = !show_charge_stage;
      }
    }

    // Reset timer
    show_charge_timer = millis();
  }
}

/**
 * Wake up callback
*/
void wake_up(void) {
  // Enable ADC
  ADCSRA |= (1 << ADEN);

  // Reset sleep timer
  wake_up_timer = millis();

  // Reset transmit timer
  transmit_timer = millis();

  // Allow RF transmitting
  tx_allowed = true;
}

/**
 * Puts arduino into sleep
*/
void sleep_begin(void) {
  // Turn off all LEDs
  digitalWrite(PIN_LED_RED, LOW);
  digitalWrite(PIN_LED_YELLOW, LOW);
  digitalWrite(PIN_LED_GREEN, LOW);

  // Turn off transmitter (just in case)
  digitalWrite(PIN_TRANSMITTER, LOW);

  // Wake up on button or charger connected
  attachInterrupt(digitalPinToInterrupt(PIN_BTN), wake_up, LOW);
  attachInterrupt(digitalPinToInterrupt(PIN_CHG), wake_up, LOW);

  // Go to sleep
  LowPower.powerDown(SLEEP_FOREVER, ADC_OFF, BOD_OFF);

  // Disable external pin interrupts on wake up
  detachInterrupt(digitalPinToInterrupt(PIN_BTN)); 
  detachInterrupt(digitalPinToInterrupt(PIN_CHG)); 
}

/**
 * Checks if charger is connected
*/
void check_charger(void) {
  charger_connected = !digitalRead(PIN_CHG);
}

/**
 * Checks if button is pressed
*/
void check_button(void) {
  button_pressed = !digitalRead(PIN_BTN);
}

/**
 * Measures VCC my measuring 1.1V reference against AVcc
*/
void vcc_read(void) {
  // Read 1.1V reference against AVcc
  #if defined(__AVR_ATmega32U4__) || defined(__AVR_ATmega1280__) || defined(__AVR_ATmega2560__)
    ADMUX = _BV(REFS0) | _BV(MUX4) | _BV(MUX3) | _BV(MUX2) | _BV(MUX1);
  #elif defined (__AVR_ATtiny24__) || defined(__AVR_ATtiny44__) || defined(__AVR_ATtiny84__)
    ADMUX = _BV(MUX5) | _BV(MUX0) ;
  #else
    ADMUX = _BV(REFS0) | _BV(MUX3) | _BV(MUX2) | _BV(MUX1);
  #endif

  // Wait for voltage to settle
  delay(10);

  // Start conversion
  ADCSRA |= _BV(ADSC);

  // Measuring
  while (bit_is_set(ADCSRA, ADSC));
  vcc = (ADCH << 8) | ADCL;

  // Back-calculate AVcc in mV
  vcc = (VREF_ACTUAL_MV * ((uint32_t) 1024)) / vcc;

  // Restore default ADC state
  analogReference(DEFAULT);
  analogRead(A5);
}

/**
 * Sends preamble and payload
*/
void send_data(void) {
  // Construct payload
  payload = 0x2020000000000000;
  payload |= 0x0001000000000001 * (uint64_t)KEYFOB_BUTTON;
  payload |= (uint64_t)random(0xFF) * (uint64_t)0x101 << (uint64_t)32;
  // ID - 28 bits
  payload |= (uint64_t)KEYFOB_ID << (uint64_t)4;

  // 4 packets per one data stream
  for (uint8_t packet_n = 0; packet_n < 4; packet_n++){
    // Send preamble
    send_preamble();

    // Wait 4130 us
    delayMicroseconds(TIME_PE_US * (unsigned int)10);

    // Send payload
    for (uint8_t i = sizeof(payload) * (uint8_t)8; i > 0; i--) {
      data_bit = bitRead(payload, i - 1);
      send_bit();
    }

    // Send battery and repeat flag
    data_bit = 1;
    send_bit();
    send_bit();

    // Delay between packets
    delayMicroseconds(TIME_PE_US * (unsigned int)39);
  }
}

/**
 * Sends one bit of data
*/
void send_bit(void) {
  if (data_bit == 0) {
    // Send 0
    set_tx_high();
    delayMicroseconds(TIME_PE_2_US);
    set_tx_low();
    delayMicroseconds(TIME_PE_US);
  }
  else {
    // Send 1
    set_tx_high();
    delayMicroseconds(TIME_PE_US);
    set_tx_low();
    delayMicroseconds(TIME_PE_2_US);
  }
}

/**
 * Sends data preamble
*/
void send_preamble(void) {
  for (uint8_t i = 0; i < 12; i++) {
    delayMicroseconds(TIME_PE_US);
    set_tx_high();
    delayMicroseconds(TIME_PE_US);
    set_tx_low();
  }
}

/**
 * Sets output to HIGH
*/
void set_tx_high(void) {
  *portOutputRegister(digitalPinToPort(PIN_TRANSMITTER)) |= digitalPinToBitMask(PIN_TRANSMITTER);
}


/**
 * Sets output to LOW
*/
void set_tx_low(void) {
  *portOutputRegister(digitalPinToPort(PIN_TRANSMITTER)) &= ~digitalPinToBitMask(PIN_TRANSMITTER);
}
