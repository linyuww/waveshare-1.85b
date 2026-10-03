// SPDX-License-Identifier: MIT
// Copyright (c) 2026 imliubo
// StopWatch port changes copyright (c) 2026 Codex Micro for StopWatch contributors
// Waveshare ESP32-S3-Touch-LCD-1.85B port copyright (c) 2026 Codex Micro port
//
// The pixel-art dashboard for the 360 x 360 ST77916 panel.
//
// This is a deliberate rewrite of the C152 orbital instrument, not a rescale
// of it. The C152 put six status keys on a hexagon around a quota dial; the
// design for this board puts a clock and date at the top, an information panel
// in the middle, and the six keys in a 3 x 2 grid along the bottom, all over a
// full-screen scene that changes with the time of day.
//
// Every coordinate below was measured off the mockup rather than eyeballed.
// The method is recorded in the plan because it matters: the day and night
// mockups share an identical UI skeleton and differ only in the background
// scene, so |day - night| isolates the chrome, and frames were then located by
// walking single rows and columns looking for a saturated accent hue. Bounding
// boxes were not usable -- the background art bleeds into them.
//
// scripts/assets/pixel_preview.py is the reference implementation of this same layout.
// The two are meant to agree pixel for pixel, so anything changed here should
// be changed there too, and vice versa.

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>

#include "Backgrounds.h"
#include "BatteryIcons.h"
#include "battery_logic.h"
#include "IconTextures.h"
#include "PixelFonts.h"
#include "gfx.h"

