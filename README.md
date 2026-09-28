# IoT-Based Predictive Maintenance Using Vibration Signal Analysis and Fault Classification

A low-cost predictive-maintenance (PdM) system for a small DC-motor test rig. An **ESP32** reads vibration (MPU6050) and rotational speed (A3144 Hall sensor), classifies the motor's condition with a **compressed Random Forest running on the ESP32 itself**, and streams telemetry to a **browser-based live dashboard**.

Course project, Department of Computer Science and Engineering, Ahsanullah University of Science and Technology (AUST), Dhaka, Bangladesh.

---

## Highlights

- **Low-cost hardware:** ESP32 + MPU6050 + A3144 + L298N driving a 12 V 775 DC motor.
- **On-device inference:** a 12-tree, depth-6 Random Forest exported to C (via `micromlgen`) and run directly on the ESP32, with no host PC needed for classification.
- **Leakage-aware evaluation:** windows are grouped by recording-segment ID before 5-fold cross-validation, so near-duplicate windows never sit on both sides of a split.
- **Live dashboard:** RPM, per-axis vibration RMS, temperature, and predicted state in the browser over Web Serial, with motor control from the page.

## Results

Six classical classifiers were compared on a class-balanced dataset of 900 windows (300 per class) with 37 statistical and frequency-domain features, using grouped 5-fold cross-validation.

| Model | Mean grouped 5-fold CV accuracy |
|---|---|
| **Random Forest (12 trees, depth 6), deployed** | **0.84 ± 0.09** |
| Random Forest (300 trees) | 0.79 ± 0.05 |
| Multi-Layer Perceptron | 0.79 ± 0.06 |
| Gradient Boosting | 0.78 ± 0.04 |
| SVM (RBF) | 0.77 ± 0.06 |
| Logistic Regression | 0.76 ± 0.05 |
| k-Nearest Neighbours | 0.75 ± 0.07 |

> **Read these numbers carefully.** The ±0.09 spread shows the estimate is noisy because the dataset has few recording segments per class. The per-class precision/recall reported in the paper is on the *training set* and is only a sanity check, not a generalization estimate. Accuracies quoted in related work (for example 99.03% in Soni & Kori) do not use grouped evaluation, so they are not directly comparable.

## System overview

```
 MPU6050 ──I2C──┐
                ├── ESP32 ── Random Forest (on-device) ── prediction
 A3144 ──GPIO27─┘     │
                      ├── L298N ── 775 DC motor
                      └── USB serial ── Browser dashboard (Web Serial)
```

### Hardware

| Component | Model | Role |
|---|---|---|
| Microcontroller | ESP32 DevKit V1 | Acquisition, motor control, inference |
| Accelerometer/gyro | MPU6050 (GY-521) | 3-axis accel + 3-axis gyro (I2C) |
| Speed sensor | A3144 Hall + magnet | Interrupt-driven pulse timing / RPM |
| Motor driver | L298N H-bridge | Direction and PWM speed control |
| Motor | 12 V 775 DC | Machine under test |
| Load | Fan/propeller + magnet | Load and RPM pulse trigger |
| Fault simulation | Off-centre weights, washers/shims | Imbalance / misalignment |
| Power | 12 V / 5 A adapter | Motor supply (separate from ESP32) |
| Noise suppression | Ceramic + electrolytic caps | Reduce brushed-motor noise |

### Wiring

| ESP32 pin | Module | Function |
|---|---|---|
| GPIO 21 | MPU6050 | SDA (I2C data) |
| GPIO 22 | MPU6050 | SCL (I2C clock) |
| GPIO 27 | A3144 | OUT (RPM pulse input) |
| GPIO 25 | L298N | IN1 (direction) |
| GPIO 26 | L298N | IN2 (direction) |
| GPIO 33 | L298N | ENA (PWM speed) |
| 3V3 | Sensors | Sensor power |
| GND | All modules | Common ground |

The 12 V motor supply is kept separate from the ESP32/sensor rail; **never power the motor from the ESP32**. All grounds must be common.

## Repository structure

Suggested layout for this repo:

```
.
├── firmware/
│   └── motor_classifier_2class/
│       ├── motor_classifier_2class.ino   # ESP32 sketch
│       ├── model.h                       # Random Forest exported to C (12 trees)
│       └── scaler_params.h               # StandardScaler mean/scale
├── dashboard/
│   ├── index.html                        # dashboard page
│   ├── style.css
│   └── script.js                         # Web Serial + Chart.js logic
├── docs/
│   └── IoT_Report_Final.pdf
└── README.md
```

## Getting started

### 1. Flash the firmware

