#include <EEPROM.h>
#include <Wire.h>
#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

constexpr uint8_t LCD_COLUMNS = 16;
constexpr uint8_t LCD_ROWS = 2;

// Hardware pins.
const int thermoSO = 12;
const int thermoCS = 10;
const int thermoSCK = 13;
const int ssrPin = 9;
const int fanPwmPin = 3;
const uint8_t buzzerPin = 2;
const int sleepPin = 4;

// Button wiring (active-low with INPUT_PULLUP).
const uint8_t upButtonPin = 5;
const uint8_t downButtonPin = 7;
const uint8_t okButtonPin = 6;

// Calibration master switch: when false, raw thermocouple reading is used
// directly (no sensor offset, no airflow interpolation).
bool calibrationEnabled = true;

class SmdFastPid {
 public:
  SmdFastPid(float kp, float ki, float kd, float hz, int bits = 16,
             bool sign = false) {
    configure(kp, ki, kd, hz, bits, sign);
  }

  bool configure(float kp, float ki, float kd, float hz, int bits = 16,
                 bool sign = false) {
    clear();
    p_ = floatToParam(kp);
    i_ = floatToParam(ki / hz);
    d_ = floatToParam(kd * hz);

    if (bits > 16 || bits < 1) {
      setConfigError();
    } else {
      const uint32_t raw_max =
          (bits == 16) ? (0xFFFFUL >> (17 - bits)) : (0xFFFFUL >> (16 - bits));
      out_max_ = (int64_t)raw_max * kParamMult;
      if (sign) {
        out_min_ =
            -(((int64_t)(0xFFFFUL >> (17 - bits)) + 1) * kParamMult);
      } else {
        out_min_ = 0;
      }
    }

    return !config_error_;
  }

  void clear() {
    sum_ = 0;
    last_setpoint_ = 0;
    last_error_ = 0;
    p_ = 0;
    i_ = 0;
    d_ = 0;
    out_min_ = 0;
    out_max_ = 0;
    config_error_ = false;
  }

  int16_t step(int16_t setpoint, int16_t feedback) {
    int32_t error = int32_t(setpoint) - int32_t(feedback);
    int32_t proportional = 0;
    int32_t integral = 0;
    int32_t derivative = 0;

    if (p_ != 0) {
      proportional = int32_t(p_) * error;
    }

    if (i_ != 0) {
      sum_ += int64_t(error) * int64_t(i_);
      if (sum_ > INT32_MAX) {
        sum_ = INT32_MAX;
      } else if (sum_ < INT32_MIN) {
        sum_ = INT32_MIN;
      }
      integral = sum_;
    }

    if (d_ != 0) {
      int32_t rate = (error - last_error_) - int32_t(setpoint - last_setpoint_);
      last_setpoint_ = setpoint;
      last_error_ = error;

      if (rate > INT16_MAX) {
        rate = INT16_MAX;
      } else if (rate < INT16_MIN) {
        rate = INT16_MIN;
      }
      derivative = int32_t(d_) * rate;
    }

    int64_t output =
        int64_t(proportional) + int64_t(integral) + int64_t(derivative);
    if (output > out_max_) {
      output = out_max_;
    } else if (output < out_min_) {
      output = out_min_;
    }

    int16_t result = output >> kParamShift;
    if ((output & (1ULL << (kParamShift - 1))) != 0) {
      result++;
    }
    return result;
  }

 private:
  static constexpr uint8_t kParamShift = 8;
  static constexpr uint32_t kParamMult = 1UL << kParamShift;
  static constexpr float kParamMax = 255.0f;

  uint32_t floatToParam(float value) {
    if (value > kParamMax || value < 0.0f) {
      config_error_ = true;
      return 0;
    }

    const uint32_t param = value * kParamMult;
    if (value != 0.0f && param == 0) {
      config_error_ = true;
      return 0;
    }
    return param;
  }

  void setConfigError() {
    config_error_ = true;
    p_ = 0;
    i_ = 0;
    d_ = 0;
  }

  uint32_t p_ = 0;
  uint32_t i_ = 0;
  uint32_t d_ = 0;
  int64_t out_max_ = 0;
  int64_t out_min_ = 0;
  bool config_error_ = false;
  int16_t last_setpoint_ = 0;
  int64_t sum_ = 0;
  int32_t last_error_ = 0;
};

class SmdMax6675 {
 public:
  SmdMax6675(int8_t clock_pin, int8_t chip_select_pin, int8_t data_pin)
      : clock_pin_(clock_pin),
        chip_select_pin_(chip_select_pin),
        data_pin_(data_pin) {
    pinMode(chip_select_pin_, OUTPUT);
    pinMode(clock_pin_, OUTPUT);
    pinMode(data_pin_, INPUT);
    digitalWrite(chip_select_pin_, HIGH);
  }

  float readCelsius() {
    digitalWrite(chip_select_pin_, LOW);
    delayMicroseconds(10);

    uint16_t raw = readByte();
    raw <<= 8;
    raw |= readByte();

    digitalWrite(chip_select_pin_, HIGH);

    if ((raw & 0x4U) != 0) {
      return NAN;
    }

    raw >>= 3;
    return raw * 0.25f;
  }

 private:
  uint8_t readByte() {
    uint8_t value = 0;

    for (int8_t bit = 7; bit >= 0; bit--) {
      digitalWrite(clock_pin_, LOW);
      delayMicroseconds(10);
      if (digitalRead(data_pin_) != LOW) {
        value |= (1U << bit);
      }
      digitalWrite(clock_pin_, HIGH);
      delayMicroseconds(10);
    }

    return value;
  }

  int8_t clock_pin_;
  int8_t chip_select_pin_;
  int8_t data_pin_;
};

class SmdLiquidCrystalI2C : public Print {
 public:
  SmdLiquidCrystalI2C(uint8_t address, uint8_t columns, uint8_t rows)
      : address_(address), columns_(columns), rows_(rows) {}

  void begin(uint8_t columns, uint8_t rows,
             uint8_t dot_size = kFiveByEightDots) {
    columns_ = columns;
    rows_ = rows;
    Wire.begin();

    display_function_ = kFourBitMode | kOneLine | kFiveByEightDots;
    if (rows_ > 1) {
      display_function_ |= kTwoLine;
    }
    if (dot_size != 0 && rows_ == 1) {
      display_function_ |= dot_size;
    }

    delay(50);
    expanderWrite(backlight_value_);
    delay(1000);

    write4Bits(0x03 << 4);
    delayMicroseconds(4500);
    write4Bits(0x03 << 4);
    delayMicroseconds(4500);
    write4Bits(0x03 << 4);
    delayMicroseconds(150);
    write4Bits(0x02 << 4);

    command(kFunctionSet | display_function_);
    display_control_ = kDisplayOn | kCursorOff | kBlinkOff;
    command(kDisplayControl | display_control_);
    clear();

    display_mode_ = kEntryLeft | kEntryShiftDecrement;
    command(kEntryModeSet | display_mode_);
    home();
  }

  void clear() {
    command(kClearDisplay);
    delayMicroseconds(2000);
  }

  void home() {
    command(kReturnHome);
    delayMicroseconds(2000);
  }

  void backlight() {
    backlight_value_ = kBacklight;
    expanderWrite(0);
  }

  void setCursor(uint8_t column, uint8_t row) {
    static const uint8_t row_offsets[] = {0x00, 0x40, 0x14, 0x54};
    if (row >= rows_) {
      row = rows_ - 1;
    }
    command(kSetDdramAddr | (column + row_offsets[row]));
  }

  void createCharFromProgmem(uint8_t location, const uint8_t *character_map) {
    location &= 0x7;
    command(kSetCgramAddr | (location << 3));
    for (uint8_t row = 0; row < 8; row++) {
      write(pgm_read_byte(character_map++));
    }
    command(kSetDdramAddr);
  }

  void command(uint8_t value) { send(value, 0); }

  size_t write(uint8_t value) override {
    send(value, kRegisterSelect);
    return 1;
  }

 private:
  static constexpr uint8_t kClearDisplay = 0x01;
  static constexpr uint8_t kReturnHome = 0x02;
  static constexpr uint8_t kEntryModeSet = 0x04;
  static constexpr uint8_t kDisplayControl = 0x08;
  static constexpr uint8_t kFunctionSet = 0x20;
  static constexpr uint8_t kSetCgramAddr = 0x40;
  static constexpr uint8_t kSetDdramAddr = 0x80;

  static constexpr uint8_t kEntryLeft = 0x02;
  static constexpr uint8_t kEntryShiftDecrement = 0x00;
  static constexpr uint8_t kDisplayOn = 0x04;
  static constexpr uint8_t kCursorOff = 0x00;
  static constexpr uint8_t kBlinkOff = 0x00;
  static constexpr uint8_t kFourBitMode = 0x00;
  static constexpr uint8_t kOneLine = 0x00;
  static constexpr uint8_t kTwoLine = 0x08;
  static constexpr uint8_t kFiveByEightDots = 0x00;
  static constexpr uint8_t kBacklight = 0x08;
  static constexpr uint8_t kEnableBit = 0x04;
  static constexpr uint8_t kRegisterSelect = 0x01;