namespace dashboard {

// ------------------------------------------------------------------ layout --

constexpr int kWidth = 360;
constexpr int kHeight = 360;
constexpr int kCenterX = 180;

// The outer segmented ring is the weekly allowance. The active arc begins at
// 12 o'clock and advances clockwise, matching the physical gauge in the
// supplied design. Sixty segments keep the progress legible without turning
// the pixel-art edge into a smooth smartwatch arc.
constexpr int kQuotaRingOuterRadius = 178;
constexpr int kQuotaRingInnerRadius = 171;
constexpr int kQuotaRingSegments = 60;
constexpr float kQuotaRingSegmentDegrees = 4.2f;

// Clock and date. The clock is 4x the 8 px em, which is a bit-exact upscale
// and lands within a pixel of the mockup's 31 px cap height; the date stays at
// 1x. Both sit directly on the scene, so both carry a halo -- see
// drawTextOutlined.
constexpr int kClockY = 56;
constexpr int kClockScale = 4;
constexpr int kClockOutline = 2;
constexpr int kDateY = 82;
constexpr int kDateOutline = 1;

// One name for "the font's own size". Nearly all the copy is set at 1x; only
// the clock and the quota figure are blown up.
constexpr int kSmallScale = 1;

// The information panel.
constexpr int kPanelX = 121;
constexpr int kPanelY = 95;
constexpr int kPanelW = 118;
constexpr int kPanelH = 111;
constexpr int kPanelCut = 4;
constexpr int kPanelCenterY = kPanelY + kPanelH / 2;

constexpr int kBatteryY = 110;
constexpr int kBatteryGap = 8;

constexpr int kDividerY = 122;
constexpr int kDividerX0 = 124;
constexpr int kDividerX1 = 235;

constexpr int kStatusLineY = 133;
constexpr int kQuotaY = 158;
constexpr int kQuotaScale = 4;
// The pairing prompt's headline. It is deliberately smaller than the quota
// figure: "PAIR" at 4x measures wider than the panel's 114 px inner width,
// while 3x leaves the same visual weight without touching the frame.
constexpr int kPairingScale = 3;
constexpr int kSendY = 182;
constexpr int kCountdownY = 194;

// The 3 x 2 key grid. Column pitch is 82 px and row pitch 41 px, both measured.
constexpr int kButtonW = 77;
constexpr int kButtonH = 35;
constexpr int kButtonCut = 3;
constexpr std::array<int, 3> kButtonColumns = {{98, 180, 262}};
constexpr std::array<int, 2> kButtonRows = {{240, 281}};
constexpr int kButtonIconDx = 19;  // icon centre, from the button's left edge
constexpr int kButtonTextDx = 47;  // text column centre, from the same edge
constexpr int kButtonTagDy = -2;   // tag (A1..A6) sits just below the top frame
constexpr int kButtonLabelDy = 10; // status label below the tag, kept clear

constexpr int kMottoTopY = 316;
constexpr int kMottoBottomY = 326;

// Every frame is drawn with the same 2 px border and 2 px step, on both the
// panel and the buttons. Measured: the mockup's border reads 1.4-2.0 px and
// the render's 2.0 px, so 2 is right and the two agree.
constexpr int kFrameThickness = 2;
constexpr int kStep = 2;

// ---------------------------------------------------------------- palette ---
// Sampled from the mockups with a peak-pixel probe, then quantised to RGB565
// because that is all the panel can show. These are the post-quantisation
// values, so no further rounding happens at draw time.

constexpr std::uint16_t kVoid = 0x0042;          // #000810 backdrop
constexpr std::uint16_t kPanel = 0x0043;         // #000B1A panel and key fill
constexpr std::uint16_t kPanelPressed = 0x08A4;  // lifted fill, touch feedback
constexpr std::uint16_t kFrame = 0x571F;         // #56E0FC panel frame
constexpr std::uint16_t kDivider = 0x461F;       // #46C2FC rule
constexpr std::uint16_t kText = 0xFFFF;          // #FFFFFF body copy
constexpr std::uint16_t kMuted = 0x6CB7;         // #6C94B9 SEND and the motto
constexpr std::uint16_t kOutline = 0x0885;       // #081028 halo
constexpr std::uint16_t kTrack = 0x2187;         // unfilled bar
constexpr std::uint16_t kAccent = 0x17F5;        // #10FFA8 CODEX LIVE
constexpr std::uint16_t kWarning = 0xFDE7;       // #FFBD3D
constexpr std::uint16_t kDanger = 0xFA2A;        // #FF4754

// Kept for the boot and error screens in main.cpp.
constexpr std::uint16_t kBackground = kVoid;

// Status hues. Each key is drawn in its own status colour, and the icon
// texture ships already carrying the same colour -- see iconFor().
constexpr std::uint16_t kStatusThink = 0x36FF;  // #32DCFE
constexpr std::uint16_t kStatusDone = 0x17F5;   // #10FFA8
constexpr std::uint16_t kStatusInput = 0xFDE7;  // #FFBD3D
constexpr std::uint16_t kStatusEmpty = 0xCEBC;  // #C8D6E6
constexpr std::uint16_t kStatusError = 0xFA2A;  // #FF4754
constexpr std::uint16_t kStatusIdle = 0x8C5F;   // #8C8AFF

// ------------------------------------------------------------ status model --

enum class AgentStatus : std::uint8_t {
  Unassigned,
  Idle,
  Thinking,
  Complete,
  RequiresInput,
  Error,
  Active,
};

enum class LinkHealth : std::uint8_t {
  Offline,
  BleOnly,
  CodexLive,
};

enum class PowerOverlay : std::uint8_t {
  None,
  HoldToPowerOff,
  PoweringOff,
};

struct ThreadVisual {
  std::uint32_t color = 0;
  float brightness = 0.0f;
  bool focused = false;
};

// A premultiplied icon texture: colour words and a coverage plane. The two
// arrays are generated side by side by scripts/assets/make_icon_textures.py, so they
// are always the same length and always belong together.
struct IconTexture {
  const std::uint16_t* words;
  const std::uint8_t* alphas;
};

struct State {
  std::array<ThreadVisual, 6> threads = {};
  LinkHealth linkHealth = LinkHealth::Offline;
  std::int8_t batteryPercent = -1;
  bool externalPower = false;
  bool quotaAvailable = false;
  bool quotaStale = false;
  float fiveHourRemainingPercent = 0.0f;
  std::uint32_t fiveHourResetInSeconds = 0;
  float weeklyRemainingPercent = 0.0f;
  bool micPressed = false;
  bool voicePressed = false;
  bool sendPressed = false;
  int activeTouchAgent = -1;
  int swipeDirection = -1;
  int completedAgent = -1;
  PowerOverlay powerOverlay = PowerOverlay::None;
  float powerHoldProgress = 0.0f;