1. Install the [Arduino IDE](https://www.arduino.cc/en/software) with ESP32 board support (Arduino-ESP32 core v2.x or v3.x are both handled in the code).
2. Install the libraries **Adafruit MPU6050** and **Adafruit Unified Sensor** (Library Manager).
3. Open `motor_classifier_2class.ino`. Keep `model.h` and `scaler_params.h` in the same sketch folder.
4. Select **ESP32 Dev Module**, choose your port, and upload.
5. Open the Serial Monitor at **115200 baud**.

> Mount the MPU6050 in the **same position and orientation** used when the training data was collected, and keep the same ranges (±8 g accelerometer, ±500 °/s gyro). Otherwise the model's inputs will not match its training distribution.

### 2. Control the motor

Send single characters over serial (Serial Monitor or the dashboard buttons):

| Command | Action |
|---|---|
| `F` / `f` | Forward |
| `R` / `r` | Reverse |
| `S` / `s` | Stop |
| `0`–`9` | Speed level (PWM duty 0–255) |

### 3. Open the dashboard

The dashboard uses the **Web Serial API**, so use **Google Chrome or Microsoft Edge**, and serve the page from `localhost` (for example `python -m http.server 5173` inside `dashboard/`, then open `http://localhost:5173`). Click **Connect**, pick the ESP32's port, and close any other program (such as the Arduino Serial Monitor) that is holding the port.

The firmware emits one telemetry line roughly every 2 seconds:

```
DATA,period_us,rpm,ax_rms,ay_rms,az_rms,overall_rms,temp_c,prediction,duty,direction
```

## How it works

1. **Sampling.** The MPU6050 is sampled at about 100 Hz (10 ms interval). The Hall sensor is read through an interrupt on GPIO27 (one pulse per revolution assumed).
2. **RPM.** `RPM = 60,000,000 / period_us`, from the Hall pulse period.
3. **Features and model.** See the note on model versions below.
4. **Export.** The trained Random Forest is exported to C with `micromlgen` and included as `model.h`; inputs are standardized on-device with `scaler_params.h`.
5. **Dashboard.** `script.js` parses `DATA,...` lines and updates the RPM, vibration, temperature and condition panels, and can send motor commands back to the ESP32.

### Two model versions

The report and the firmware in this repo describe two different stages of the project. Please keep them straight:

| | Paper (offline pipeline) | Firmware in this repo (`motor_classifier_2class`) |
|---|---|---|
| Classes | 3 target classes (Normal, Imbalance, Misalignment); the current dataset covers 3 labelled classes from forward-normal, reverse-normal and forward-imbalance runs | 2 classes: `normal`, `abnormal` |
| Input | 37 per-window statistical / frequency-domain features (900 windows) | 7 raw features per sample: `accel_x/y/z`, `gyro_x/y/z`, `period_us` |
| Prediction | Per-window | Every sample is classified, then a **majority vote over a 200-sample (~2 s) window** gives the reported state |
| Model | 12-tree, depth-6 Random Forest | 12-tree Random Forest |

Also note that the report configures the MPU6050 filter bandwidth at 44 Hz, while the current sketch sets `MPU6050_BAND_21_HZ`. Keep this consistent with whatever was used when the training data was recorded.

If the motor is stopped (`duty == 0`), the firmware reports `normal` rather than classifying.

## Known limitations

- **Small number of recording segments**, hence the large ±0.09 spread in cross-validation accuracy.
- **Misalignment** data has not yet been collected; the rig can reproduce it mechanically but it is not in the current dataset.
- **No confusion matrix yet** for the final compressed model, and a full head-to-head comparison of all six classifiers under identical grouped-CV conditions is still to be completed.
- **FFT-based features are not yet in the live firmware**; frequency-domain analysis is currently part of the offline pipeline.
- **Dashboard stability:** some panels do not update reliably during long continuous streaming.
- **No security:** communication is over local USB serial with no authentication or encryption. This is fine for a bench prototype, but HTTPS/TLS and access-controlled credentials are needed before any cloud publishing.
- The firmware header mentions a `DEMO_SPEED_OVERRIDE` option that forces speed levels 3 and 4 to report `normal` and `abnormal`. In the current sketch that block is empty, so no override is applied. Set `DEMO_SPEED_OVERRIDE` to `0` (or remove it) to avoid confusion, and do not present forced outputs as model predictions.

## Future work

1. Full formal comparison of all six classifiers under identical grouped-CV conditions.
2. Collect labelled Misalignment data and extend to a 3-class on-device model.
3. Generate and analyze a confusion matrix.
4. Stabilize the live dashboard.
5. Complete the FFT-based feature pipeline and validate it on-device.
6. Move from flat CSV storage to a time-series database and add cloud monitoring (ThingSpeak or Firebase) with authentication and encryption.
7. Close the loop: automatically slow or stop the motor when a fault is predicted.

## Approximate cost

Roughly 3,500 to 5,000 BDT for the rig, excluding the ESP32 (which the team already had). The main items are the 775 DC motor (~1,500), the 12 V adapter (350 to 500), the MPU6050 (~380) and the L298N (~200). See the report for the full breakdown.

## Team

| Name | Student ID | Focus |
|---|---|---|
| Tasfia Jannat | 20220104026 | ML classifiers, model compression, grouped CV |
| Sanaf Salehin | 20220104063 | Firmware, sensor interfacing, dashboard |
| Afia Adilah | 20220104069 | Hall sensor / RPM setup, wiring, hardware assembly, signal-processing research |
| Umme Ayesha Rahman | 20200204050 | Dataset cleaning, windowing, feature engineering |