  void send(uint8_t value, uint8_t mode) {
    const uint8_t high_nibble = value & 0xF0;
    const uint8_t low_nibble = (value << 4) & 0xF0;
    write4Bits(high_nibble | mode);
    write4Bits(low_nibble | mode);
  }

  void write4Bits(uint8_t value) {
    expanderWrite(value);
    pulseEnable(value);
  }

  void expanderWrite(uint8_t data) {
    Wire.beginTransmission(address_);
    Wire.write(data | backlight_value_);
    Wire.endTransmission();
  }

  void pulseEnable(uint8_t data) {
    expanderWrite(data | kEnableBit);
    delayMicroseconds(1);
    expanderWrite(data & ~kEnableBit);
    delayMicroseconds(50);
  }

  uint8_t address_;
  uint8_t columns_;
  uint8_t rows_;
  uint8_t display_function_ = 0;
  uint8_t display_control_ = 0;
  uint8_t display_mode_ = 0;
  uint8_t backlight_value_ = 0;
};

/*
 * Big-digit glyphs derived from LCDBigNumbers 1.2.2, reduced to only the
 * 3x2 variant-2 font used by this sketch. LCDBigNumbers is GPL-3.0-or-later.
 */
const uint8_t smdBigDigitCustomPatterns[8][8] PROGMEM = {
    {B11100, B11110, B11110, B11110, B11110, B11110, B11110, B11100},
    {B00111, B01111, B01111, B01111, B01111, B01111, B01111, B00111},
    {B11111, B11111, B00000, B00000, B00000, B00000, B11111, B11111},
    {B11110, B11100, B00000, B00000, B00000, B00000, B11000, B11100},
    {B01111, B00111, B00000, B00000, B00000, B00000, B00011, B00111},
    {B00000, B00000, B00000, B00000, B00000, B00000, B11111, B11111},
    {B00000, B00000, B00000, B00000, B00000, B00000, B00111, B01111},
    {B11111, B11111, B00000, B00000, B00000, B00000, B00000, B00000},
};

const uint8_t smdBigDigitGlyphs[10][2][3] PROGMEM = {
    {{0x01, 0x07, 0x00}, {0x01, 0x05, 0x00}},
    {{0xFE, 0x00, 0xFE}, {0xFE, 0x00, 0xFE}},
    {{0x04, 0x02, 0x00}, {0x01, 0x05, 0x05}},
    {{0x04, 0x02, 0x00}, {0x06, 0x05, 0x00}},
    {{0x01, 0x05, 0x00}, {0xFE, 0xFE, 0x00}},
    {{0x01, 0x02, 0x03}, {0x06, 0x05, 0x00}},
    {{0x01, 0x02, 0x03}, {0x01, 0x05, 0x00}},
    {{0x01, 0x07, 0x00}, {0xFE, 0xFE, 0x00}},
    {{0x01, 0x02, 0x00}, {0x01, 0x05, 0x00}},
    {{0x01, 0x02, 0x00}, {0x06, 0x05, 0x00}},
};

class SmdBigDigits1602 : public Print {
 public:
  explicit SmdBigDigits1602(SmdLiquidCrystalI2C *lcd) : lcd_(lcd) {}

  void begin() {
    for (uint8_t index = 0; index < 8; index++) {
      lcd_->createCharFromProgmem(index, smdBigDigitCustomPatterns[index]);
    }
  }

  void setBigNumberCursor(uint8_t column, uint8_t row = 0) {
    upper_left_column_ = column;
    upper_left_row_ = row;
  }

  size_t write(uint8_t value) override {
    bool draw_blank = false;
    uint8_t digit = 0;

    if (value == ' ') {
      draw_blank = true;
    } else {
      digit = (value <= 9) ? value : uint8_t(value - '0');
      if (digit > 9) {
        draw_blank = true;
      }
    }

    for (uint8_t row = 0; row < 2; row++) {
      lcd_->setCursor(upper_left_column_, upper_left_row_ + row);
      for (uint8_t column = 0; column < 3; column++) {
        const uint8_t glyph =
            draw_blank ? ' '
                       : pgm_read_byte(&smdBigDigitGlyphs[digit][row][column]);
        lcd_->write(glyph);
      }
    }

    upper_left_column_ += 3;
    return 3;
  }

 private:
  SmdLiquidCrystalI2C *lcd_;
  uint8_t upper_left_column_ = 0;
  uint8_t upper_left_row_ = 0;
};
// EEPROM layout.
const int eepromSensorOffsetAddr = 0;
const int eepromSetpointAddr = 2;
const int eepromFanPercentAddr = 4;
const int eepromFanActualMinAddr = 6;

// Timing and UI constants.
const uint8_t firmwareVersion = 3;
const unsigned long controlIntervalMs = 200;
const unsigned long stateTelemetryRateHz = 4;
const unsigned long stateTelemetryIntervalMs = 1000UL / stateTelemetryRateHz;
constexpr unsigned long serialBaudRate = 115200;
const unsigned long displayIntervalRuntimeMs = 300;
const unsigned long displayIntervalCalibrationMs = 500;
const unsigned long buttonDebounceMs = 50;
const unsigned long buttonComboHoldMs = 2000;
const unsigned long buttonHoldRepeatDelayMs = 500;
const unsigned long buttonHoldRepeatIntervalMs = 150;

const uint16_t buttonBeepOnMs = 35;
const uint16_t sleepTransitionBeepOnMs = 60;
const uint16_t sleepTransitionGapMs = 80;
const uint16_t targetReachedBeepOnMs = 350;

const int setpointStepC = 10;
const int fanStepPercent = 5;
const int holdAdjustStep = 10;
const int minSetpointC = 100;
const int maxSetpointC = 500;
const int minFanPercent = 10;
const int maxFanPercent = 100;
const int fanDisplayMinPercent = 10;
int fanActualMinPercent = 30;
const int fanActualMinFloor = 10;
const int fanActualMinCeil = 100;
const int maxCalibrationOffsetC = 500;

const float sleepCooldownTempC = 45.0f;

// Recorded steady-state mapping from sensor-domain temperature to measured real
// output.
const int16_t airflowCalibrationFanPercentPoints[] = {30, 60, 90};
const int16_t airflowCalibrationSensorTempPointsC[] = {100, 150, 200, 250, 300,
                                                       350, 400, 450, 500};
int16_t airflowCalibrationRealTempTableC[][9] = {
    {81, 116, 156, 193, 235, 273, 315, 349, 383},
    {85, 119, 150, 189, 225, 260, 310, 343, 384},
    {85, 127, 160, 190, 229, 261, 300, 333, 367}};
const uint8_t airflowCalibrationFanPointCount =
    sizeof(airflowCalibrationFanPercentPoints) /
    sizeof(airflowCalibrationFanPercentPoints[0]);
const uint8_t airflowCalibrationTempPointCount =
    sizeof(airflowCalibrationSensorTempPointsC) /
    sizeof(airflowCalibrationSensorTempPointsC[0]);

// PID configuration.
float Kp = 0.9f;
float Ki = 0.0199995f;
float Kd = 0.3f;
float Hz = 6.0f;
int outputBits = 8;
bool outputSigned = false;


// Runtime settings (persisted).
int setpointTempC = 200;
int fanCommandPercent = 50;

// Sensor calibration offset (persisted).
int16_t sensorOffsetC = 0;

// Sensor values.
float lastRawTempC = NAN;
float lastSensorCalibratedTempC = NAN;
float lastEstimatedRealTempC = NAN;
float smoothedDisplayTempC = NAN;
bool lastThermocoupleValid = false;
bool lastThermocoupleValidKnown = false;
uint8_t lastHeaterPwm = 0;
int lastFanActualPercent = 0;
unsigned long lastStateTelemetryMs = 0;

bool sleepCooldownComplete = false;
bool lastSleepModeActive = false;
bool setpointReachedBeepPending = true;
bool setpointReachedCrossingArmed = false;

unsigned long lastControlUpdateMs = 0;
unsigned long lastDisplayUpdateMs = 0;
bool displayNeedsClear = true;

const size_t serialLineBufferSize = 80;
char serialLineBuffer[serialLineBufferSize];
size_t serialLineIndex = 0;
bool serialLineOverflow = false;

struct ButtonState {
  uint8_t pin;
  bool stablePressed;
  bool lastSamplePressed;
  unsigned long lastDebounceMs;
  bool pressedEvent;
  unsigned long pressedSinceMs;
  unsigned long lastRepeatMs;
};

struct BuzzerPlayback {
  const uint16_t *patternMs;
  uint8_t stepCount;
  uint8_t stepIndex;
  bool outputHigh;
  bool active;
  unsigned long stepStartedMs;
};

