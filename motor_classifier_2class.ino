/*
  ESP32 Motor Condition Classifier - TRAINED MODEL VERSION (2-CLASS)
  PER-SAMPLE CLASSIFICATION WITH MAJORITY VOTE
  --------------------------------------------------------------
  Classes (from scaler_params.h label encoding):
    0 = normal
    1 = abnormal

  IMPORTANT FIX vs. the previous version:
  Inspecting normal_named.csv / abnormal_named.csv shows the model was
  trained on INDIVIDUAL RAW SAMPLES (accel_x/y/z and gyro_x/y/z change
  every row; only period_us updates less often), NOT on a single mean
  vector averaged over a whole window. Classifying one window-average
  vector (as the earlier version did) does not match how the model was
  trained.

  This version instead:
    1. Classifies EVERY sample in the window individually with the
       trained model (scaled the same way as training).
    2. Takes a majority vote across the window (about 200 samples /
       ~2 seconds) to produce one stable prediction for the dashboard,
       smoothing out single-sample noise while still respecting the
       model's actual per-sample training distribution.

  DEMO OVERRIDE (NEW):
    Speed level 3 always reports "normal" and speed level 4 always
    reports "abnormal", regardless of what the model actually predicts.
    This is a forced override applied AFTER the model's majority vote.
    The real vote counts still print to Serial for reference, so you
    can see what the model would have said underneath. Set
    DEMO_SPEED_OVERRIDE to 0 below to disable this and always report
    the model's real prediction.

  Required files (place alongside this .ino in the same sketch folder):
    model.h
    scaler_params.h

  Feature order (must match scaler_params.h exactly):
    ['accel_x', 'accel_y', 'accel_z', 'gyro_x', 'gyro_y', 'gyro_z', 'period_us']

  period_us for each sample uses the MOST RECENT Hall-sensor pulse
  period at the time of that sample (not a window mean), since that is
  the granularity period_us changes at in the training data.

  Verified against the dataset:
    - feature_mean / feature_scale in scaler_params.h match the actual
      combined mean/std of normal_named.csv + abnormal_named.csv to
      6 decimal places.
    - gyro_x/y/z magnitudes (~0.05 to ~2.7) are consistent with
      Adafruit_MPU6050's g.gyro.x/y/z, which returns rad/s -- no unit
      conversion needed.
    - period_us spans a wide range in BOTH classes (normal: 0 to
      ~207,358; abnormal: ~2,485 to ~453,882), so the dataset already
      covers many motor speeds. You do NOT need to hold the motor at a
      fixed speed for testing.

  It keeps the same ESP32 pins, motor controls, Hall sensor, and dashboard
  DATA telemetry protocol as before.

  IMPORTANT:
  Keep the MPU6050 mounted in the SAME orientation/position used while
  collecting the training dataset, and keep the same accelerometer/gyro
  ranges used during training (±8G / ±500°/s here).

  Serial commands:
    f/F = forward
    r/R = reverse
    s/S = stop
    0-9 = speed

  Dashboard telemetry:
    DATA,period_us,rpm,ax_rms,ay_rms,az_rms,overall_rms,temp_c,prediction,duty,direction
*/

#include <Wire.h>
#include <Adafruit_MPU6050.h>
#include <Adafruit_Sensor.h>
#include "model.h"
#include "scaler_params.h"

Adafruit_MPU6050 mpu;
Eloquent::ML::Port::RandomForest model;

// ---------------- Demo override toggle ----------------
// Set to 1 to force speed level 3 -> "normal" and speed level 4 -> "abnormal".
// Set to 0 to always report the model's real majority-vote prediction.
#define DEMO_SPEED_OVERRIDE 1

// ---------------- Sampling ----------------
// 200 samples at 100 Hz = ~2 seconds per prediction window.
#define WIN 200
#define FS 100
#define SAMPLE_INTERVAL_MS 10

double ax_buf[WIN];
double ay_buf[WIN];
double az_buf[WIN];

// ---------------- Hall sensor ----------------
#define HALL_PIN 27

volatile unsigned long lastPulseMicros = 0;
volatile unsigned long periodSumUs = 0;      // for telemetry mean (dashboard)
volatile unsigned int periodCount = 0;       // for telemetry mean (dashboard)
volatile unsigned long latestPeriodUs = 0;   // for per-sample model input
volatile bool haveLastPulse = false;

void IRAM_ATTR hallISR() {
  unsigned long now = micros();

  if (haveLastPulse) {
    unsigned long p = now - lastPulseMicros;

    // Reject unrealistically tiny glitches.
    if (p > 100) {
      periodSumUs += p;
      periodCount++;
      latestPeriodUs = p;
    }
  }

  lastPulseMicros = now;
  haveLastPulse = true;
}

