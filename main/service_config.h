#pragma once

/*
 * The real deployment settings live in the ignored clock_secrets.h file.
 * Public builds intentionally target an invalid hostname until configured.
 */
#if __has_include("clock_secrets.h")
#include "clock_secrets.h"
#endif

#ifndef CLOCK_WIFI_SSID
#define CLOCK_WIFI_SSID ""
#endif
#ifndef CLOCK_WIFI_PASSWORD
#define CLOCK_WIFI_PASSWORD ""
#endif
#ifndef CLOCK_API_BASE
#define CLOCK_API_BASE "https://device-api.example.invalid:8080"
#endif
#ifndef CLOCK_WALLPAPER_TOKEN
#define CLOCK_WALLPAPER_TOKEN ""
#endif
#ifndef CLOCK_WALLPAPER_URL
#define CLOCK_WALLPAPER_URL CLOCK_API_BASE "/v1/wallpaper"
#endif
#ifndef CLOCK_WALLPAPER_GENERATE_URL
#define CLOCK_WALLPAPER_GENERATE_URL CLOCK_API_BASE "/v1/wallpaper/generate"
#endif
#ifndef CLOCK_TRANSCRIBE_URL
#define CLOCK_TRANSCRIBE_URL CLOCK_API_BASE "/v1/transcribe"
#endif
#ifndef CLOCK_WEATHER_URL
#define CLOCK_WEATHER_URL CLOCK_API_BASE "/v1/weather"
#endif
#ifndef CLOCK_QUOTA_URL
#define CLOCK_QUOTA_URL CLOCK_API_BASE "/v1/quota"
#endif
#ifndef CLOCK_CODEX_RESET_URL
#define CLOCK_CODEX_RESET_URL CLOCK_API_BASE "/v1/codex-reset"
#endif
#ifndef CLOCK_MUSIC_URL
#define CLOCK_MUSIC_URL CLOCK_API_BASE "/v1/music"
#endif
#ifndef CLOCK_MUSIC_AUDIO_URL
#define CLOCK_MUSIC_AUDIO_URL CLOCK_API_BASE "/v1/music/latest"
#endif
#ifndef CLOCK_TIMEZONE
#define CLOCK_TIMEZONE "CST-8"
#endif
