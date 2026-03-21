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

// --- SML parsing ---
// Buffer to collect SML response (need at least 2 messages)
static const int SML_BUF_SIZE = 1024;
static uint8_t sml_buf[SML_BUF_SIZE];
static int sml_buf_len = 0;
// Track how many SML start sequences we've seen
static int sml_start_count = 0;

// SML escape sequences
static const uint8_t SML_START[] = {0x1b, 0x1b, 0x1b, 0x1b, 0x01, 0x01, 0x01, 0x01};
static const uint8_t SML_END[]   = {0x1b, 0x1b, 0x1b, 0x1b, 0x1a};

// OBIS codes we look for
// 1-0:1.8.0*255 = total energy import (kWh)  → 01 00 01 08 00 FF
// 1-0:2.8.0*255 = total energy export (kWh)  → 01 00 02 08 00 FF
static const uint8_t OBIS_TOTAL_KWH[]  = {0x01, 0x00, 0x01, 0x08, 0x00, 0xFF};
static const uint8_t OBIS_TOTAL_KWH2[] = {0x01, 0x00, 0x02, 0x08, 0x00, 0xFF};

// Power OBIS codes — meters use different ones:
// 1-0:16.7.0*255 = sum active instantaneous power  → 01 00 10 07 00 FF
// 1-0:1.7.0*255  = positive active instantaneous    → 01 00 01 07 00 FF
// 1-0:2.7.0*255  = negative active instantaneous    → 01 00 02 07 00 FF
// 1-0:15.7.0*255 = absolute active instantaneous    → 01 00 0F 07 00 FF
static const uint8_t OBIS_POWER_W1[] = {0x01, 0x00, 0x10, 0x07, 0x00, 0xFF}; // 1-0:16.7.0
static const uint8_t OBIS_POWER_W2[] = {0x01, 0x00, 0x01, 0x07, 0x00, 0xFF}; // 1-0:1.7.0
static const uint8_t OBIS_POWER_W3[] = {0x01, 0x00, 0x02, 0x07, 0x00, 0xFF}; // 1-0:2.7.0
static const uint8_t OBIS_POWER_W4[] = {0x01, 0x00, 0x0F, 0x07, 0x00, 0xFF}; // 1-0:15.7.0

// Parsed SML results
static bool sml_power_valid = false;
static bool sml_energy_valid = false;
static double sml_power_w = 0.0;
static double sml_energy_kwh = 0.0;

// --- SML value extraction ---
// After an OBIS code in an SML GetListResponse, the structure is:
//   OBIS (6 bytes) → status (optional) → valTime (optional) → unit (optional) → scaler → value
// We search for the OBIS pattern, then skip forward to extract the integer value + scaler.
//
// SML TL (type-length) byte: upper nibble = type, lower nibble = length
//   type 0x5 = signed integer, 0x6 = unsigned integer, 0x0 = octet string
//   For multi-byte length: if bit 4 of nibble is set, length continues.
//   Simple approach: we parse the TL byte to get the data length.

// Read a signed integer of `len` bytes from buf at offset
static int64_t sml_read_int(const uint8_t* buf, int offset, int len) {
  if (len == 0) return 0;
  int64_t val = (int8_t)buf[offset]; // sign-extend first byte
  for (int i = 1; i < len; i++) {
    val = (val << 8) | buf[offset + i];
  }
  return val;
}

// Read an unsigned integer of `len` bytes from buf at offset
static uint64_t sml_read_uint(const uint8_t* buf, int offset, int len) {
  if (len == 0) return 0;
  uint64_t val = 0;
  for (int i = 0; i < len; i++) {
    val = (val << 8) | buf[offset + i];
  }
  return val;
}

// Get length from SML TL byte. Returns data length (excluding TL byte itself).
// For simple single-byte TL: lower nibble is total field length including TL byte.
// So data length = (tl & 0x0F) - 1.
// For type 0x00 with length 0x01 → "optional empty" (SML_SKIP).
// Returns -1 on error.
static int sml_tl_data_len(uint8_t tl) {
  int total_len = tl & 0x0F;
  if (total_len == 0) return -1;
  return total_len - 1;
}