  // Clock and theme, from wifi_time. `timeValid` is false until the first NTP
  // sync, and while it is false `night` must be left at whatever it was last
  // known to be -- the hour is meaningless, and treating an unknown hour as 0
  // would force the night theme on every cold boot.
  bool timeValid = false;
  char clock[8] = "--:--";
  char date[16] = "-- --- ---";
  bool night = false;

  // Set while the SoftAP setup portal is up. Without this the device would sit
  // there showing "--:--" with no hint of how to fix it: the access point name
  // and the address to open are the only two things the user needs, and they
  // exist nowhere on screen otherwise.
  bool setupPortal = false;
  char setupSsid[33] = {};

  // Set while the user has put the device into BLE pairing mode. Like the
  // setup portal, this is the one screen where the panel must carry
  // instructions rather than telemetry: the whole point of the gesture is that
  // the user is now standing at the host looking for a device to add, and the
  // name to look for is not written anywhere else.
  bool pairing = false;
  std::uint32_t pairingSecondsLeft = 0;
};

inline AgentStatus classify(const ThreadVisual& light) {
  if (light.brightness <= 0.01f) return AgentStatus::Unassigned;

  const int red = (light.color >> 16) & 0xFF;
  const int green = (light.color >> 8) & 0xFF;
  const int blue = light.color & 0xFF;
  const int maximum = std::max(red, std::max(green, blue));
  const int minimum = std::min(red, std::min(green, blue));

  if (maximum - minimum < 48 && maximum > 150) return AgentStatus::Idle;
  if (green > red * 1.12f && green > blue * 1.15f) return AgentStatus::Complete;
  if (blue > red * 1.12f && blue > green * 1.05f) return AgentStatus::Thinking;
  if (red > 150 && green > 70 && green > blue * 1.45f) {
    return AgentStatus::RequiresInput;
  }
  if (red > green * 1.20f && red > blue * 1.20f) return AgentStatus::Error;
  return AgentStatus::Active;
}

// The design shows six distinct states, one per key, which is a showcase of
// the palette rather than a runtime assignment: at runtime each key shows its
// own agent's live state, and the "A1".."A6" tag is what identifies the slot.
//
// Active is the seventh value, the fallback for a running agent whose hue the
// classifier does not recognise. It is presented exactly as Thinking -- same
// brain icon, same blue, same word. Both mean "this agent is working", and the
// alternative, a seventh texture and a wider label, does not fit the 52 px the
// button leaves to the right of its text column.
inline IconTexture iconFor(AgentStatus status) {
  switch (status) {
    case AgentStatus::Thinking:
    case AgentStatus::Active:
      return {icon_data::kIconThink_rgb, icon_data::kIconThink_a};
    case AgentStatus::Complete:
      return {icon_data::kIconDone_rgb, icon_data::kIconDone_a};
    case AgentStatus::RequiresInput:
      return {icon_data::kIconInput_rgb, icon_data::kIconInput_a};
    case AgentStatus::Error:
      return {icon_data::kIconError_rgb, icon_data::kIconError_a};
    case AgentStatus::Idle:
      return {icon_data::kIconIdle_rgb, icon_data::kIconIdle_a};
    case AgentStatus::Unassigned:
      return {icon_data::kIconEmpty_rgb, icon_data::kIconEmpty_a};
  }
  return {icon_data::kIconEmpty_rgb, icon_data::kIconEmpty_a};
}

inline std::uint16_t statusColor(AgentStatus status) {
  switch (status) {
    case AgentStatus::Thinking:
    case AgentStatus::Active:
      return kStatusThink;
    case AgentStatus::Complete:
      return kStatusDone;
    case AgentStatus::RequiresInput:
      return kStatusInput;
    case AgentStatus::Error:
      return kStatusError;
    case AgentStatus::Idle:
      return kStatusIdle;
    case AgentStatus::Unassigned:
      return kStatusEmpty;
  }
  return kStatusEmpty;
}

inline const char* statusLabel(AgentStatus status) {
  switch (status) {
    case AgentStatus::Thinking:
    case AgentStatus::Active:
      return "THINK";
    case AgentStatus::Complete:
      return "DONE";
    case AgentStatus::RequiresInput:
      return "INPUT";
    case AgentStatus::Error:
      return "ERROR";
    case AgentStatus::Idle:
      return "IDLE";
    case AgentStatus::Unassigned:
      return "EMPTY";
  }
  return "EMPTY";
}

inline const char* linkHealthLabel(LinkHealth health) {
  switch (health) {
    case LinkHealth::CodexLive: return "CODEX LIVE";
    case LinkHealth::BleOnly:   return "BLE ONLY";
    case LinkHealth::Offline:   return "OFFLINE";
  }
  return "OFFLINE";
}

inline std::uint16_t linkHealthColor(LinkHealth health) {
  switch (health) {
    case LinkHealth::CodexLive: return kAccent;
    case LinkHealth::BleOnly:   return kStatusThink;
    case LinkHealth::Offline:   return kDanger;
  }
  return kDanger;
}

// The countdown the design shows as "4D 17H". Short forms only: the panel is
// 118 px wide and the string is drawn at 1x, so five characters is the budget.
inline void formatReset(std::uint32_t seconds, char* output, std::size_t size) {
  if (seconds == 0) {
    std::snprintf(output, size, "--");
    return;
  }
  const std::uint32_t days = seconds / 86400;
  const std::uint32_t hours = (seconds % 86400) / 3600;
  const std::uint32_t minutes = (seconds % 3600) / 60;
  if (days > 0) {
    std::snprintf(output, size, "%luD %02luH",
                  static_cast<unsigned long>(days),
                  static_cast<unsigned long>(hours));
  } else if (hours > 0) {
    std::snprintf(output, size, "%luH %02luM",
                  static_cast<unsigned long>(hours),
                  static_cast<unsigned long>(minutes));
  } else {
    std::snprintf(output, size, "%luM",
                  static_cast<unsigned long>(minutes));
  }
}

inline const gfx::Font& pixelFont() {
  static gfx::Font font;
  static bool loaded = false;
  if (!loaded) {
    font.load(font_data::kPixel8Vlw);
    loaded = true;
  }
  return font;
}

// ------------------------------------------------------------------ drawing --

inline void drawClockAndDate(gfx::Canvas& canvas, const State& state) {
  const gfx::Font& font = pixelFont();
  // The halo is not optional. In the day theme the clock sits on a bright sky,
  // and white on pale blue is unreadable without it; the mockups draw the same
  // halo. The clock gets a thicker one because it is four times the size and
  // so has four times the perimeter to separate.
  canvas.drawTextOutlined(font, state.clock, kCenterX, kClockY,
                          gfx::Datum::MiddleCenter, kText, kOutline,
                          kClockOutline, kClockScale);
  canvas.drawTextOutlined(font, state.date, kCenterX, kDateY,
                          gfx::Datum::MiddleCenter, kText, kOutline,
                          kDateOutline, kSmallScale);
}

inline void drawWeeklyQuotaRing(gfx::Canvas& canvas, const State& state) {
  if (state.setupPortal || state.pairing) return;

  const float remaining =
      std::max(0.0f, std::min(100.0f, state.weeklyRemainingPercent));
  const int activeSegments = state.quotaAvailable
      ? static_cast<int>(std::ceil(remaining * kQuotaRingSegments / 100.0f))
      : 0;
  const std::uint16_t activeColor = state.quotaStale
      ? kMuted
      : (remaining <= 20.0f ? kWarning : kFrame);

  canvas.fillSegmentedRing(kCenterX, kHeight / 2, kQuotaRingOuterRadius,
                           kQuotaRingInnerRadius, kQuotaRingSegments,
                           kQuotaRingSegmentDegrees, activeSegments,
                           activeColor, kTrack);
}

inline void drawBattery(gfx::Canvas& canvas, std::int8_t percent,
                       bool externalPower) {
  const gfx::Font& font = pixelFont();
  char label[16];
  const int displayPercent = battery_logic::clampPercent(percent);
  if (displayPercent < 0) {
    std::snprintf(label, sizeof(label), "--%%");
  } else {
    std::snprintf(label, sizeof(label), "%d%%", displayPercent);
  }

  // Pick one of the artist's 5x2 battery textures. The inclusive bins are
  // 0..20, 21..40, 41..60, 61..80 and 81..100; unknown shares level 1.
  // The whole texture (body + nub) replaces the procedural rails,
  // so the icon's exact proportions follow the artist rather than the
  // kBatteryBodyW/NubW/H constants that used to drive the geometry.
  const int level = battery_logic::iconLevel(displayPercent);
  // Declared uint16_t, not a byte stream: the arrays are constexpr
  // std::uint16_t in BatteryIcons.h, so this needs no cast and stays
  // naturally aligned. (A uint8_t* reinterpret_cast to uint16_t* would be
  // 1-byte aligned, and Xtensa faults on a misaligned 16-bit load.)
  const std::uint16_t* rgb;
  const std::uint8_t* alpha;
  if (externalPower) {
    switch (level) {
      case 1: rgb = battery_data::kBatteryCharging1_rgb;
              alpha = battery_data::kBatteryCharging1_a; break;
      case 2: rgb = battery_data::kBatteryCharging2_rgb;
              alpha = battery_data::kBatteryCharging2_a; break;
      case 3: rgb = battery_data::kBatteryCharging3_rgb;
              alpha = battery_data::kBatteryCharging3_a; break;
      case 4: rgb = battery_data::kBatteryCharging4_rgb;
              alpha = battery_data::kBatteryCharging4_a; break;
      default: rgb = battery_data::kBatteryCharging5_rgb;
              alpha = battery_data::kBatteryCharging5_a; break;
    }
  } else {
    switch (level) {
      case 1: rgb = battery_data::kBatteryIdle1_rgb;
              alpha = battery_data::kBatteryIdle1_a; break;
      case 2: rgb = battery_data::kBatteryIdle2_rgb;
              alpha = battery_data::kBatteryIdle2_a; break;
      case 3: rgb = battery_data::kBatteryIdle3_rgb;
              alpha = battery_data::kBatteryIdle3_a; break;
      case 4: rgb = battery_data::kBatteryIdle4_rgb;
              alpha = battery_data::kBatteryIdle4_a; break;
      default: rgb = battery_data::kBatteryIdle5_rgb;
              alpha = battery_data::kBatteryIdle5_a; break;
    }
  }

  // Layout: icon centred with the percentage text alongside. The totalIcon
  // uses the texture's full width (body + nub), so the right edge still
  // sits the same distance from the percentage text as before.
  constexpr int totalIcon = battery_data::kBatteryIconW;
  constexpr int iconH = battery_data::kBatteryIconH;
  const int textWidth = font.textWidth(label) * kSmallScale;
  const int left = kCenterX - (totalIcon + kBatteryGap + textWidth) / 2;
  const int top = kBatteryY - iconH / 2;

  canvas.drawTexture(left, top, rgb, alpha, totalIcon, iconH);

  canvas.drawTextInteger(font, label, left + totalIcon + kBatteryGap,
                         kBatteryY, gfx::Datum::MiddleLeft, kText,
                         kSmallScale);
}

inline void drawPanel(gfx::Canvas& canvas, const State& state) {
  const gfx::Font& font = pixelFont();

  canvas.fillPixelFrame(kPanelX, kPanelY, kPanelW, kPanelH, kFrame, kPanel,
                        kPanelCut, kFrameThickness, kStep);

  if (state.setupPortal) {
    // The panel becomes the setup prompt. Link health and quota are both
    // meaningless before the device is on a network, so replacing them costs
    // nothing and the user gets the two facts they actually need: which access
    // point to join, and where to point a browser. The four rows are the same
    // four the normal readout uses, so the layout does not move.
    canvas.drawTextInteger(font, "WIFI SETUP", kCenterX, kStatusLineY,
                           gfx::Datum::MiddleCenter, kWarning, kSmallScale);
    canvas.drawTextInteger(font, state.setupSsid, kCenterX, kQuotaY,
                           gfx::Datum::MiddleCenter, kAccent, kSmallScale);
    canvas.drawTextInteger(font, "OPEN BROWSER", kCenterX, kSendY,
                           gfx::Datum::MiddleCenter, kMuted, kSmallScale);
    canvas.drawTextInteger(font, "192.168.4.1", kCenterX, kCountdownY,
                           gfx::Datum::MiddleCenter, kText, kSmallScale);
    return;
  }

  drawBattery(canvas, state.batteryPercent, state.externalPower);
  canvas.fillRect(kDividerX0, kDividerY, kDividerX1 - kDividerX0, 1, kDivider);

  if (state.pairing) {
    // The panel becomes the pairing prompt. Link health and quota are both
    // meaningless here -- the link was just torn down on purpose -- so the
    // four rows carry the only three facts the user needs: that the board is
    // in pairing mode, the name to look for in the host's Bluetooth settings,
    // and how long the window lasts. The battery row above is kept, because
    // running out of charge halfway through a re-pair is a real failure mode.
    canvas.drawTextInteger(font, "BLUETOOTH", kCenterX, kStatusLineY,
                           gfx::Datum::MiddleCenter, kAccent, kSmallScale);
    canvas.drawTextInteger(font, "PAIR", kCenterX, kQuotaY,
                           gfx::Datum::MiddleCenter, kAccent, kPairingScale);
    canvas.drawTextInteger(font, "CODEX MICRO", kCenterX, kSendY,
                           gfx::Datum::MiddleCenter, kText, kSmallScale);

    // Sized for the whole unsigned long range rather than for the two-minute
    // window the value actually occupies: this build turns
    // -Wformat-truncation into an error, so the buffer has to rule out the
    // worst case the format string can produce, not the worst case that can
    // occur. (20 digits + "S LEFT" + NUL.)
    char window[32];
    if (state.pairingSecondsLeft == 0) {
      std::snprintf(window, sizeof(window), "WAITING");
    } else {
      std::snprintf(window, sizeof(window), "%luS LEFT",
                    static_cast<unsigned long>(state.pairingSecondsLeft));
    }
    canvas.drawTextInteger(font, window, kCenterX, kCountdownY,
                           gfx::Datum::MiddleCenter, kMuted, kSmallScale);
    return;
  }

  canvas.drawTextInteger(font, linkHealthLabel(state.linkHealth), kCenterX,
                         kStatusLineY, gfx::Datum::MiddleCenter,
                         linkHealthColor(state.linkHealth), kSmallScale);

  char quota[8];
  if (state.quotaAvailable) {
    std::snprintf(quota, sizeof(quota), "%.0f%%",
                  std::max(0.0f,
                           std::min(100.0f, state.fiveHourRemainingPercent)));
  } else {
    std::snprintf(quota, sizeof(quota), "--");
  }
  canvas.drawTextInteger(font, quota, kCenterX, kQuotaY,
                         gfx::Datum::MiddleCenter,
                         state.quotaStale ? kMuted : kText, kQuotaScale);

  // The design labels this line SEND, and the panel is the Send target -- see
  // sendAtPoint(). The label is honest, not a leftover from the old dial.
  canvas.drawTextInteger(font, "SEND", kCenterX, kSendY,
                         gfx::Datum::MiddleCenter, kMuted, kSmallScale);

  char countdown[16];
  std::uint16_t countdownColor = kText;
  if (state.quotaStale) {
    std::snprintf(countdown, sizeof(countdown), "STALE");
    countdownColor = kWarning;
  } else if (!state.quotaAvailable) {
    std::snprintf(countdown, sizeof(countdown), "NO QUOTA");
    countdownColor = kMuted;
  } else {
    char reset[12];
    formatReset(state.fiveHourResetInSeconds, reset, sizeof(reset));
    std::snprintf(countdown, sizeof(countdown), "5H %s", reset);
  }
  canvas.drawTextInteger(font, countdown, kCenterX, kCountdownY,
                         gfx::Datum::MiddleCenter, countdownColor, kSmallScale);
}

inline void drawAgentKeys(gfx::Canvas& canvas, const State& state) {
  const gfx::Font& font = pixelFont();
  for (int i = 0; i < 6; ++i) {
    const int cx = kButtonColumns[i % 3];
    const int cy = kButtonRows[i / 3];
    const AgentStatus status = classify(state.threads[i]);
    const std::uint16_t color = statusColor(status);
    const int x = cx - kButtonW / 2;
    const int y = cy - kButtonH / 2;

    // A pressed key lifts its fill and dims its icon. The preview has no
    // pressed state to mirror -- it renders a still frame -- so this is an
    // addition, kept deliberately subtle so it cannot be mistaken for a
    // status change.
    const bool pressed = state.activeTouchAgent == i;
    canvas.fillPixelFrame(x, y, kButtonW, kButtonH, color,
                          pressed ? kPanelPressed : kPanel, kButtonCut,
                          kFrameThickness, kStep);

    const IconTexture icon = iconFor(status);
    canvas.drawTexture(x + kButtonIconDx - icon_data::kIconTextureSize / 2,
                       cy - icon_data::kIconTextureSize / 2, icon.words,
                       icon.alphas, icon_data::kIconTextureSize,
                       icon_data::kIconTextureSize, pressed ? 0.6f : 1.0f);

    char tag[4];
    std::snprintf(tag, sizeof(tag), "A%d", i + 1);
    canvas.drawTextInteger(font, tag, x + kButtonTextDx, cy + kButtonTagDy,
                           gfx::Datum::MiddleCenter, color, 2);
    canvas.drawTextInteger(font, statusLabel(status), x + kButtonTextDx,
                           cy + kButtonLabelDy, gfx::Datum::MiddleCenter,
                           color, kSmallScale);
  }
}

inline void drawMotto(gfx::Canvas& canvas) {
  const gfx::Font& font = pixelFont();
  // The motto sits on open scene rather than on a plate, so it needs the same
  // halo as the clock: over the day theme's grass a bare dim grey is
  // unreadable.
  canvas.drawTextOutlined(font, "GOOD IDEAS", kCenterX, kMottoTopY,
                          gfx::Datum::MiddleCenter, kMuted, kOutline,
                          kDateOutline, kSmallScale);
  canvas.drawTextOutlined(font, "TAKE TIME", kCenterX, kMottoBottomY,
                          gfx::Datum::MiddleCenter, kMuted, kOutline,
                          kDateOutline, kSmallScale);
}

inline void drawTransient(gfx::Canvas& canvas, const State& state) {
  // The pairing prompt owns the panel while it is up, and the gesture that
  // enters it deliberately releases the mic first, so a leftover LISTENING
  // plate would both hide the instructions and be stale.
  if (state.pairing) return;

  const char* message = nullptr;
  std::uint16_t color = kAccent;
  if (state.sendPressed) {
    message = "SEND";
    color = kText;
  } else if (state.micPressed) {
    message = "LISTENING";
    color = kAccent;
  } else if (state.voicePressed) {
    message = "VOICE CHAT";
  } else if (state.swipeDirection >= 0 && state.swipeDirection < 4) {
    static constexpr const char* kSwipeMessages[] = {
        "STICK UP", "STICK RIGHT", "STICK DOWN", "STICK LEFT"};
    message = kSwipeMessages[state.swipeDirection];
    color = kAccent;
  } else if (state.completedAgent >= 0 && state.completedAgent < 6) {
    // A table rather than snprintf("A%d DONE"): the agent index is already
    // bounded to the six keys, so the table states that invariant outright and
    // needs no buffer for the compiler to reason about. Formatting an int into
    // a fixed buffer here is what -Wformat-truncation objects to, since it has
    // to assume the value could be a full-width negative.
    static constexpr const char* kCompletedMessages[] = {
        "A1 DONE", "A2 DONE", "A3 DONE", "A4 DONE", "A5 DONE", "A6 DONE"};
    message = kCompletedMessages[state.completedAgent];
  }
  if (message == nullptr) return;

  // A plate dropped over the panel, drawn in the same stepped-frame language
  // as the rest of the chrome. Sized to the panel's 118 px width so it cannot
  // reach the key grid.
  constexpr int kPlateW = 112;
  constexpr int kPlateH = 40;
  const int x = kCenterX - kPlateW / 2;
  const int y = kPanelCenterY - kPlateH / 2;
  canvas.fillPixelFrame(x, y, kPlateW, kPlateH, color, kPanel, 2,
                        kFrameThickness, kStep);
  canvas.drawTextInteger(pixelFont(), message, kCenterX, kPanelCenterY,
                         gfx::Datum::MiddleCenter, color, kSmallScale);
}

inline void drawPowerOverlay(gfx::Canvas& canvas, const State& state) {
  if (state.powerOverlay == PowerOverlay::None) return;

  const bool confirming = state.powerOverlay == PowerOverlay::HoldToPowerOff;
  const char* heading = confirming ? "KEEP HOLDING" : "POWERING OFF";
  const char* detail = confirming ? "SCREEN SLEEP" : "BOOT OR TOUCH WAKE";
  const std::uint16_t color = confirming ? kWarning : kDanger;

  // The panel is the Send control and holding it is the power gesture, so the
  // prompt belongs on the panel -- there is no ring to put it on any more.
  canvas.fillPixelFrame(kPanelX, kPanelY, kPanelW, kPanelH, color, kPanel,
                        kPanelCut, kFrameThickness, kStep);
  const gfx::Font& font = pixelFont();
  canvas.drawTextInteger(font, heading, kCenterX, kPanelCenterY - 12,
                         gfx::Datum::MiddleCenter, color, kSmallScale);
  canvas.drawTextInteger(font, detail, kCenterX, kPanelCenterY + 14,
                         gfx::Datum::MiddleCenter, kText, kSmallScale);

  // A bar along the bottom gives the hold a visible end. The old design used a
  // ring arc for this; a bar is what fits the new plate.
  constexpr int kBarInset = 12;
  constexpr int kBarY = kPanelY + kPanelH - 20;
  constexpr int kBarH = 4;
  const int barWidth = kPanelW - 2 * kBarInset;
  canvas.fillRect(kPanelX + kBarInset, kBarY, barWidth, kBarH, kTrack);
  const float progress =
      confirming ? std::max(0.0f, std::min(1.0f, state.powerHoldProgress))
                 : 1.0f;
  const int filled = static_cast<int>(barWidth * progress);
  if (filled > 0) {
    canvas.fillRect(kPanelX + kBarInset, kBarY, filled, kBarH, color);
  }
}

inline void render(gfx::Canvas& canvas, const State& state) {
  const backgrounds::Scene scene = backgrounds::forTheme(state.night);
  if (scene.data != nullptr && scene.size == backgrounds::kBytes) {
    canvas.drawBitmap565(0, 0, backgrounds::kWidth, backgrounds::kHeight,
                         scene.data, backgrounds::kWidth);
  } else {
    // A missing or truncated asset must not produce a torn image, so fall back
    // to the flat backdrop the rest of the chrome is designed against.
    canvas.fillScreen(kBackground);
  }

  drawWeeklyQuotaRing(canvas, state);
  drawClockAndDate(canvas, state);
  drawPanel(canvas, state);
  drawAgentKeys(canvas, state);
  drawMotto(canvas);
  drawTransient(canvas, state);
  drawPowerOverlay(canvas, state);
}

// ------------------------------------------------------------------- input --

// Rectangular containment, not a radius test: the keys moved from an orbit to
// a grid, and a circle around a 77 x 35 key would either miss its corners or
// overlap its neighbour.
inline int agentAtPoint(int x, int y) {
  for (int i = 0; i < 6; ++i) {
    const int left = kButtonColumns[i % 3] - kButtonW / 2;
    const int top = kButtonRows[i / 3] - kButtonH / 2;
    if (x >= left && x < left + kButtonW && y >= top && y < top + kButtonH) {
      return i;
    }
  }
  return -1;
}

// The design dropped the Send ring, but it still prints SEND on the panel, so
// the panel is the tap target. That keeps the label truthful and preserves the
// press-and-hold-to-power-off gesture, which is anchored to Send.
inline bool sendAtPoint(int x, int y) {
  return x >= kPanelX && x < kPanelX + kPanelW && y >= kPanelY &&
         y < kPanelY + kPanelH;
}

}  // namespace dashboard