ButtonState upButton = {upButtonPin, false, false, 0, false, 0, 0};
ButtonState downButton = {downButtonPin, false, false, 0, false, 0, 0};
ButtonState okButton = {okButtonPin, false, false, 0, false, 0, 0};

// Active buzzer patterns alternate ON and OFF durations, always starting ON.
const uint16_t buttonBeepPatternMs[] = {buttonBeepOnMs};
const uint16_t sleepEnterBeepPatternMs[] = {
    sleepTransitionBeepOnMs, sleepTransitionGapMs, sleepTransitionBeepOnMs};
const uint16_t sleepExitBeepPatternMs[] = {
    sleepTransitionBeepOnMs, sleepTransitionGapMs, sleepTransitionBeepOnMs,
    sleepTransitionGapMs, sleepTransitionBeepOnMs};
const uint16_t targetReachedBeepPatternMs[] = {targetReachedBeepOnMs};

BuzzerPlayback buzzerPlayback = {NULL, 0, 0, false, false, 0};

unsigned long comboHoldStartMs = 0;
bool comboTriggered = false;

enum ControllerState { STATE_TMP, STATE_FAN, STATE_CAL, STATE_CAL_AIR };

struct RuntimeSnapshot {
  ControllerState mode;
  bool tcOk;
  bool sleepActive;
  bool cooldownActive;
  bool heaterEnabled;
  bool calibrationEnabled;
  float rawTempC;
  float sensorTempC;
  float realTempC;
  float displayTempC;
  int setpointC;
  uint8_t heaterPwm;
  int fanCommandPercent;
  int fanActualPercent;
  int fanMinPercent;
  int16_t offsetC;
  float kp;
  float ki;
  float kd;
  float hz;
};

ControllerState currentState = STATE_TMP;

SmdMax6675 tc(thermoSCK, thermoCS, thermoSO);
SmdLiquidCrystalI2C myLCD(0x27, LCD_COLUMNS, LCD_ROWS);
SmdBigDigits1602 bigNumberLCD(&myLCD);
SmdFastPid pid(Kp, Ki, Kd, Hz, outputBits, outputSigned);

void initFanPwm();
void setFanDutyPercent(float percent);
void setHeaterPwm(uint8_t pwm);
void setState(ControllerState newState);
void handleButtons(unsigned long nowMs);
void handleSerialInput();
void handleSerialCommand(const char *line);
void updateDisplay(unsigned long nowMs);
void readThermocouple();
void controlOutputs(bool sleepModeActive);
void suppressButtonDuringSleep(ButtonState &button, unsigned long nowMs);
void resetSetpointReachedBeepState();
void updateBuzzer(unsigned long nowMs);
void evaluateSetpointReachedBeep(bool sleepModeActive);
void stopBuzzer();
bool startBuzzerPattern(const uint16_t *patternMs, uint8_t stepCount,
                        bool overrideActive);
void requestButtonBeep();
void requestSleepEnterBeep();
void requestSleepExitBeep();
bool requestSetpointReachedBeep();
float applySensorCalibrationCorrection(float rawTempC);
int mapFanDisplayToActual(int displayPercent);
void invalidateDisplay();
bool hasValidTemperatureReadings();
void clearTemperatureReadings();
bool loadPersistedInt16(int address, int minValue, int maxValue, int &target);
void savePersistedInt16(int address, int value);
float interpolateLinearly(float input, float input0, float input1,
                          float output0, float output1);
float interpolateSeries(float input, const int16_t *inputPoints,
                        const int16_t *outputPoints, uint8_t pointCount);
float estimateRealTempAtAirflow(float sensorTempC, uint8_t airflowIndex);
float applyAirflowCalibration(float sensorTempC, int airflowPercent);
bool parseFloatArgument(const char *text, float &value);
bool parseLongArgument(const char *text, long &value);
bool parseCalibrationRowArguments(const char *text, int &fanIdx, int &tempIdx,
                                  int &cellValue);
bool applyPidConfiguration(float newKp, float newKi, float newKd, float newHz);
RuntimeSnapshot captureRuntimeSnapshot();
void emitBootPacket();
void emitStatePacket(const RuntimeSnapshot &snapshot);
void emitEventPacket(const __FlashStringHelper *type);
void emitAckPacket(const __FlashStringHelper *cmd);
void emitAckPacket(const __FlashStringHelper *cmd,
                   const __FlashStringHelper *field, int value);
void emitAckPacket(const __FlashStringHelper *cmd,
                   const __FlashStringHelper *field, float value,
                   uint8_t decimals);
void emitAckPacket(const __FlashStringHelper *cmd,
                   const __FlashStringHelper *field1, int value1,
                   const __FlashStringHelper *field2, int value2);
void emitAckPacket(const __FlashStringHelper *cmd,
                   const __FlashStringHelper *field1, int value1,
                   const __FlashStringHelper *field2, int value2,
                   const __FlashStringHelper *field3, int value3);
void emitAckPacket(const __FlashStringHelper *cmd,
                   const __FlashStringHelper *field1, int value1,
                   const __FlashStringHelper *field2, int value2,
                   const __FlashStringHelper *field3, int value3,
                   const __FlashStringHelper *field4, int value4);
void emitErrorPacket(const __FlashStringHelper *cmd,
                     const __FlashStringHelper *reason);
void emitErrorPacket(const __FlashStringHelper *cmd,
                     const __FlashStringHelper *reason,
                     const __FlashStringHelper *field, int value);
void emitErrorPacket(const __FlashStringHelper *cmd,
                     const __FlashStringHelper *reason,
                     const __FlashStringHelper *field1, int value1,
                     const __FlashStringHelper *field2, int value2);
void maybeEmitStateTelemetry(unsigned long nowMs);
const __FlashStringHelper *modeToken(const RuntimeSnapshot &snapshot);
int consumeAdjustmentDelta(ButtonState &increaseButton,
                           ButtonState &decreaseButton, int pressStep,
                           int holdStep, unsigned long nowMs);

bool isCalibrationState(ControllerState state) {
  return state == STATE_CAL || state == STATE_CAL_AIR;
}

void beginPacket(const __FlashStringHelper *type) {
  Serial.print('@');
  Serial.print(type);
}

void emitField(const __FlashStringHelper *key, const __FlashStringHelper *value) {
  Serial.print(' ');
  Serial.print(key);
  Serial.print('=');
  Serial.print(value);
}

void emitField(const __FlashStringHelper *key, int value) {
  Serial.print(' ');
  Serial.print(key);
  Serial.print('=');
  Serial.print(value);
}

void emitField(const __FlashStringHelper *key, float value, uint8_t decimals) {
  Serial.print(' ');
  Serial.print(key);
  Serial.print('=');
  Serial.print(value, decimals);
}

void emitField(const __FlashStringHelper *key, bool value) {
  Serial.print(' ');
  Serial.print(key);
  Serial.print('=');
  Serial.print(value ? 1 : 0);
}

void endPacket() {
  Serial.println();
}

RuntimeSnapshot captureRuntimeSnapshot() {
  RuntimeSnapshot snapshot;
  snapshot.mode = currentState;
  snapshot.sleepActive = (digitalRead(sleepPin) == LOW);
  snapshot.cooldownActive = snapshot.sleepActive && !sleepCooldownComplete;
  snapshot.tcOk = hasValidTemperatureReadings();
  snapshot.heaterEnabled = lastHeaterPwm > 0;
  snapshot.calibrationEnabled = calibrationEnabled;
  snapshot.rawTempC = lastRawTempC;
  snapshot.sensorTempC = lastSensorCalibratedTempC;
  snapshot.realTempC = lastEstimatedRealTempC;
  snapshot.displayTempC = smoothedDisplayTempC;
  snapshot.setpointC = setpointTempC;
  snapshot.heaterPwm = lastHeaterPwm;
  snapshot.fanCommandPercent = fanCommandPercent;
  snapshot.fanActualPercent = lastFanActualPercent;
  snapshot.fanMinPercent = fanActualMinPercent;
  snapshot.offsetC = sensorOffsetC;
  snapshot.kp = Kp;
  snapshot.ki = Ki;
  snapshot.kd = Kd;
  snapshot.hz = Hz;
  return snapshot;
}

const __FlashStringHelper *modeToken(const RuntimeSnapshot &snapshot) {
  if (!snapshot.tcOk) {
    return F("TC_ERROR");
  }
  if (snapshot.sleepActive) {
    return F("SLEEP");
  }
  if (snapshot.mode == STATE_CAL) {
    return F("CAL");
  }
  if (snapshot.mode == STATE_CAL_AIR) {
    return F("CAL_AIR");
  }
  return F("NORMAL");
}

