#pragma once

// Copy this file to clock_secrets.h. The real file is ignored by Git.
#define CLOCK_WIFI_SSID "your-wifi-name"
#define CLOCK_WIFI_PASSWORD "your-wifi-password"

// Use an HTTPS hostname or IP covered by the server certificate.
#define CLOCK_API_BASE "https://device-api.example.com:8080"

// Generate at least 32 random bytes. It must match WALLPAPER_TOKEN on the server.
#define CLOCK_WALLPAPER_TOKEN "replace-with-a-random-device-token"

// POSIX timezone. This value is China Standard Time (UTC+8).
#define CLOCK_TIMEZONE "CST-8"
