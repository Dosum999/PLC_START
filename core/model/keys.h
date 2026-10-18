#pragma once

// Property keys, following the Kepware Configuration API naming so projects and
// API payloads stay familiar (and importable).
// TODO(verify): names reconstructed from Kepware docs/examples; check each
// against a live /config/v1/doc response before claiming import compatibility.
namespace logict::model::keys {

inline constexpr const char* kName = "common.ALLTYPES_NAME";
inline constexpr const char* kDescription = "common.ALLTYPES_DESCRIPTION";

inline constexpr const char* kProjectTitle = "servermain.PROJECT_TITLE";

// Channel
inline constexpr const char* kChannelDriver = "servermain.MULTIPLE_TYPES_DEVICE_DRIVER";
inline constexpr const char* kChannelWriteMethod = "servermain.CHANNEL_WRITE_OPTIMIZATIONS_METHOD";
inline constexpr const char* kChannelDutyCycle = "servermain.CHANNEL_WRITE_OPTIMIZATIONS_DUTY_CYCLE";
inline constexpr const char* kChannelFloatHandling = "servermain.CHANNEL_NON_NORMALIZED_FLOATING_POINT_HANDLING";
inline constexpr const char* kChannelDiagnostics = "servermain.CHANNEL_DIAGNOSTICS_CAPTURE";

// Device
inline constexpr const char* kDeviceModel = "servermain.DEVICE_MODEL";
inline constexpr const char* kDeviceId = "servermain.DEVICE_ID_STRING";
inline constexpr const char* kDeviceEnabled = "servermain.DEVICE_DATA_COLLECTION";
inline constexpr const char* kDeviceSimulated = "servermain.DEVICE_SIMULATED";
inline constexpr const char* kDeviceScanMode = "servermain.DEVICE_SCAN_MODE";
inline constexpr const char* kDeviceScanRate = "servermain.DEVICE_SCAN_MODE_RATE_MS";
inline constexpr const char* kDeviceInitialFromCache = "servermain.DEVICE_SCAN_MODE_PROVIDE_INITIAL_UPDATES_FROM_CACHE";
inline constexpr const char* kDeviceConnectTimeout = "servermain.DEVICE_CONNECTION_TIMEOUT_SECONDS";
inline constexpr const char* kDeviceRequestTimeout = "servermain.DEVICE_REQUEST_TIMEOUT_MILLISECONDS";
inline constexpr const char* kDeviceAttempts = "servermain.DEVICE_RETRY_ATTEMPTS";
inline constexpr const char* kDeviceInterRequestDelay = "servermain.DEVICE_INTER_REQUEST_DELAY_MILLISECONDS";
inline constexpr const char* kDeviceDemotionEnable = "servermain.DEVICE_AUTO_DEMOTION_ENABLE_ON_COMMUNICATIONS_FAILURES";
inline constexpr const char* kDeviceDemoteAfter = "servermain.DEVICE_AUTO_DEMOTION_DEMOTE_AFTER_SUCCESSIVE_TIMEOUTS";
inline constexpr const char* kDeviceDemotionPeriod = "servermain.DEVICE_AUTO_DEMOTION_PERIOD_MS";
inline constexpr const char* kDeviceDemotionDiscardWrites = "servermain.DEVICE_AUTO_DEMOTION_DISCARD_WRITES";

// Tag
inline constexpr const char* kTagAddress = "servermain.TAG_ADDRESS";
inline constexpr const char* kTagDataType = "servermain.TAG_DATA_TYPE";
inline constexpr const char* kTagAccess = "servermain.TAG_READ_WRITE_ACCESS";
inline constexpr const char* kTagScanRate = "servermain.TAG_SCAN_RATE_MILLISECONDS";
inline constexpr const char* kTagScalingType = "servermain.TAG_SCALING_TYPE";
inline constexpr const char* kTagScalingRawLow = "servermain.TAG_SCALING_RAW_LOW";
inline constexpr const char* kTagScalingRawHigh = "servermain.TAG_SCALING_RAW_HIGH";
inline constexpr const char* kTagScalingScaledType = "servermain.TAG_SCALING_SCALED_DATA_TYPE";
inline constexpr const char* kTagScalingScaledLow = "servermain.TAG_SCALING_SCALED_LOW";
inline constexpr const char* kTagScalingScaledHigh = "servermain.TAG_SCALING_SCALED_HIGH";
inline constexpr const char* kTagScalingClampLow = "servermain.TAG_SCALING_CLAMP_LOW";
inline constexpr const char* kTagScalingClampHigh = "servermain.TAG_SCALING_CLAMP_HIGH";
inline constexpr const char* kTagScalingNegate = "servermain.TAG_SCALING_NEGATE_VALUE";
inline constexpr const char* kTagScalingUnits = "servermain.TAG_SCALING_UNITS";

}  // namespace logict::model::keys