void emitBootPacket() {
  const RuntimeSnapshot snapshot = captureRuntimeSnapshot();
  beginPacket(F("BOOT"));
  emitField(F("fw"), firmwareVersion);
  emitField(F("proto"), 2);
  emitField(F("board"), F("UNO"));
  emitField(F("rate_hz"), (int)stateTelemetryRateHz);
  emitField(F("kp"), snapshot.kp, 6);
  emitField(F("ki"), snapshot.ki, 6);
  emitField(F("kd"), snapshot.kd, 6);
  emitField(F("hz"), snapshot.hz, 6);
  endPacket();
}

void emitStatePacket(const RuntimeSnapshot &snapshot) {
  beginPacket(F("STATE"));
  emitField(F("mode"), modeToken(snapshot));
  emitField(F("raw"), snapshot.rawTempC, 1);
  emitField(F("sns"), snapshot.sensorTempC, 1);
  emitField(F("real"), snapshot.realTempC, 1);
  emitField(F("disp"), snapshot.displayTempC, 1);
  emitField(F("set"), snapshot.setpointC);
  emitField(F("pwm"), snapshot.heaterPwm);
  emitField(F("heater"), snapshot.heaterEnabled);
  emitField(F("fan_cmd"), snapshot.fanCommandPercent);
  emitField(F("fan_actual"), snapshot.fanActualPercent);
  emitField(F("fan_min"), snapshot.fanMinPercent);
  emitField(F("offset"), snapshot.offsetC);
  emitField(F("cal"), snapshot.calibrationEnabled);
  emitField(F("sleep"), snapshot.sleepActive);
  emitField(F("cooldown"), snapshot.cooldownActive);
  emitField(F("tc_ok"), snapshot.tcOk);
  emitField(F("kp"), snapshot.kp, 6);
  emitField(F("ki"), snapshot.ki, 6);
  emitField(F("kd"), snapshot.kd, 6);
  emitField(F("hz"), snapshot.hz, 6);
  endPacket();
  lastStateTelemetryMs = millis();
}

void emitEventPacket(const __FlashStringHelper *type) {
  beginPacket(F("EVENT"));
  emitField(F("type"), type);
  endPacket();
}

void emitAckPacket(const __FlashStringHelper *cmd) {
  beginPacket(F("ACK"));
  emitField(F("cmd"), cmd);
  endPacket();
}

void emitAckPacket(const __FlashStringHelper *cmd,
                   const __FlashStringHelper *field, int value) {
  beginPacket(F("ACK"));
  emitField(F("cmd"), cmd);
  emitField(field, value);
  endPacket();
}

void emitAckPacket(const __FlashStringHelper *cmd,
                   const __FlashStringHelper *field, float value,
                   uint8_t decimals) {
  beginPacket(F("ACK"));
  emitField(F("cmd"), cmd);
  emitField(field, value, decimals);
  endPacket();
}

void emitAckPacket(const __FlashStringHelper *cmd,
                   const __FlashStringHelper *field1, int value1,
                   const __FlashStringHelper *field2, int value2) {
  beginPacket(F("ACK"));
  emitField(F("cmd"), cmd);
  emitField(field1, value1);
  emitField(field2, value2);
  endPacket();
}

void emitAckPacket(const __FlashStringHelper *cmd,
                   const __FlashStringHelper *field1, int value1,
                   const __FlashStringHelper *field2, int value2,
                   const __FlashStringHelper *field3, int value3) {
  beginPacket(F("ACK"));
  emitField(F("cmd"), cmd);
  emitField(field1, value1);
  emitField(field2, value2);
  emitField(field3, value3);
  endPacket();
}

void emitAckPacket(const __FlashStringHelper *cmd,
                   const __FlashStringHelper *field1, int value1,
                   const __FlashStringHelper *field2, int value2,
                   const __FlashStringHelper *field3, int value3,
                   const __FlashStringHelper *field4, int value4) {
  beginPacket(F("ACK"));
  emitField(F("cmd"), cmd);
  emitField(field1, value1);
  emitField(field2, value2);
  emitField(field3, value3);
  emitField(field4, value4);
  endPacket();
}

void emitErrorPacket(const __FlashStringHelper *cmd,
                     const __FlashStringHelper *reason) {
  beginPacket(F("ERR"));
  emitField(F("cmd"), cmd);
  emitField(F("reason"), reason);
  endPacket();
}

void emitErrorPacket(const __FlashStringHelper *cmd,
                     const __FlashStringHelper *reason,
                     const __FlashStringHelper *field, int value) {
  beginPacket(F("ERR"));
  emitField(F("cmd"), cmd);
  emitField(F("reason"), reason);
  emitField(field, value);
  endPacket();
}

void emitErrorPacket(const __FlashStringHelper *cmd,
                     const __FlashStringHelper *reason,
                     const __FlashStringHelper *field1, int value1,
                     const __FlashStringHelper *field2, int value2) {
  beginPacket(F("ERR"));
  emitField(F("cmd"), cmd);
  emitField(F("reason"), reason);
  emitField(field1, value1);
  emitField(field2, value2);
  endPacket();
}

void maybeEmitStateTelemetry(unsigned long nowMs) {
  if ((nowMs - lastStateTelemetryMs) < stateTelemetryIntervalMs) {
    return;
  }

  emitStatePacket(captureRuntimeSnapshot());
}

void invalidateDisplay() {
  displayNeedsClear = true;
}

bool hasValidTemperatureReadings() {
  return !isnan(lastRawTempC) && !isnan(lastSensorCalibratedTempC) &&
         !isnan(lastEstimatedRealTempC);
}

void clearTemperatureReadings() {
  lastRawTempC = NAN;
  lastSensorCalibratedTempC = NAN;
  lastEstimatedRealTempC = NAN;
  smoothedDisplayTempC = NAN;
}

void setHeaterPwm(uint8_t pwm) {
  analogWrite(ssrPin, pwm);
  lastHeaterPwm = pwm;
}

bool loadPersistedInt16(int address, int minValue, int maxValue, int &target) {
  int16_t persistedValue = 0;
  EEPROM.get(address, persistedValue);
  if (persistedValue < minValue || persistedValue > maxValue) {
    return false;
  }

  target = persistedValue;
  return true;
}

void savePersistedInt16(int address, int value) {
  const int16_t persistedValue = (int16_t)value;
  EEPROM.put(address, persistedValue);
}

void loadPersistentSettings() {
  int persistedValue = 0;

  if (loadPersistedInt16(eepromSetpointAddr, minSetpointC, maxSetpointC,
                         persistedValue)) {
    setpointTempC = persistedValue;
  }

  if (loadPersistedInt16(eepromFanPercentAddr, minFanPercent, maxFanPercent,
                         persistedValue)) {
    fanCommandPercent = persistedValue;
  }

  if (loadPersistedInt16(eepromSensorOffsetAddr, -maxCalibrationOffsetC,
                         maxCalibrationOffsetC, persistedValue)) {
    sensorOffsetC = (int16_t)persistedValue;
  } else {
    sensorOffsetC = 0;
  }

  if (loadPersistedInt16(eepromFanActualMinAddr, fanActualMinFloor,
                         fanActualMinCeil, persistedValue)) {
    fanActualMinPercent = persistedValue;
  }
}

float applySensorCalibrationCorrection(float rawTempC) {
  return rawTempC + (float)sensorOffsetC;
}

float interpolateLinearly(float input, float input0, float input1,
                          float output0, float output1) {
  if (input1 == input0) {
    return output0;
  }

  const float ratio = (input - input0) / (input1 - input0);
  return output0 + (output1 - output0) * ratio;
}

float interpolateSeries(float input, const int16_t *inputPoints,
                        const int16_t *outputPoints, uint8_t pointCount) {
  if (pointCount == 0) {
    return input;
  }
  if (pointCount == 1) {
    return (float)outputPoints[0];
  }

  if (input <= (float)inputPoints[0]) {
    return interpolateLinearly(input, (float)inputPoints[0],
                               (float)inputPoints[1], (float)outputPoints[0],
                               (float)outputPoints[1]);
  }

  for (uint8_t index = 1; index < pointCount; index++) {
    if (input <= (float)inputPoints[index]) {
      return interpolateLinearly(
          input, (float)inputPoints[index - 1], (float)inputPoints[index],
          (float)outputPoints[index - 1], (float)outputPoints[index]);
    }
  }

  return interpolateLinearly(input, (float)inputPoints[pointCount - 2],
                             (float)inputPoints[pointCount - 1],
                             (float)outputPoints[pointCount - 2],
                             (float)outputPoints[pointCount - 1]);
}

float estimateRealTempAtAirflow(float sensorTempC, uint8_t airflowIndex) {
  return interpolateSeries(sensorTempC, airflowCalibrationSensorTempPointsC,
                           airflowCalibrationRealTempTableC[airflowIndex],
                           airflowCalibrationTempPointCount);
}