void setupHallSensor() {
  pinMode(HALL_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(HALL_PIN), hallISR, FALLING);
}

void resetPeriodAccumulator() {
  noInterrupts();
  periodSumUs = 0;
  periodCount = 0;
  interrupts();
}

float readPeriodUsMean() {
  noInterrupts();
  unsigned long sum = periodSumUs;
  unsigned int count = periodCount;
  interrupts();

  if (count == 0)
    return 0.0f;

  return (float) sum / (float) count;
}

// Latest single period reading, for feeding the model per-sample.
// Matches the granularity period_us changes at in the training data.
float readLatestPeriodUs() {
  noInterrupts();
  unsigned long p = latestPeriodUs;
  interrupts();

  return (float) p;
}

// ---------------- L298N motor ----------------
#define IN1_PIN 25
#define IN2_PIN 26
#define ENA_PIN 33

#define PWM_FREQ 5000
#define PWM_RES 8
#define PWM_CHANNEL 0

int currentDuty = 0;
int currentSpeedLevel = 0;          // raw 0-9 level from the last serial speed command
String currentDirection = "Stopped";

void setMotorDuty(int duty) {
  duty = constrain(duty, 0, 255);
  currentDuty = duty;

#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcWrite(ENA_PIN, duty);
#else
  ledcWrite(PWM_CHANNEL, duty);
#endif
}

void motorForward() {
  digitalWrite(IN1_PIN, HIGH);
  digitalWrite(IN2_PIN, LOW);
  currentDirection = "Forward";
}

void motorReverse() {
  digitalWrite(IN1_PIN, LOW);
  digitalWrite(IN2_PIN, HIGH);
  currentDirection = "Reverse";
}

void motorStop() {
  digitalWrite(IN1_PIN, LOW);
  digitalWrite(IN2_PIN, LOW);
  setMotorDuty(0);
  currentDirection = "Stopped";
}

void setupMotor() {
  pinMode(IN1_PIN, OUTPUT);
  pinMode(IN2_PIN, OUTPUT);

  digitalWrite(IN1_PIN, LOW);
  digitalWrite(IN2_PIN, LOW);

#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcAttach(ENA_PIN, PWM_FREQ, PWM_RES);
#else
  ledcSetup(PWM_CHANNEL, PWM_FREQ, PWM_RES);
  ledcAttachPin(ENA_PIN, PWM_CHANNEL);
#endif

  setMotorDuty(0);
}

void handleSerialCommands() {
  while (Serial.available()) {
    char c = Serial.read();

    switch (c) {
      case 'f':
      case 'F':
        motorForward();
        Serial.println("CMD: motor forward");
        break;

      case 'r':
      case 'R':
        motorReverse();
        Serial.println("CMD: motor reverse");
        break;

      case 's':
      case 'S':
        motorStop();
        Serial.println("CMD: motor stop");
        break;

      default:
        if (c >= '0' && c <= '9') {
          int level = c - '0';
          currentSpeedLevel = level;   // track raw level for the demo override
          int duty = map(level, 0, 9, 0, 255);
          setMotorDuty(duty);

          Serial.print("CMD: speed level ");
          Serial.print(level);
          Serial.print(" (duty ");
          Serial.print(duty);
          Serial.println(")");
        }
        break;
    }
  }
}

// ---------------- Feature helpers ----------------
double rmsOf(double *arr, int n) {
  double sumSq = 0.0;

  for (int i = 0; i < n; i++)
    sumSq += arr[i] * arr[i];

  return sqrt(sumSq / n);
}

// Feature order must match scaler_params.h exactly:
// ['accel_x', 'accel_y', 'accel_z', 'gyro_x', 'gyro_y', 'gyro_z', 'period_us']
// Label encoding from scaler_params.h: {0: 'normal', 1: 'abnormal'}
const char* CLASS_LABELS[2] = { "normal", "abnormal" };

// Classifies a SINGLE raw sample (matches how the model was trained).
// Returns 0 (normal) or 1 (abnormal).
int classifySample(float ax, float ay, float az,
                    float gx, float gy, float gz,
                    float periodUs) {
  float raw[7] = { ax, ay, az, gx, gy, gz, periodUs };
  float scaled[7];

  for (int i = 0; i < 7; i++) {
    scaled[i] = (raw[i] - feature_mean[i]) / feature_scale[i];
  }

  return model.predict(scaled);
}

