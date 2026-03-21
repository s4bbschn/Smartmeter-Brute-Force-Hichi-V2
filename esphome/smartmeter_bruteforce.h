#pragma once
#include "esphome.h"

// --- Timing constants (EMH smart meter specific) ---
static const uint32_t PULSE_HIGH_MS       = 250;
static const uint32_t PULSE_LOW_MS        = 250;
static const uint32_t DIGIT_DELAY_MS      = 3100;
static const uint32_t INIT_DELAY_MS       = 500;
static const uint32_t INIT_PULSE_GAP_MS   = 1000;
static const int      MIN_PIN             = 0;
static const int      MAX_PIN             = 9999;
static const int      LENGTH_DIFF_THRESHOLD = 100;

// --- Top-level state machine ---
enum BruteState {
  STATE_IDLE,
  STATE_MEASURE_REF,
  STATE_WAIT_AFTER_REF,
  STATE_SEND_PIN,
  STATE_WAIT_FOR_RESPONSE,
  STATE_MEASURE_RESPONSE,
  STATE_WAIT_BEFORE_NEXT,
  STATE_CHECK_RESULT,
  STATE_FOUND,
};

// --- Sub-state machine for non-blocking PIN sending ---
enum SendState {
  SEND_IDLE,
  SEND_PRE_DELAY,        // 800ms before first init pulse
  SEND_INIT_PULSE1_HIGH, // IR on
  SEND_INIT_PULSE1_LOW,  // IR off
  SEND_INIT_GAP,         // gap between init pulses
  SEND_INIT_PULSE2_HIGH,
  SEND_INIT_PULSE2_LOW,
  SEND_INIT_DELAY,       // delay after init
  SEND_DIGIT_PULSE_HIGH, // digit pulse IR on
  SEND_DIGIT_PULSE_LOW,  // digit pulse IR off
  SEND_DIGIT_GAP,        // gap between digits
  SEND_DONE,
};

// --- Sub-state machine for non-blocking measurement ---
enum MeasureState {
  MEAS_IDLE,
  MEAS_WAIT_DATA,    // waiting for first byte (up to 5s)
  MEAS_READ_DATA,    // reading bytes (up to 12s, 500ms idle = done)
  MEAS_ATTEMPT_DONE, // short pause between attempts
  MEAS_DONE,
};

// --- Top-level state ---
static BruteState brute_state = STATE_IDLE;
static unsigned long state_timer = 0;

// --- Send sub-state ---
static SendState send_state = SEND_IDLE;
static unsigned long send_timer = 0;
static int send_digits[4] = {0};
static int send_digit_idx = 0;   // which digit (0-3)
static int send_pulse_idx = 0;   // which pulse within digit
static int send_current_pin = 0;

// --- Measure sub-state ---
static MeasureState meas_state = MEAS_IDLE;
static unsigned long meas_timer = 0;
static unsigned long meas_last_byte = 0;
static int meas_attempt = 0;
static int meas_current_len = 0;
static int meas_max_len = 0;
static bool meas_any_data_ever = false;
static bool meas_is_reference = false;
static const int MEAS_MAX_ATTEMPTS = 5;

// --- Helper: publish debug + log ---
static void brute_log(const char* fmt, ...) {
  char buf[200];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  ESP_LOGI("brute", "%s", buf);
  id(debug_sensor).publish_state(buf);
}

// --- Helper: publish progress ---
static void publish_progress(int pin) {
  float progress;
  if (id(pin_direction) > 0) {
    progress = ((float)(pin - MIN_PIN) / (float)(MAX_PIN - MIN_PIN)) * 100.0f;
  } else {
    progress = ((float)(MAX_PIN - pin) / (float)(MAX_PIN - MIN_PIN)) * 100.0f;
  }
  if (progress < 0) progress = 0;
  if (progress > 100) progress = 100;
  id(progress_sensor).publish_state(progress);
}

// --- Helper: advance to next PIN ---
static void advance_pin() {
  id(current_pin) += id(pin_direction);
  if (id(current_pin) > MAX_PIN) {
    id(current_pin) = MAX_PIN;
    id(pin_direction) = -1;
  } else if (id(current_pin) < MIN_PIN) {
    id(current_pin) = MIN_PIN;
    id(pin_direction) = 1;
  }
}