float applyAirflowCalibration(float sensorTempC, int airflowPercent) {
  if (airflowCalibrationFanPointCount == 0) {
    return sensorTempC;
  }
  if (airflowCalibrationFanPointCount == 1) {
    return estimateRealTempAtAirflow(sensorTempC, 0);
  }

  uint8_t lowerIndex = 0;
  uint8_t upperIndex = 1;

  if (airflowPercent <= airflowCalibrationFanPercentPoints[0]) {
    lowerIndex = 0;
    upperIndex = 1;
  } else if (airflowPercent >= airflowCalibrationFanPercentPoints
                                   [airflowCalibrationFanPointCount - 1]) {
    lowerIndex = airflowCalibrationFanPointCount - 2;
    upperIndex = airflowCalibrationFanPointCount - 1;
  } else {
    for (uint8_t index = 1; index < airflowCalibrationFanPointCount; index++) {
      if (airflowPercent <= airflowCalibrationFanPercentPoints[index]) {
        lowerIndex = index - 1;
        upperIndex = index;
        break;
      }
    }
  }

  const float lowerRealTempC =
      estimateRealTempAtAirflow(sensorTempC, lowerIndex);
  const float upperRealTempC =
      estimateRealTempAtAirflow(sensorTempC, upperIndex);

  return interpolateLinearly(
      (float)airflowPercent,
      (float)airflowCalibrationFanPercentPoints[lowerIndex],
      (float)airflowCalibrationFanPercentPoints[upperIndex], lowerRealTempC,
      upperRealTempC);
}

void stopBuzzer() {
  digitalWrite(buzzerPin, LOW);
  buzzerPlayback.patternMs = NULL;
  buzzerPlayback.stepCount = 0;
  buzzerPlayback.stepIndex = 0;
  buzzerPlayback.outputHigh = false;
  buzzerPlayback.active = false;
  buzzerPlayback.stepStartedMs = 0;
}

bool startBuzzerPattern(const uint16_t *patternMs, uint8_t stepCount,
                        bool overrideActive) {
  if (patternMs == NULL || stepCount == 0) {
    stopBuzzer();
    return false;
  }

  if (buzzerPlayback.active && !overrideActive) {
    return false;
  }

  buzzerPlayback.patternMs = patternMs;
  buzzerPlayback.stepCount = stepCount;
  buzzerPlayback.stepIndex = 0;
  buzzerPlayback.outputHigh = true;
  buzzerPlayback.active = true;
  buzzerPlayback.stepStartedMs = millis();
  digitalWrite(buzzerPin, HIGH);
  return true;
}

void requestButtonBeep() {
  startBuzzerPattern(
      buttonBeepPatternMs,
      sizeof(buttonBeepPatternMs) / sizeof(buttonBeepPatternMs[0]), false);
}

void requestSleepEnterBeep() {
  startBuzzerPattern(sleepEnterBeepPatternMs,
                     sizeof(sleepEnterBeepPatternMs) /
                         sizeof(sleepEnterBeepPatternMs[0]),
                     true);
}

void requestSleepExitBeep() {
  startBuzzerPattern(
      sleepExitBeepPatternMs,
      sizeof(sleepExitBeepPatternMs) / sizeof(sleepExitBeepPatternMs[0]), true);
}

bool requestSetpointReachedBeep() {
  return startBuzzerPattern(targetReachedBeepPatternMs,
                            sizeof(targetReachedBeepPatternMs) /
                                sizeof(targetReachedBeepPatternMs[0]),
                            false);
}

void updateBuzzer(unsigned long nowMs) {
  if (!buzzerPlayback.active) {
    return;
  }

  if ((nowMs - buzzerPlayback.stepStartedMs) <
      buzzerPlayback.patternMs[buzzerPlayback.stepIndex]) {
    return;
  }

  buzzerPlayback.stepIndex++;
  if (buzzerPlayback.stepIndex >= buzzerPlayback.stepCount) {
    stopBuzzer();
    return;
  }

  buzzerPlayback.outputHigh = !buzzerPlayback.outputHigh;
  digitalWrite(buzzerPin, buzzerPlayback.outputHigh ? HIGH : LOW);
  buzzerPlayback.stepStartedMs = nowMs;
}

void resetSetpointReachedBeepState() {
  setpointReachedBeepPending = true;
  setpointReachedCrossingArmed = !isnan(lastEstimatedRealTempC) &&
                                 lastEstimatedRealTempC < (float)setpointTempC;
}

void evaluateSetpointReachedBeep(bool sleepModeActive) {
  if (!setpointReachedBeepPending || sleepModeActive ||
      isCalibrationState(currentState)) {
    return;
  }

  if (isnan(lastEstimatedRealTempC)) {
    return;
  }

  if (lastEstimatedRealTempC < (float)setpointTempC) {
    setpointReachedCrossingArmed = true;
    return;
  }

  if (!setpointReachedCrossingArmed) {
    return;
  }

  if (!requestSetpointReachedBeep()) {
    return;
  }

  emitEventPacket(F("target_reached"));
  setpointReachedBeepPending = false;
  setpointReachedCrossingArmed = false;
}

void setState(ControllerState newState) {
  const ControllerState previousState = currentState;
  currentState = newState;
  const bool wasCalibrationState = isCalibrationState(previousState);
  const bool isCalibrationNow = isCalibrationState(currentState);

  if (wasCalibrationState || isCalibrationNow) {
    pid.clear();
  }

  if (currentState == STATE_CAL) {
    emitEventPacket(F("cal_enter"));
  }

  if (currentState == STATE_CAL_AIR) {
    emitEventPacket(F("cal_air_enter"));
  }

  if (currentState == STATE_TMP && wasCalibrationState) {
    emitEventPacket(F("cal_exit"));
  }

  invalidateDisplay();
  lastDisplayUpdateMs = 0;
}

void updateButton(ButtonState &button, unsigned long nowMs) {
  const bool sampledPressed = (digitalRead(button.pin) == LOW);

  if (sampledPressed != button.lastSamplePressed) {
    button.lastSamplePressed = sampledPressed;
    button.lastDebounceMs = nowMs;
  }

  if ((nowMs - button.lastDebounceMs) >= buttonDebounceMs &&
      sampledPressed != button.stablePressed) {
    button.stablePressed = sampledPressed;
    if (button.stablePressed) {
      button.pressedEvent = true;
      button.pressedSinceMs = nowMs;
      button.lastRepeatMs = nowMs;
    } else {
      button.pressedSinceMs = 0;
      button.lastRepeatMs = 0;
    }
  }
}

void suppressButtonDuringSleep(ButtonState &button, unsigned long nowMs) {
  const bool sampledPressed = (digitalRead(button.pin) == LOW);
  button.stablePressed = sampledPressed;
  button.lastSamplePressed = sampledPressed;
  button.lastDebounceMs = nowMs;
  button.pressedEvent = false;

  if (sampledPressed) {
    button.pressedSinceMs = nowMs;
    button.lastRepeatMs = nowMs;
  } else {
    button.pressedSinceMs = 0;
    button.lastRepeatMs = 0;
  }
}

bool consumePressedEvent(ButtonState &button) {
  if (!button.pressedEvent) {
    return false;
  }
  button.pressedEvent = false;
  return true;
}

bool consumeHoldRepeat(ButtonState &button, unsigned long nowMs) {
  if (!button.stablePressed || button.pressedSinceMs == 0) {
    return false;
  }

  if ((nowMs - button.pressedSinceMs) < buttonHoldRepeatDelayMs) {
    return false;
  }

  if ((nowMs - button.lastRepeatMs) < buttonHoldRepeatIntervalMs) {
    return false;
  }

  button.lastRepeatMs = nowMs;
  return true;
}

int consumeAdjustmentDelta(ButtonState &increaseButton,
                           ButtonState &decreaseButton, int pressStep,
                           int holdStep, unsigned long nowMs) {
  int delta = 0;

  if (consumePressedEvent(increaseButton) && !decreaseButton.stablePressed) {
    delta += pressStep;
  }

  if (consumePressedEvent(decreaseButton) && !increaseButton.stablePressed) {
    delta -= pressStep;
  }

  if (increaseButton.stablePressed && !decreaseButton.stablePressed &&
      consumeHoldRepeat(increaseButton, nowMs)) {
    delta += holdStep;
  }

  if (decreaseButton.stablePressed && !increaseButton.stablePressed &&
      consumeHoldRepeat(decreaseButton, nowMs)) {
    delta -= holdStep;
  }

  return delta;
}

void adjustSetpoint(int deltaC) {
  const int newValue =
      constrain(setpointTempC + deltaC, minSetpointC, maxSetpointC);
  if (newValue == setpointTempC) {
    return;
  }

  setpointTempC = newValue;
  savePersistedInt16(eepromSetpointAddr, setpointTempC);
  resetSetpointReachedBeepState();
  requestButtonBeep();
  invalidateDisplay();
}