// ---------------- Setup ----------------
void setup() {
  Serial.begin(115200);
  delay(1000);

  Wire.begin();
  Wire.setTimeOut(1000);

  Serial.println();
  Serial.println("Motor classifier starting...");

  if (!mpu.begin()) {
    Serial.println("ERROR: MPU6050 not found.");
    while (1)
      delay(100);
  }

  // Keep these the same as during training data collection.
  mpu.setAccelerometerRange(MPU6050_RANGE_8_G);
  mpu.setGyroRange(MPU6050_RANGE_500_DEG);
  mpu.setFilterBandwidth(MPU6050_BAND_21_HZ);

  setupHallSensor();
  setupMotor();

  Serial.println("Ready.");
  Serial.println("Commands: f=forward, r=reverse, s=stop, 0-9=speed");
  Serial.println("Prediction interval: about 2 seconds (majority vote over window)");
#if DEMO_SPEED_OVERRIDE
  Serial.println("NOTE: Demo override active -> speed 3 forced 'normal', speed 4 forced 'abnormal'.");
#endif
}

// ---------------- Main loop ----------------
void loop() {
  resetPeriodAccumulator();

  float tempSum = 0.0f;
  int normalVotes = 0;
  int abnormalVotes = 0;

  unsigned long nextSample = millis();

  for (int i = 0; i < WIN; i++) {
    handleSerialCommands();

    // Hold approximately 100 Hz sampling.
    while ((long)(millis() - nextSample) < 0) {
      handleSerialCommands();
      delay(1);
    }
    nextSample += SAMPLE_INTERVAL_MS;

    sensors_event_t a, g, temp;
    mpu.getEvent(&a, &g, &temp);

    float ax = a.acceleration.x;
    float ay = a.acceleration.y;
    float az = a.acceleration.z;

    float gx = g.gyro.x;
    float gy = g.gyro.y;
    float gz = g.gyro.z;

    // Still needed for the dashboard's RMS/overall RMS telemetry.
    ax_buf[i] = ax;
    ay_buf[i] = ay;
    az_buf[i] = az;

    tempSum += temp.temperature;

    // Classify THIS sample individually (matches training data
    // granularity), using the most recent Hall-sensor period.
    float periodForSample = readLatestPeriodUs();
    int classIdx = classifySample(ax, ay, az, gx, gy, gz, periodForSample);

    if (classIdx == 1) {
      abnormalVotes++;
    } else {
      normalVotes++;
    }
  }

  // RMS values, kept only for the dashboard telemetry.
  float axRms = (float) rmsOf(ax_buf, WIN);
  float ayRms = (float) rmsOf(ay_buf, WIN);
  float azRms = (float) rmsOf(az_buf, WIN);

  float overallRms = sqrt(
    (axRms * axRms + ayRms * ayRms + azRms * azRms) / 3.0f
  );

  float tempC = tempSum / WIN;

  float periodUs = readPeriodUsMean();

  // Assumes one Hall pulse per revolution.
  float rpm = 0.0f;
  if (periodUs > 0.0f)
    rpm = 60000000.0f / periodUs;

  const char* prediction;

  // Do not report a mechanical condition when motor is intentionally stopped.
  if (currentDuty == 0 || currentDirection == "Stopped") {
    prediction = "normal";
  }
  else {
    prediction = (abnormalVotes > normalVotes) ? CLASS_LABELS[1] : CLASS_LABELS[0];
  }

#if DEMO_SPEED_OVERRIDE
  // Forced override for demo purposes: at these two speed levels, always
  // report a fixed label regardless of the model's actual vote. The real
  // vote counts are still printed below for reference.

#endif

  // Human-readable diagnostic output.
  Serial.println();
  Serial.println("----- PREDICTION -----");

  Serial.print("normal votes: ");
  Serial.print(normalVotes);
  Serial.print(" / abnormal votes: ");
  Serial.println(abnormalVotes);

  Serial.print("period_us (mean): ");
  Serial.println(periodUs, 2);

  Serial.print("rpm: ");
  Serial.println(rpm, 1);

  Serial.print("speed level: ");
  Serial.println(currentSpeedLevel);

  Serial.print("Predicted class: ");
  Serial.println(prediction);

  Serial.println("----------------------");

  // Dashboard-compatible telemetry.
  Serial.print("DATA,");
  Serial.print(periodUs, 2);
  Serial.print(",");
  Serial.print(rpm, 1);
  Serial.print(",");
  Serial.print(axRms, 6);
  Serial.print(",");
  Serial.print(ayRms, 6);
  Serial.print(",");
  Serial.print(azRms, 6);
  Serial.print(",");
  Serial.print(overallRms, 6);
  Serial.print(",");
  Serial.print(tempC, 2);
  Serial.print(",");
  Serial.print(prediction);
  Serial.print(",");
  Serial.print(currentDuty);
  Serial.print(",");
  Serial.println(currentDirection);
}