// Skip one SML field at buf[offset], return new offset after the field.
// Returns -1 on error.
static int sml_skip_field(const uint8_t* buf, int buf_len, int offset) {
  if (offset >= buf_len) return -1;
  uint8_t tl = buf[offset];

  // SML list (type 7): lower nibble = number of list elements
  if ((tl & 0xF0) == 0x70) {
    int count = tl & 0x0F;
    offset++;
    for (int i = 0; i < count; i++) {
      offset = sml_skip_field(buf, buf_len, offset);
      if (offset < 0) return -1;
    }
    return offset;
  }

  // Optional/empty field: 0x01 means "optional not present"
  if (tl == 0x01) {
    return offset + 1;
  }

  // Multi-byte length: bit 4 of upper nibble set means length continues
  // For simplicity handle the common 2-byte TL case
  if (tl & 0x80) {
    // Extended TL: first byte has partial length, second byte continues
    if (offset + 1 >= buf_len) return -1;
    int total_len = ((tl & 0x0F) << 4) | (buf[offset + 1] & 0x0F);
    return offset + total_len;  // total_len includes TL bytes
  }

  int total_len = tl & 0x0F;
  if (total_len == 0) return -1;
  return offset + total_len;
}

// Extract value after OBIS code match.
// In SML GetList, after the OBIS octet string, the valListEntry is a list of:
//   objName, status, valTime, unit, scaler, value, valueSignature
// We already matched objName (OBIS). Now we need to skip status, valTime, unit,
// read scaler, read value.
static bool sml_extract_value_after_obis(const uint8_t* buf, int buf_len, int offset,
                                          int8_t &scaler_out, int64_t &value_out) {
  // Skip status
  offset = sml_skip_field(buf, buf_len, offset);
  if (offset < 0) return false;

  // Skip valTime
  offset = sml_skip_field(buf, buf_len, offset);
  if (offset < 0) return false;

  // Skip unit
  offset = sml_skip_field(buf, buf_len, offset);
  if (offset < 0) return false;

  // Read scaler (signed int8)
  if (offset >= buf_len) return false;
  uint8_t scaler_tl = buf[offset];
  int scaler_len = sml_tl_data_len(scaler_tl);
  if (scaler_len < 0 || scaler_len == 0) {
    // Optional empty scaler → default 0
    scaler_out = 0;
    offset++;
  } else {
    offset++;
    if (offset + scaler_len > buf_len) return false;
    scaler_out = (int8_t)sml_read_int(buf, offset, scaler_len);
    offset += scaler_len;
  }

  // Read value (signed or unsigned integer)
  if (offset >= buf_len) return false;
  uint8_t val_tl = buf[offset];
  uint8_t val_type = (val_tl & 0xF0) >> 4;
  int val_len = sml_tl_data_len(val_tl);
  if (val_len <= 0) return false;
  offset++;
  if (offset + val_len > buf_len) return false;

  if (val_type == 5) {
    // Signed integer
    value_out = sml_read_int(buf, offset, val_len);
  } else if (val_type == 6) {
    // Unsigned integer
    value_out = (int64_t)sml_read_uint(buf, offset, val_len);
  } else {
    return false; // unexpected type
  }

  return true;
}

// Find OBIS code pattern in buffer starting from search_from, extract value.
static bool sml_find_obis_value(const uint8_t* buf, int buf_len,
                                 const uint8_t* obis, int obis_len,
                                 double &result, int search_from = 0) {
  for (int i = search_from; i < buf_len - obis_len - 1; i++) {
    if (buf[i] == 0x07 && i + 1 + obis_len <= buf_len) {
      if (memcmp(&buf[i + 1], obis, obis_len) == 0) {
        int after_obis = i + 1 + obis_len;
        int8_t scaler = 0;
        int64_t value = 0;
        if (sml_extract_value_after_obis(buf, buf_len, after_obis, scaler, value)) {
          result = (double)value * pow(10.0, (double)scaler);
          return true;
        }
      }
    }
  }
  return false;
}