// =====================================================
// Non-blocking PIN sender
// =====================================================
static void send_start(int pin) {
  send_current_pin = pin;
  char formatted[5];
  snprintf(formatted, sizeof(formatted), "%04d", pin);
  for (int i = 0; i < 4; i++) {
    send_digits[i] = formatted[i] - '0';
  }
  send_digit_idx = 0;
  send_pulse_idx = 0;
  brute_log("Sende PIN: %04d", pin);
  send_state = SEND_PRE_DELAY;
  send_timer = millis();
}

static bool send_tick() {
  switch (send_state) {

    case SEND_IDLE:
      return true;

    case SEND_PRE_DELAY:
      if (millis() - send_timer >= 800) {
        id(ir_led).turn_on();
        send_timer = millis();
        send_state = SEND_INIT_PULSE1_HIGH;
      }
      return false;

    case SEND_INIT_PULSE1_HIGH:
      if (millis() - send_timer >= PULSE_HIGH_MS) {
        id(ir_led).turn_off();
        send_timer = millis();
        send_state = SEND_INIT_PULSE1_LOW;
      }
      return false;

    case SEND_INIT_PULSE1_LOW:
      if (millis() - send_timer >= PULSE_LOW_MS) {
        send_timer = millis();
        send_state = SEND_INIT_GAP;
      }
      return false;

    case SEND_INIT_GAP:
      if (millis() - send_timer >= INIT_PULSE_GAP_MS) {
        id(ir_led).turn_on();
        send_timer = millis();
        send_state = SEND_INIT_PULSE2_HIGH;
      }
      return false;

    case SEND_INIT_PULSE2_HIGH:
      if (millis() - send_timer >= PULSE_HIGH_MS) {
        id(ir_led).turn_off();
        send_timer = millis();
        send_state = SEND_INIT_PULSE2_LOW;
      }
      return false;

    case SEND_INIT_PULSE2_LOW:
      if (millis() - send_timer >= PULSE_LOW_MS) {
        send_timer = millis();
        send_state = SEND_INIT_DELAY;
      }
      return false;

    case SEND_INIT_DELAY:
      if (millis() - send_timer >= INIT_DELAY_MS) {
        send_digit_idx = 0;
        send_pulse_idx = 0;
        if (send_digits[0] == 0) {
          // digit 0 = no pulses, just wait the digit gap
          send_timer = millis();
          send_state = SEND_DIGIT_GAP;
        } else {
          id(ir_led).turn_on();
          send_timer = millis();
          send_state = SEND_DIGIT_PULSE_HIGH;
        }
      }
      return false;

    case SEND_DIGIT_PULSE_HIGH:
      if (millis() - send_timer >= PULSE_HIGH_MS) {
        id(ir_led).turn_off();
        send_timer = millis();
        send_state = SEND_DIGIT_PULSE_LOW;
      }
      return false;

    case SEND_DIGIT_PULSE_LOW:
      if (millis() - send_timer >= PULSE_LOW_MS) {
        send_pulse_idx++;
        if (send_pulse_idx < send_digits[send_digit_idx]) {
          // more pulses for this digit
          id(ir_led).turn_on();
          send_timer = millis();
          send_state = SEND_DIGIT_PULSE_HIGH;
        } else {
          // digit done, wait gap
          send_timer = millis();
          send_state = SEND_DIGIT_GAP;
        }
      }
      return false;

    case SEND_DIGIT_GAP:
      if (millis() - send_timer >= DIGIT_DELAY_MS) {
        brute_log("Ziffer #%d: %d (%d Pulse)", send_digit_idx + 1, send_digits[send_digit_idx], send_digits[send_digit_idx]);
        send_digit_idx++;
        if (send_digit_idx < 4) {
          send_pulse_idx = 0;
          if (send_digits[send_digit_idx] == 0) {
            send_timer = millis();
            send_state = SEND_DIGIT_GAP;
          } else {
            id(ir_led).turn_on();
            send_timer = millis();
            send_state = SEND_DIGIT_PULSE_HIGH;
          }
        } else {
          send_state = SEND_DONE;
        }
      }
      return false;

    case SEND_DONE:
      send_state = SEND_IDLE;
      return true;

    default:
      send_state = SEND_IDLE;
      return true;
  }
}

