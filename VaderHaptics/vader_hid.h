#ifndef VADER_HID_H
#define VADER_HID_H

#include <windows.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Initializes the background HID worker thread and connects to Flydigi Vader 5 Pro V2
void Vader_Init(void);

// Updates the desired vibration state for all 4 channels (0-255)
void Vader_SetVibration(uint8_t low_grip, uint8_t high_grip, uint8_t left_trigger, uint8_t right_trigger);

// Returns TRUE if the Flydigi Vendor HID command endpoint is currently open
BOOL Vader_IsConnected(void);

// Enables or disables file logging
void Vader_SetLoggingEnabled(BOOL enabled);

// Stops vibration and cleans up handles cleanly (when dynamically unloaded)
void Vader_Shutdown(void);

// Non-blocking emergency zero-vibration stop for process termination under Loader Lock
void Vader_EmergencyStop(void);

// Log message to vader_haptics.log
void Vader_Log(const char *format, ...);

#ifdef __cplusplus
}
#endif

#endif // VADER_HID_H