// Parse the collected SML buffer. Returns true if both power and energy were found.
static bool sml_parse() {
  sml_power_valid = false;
  sml_energy_valid = false;

  if (sml_buf_len < 16) return false;

  // Find the SECOND SML start sequence — the first message is the basic/locked
  // response that all PINs get. The second message (if present) is the extended
  // data that only appears with the correct PIN and contains 16.7.0 (power).
  int second_start = -1;
  int start_count = 0;
  for (int i = 0; i <= sml_buf_len - 8; i++) {
    if (memcmp(&sml_buf[i], SML_START, 8) == 0) {
      start_count++;
      if (start_count == 2) {
        second_start = i;
        break;
      }
    }
  }

  if (second_start < 0) {
    // Only one SML message — no extended data, PIN is wrong
    return false;
  }

  ESP_LOGI("sml", "Zweite SML-Nachricht ab Offset %d", second_start);

  // Search for power and energy only in the second message onwards
  double power = 0.0;
  if (sml_find_obis_value(sml_buf, sml_buf_len, OBIS_POWER_W1, 6, power, second_start) ||
      sml_find_obis_value(sml_buf, sml_buf_len, OBIS_POWER_W2, 6, power, second_start) ||
      sml_find_obis_value(sml_buf, sml_buf_len, OBIS_POWER_W3, 6, power, second_start) ||
      sml_find_obis_value(sml_buf, sml_buf_len, OBIS_POWER_W4, 6, power, second_start)) {
    sml_power_w = power;
    sml_power_valid = true;
  }

  double energy = 0.0;
  if (sml_find_obis_value(sml_buf, sml_buf_len, OBIS_TOTAL_KWH, 6, energy, second_start) ||
      sml_find_obis_value(sml_buf, sml_buf_len, OBIS_TOTAL_KWH2, 6, energy, second_start)) {
    sml_energy_kwh = energy;
    sml_energy_valid = true;
  }

  return sml_power_valid && sml_energy_valid;
}

// --- Top-level state machine ---
enum BruteState {
  STATE_IDLE,
  STATE_MEASURE_REF,
  STATE_WAIT_AFTER_REF,
  STATE_SEND_PIN,
  STATE_WAIT_FOR_RESPONSE,
  STATE_COLLECT_SML,
  STATE_WAIT_BEFORE_NEXT,
  STATE_CHECK_RESULT,
  STATE_FOUND,
};

// --- Sub-state machine for non-blocking PIN sending ---
enum SendState {
  SEND_IDLE,
  SEND_PRE_DELAY,
  SEND_INIT_PULSE1_HIGH,
  SEND_INIT_PULSE1_LOW,
  SEND_INIT_GAP,
  SEND_INIT_PULSE2_HIGH,
  SEND_INIT_PULSE2_LOW,
  SEND_INIT_DELAY,
  SEND_DIGIT_PULSE_HIGH,
  SEND_DIGIT_PULSE_LOW,
  SEND_DIGIT_GAP,
  SEND_DONE,
};

// --- Sub-state machine for SML collection ---
enum CollectState {
  COLLECT_IDLE,
  COLLECT_WAIT_DATA,
  COLLECT_READ_DATA,
  COLLECT_DONE,
};

// --- Top-level state ---
static BruteState brute_state = STATE_IDLE;
static unsigned long state_timer = 0;

// --- Send sub-state ---
static SendState send_state = SEND_IDLE;
static unsigned long send_timer = 0;
static int send_digits[4] = {0};
static int send_digit_idx = 0;
static int send_pulse_idx = 0;
static int send_current_pin = 0;