void adjustFan(int deltaPercent) {
  const int newValue =
      constrain(fanCommandPercent + deltaPercent, minFanPercent, maxFanPercent);
  if (newValue == fanCommandPercent) {
    return;
  }

  fanCommandPercent = newValue;
  savePersistedInt16(eepromFanPercentAddr, fanCommandPercent);
  requestButtonBeep();
  invalidateDisplay();
}

void adjustCalibrationOffset(int deltaC) {
  const int newValue = constrain(sensorOffsetC + deltaC, -maxCalibrationOffsetC,
                                 maxCalibrationOffsetC);
  if (newValue == sensorOffsetC) {
    return;
  }

  sensorOffsetC = (int16_t)newValue;
  requestButtonBeep();
  invalidateDisplay();
}

void commitCalibration() {
  savePersistedInt16(eepromSensorOffsetAddr, sensorOffsetC);
  requestButtonBeep();
  setState(STATE_CAL_AIR);
}

void adjustAirCalibration(int deltaPercent) {
  const int newValue = constrain(fanActualMinPercent + deltaPercent,
                                 fanActualMinFloor, fanActualMinCeil);
  if (newValue == fanActualMinPercent) {
    return;
  }

  fanActualMinPercent = newValue;
  requestButtonBeep();
  invalidateDisplay();
}

void commitAirCalibration() {
  savePersistedInt16(eepromFanActualMinAddr, fanActualMinPercent);
  requestButtonBeep();
  setState(STATE_TMP);
}

void handleCalibrationButtons(unsigned long nowMs) {
  if (currentState == STATE_CAL) {
    if (consumePressedEvent(okButton)) {
      commitCalibration();
      return;
    }

    const int delta = consumeAdjustmentDelta(upButton, downButton, 1, 1, nowMs);
    if (delta != 0) {
      adjustCalibrationOffset(delta);
    }
  } else if (currentState == STATE_CAL_AIR) {
    if (consumePressedEvent(okButton)) {
      commitAirCalibration();
      return;
    }

    const int delta = consumeAdjustmentDelta(upButton, downButton,
                                             fanStepPercent, fanStepPercent,
                                             nowMs);
    if (delta != 0) {
      adjustAirCalibration(delta);
    }
  }
}

void handleRuntimeButtons(unsigned long nowMs) {
  const bool bothHeld = upButton.stablePressed && downButton.stablePressed;

  if (bothHeld) {
    if (comboHoldStartMs == 0) {
      comboHoldStartMs = nowMs;
    }
    if (!comboTriggered && (nowMs - comboHoldStartMs) >= buttonComboHoldMs) {
      comboTriggered = true;
      setState(STATE_CAL);
      return;
    }
  } else {
    comboHoldStartMs = 0;
    comboTriggered = false;
  }

  if (bothHeld) {
    upButton.pressedEvent = false;
    downButton.pressedEvent = false;
    return;
  }

  if (consumePressedEvent(okButton)) {
    if (currentState == STATE_TMP) {
      requestButtonBeep();
      setState(STATE_FAN);
    } else if (currentState == STATE_FAN) {
      requestButtonBeep();
      setState(STATE_TMP);
    }
    return;
  }

  const int pressStep =
      (currentState == STATE_TMP) ? setpointStepC : fanStepPercent;
  const int delta =
      consumeAdjustmentDelta(upButton, downButton, pressStep, holdAdjustStep,
                             nowMs);
  if (delta == 0) {
    return;
  }

  if (currentState == STATE_TMP) {
    adjustSetpoint(delta);
  } else if (currentState == STATE_FAN) {
    adjustFan(delta);
  }
}

void handleButtons(unsigned long nowMs) {
  if (digitalRead(sleepPin) == LOW) {
    suppressButtonDuringSleep(upButton, nowMs);
    suppressButtonDuringSleep(downButton, nowMs);
    suppressButtonDuringSleep(okButton, nowMs);
    comboHoldStartMs = 0;
    comboTriggered = false;
    return;
  }

  updateButton(upButton, nowMs);
  updateButton(downButton, nowMs);
  updateButton(okButton, nowMs);

  if (isCalibrationState(currentState)) {
    handleCalibrationButtons(nowMs);
  } else {
    handleRuntimeButtons(nowMs);
  }
}

void renderRuntimeScreen() {
  static uint8_t lastDegreeColumn = 9;
  static unsigned long nearSetpointSinceMs = 0;
  static bool snappedToSetpoint = false;

  if (isnan(smoothedDisplayTempC)) {
    myLCD.setCursor(0, 0);
    myLCD.print(F("TC ERROR        "));
    myLCD.setCursor(0, 1);
    myLCD.print(F("Heater OFF      "));
    nearSetpointSinceMs = 0;
    snappedToSetpoint = false;
    return;
  }

  const unsigned long nowMs = millis();
  if (abs(smoothedDisplayTempC - (float)setpointTempC) <= 10.0f) {
    if (nearSetpointSinceMs == 0) {
      nearSetpointSinceMs = nowMs;
    } else if (!snappedToSetpoint && (nowMs - nearSetpointSinceMs >= 2000)) {
      snappedToSetpoint = true;
    }
  } else {
    nearSetpointSinceMs = 0;
    snappedToSetpoint = false;
  }

  const int displayTempC =
      constrain((int)lroundf(snappedToSetpoint ? (float)setpointTempC
                                               : smoothedDisplayTempC),
                0, 999);
  const uint8_t digitCount =
      (displayTempC >= 100) ? 3 : ((displayTempC >= 10) ? 2 : 1);
  const uint8_t degreeColumn = digitCount * 3;
  char bigTempText[4];
  snprintf(bigTempText, sizeof(bigTempText), "%-3d", displayTempC);

  myLCD.setCursor(lastDegreeColumn, 0);
  myLCD.print(' ');

  bigNumberLCD.setBigNumberCursor(0);
  bigNumberLCD.print(bigTempText);

  myLCD.setCursor(degreeColumn, 0);
  myLCD.write((uint8_t)223);
  myLCD.setCursor(degreeColumn, 1);
  myLCD.print(' ');
  lastDegreeColumn = degreeColumn;

  myLCD.setCursor(10, 0);
  myLCD.print(F("      "));
  myLCD.setCursor(10, 1);
  myLCD.print(F("      "));
  myLCD.setCursor(11, 0);
  myLCD.print("|");
  myLCD.setCursor(11, 1);
  myLCD.print("|");

  if (currentState == STATE_TMP) {
    myLCD.setCursor(13, 0);
    myLCD.print(F("SET"));
    char rightLine[5];
    snprintf(rightLine, sizeof(rightLine), "%4d", setpointTempC);
    myLCD.setCursor(12, 1);
    myLCD.print(rightLine);
  } else {
    myLCD.setCursor(13, 0);
    myLCD.print(F("AIR"));
    char rightLine[5];
    snprintf(rightLine, sizeof(rightLine), "%3d%%", fanCommandPercent);
    myLCD.setCursor(12, 1);
    myLCD.print(rightLine);
  }
}

void renderSleepScreen() {
  myLCD.setCursor(3, 0);
  myLCD.print(F("Sleep Mode"));
  myLCD.setCursor(0, 1);
  myLCD.print(F("                "));
}

void renderCalibrationScreen() {
  char tmpField[5];
  if (isnan(lastRawTempC)) {
    strcpy(tmpField, "---");
  } else {
    const int roundedRaw = constrain((int)lroundf(lastRawTempC), 0, 999);
    snprintf(tmpField, sizeof(tmpField), "%3d", roundedRaw);
  }

  myLCD.setCursor(0, 0);
  myLCD.print(F("RAW: "));
  myLCD.print(tmpField);
  myLCD.print(F("C      "));

  char row1[17];
  const char sign = (sensorOffsetC >= 0) ? '+' : '-';
  const int absOffset = abs(sensorOffsetC);
  snprintf(row1, sizeof(row1), "OFF: %c%d        ", sign, absOffset);

  myLCD.setCursor(0, 1);
  myLCD.print(row1);
}

void renderAirCalibrationScreen() {
  const int actualMapped = mapFanDisplayToActual(fanDisplayMinPercent);

  myLCD.setCursor(0, 0);
  myLCD.print(F("DISP 10% ACT:"));
  char actField[5];
  snprintf(actField, sizeof(actField), "%3d", actualMapped);
  myLCD.print(actField);

  char row1[17];
  snprintf(row1, sizeof(row1), "MIN ACT: %d%%    ", fanActualMinPercent);

  myLCD.setCursor(0, 1);
  myLCD.print(row1);
}