// =====================================================
// Non-blocking message length measurement
// =====================================================
static void meas_start(bool is_reference) {
  meas_is_reference = is_reference;
  meas_attempt = 0;
  meas_max_len = 0;
  meas_any_data_ever = false;
  meas_current_len = 0;
  brute_log("=== Starte Messung%s ===", is_reference ? " (Referenz)" : "");
  // Begin first attempt
  meas_state = MEAS_WAIT_DATA;
  meas_timer = millis();
  brute_log("Versuch %d", meas_attempt + 1);
}

// Returns true when measurement is complete. Result in meas_max_len.
static bool meas_tick() {
  auto &uart = id(ir_uart);
  uint8_t byte_buf;

  switch (meas_state) {

    case MEAS_IDLE:
      return true;

    case MEAS_WAIT_DATA:
      // Poll for first byte, up to 5s
      if (uart.available()) {
        // Got first byte — start reading
        uart.read_byte(&byte_buf);
        meas_current_len = 1;
        meas_any_data_ever = true;
        meas_last_byte = millis();
        meas_timer = millis();  // reuse as read start
        meas_state = MEAS_READ_DATA;
      } else if (millis() - meas_timer >= 5000) {
        brute_log("  Versuch %d: Timeout, keine Daten", meas_attempt + 1);
        if (!meas_any_data_ever) {
          brute_log("Keine Daten empfangen, breche ab");
          meas_state = MEAS_DONE;
        } else {
          // Try next attempt
          meas_attempt++;
          if (meas_attempt >= MEAS_MAX_ATTEMPTS) {
            meas_state = MEAS_DONE;
          } else {
            meas_timer = millis();
            meas_state = MEAS_ATTEMPT_DONE;
          }
        }
      }
      return false;

    case MEAS_READ_DATA:
      // Read all available bytes (non-blocking drain)
      while (uart.available()) {
        uart.read_byte(&byte_buf);
        meas_current_len++;
        meas_last_byte = millis();
      }
      // 500ms idle → message complete
      if (millis() - meas_last_byte > 500) {
        if (meas_current_len > meas_max_len) {
          meas_max_len = meas_current_len;
        }
        brute_log("Versuch %d: %d Bytes", meas_attempt + 1, meas_current_len);
        meas_attempt++;
        if (meas_attempt >= MEAS_MAX_ATTEMPTS) {
          meas_state = MEAS_DONE;
        } else {
          meas_timer = millis();
          meas_state = MEAS_ATTEMPT_DONE;
        }
      }
      // Also enforce 12s max read time
      if (millis() - meas_timer > 12000) {
        if (meas_current_len > meas_max_len) {
          meas_max_len = meas_current_len;
        }
        brute_log("Versuch %d: %d Bytes (Timeout)", meas_attempt + 1, meas_current_len);
        meas_attempt++;
        if (meas_attempt >= MEAS_MAX_ATTEMPTS) {
          meas_state = MEAS_DONE;
        } else {
          meas_timer = millis();
          meas_state = MEAS_ATTEMPT_DONE;
        }
      }
      return false;

    case MEAS_ATTEMPT_DONE:
      // 200ms pause between attempts
      if (millis() - meas_timer >= 200) {
        meas_current_len = 0;
        brute_log("Versuch %d", meas_attempt + 1);
        meas_timer = millis();
        meas_state = MEAS_WAIT_DATA;
      }
      return false;

    case MEAS_DONE:
      brute_log("=== Maximum: %d Bytes ===", meas_max_len);
      meas_state = MEAS_IDLE;
      return true;

    default:
      meas_state = MEAS_IDLE;
      return true;
  }
}

// =====================================================
// Single-PIN test state machine
// =====================================================
enum TestState {
  TEST_IDLE,
  TEST_MEASURE_REF,
  TEST_WAIT_AFTER_REF,
  TEST_SEND_PIN,
  TEST_WAIT_FOR_RESPONSE,
  TEST_MEASURE_RESPONSE,
  TEST_CHECK_RESULT,
  TEST_DONE,
};

