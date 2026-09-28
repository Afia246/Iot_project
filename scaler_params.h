// StandardScaler parameters -- apply as (x - mean) / scale to each raw feature
// before calling the model's predict(), in the SAME feature order as below

// model type: Random Forest
// feature order: ['accel_x', 'accel_y', 'accel_z', 'gyro_x', 'gyro_y', 'gyro_z', 'period_us']
// label encoding: {0: 'normal', 1: 'abnormal'}

const float feature_mean[] = {2.247504f, -0.996410f, 7.808066f, -0.045526f, 0.051869f, -0.016687f, 28832.325983f};
const float feature_scale[] = {0.433271f, 1.156042f, 0.692936f, 0.109115f, 0.063647f, 0.031828f, 45807.970604f};