void updateDisplay(unsigned long nowMs) {
  const bool sleepModeActive = (digitalRead(sleepPin) == LOW);
  static bool lastSleepDisplayMode = false;
  if (sleepModeActive != lastSleepDisplayMode) {
    invalidateDisplay();
    lastSleepDisplayMode = sleepModeActive;
  }

  const unsigned long intervalMs =
      sleepModeActive
          ? displayIntervalRuntimeMs
          : (isCalibrationState(currentState) ? displayIntervalCalibrationMs
                                              : displayIntervalRuntimeMs);

  if (!displayNeedsClear && (nowMs - lastDisplayUpdateMs) < intervalMs) {
    return;
  }

  lastDisplayUpdateMs = nowMs;

  if (displayNeedsClear) {
    myLCD.clear();
    displayNeedsClear = false;
  }

  if (sleepModeActive) {
    renderSleepScreen();
    return;
  }

  if (isCalibrationState(currentState)) {
    if (currentState == STATE_CAL) {
      renderCalibrationScreen();
    } else {
      renderAirCalibrationScreen();
    }
  } else {
    renderRuntimeScreen();
  }
}

void handleThermocoupleValidityTransition(bool tcValid) {
  if (!lastThermocoupleValidKnown) {
    lastThermocoupleValidKnown = true;
    lastThermocoupleValid = tcValid;
    return;
  }

  if (tcValid == lastThermocoupleValid) {
    return;
  }

  lastThermocoupleValid = tcValid;
  emitEventPacket(tcValid ? F("tc_recovered") : F("tc_fault"));
}

void readThermocouple() {
  const double raw = tc.readCelsius();
  if (isnan(raw)) {
    handleThermocoupleValidityTransition(false);
    clearTemperatureReadings();
    return;
  }

  handleThermocoupleValidityTransition(true);
  lastRawTempC = (float)raw;

  const int actualFanPercent = mapFanDisplayToActual(fanCommandPercent);

  if (calibrationEnabled) {
    lastSensorCalibratedTempC = applySensorCalibrationCorrection(lastRawTempC);
    lastEstimatedRealTempC =
        applyAirflowCalibration(lastSensorCalibratedTempC, actualFanPercent);
  } else {
    // Calibration bypassed: use raw value for everything.
    lastSensorCalibratedTempC = lastRawTempC;
    lastEstimatedRealTempC = lastRawTempC;
  }

  if (calibrationEnabled) {
    if (isnan(smoothedDisplayTempC)) {
      smoothedDisplayTempC = lastEstimatedRealTempC;
    } else {
      smoothedDisplayTempC =
          smoothedDisplayTempC * 0.85f + lastEstimatedRealTempC * 0.15f;
    }
  } else {
    // Calibration off: no averaging, display raw value immediately.
    smoothedDisplayTempC = lastEstimatedRealTempC;
  }
}

void handleSleepTransition(bool sleepModeActive) {
  if (sleepModeActive == lastSleepModeActive) {
    return;
  }

  sleepCooldownComplete = false;

  if (sleepModeActive) {
    requestSleepEnterBeep();
    emitEventPacket(F("sleep_enter"));
  } else {
    requestSleepExitBeep();
    emitEventPacket(F("sleep_exit"));
  }

  lastSleepModeActive = sleepModeActive;
}

void controlOutputs(bool sleepModeActive) {
  if (!hasValidTemperatureReadings()) {
    setHeaterPwm(0);
    if (sleepModeActive && sleepCooldownComplete) {
      setFanDutyPercent(0.0f);
    } else {
      setFanDutyPercent(100.0f);
    }
    return;
  }

  if (sleepModeActive) {
    setHeaterPwm(0);

    if (sleepCooldownComplete) {
      setFanDutyPercent(0.0f);
    } else if (lastRawTempC >= sleepCooldownTempC) {
      setFanDutyPercent(100.0f);
    } else {
      sleepCooldownComplete = true;
      setFanDutyPercent(0.0f);
    }
    return;
  }

  setFanDutyPercent((float)mapFanDisplayToActual(fanCommandPercent));

  const uint8_t output =
      pid.step(setpointTempC, (int)lroundf(lastEstimatedRealTempC));
  setHeaterPwm(output);
  evaluateSetpointReachedBeep(sleepModeActive);
}

void initFanPwm() {
  pinMode(fanPwmPin, OUTPUT);

  // Timer2: phase-correct PWM, TOP=0xFF, OC2B non-inverting, prescaler=1.
  // Frequency = 16 MHz / (510 * 1) = ~31.37 kHz.
  TCCR2A = 0;
  TCCR2B = 0;
  TCNT2 = 0;

  TCCR2A |= _BV(COM2B1) | _BV(WGM20);
  TCCR2B |= _BV(CS20);
}

void setFanDutyPercent(float percent) {
  if (percent < 0.0f) {
    percent = 0.0f;
  }
  if (percent > 100.0f) {
    percent = 100.0f;
  }

  lastFanActualPercent = constrain((int)lroundf(percent), 0, 100);
  OCR2B = (uint8_t)((percent * 255.0f / 100.0f) + 0.5f);
}

// Maps display fan percent (10-100) to actual fan percent (30-100).
// At display 10% the actual output is 30%, linearly scaling to 100% at display
// 100%.
int mapFanDisplayToActual(int displayPercent) {
  if (displayPercent <= fanDisplayMinPercent) {
    return fanActualMinPercent;
  }
  if (displayPercent >= maxFanPercent) {
    return maxFanPercent;
  }
  return fanActualMinPercent + (displayPercent - fanDisplayMinPercent) *
                                   (maxFanPercent - fanActualMinPercent) /
                                   (maxFanPercent - fanDisplayMinPercent);
}

bool parseFloatArgument(const char *text, float &value) {
  char *end_ptr = NULL;
  const double parsed_value = strtod(text, &end_ptr);
  if (end_ptr == text || *end_ptr != '\0') {
    return false;
  }

  value = (float)parsed_value;
  return true;
}

bool parseLongArgument(const char *text, long &value) {
  char *endPtr = NULL;
  value = strtol(text, &endPtr, 10);
  return endPtr != text && *endPtr == '\0';
}

bool parseCalibrationRowArguments(const char *text, int &fanIdx, int &tempIdx,
                                  int &cellValue) {
  char *endPtr = NULL;
  const long parsedFanIdx = strtol(text, &endPtr, 10);
  if (endPtr == text || *endPtr == '\0') {
    return false;
  }

  text = endPtr;
  while (*text == ' ') {
    text++;
  }

  const long parsedTempIdx = strtol(text, &endPtr, 10);
  if (endPtr == text || *endPtr == '\0') {
    return false;
  }

  text = endPtr;
  while (*text == ' ') {
    text++;
  }

  const long parsedCellValue = strtol(text, &endPtr, 10);
  if (endPtr == text || *endPtr != '\0') {
    return false;
  }

  fanIdx = (int)parsedFanIdx;
  tempIdx = (int)parsedTempIdx;
  cellValue = (int)parsedCellValue;
  return true;
}

bool applyPidConfiguration(float newKp, float newKi, float newKd, float newHz) {
  if (!pid.configure(newKp, newKi, newKd, newHz, outputBits, outputSigned)) {
    pid.configure(Kp, Ki, Kd, Hz, outputBits, outputSigned);
    return false;
  }

  Kp = newKp;
  Ki = newKi;
  Kd = newKd;
  Hz = newHz;
  return true;
}

void printHelp() {
  Serial.println(F("--- Commands ---"));
  Serial.println(F("  SET <100-500>      Set temperature setpoint (C)"));
  Serial.println(F("  FAN <10-100>       Set fan speed (%)"));
  Serial.println(F("  FANMIN <10-100>    Set fan actual-min % (calibration)"));
  Serial.println(F("  OFFSET <-500-500>  Set sensor offset (C)"));
  Serial.println(F("  KP <value>         Set PID Kp"));
  Serial.println(F("  KI <value>         Set PID Ki"));
  Serial.println(F("  KD <value>         Set PID Kd"));
  Serial.println(F("  HZ <value>         Set PID update rate (Hz)"));
  Serial.println(F("  CALEN              Enable calibration (offset + airflow)"));
  Serial.println(F("  CALDIS             Disable calibration (bypass offset+airflow)"));
  Serial.println(F("  CALROW <fi> <ti> <val>  Set cal table cell (fan-idx 0-2, temp-idx 0-8)"));
  Serial.println(F("  STATUS             Print current settings"));
  Serial.println(F("  INFO               Print firmware/protocol info"));
  Serial.println(F("  HELP               Print this help"));
}