static TestState test_state = TEST_IDLE;
static unsigned long test_timer = 0;

static void smartmeter_test_pin_loop() {
  if (!id(test_single_running)) {
    if (test_state != TEST_IDLE) {
      test_state = TEST_IDLE;
      send_state = SEND_IDLE;
      meas_state = MEAS_IDLE;
      id(ir_led).turn_off();
    }
    return;
  }

  // Kick off
  if (test_state == TEST_IDLE) {
    id(status_sensor).publish_state("Einzeltest: Messe Referenz...");
    meas_start(true);
    test_state = TEST_MEASURE_REF;
    return;
  }

  switch (test_state) {

    case TEST_MEASURE_REF: {
      if (meas_tick()) {
        id(reference_msg_length) = meas_max_len;
        brute_log("Einzeltest Referenz: %d Bytes", meas_max_len);
        id(status_sensor).publish_state("Referenz: " + to_string(meas_max_len) + " Bytes");
        test_timer = millis();
        test_state = TEST_WAIT_AFTER_REF;
      }
      break;
    }

    case TEST_WAIT_AFTER_REF: {
      if (millis() - test_timer >= 3000) {
        test_state = TEST_SEND_PIN;
      }
      break;
    }

    case TEST_SEND_PIN: {
      int pin = id(test_single_pin);
      id(current_pin_sensor).publish_state(pin);

      char msg[80];
      snprintf(msg, sizeof(msg), "Einzeltest: Sende PIN %04d (Ref: %d Bytes)", pin, id(reference_msg_length));
      id(status_sensor).publish_state(msg);

      send_start(pin);
      test_state = TEST_WAIT_FOR_RESPONSE;
      break;
    }

    case TEST_WAIT_FOR_RESPONSE: {
      if (send_tick()) {
        brute_log("Einzeltest: Warte 5s auf Antwort...");
        test_timer = millis();
        test_state = TEST_MEASURE_RESPONSE;
      }
      break;
    }

    case TEST_MEASURE_RESPONSE: {
      if (millis() - test_timer < 5000) break;
      if (meas_state == MEAS_IDLE) {
        meas_start(false);
      }
      if (meas_tick()) {
        test_state = TEST_CHECK_RESULT;
      }
      break;
    }

    case TEST_CHECK_RESULT: {
      int msg_len = meas_max_len;
      id(message_length_sensor).publish_state(msg_len);

      int diff = msg_len - id(reference_msg_length);
      brute_log("Einzeltest: Länge %d Bytes (Ref: %d, Diff: %d)", msg_len, id(reference_msg_length), diff);

      char result[100];
      if (diff > LENGTH_DIFF_THRESHOLD) {
        snprintf(result, sizeof(result), "PIN %04d: TREFFER! +%d Bytes", id(test_single_pin), diff);
        id(result_sensor).publish_state(result);
        id(status_sensor).publish_state("Einzeltest: PIN könnte korrekt sein!");
      } else {
        snprintf(result, sizeof(result), "PIN %04d: Kein Treffer (Diff: %d)", id(test_single_pin), diff);
        id(result_sensor).publish_state(result);
        id(status_sensor).publish_state("Einzeltest: PIN nicht korrekt");
      }

      test_state = TEST_DONE;
      break;
    }

    case TEST_DONE: {
      id(test_single_running) = false;
      test_state = TEST_IDLE;
      brute_log("Einzeltest abgeschlossen");
      break;
    }

    default:
      break;
  }
}