// --- Collect sub-state ---
static CollectState collect_state = COLLECT_IDLE;
static unsigned long collect_timer = 0;
static unsigned long collect_last_byte = 0;

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
// Non-blocking PIN sender (unchanged logic)
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
          id(ir_led).turn_on();
          send_timer = millis();
          send_state = SEND_DIGIT_PULSE_HIGH;
        } else {
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
// Non-blocking SML data collector
// Collects bytes into sml_buf. Stops after 2s idle or buffer full.
// For wrong PINs: ~364 bytes, 1 message, fast timeout.
// For correct PINs: multiple messages with 16.7.0 OBIS.
// =====================================================
static void collect_start() {
  sml_buf_len = 0;
  collect_state = COLLECT_WAIT_DATA;
  collect_timer = millis();
  brute_log("Sammle SML-Daten...");
}

// Returns true when collection is complete. Data in sml_buf / sml_buf_len.
static bool collect_tick() {
  auto &uart = id(ir_uart);
  uint8_t byte_buf;

  switch (collect_state) {
    case COLLECT_IDLE:
      return true;

    case COLLECT_WAIT_DATA:
      if (uart.available()) {
        uart.read_byte(&byte_buf);
        if (sml_buf_len < SML_BUF_SIZE) {
          sml_buf[sml_buf_len++] = byte_buf;
        }
        collect_last_byte = millis();
        collect_state = COLLECT_READ_DATA;
      } else if (millis() - collect_timer >= 15000) {
        brute_log("Timeout: keine SML-Daten empfangen");
        collect_state = COLLECT_DONE;
      }
      return false;

    case COLLECT_READ_DATA:
      while (uart.available()) {
        uart.read_byte(&byte_buf);
        if (sml_buf_len < SML_BUF_SIZE) {
          sml_buf[sml_buf_len++] = byte_buf;
        }
        collect_last_byte = millis();
      }
      // Buffer full → stop
      if (sml_buf_len >= SML_BUF_SIZE) {
        brute_log("SML-Daten: %d Bytes (Puffer voll)", sml_buf_len);
        collect_state = COLLECT_DONE;
      }
      // 2s idle → done (wrong PIN: meter stops after 1 msg; correct PIN: gap between msgs is <1s)
      else if (millis() - collect_last_byte > 2000) {
        brute_log("SML-Daten: %d Bytes empfangen", sml_buf_len);
        collect_state = COLLECT_DONE;
      }
      // 30s max safety
      else if (millis() - collect_timer > 30000) {
        brute_log("SML-Daten: %d Bytes (Timeout)", sml_buf_len);
        collect_state = COLLECT_DONE;
      }
      return false;

    case COLLECT_DONE:
      collect_state = COLLECT_IDLE;
      return true;

    default:
      collect_state = COLLECT_IDLE;
      return true;
  }
}

// =====================================================
// Single-PIN test state machine
// =====================================================
enum TestState {
  TEST_IDLE,
  TEST_SEND_PIN,
  TEST_WAIT_FOR_RESPONSE,
  TEST_COLLECT_SML,
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
      collect_state = COLLECT_IDLE;
      id(ir_led).turn_off();
    }
    return;
  }

  // Kick off
  if (test_state == TEST_IDLE) {
    test_state = TEST_SEND_PIN;
  }

  switch (test_state) {
    case TEST_SEND_PIN: {
      int pin = id(test_single_pin);
      id(current_pin_sensor).publish_state(pin);

      char msg[80];
      snprintf(msg, sizeof(msg), "Einzeltest: Sende PIN %04d", pin);
      id(status_sensor).publish_state(msg);

      send_start(pin);
      test_state = TEST_WAIT_FOR_RESPONSE;
      break;
    }

    case TEST_WAIT_FOR_RESPONSE: {
      if (send_tick()) {
        brute_log("Einzeltest: Warte 5s auf Antwort...");
        test_timer = millis();
        test_state = TEST_COLLECT_SML;
      }
      break;
    }

    case TEST_COLLECT_SML: {
      if (millis() - test_timer < 5000) break;
      if (collect_state == COLLECT_IDLE) {
        collect_start();
      }
      if (collect_tick()) {
        test_state = TEST_CHECK_RESULT;
      }
      break;
    }

    case TEST_CHECK_RESULT: {
      id(message_length_sensor).publish_state(sml_buf_len);

      bool parsed = sml_parse();
      char result[120];

      if (parsed) {
        snprintf(result, sizeof(result), "PIN %04d: TREFFER! Leistung: %.1f W, Zähler: %.1f kWh",
                 id(test_single_pin), sml_power_w, sml_energy_kwh);
        id(result_sensor).publish_state(result);
        id(status_sensor).publish_state("Einzeltest: PIN korrekt! SML-Daten gelesen.");
        id(power_sensor).publish_state(sml_power_w);
        id(energy_sensor).publish_state(sml_energy_kwh);
      } else {
        snprintf(result, sizeof(result), "PIN %04d: Kein SML (Leistung: %s, Zähler: %s, %d Bytes)",
                 id(test_single_pin),
                 sml_power_valid ? "ja" : "nein",
                 sml_energy_valid ? "ja" : "nein",
                 sml_buf_len);
        id(result_sensor).publish_state(result);
        id(status_sensor).publish_state("Einzeltest: PIN nicht korrekt (kein gültiges SML)");
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
// Main brute-force loop — called every 50ms from ESPHome interval
// =====================================================
void smartmeter_brute_loop() {
  // Single-pin test takes priority
  if (id(test_single_running) || test_state != TEST_IDLE) {
    smartmeter_test_pin_loop();
    return;
  }

  // Not started yet
  if (!id(brute_force_running) && brute_state == STATE_IDLE) {
    return;
  }

  // Just started → begin with first PIN directly (no reference measurement needed)
  if (id(brute_force_running) && brute_state == STATE_IDLE) {
    id(status_sensor).publish_state("Brute-Force gestartet (SML-Erkennung)");
    brute_log("Starte Brute-Force mit SML-Parsing ab PIN %04d", id(current_pin));
    brute_state = STATE_SEND_PIN;
    return;
  }

  // Stopped externally
  if (!id(brute_force_running) && brute_state != STATE_FOUND) {
    brute_state = STATE_IDLE;
    send_state = SEND_IDLE;
    collect_state = COLLECT_IDLE;
    id(ir_led).turn_off();
    return;
  }

  switch (brute_state) {
    case STATE_SEND_PIN: {
      if (id(pin_found)) {
        brute_state = STATE_FOUND;
        break;
      }

      int pin = id(current_pin);
      id(current_pin_sensor).publish_state(pin);
      publish_progress(pin);

      char msg[80];
      snprintf(msg, sizeof(msg), "Teste PIN %04d", pin);
      id(status_sensor).publish_state(msg);

      send_start(pin);
      brute_state = STATE_WAIT_FOR_RESPONSE;
      break;
    }

    case STATE_WAIT_FOR_RESPONSE: {
      if (send_tick()) {
        brute_log("Warte 5s auf Antwort...");
        state_timer = millis();
        brute_state = STATE_COLLECT_SML;
      }
      break;
    }

    case STATE_COLLECT_SML: {
      if (millis() - state_timer < 5000) break;
      if (collect_state == COLLECT_IDLE) {
        collect_start();
      }
      if (collect_tick()) {
        brute_state = STATE_CHECK_RESULT;
      }
      break;
    }

    case STATE_CHECK_RESULT: {
      id(message_length_sensor).publish_state(sml_buf_len);

      bool parsed = sml_parse();

      if (parsed) {
        id(pin_found) = true;
        brute_log("*** PIN GEFUNDEN: %04d ***", id(current_pin));
        brute_log("Leistung: %.1f W, Zählerstand: %.1f kWh", sml_power_w, sml_energy_kwh);

        char result[100];
        snprintf(result, sizeof(result), "PIN %04d: %.1f W, %.1f kWh",
                 id(current_pin), sml_power_w, sml_energy_kwh);
        id(result_sensor).publish_state(result);
        id(status_sensor).publish_state("PIN gefunden! SML-Daten erfolgreich gelesen.");
        id(progress_sensor).publish_state(100.0f);
        id(power_sensor).publish_state(sml_power_w);
        id(energy_sensor).publish_state(sml_energy_kwh);
        brute_state = STATE_FOUND;
      } else {
        if (sml_buf_len > 0) {
          brute_log("PIN %04d: %d Bytes, kein gültiges SML (P:%s E:%s)",
                    id(current_pin), sml_buf_len,
                    sml_power_valid ? "ja" : "nein",
                    sml_energy_valid ? "ja" : "nein");
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