void handleSerialCommand(const char *line) {
  // --- SET <temp> ---
  if (strncmp(line, "SET ", 4) == 0) {
    long value = 0;
    if (!parseLongArgument(line + 4, value)) {
      emitErrorPacket(F("SET"), F("parse"));
      return;
    }
    if (value < minSetpointC || value > maxSetpointC) {
      emitErrorPacket(F("SET"), F("range"), F("min"), minSetpointC, F("max"),
                      maxSetpointC);
      return;
    }
    const int newValue = (int)value;
    if (newValue != setpointTempC) {
      setpointTempC = newValue;
      savePersistedInt16(eepromSetpointAddr, setpointTempC);
      resetSetpointReachedBeepState();
    }
    invalidateDisplay();
    emitAckPacket(F("SET"), F("set"), setpointTempC);
    return;
  }

  // --- FAN <percent> ---
  if (strncmp(line, "FAN ", 4) == 0) {
    long value = 0;
    if (!parseLongArgument(line + 4, value)) {
      emitErrorPacket(F("FAN"), F("parse"));
      return;
    }
    if (value < fanDisplayMinPercent || value > maxFanPercent) {
      emitErrorPacket(F("FAN"), F("range"), F("min"), fanDisplayMinPercent,
                      F("max"), maxFanPercent);
      return;
    }
    fanCommandPercent = (int)value;
    savePersistedInt16(eepromFanPercentAddr, fanCommandPercent);
    invalidateDisplay();
    emitAckPacket(F("FAN"), F("fan_cmd"), fanCommandPercent);
    return;
  }

  // --- FANMIN <percent> ---
  if (strncmp(line, "FANMIN ", 7) == 0) {
    long value = 0;
    if (!parseLongArgument(line + 7, value)) {
      emitErrorPacket(F("FANMIN"), F("parse"));
      return;
    }
    if (value < fanActualMinFloor || value > fanActualMinCeil) {
      emitErrorPacket(F("FANMIN"), F("range"), F("min"), fanActualMinFloor,
                      F("max"), fanActualMinCeil);
      return;
    }
    fanActualMinPercent = (int)value;
    savePersistedInt16(eepromFanActualMinAddr, fanActualMinPercent);
    emitAckPacket(F("FANMIN"), F("fan_min"), fanActualMinPercent);
    return;
  }

  // --- OFFSET <value> ---
  if (strncmp(line, "OFFSET ", 7) == 0) {
    long value = 0;
    if (!parseLongArgument(line + 7, value)) {
      emitErrorPacket(F("OFFSET"), F("parse"));
      return;
    }
    if (value < -maxCalibrationOffsetC || value > maxCalibrationOffsetC) {
      emitErrorPacket(F("OFFSET"), F("range"), F("min"),
                      -maxCalibrationOffsetC, F("max"), maxCalibrationOffsetC);
      return;
    }
    sensorOffsetC = (int16_t)value;
    savePersistedInt16(eepromSensorOffsetAddr, sensorOffsetC);
    emitAckPacket(F("OFFSET"), F("offset"), sensorOffsetC);
    return;
  }

  // --- KP <value> ---
  if (strncmp(line, "KP ", 3) == 0) {
    float value = 0.0f;
    if (!parseFloatArgument(line + 3, value)) {
      emitErrorPacket(F("KP"), F("parse"));
      return;
    }
    if (!applyPidConfiguration(value, Ki, Kd, Hz)) {
      emitErrorPacket(F("KP"), F("range"));
      return;
    }
    emitAckPacket(F("KP"), F("kp"), Kp, 6);
    return;
  }

  // --- KI <value> ---
  if (strncmp(line, "KI ", 3) == 0) {
    float value = 0.0f;
    if (!parseFloatArgument(line + 3, value)) {
      emitErrorPacket(F("KI"), F("parse"));
      return;
    }
    if (!applyPidConfiguration(Kp, value, Kd, Hz)) {
      emitErrorPacket(F("KI"), F("range"));
      return;
    }
    emitAckPacket(F("KI"), F("ki"), Ki, 6);
    return;
  }

  // --- KD <value> ---
  if (strncmp(line, "KD ", 3) == 0) {
    float value = 0.0f;
    if (!parseFloatArgument(line + 3, value)) {
      emitErrorPacket(F("KD"), F("parse"));
      return;
    }
    if (!applyPidConfiguration(Kp, Ki, value, Hz)) {
      emitErrorPacket(F("KD"), F("range"));
      return;
    }
    emitAckPacket(F("KD"), F("kd"), Kd, 6);
    return;
  }

  // --- HZ <value> ---
  if (strncmp(line, "HZ ", 3) == 0) {
    float value = 0.0f;
    if (!parseFloatArgument(line + 3, value)) {
      emitErrorPacket(F("HZ"), F("parse"));
      return;
    }
    if (value <= 0.0f) {
      emitErrorPacket(F("HZ"), F("range"), F("min"), 1);
      return;
    }
    if (!applyPidConfiguration(Kp, Ki, Kd, value)) {
      emitErrorPacket(F("HZ"), F("range"));
      return;
    }
    emitAckPacket(F("HZ"), F("hz"), Hz, 6);
    return;
  }

  // --- CALEN ---
  if (strcmp(line, "CALEN") == 0) {
    calibrationEnabled = true;
    emitAckPacket(F("CALEN"), F("cal"), 1);
    return;
  }

  // --- CALDIS ---
  if (strcmp(line, "CALDIS") == 0) {
    calibrationEnabled = false;
    emitAckPacket(F("CALDIS"), F("cal"), 0);
    return;
  }

  // --- CALROW <fanIdx> <tempIdx> <value> ---
  if (strncmp(line, "CALROW ", 7) == 0) {
    int fanIdx = -1, tempIdx = -1, cellVal = 0;
    if (!parseCalibrationRowArguments(line + 7, fanIdx, tempIdx, cellVal)) {
      emitErrorPacket(F("CALROW"), F("parse"));
      return;
    }

    if (fanIdx < 0 || fanIdx >= (int)airflowCalibrationFanPointCount) {
      emitErrorPacket(F("CALROW"), F("fan_idx_range"), F("min"), 0, F("max"),
                      (int)airflowCalibrationFanPointCount - 1);
      return;
    }
    if (tempIdx < 0 || tempIdx >= (int)airflowCalibrationTempPointCount) {
      emitErrorPacket(F("CALROW"), F("temp_idx_range"), F("min"), 0, F("max"),
                      (int)airflowCalibrationTempPointCount - 1);
      return;
    }

    airflowCalibrationRealTempTableC[fanIdx][tempIdx] = (int16_t)cellVal;
    emitAckPacket(F("CALROW"), F("fan_idx"), fanIdx, F("temp_idx"), tempIdx,
                  F("value"), cellVal);
    return;
  }

  // --- STATUS ---
  if (strcmp(line, "STATUS") == 0) {
    emitStatePacket(captureRuntimeSnapshot());
    return;
  }

  // --- INFO ---
  if (strcmp(line, "INFO") == 0) {
    emitBootPacket();
    return;
  }

  // --- HELP ---
  if (strcmp(line, "HELP") == 0) {
    printHelp();
    return;
  }

  emitErrorPacket(F("CMD"), F("parse"));
}

void handleSerialInput() {
  while (Serial.available() > 0) {
    const char c = (char)Serial.read();

    if (c == '\r') {
      continue;
    }

    if (c == '\n') {
      if (serialLineOverflow) {
        emitErrorPacket(F("LINE"), F("parse"));
      } else if (serialLineIndex > 0) {
        serialLineBuffer[serialLineIndex] = '\0';
        handleSerialCommand(serialLineBuffer);
      }

      serialLineIndex = 0;
      serialLineOverflow = false;
      continue;
    }

    if (serialLineOverflow) {
      continue;
    }

    if (serialLineIndex < serialLineBufferSize - 1) {
      serialLineBuffer[serialLineIndex++] = c;
    } else {
      serialLineOverflow = true;
    }
  }
}

void setup() {
  Serial.begin(serialBaudRate);

  pinMode(ssrPin, OUTPUT);
  pinMode(buzzerPin, OUTPUT);
  pinMode(sleepPin, INPUT_PULLUP);
  pinMode(upButtonPin, INPUT_PULLUP);
  pinMode(downButtonPin, INPUT_PULLUP);
  pinMode(okButtonPin, INPUT_PULLUP);

  stopBuzzer();
  loadPersistentSettings();
  lastSleepModeActive = (digitalRead(sleepPin) == LOW);
  resetSetpointReachedBeepState();

  initFanPwm();
  setFanDutyPercent((float)mapFanDisplayToActual(fanCommandPercent));
  if (!pid.configure(Kp, Ki, Kd, Hz, outputBits, outputSigned)) {
    emitErrorPacket(F("BOOT"), F("pid_config"));
  }

  myLCD.begin(LCD_COLUMNS, LCD_ROWS);
  myLCD.backlight();
  bigNumberLCD.begin();

  setState(STATE_TMP);
  emitBootPacket();
}

void loop() {
  const unsigned long nowMs = millis();

  handleSerialInput();
  updateBuzzer(nowMs);
  handleButtons(nowMs);

  if (nowMs - lastControlUpdateMs >= controlIntervalMs) {
    lastControlUpdateMs = nowMs;
    const bool sleepModeActive = (digitalRead(sleepPin) == LOW);
    handleSleepTransition(sleepModeActive);
    readThermocouple();
    controlOutputs(sleepModeActive);
  }

  maybeEmitStateTelemetry(nowMs);
  updateDisplay(nowMs);
}