// =====================================================
// Main loop — called every 100ms from ESPHome interval
// =====================================================
void smartmeter_brute_loop() {
  // Single-pin test takes priority over brute force
  if (id(test_single_running) || test_state != TEST_IDLE) {
    smartmeter_test_pin_loop();
    return;
  }

  // Not started yet
  if (!id(brute_force_running) && brute_state == STATE_IDLE) {
    return;
  }

  // Just started → kick off reference measurement
  if (id(brute_force_running) && brute_state == STATE_IDLE) {
    id(status_sensor).publish_state("Messe Referenz-Nachrichtenlänge...");
    meas_start(true);
    brute_state = STATE_MEASURE_REF;
    return;
  }

  // Stopped externally
  if (!id(brute_force_running) && brute_state != STATE_FOUND) {
    brute_state = STATE_IDLE;
    send_state = SEND_IDLE;
    meas_state = MEAS_IDLE;
    id(ir_led).turn_off();
    return;
  }

  switch (brute_state) {

    case STATE_MEASURE_REF: {
      if (meas_tick()) {
        id(reference_msg_length) = meas_max_len;
        brute_log("Referenzlänge: %d Bytes", meas_max_len);
        id(status_sensor).publish_state("Referenz: " + to_string(meas_max_len) + " Bytes");
        if (meas_max_len == 0) {
          brute_log("WARNUNG: Keine UART-Daten! Teste trotzdem...");
        }
        brute_log("Starte in 3 Sekunden...");
        state_timer = millis();
        brute_state = STATE_WAIT_AFTER_REF;
      }
      break;
    }

    case STATE_WAIT_AFTER_REF: {
      if (millis() - state_timer >= 3000) {
        id(status_sensor).publish_state("Brute-Force läuft...");
        brute_state = STATE_SEND_PIN;
      }
      break;
    }

    case STATE_SEND_PIN: {
      if (id(pin_found)) {
        brute_state = STATE_FOUND;
        break;
      }

      int pin = id(current_pin);
      id(current_pin_sensor).publish_state(pin);
      publish_progress(pin);

      char msg[80];
      snprintf(msg, sizeof(msg), "Teste PIN %04d (Ref: %d Bytes)", pin, id(reference_msg_length));
      id(status_sensor).publish_state(msg);

      // Start non-blocking send
      send_start(pin);
      brute_state = STATE_WAIT_FOR_RESPONSE;
      break;
    }

    case STATE_WAIT_FOR_RESPONSE: {
      // Tick the sender until done
      if (send_tick()) {
        // PIN fully sent — wait 5s for meter to respond
        brute_log("Warte 5s auf Antwort...");
        state_timer = millis();
        brute_state = STATE_MEASURE_RESPONSE;
      }
      break;
    }

    case STATE_MEASURE_RESPONSE: {
      // Wait 5s, then start measurement
      if (millis() - state_timer < 5000) {
        break;
      }
      // Kick off measurement (only once)
      if (meas_state == MEAS_IDLE) {
        meas_start(false);
      }
      if (meas_tick()) {
        brute_state = STATE_CHECK_RESULT;
      }
      break;
    }

    case STATE_CHECK_RESULT: {
      int msg_len = meas_max_len;
      id(message_length_sensor).publish_state(msg_len);

      int diff = msg_len - id(reference_msg_length);
      brute_log("Länge: %d Bytes (Ref: %d, Diff: %d)", msg_len, id(reference_msg_length), diff);

      if (diff > LENGTH_DIFF_THRESHOLD) {
        id(pin_found) = true;
        brute_log("*** PIN GEFUNDEN: %04d ***", id(current_pin));

        char result[60];
        snprintf(result, sizeof(result), "PIN gefunden: %04d", id(current_pin));
        id(result_sensor).publish_state(result);
        id(status_sensor).publish_state("PIN erfolgreich gefunden!");
        id(progress_sensor).publish_state(100.0f);
        brute_state = STATE_FOUND;
      } else {
        if (diff > 0) {
          brute_log("+%d Bytes (zu wenig, brauche >%d)", diff, LENGTH_DIFF_THRESHOLD);
        }
        advance_pin();
        state_timer = millis();
        brute_state = STATE_WAIT_BEFORE_NEXT;
      }
      break;
    }

    case STATE_WAIT_BEFORE_NEXT: {
      if (millis() - state_timer >= 500) {
        brute_state = STATE_SEND_PIN;
      }
      break;
    }

    case STATE_FOUND: {
      if (!id(pin_found)) {
        brute_state = STATE_SEND_PIN;
      }
      break;
    }

    default:
      break;
  }
}
