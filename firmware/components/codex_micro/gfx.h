// SPDX-License-Identifier: MIT
// Minimal software 2D canvas for the 360x360 ST77916 panel.
//
// The original C152 port renders through M5GFX primitives. This file keeps the
// same primitive set (smooth circles, ring arcs, rounded rects, VLW text) but
// targets a plain RGB565 framebuffer that is pushed with esp_lcd.
//
// The framebuffer is stored big-endian because the panel receives RGB565
// most-significant-byte first over QSPI.

#pragma once

#include <cstddef>
#include <cstdint>

namespace gfx {

constexpr int kWidth = 360;
constexpr int kHeight = 360;

inline uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
  return static_cast<uint16_t>(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

inline uint16_t swap16(uint16_t value) {
  return static_cast<uint16_t>((value >> 8) | (value << 8));
}

enum class Datum {
  MiddleLeft,
  MiddleCenter,
};

// Parsed view over an M5GFX-compatible VLW font blob.
struct Font {
  const uint8_t* data = nullptr;
  const uint8_t* bitmapBase = nullptr;
  int glyphCount = 0;
  int size = 0;
  int ascent = 0;
  int descent = 0;
  int lineHeight = 0;
  int capHeight = 0;  // ink height of an uppercase glyph, for layout maths

  bool load(const uint8_t* vlw);
  int textWidth(const char* text) const;
  // Distance from the requested vertical centre down to the baseline.
  int baselineOffset() const { return (ascent - descent) / 2; }
  // Height the text actually occupies on screen.
  int textHeight() const { return capHeight > 0 ? capHeight : size; }
};

class Canvas {
 public:
  ~Canvas();
  Canvas() = default;
  Canvas(const Canvas&) = delete;
  Canvas& operator=(const Canvas&) = delete;
  bool begin();
  void release();
  void fillScreen(uint16_t color);

  void fillRect(int x, int y, int w, int h, uint16_t color);
  void fillCircle(int cx, int cy, int r, uint16_t color);
  // Angles in degrees, 0 at 12 o'clock, increasing clockwise.
  void fillRingArc(int cx, int cy, int rOuter, int rInner, float startDeg,
                   float endDeg, uint16_t color);
  void fillSegmentedRing(int cx, int cy, int rOuter, int rInner, int segments,
                         float segmentDegrees, int activeSegments,
                         uint16_t activeColor, uint16_t inactiveColor);
  void fillRoundRect(int x, int y, int w, int h, int radius, uint16_t color);
  void fillTriangle(int x0, int y0, int x1, int y1, int x2, int y2,
                    uint16_t color);

  // ------------------------------------------------- pixel-art primitives --
  // The dashboard's frame is not a smooth diagonal: it is a staircase of
  // discrete steps. `cut` is how many steps, `step` how many pixels of inset
  // each one contributes, so the outermost row is inset by cut * step and the
  // level shrinks by `step` per row until it vanishes. Both mirror
  // scripts/assets/pixel_preview.py's pixel_rect()/pixel_frame() exactly, so the PC
  // preview and the panel agree pixel for pixel.
  void fillPixelRect(int x, int y, int w, int h, uint16_t color, int cut,
                     int step = 2);
  void fillPixelFrame(int x, int y, int w, int h, uint16_t frame,
                      uint16_t fill, int cut, int thickness, int step = 2);

  // Copies a raw RGB565 image straight into the framebuffer. The source must
  // already be in panel byte order (high byte first), which is how
  // scripts/assets/make_backgrounds.py writes it -- so this is a row-wise memcpy with
  // no per-pixel work. `sourceWidth` is the source's row stride in pixels.
  // There is no scale parameter on purpose: the scene art has no consistent
  // pixel grid to snap to, so it is stored at native 360x360 and blitted 1:1.
  void drawBitmap565(int x, int y, int w, int h, const uint8_t* source,
                     int sourceWidth);

  // An 8-bit coverage mask, tinted with `color`. Used for the battery icon,
  // whose colour tracks the charge level and so cannot be baked into a
  // texture.
  void drawMask(int x, int y, const uint8_t* mask, int w, int h,
                uint16_t color, float alpha = 1.0f);

  // A premultiplied RGB565 + A8 texture: `out = src + dst * (255 - a) / 255`.
  // The artist's icons carry internal shading, so they ship as colour textures
  // rather than a coverage mask plus a tint; premultiplying is also what lets
  // the brain's fold lines show the button fill through, as the design does.
  void drawTexture(int x, int y, const uint16_t* words, const uint8_t* alphas,
                   int w, int h, float alpha = 1.0f);

  void drawText(const Font& font, const char* text, int x, int y, Datum datum,
                uint16_t color);
  // Draws `text` at an integer pixel scale. Every integer multiple of the 8 px
  // em is a bit-exact upscale, which is why a single font size ships instead
  // of four. Mirrors pixel_preview.py's glyph_pixels() so the two agree.
  void drawTextInteger(const Font& font, const char* text, int x, int y,
                       Datum datum, uint16_t color, int scale);
  // Text with a dark halo. Required rather than decorative: the clock and date
  // sit directly on the scene, and in the day theme that scene is a bright
  // sky, where white text without a halo is unreadable.
  void drawTextOutlined(const Font& font, const char* text, int x, int y,
                        Datum datum, uint16_t color, uint16_t outline,
                        int thickness, int scale);
  // Draws `text` centred/left aligned at (x, y), shrinking it horizontally
  // only when it does not fit `maxWidth`. Never grows the text.
  void drawTextFitted(const Font& font, const char* text, int x, int y,
                      Datum datum, uint16_t color, int maxWidth);

  uint16_t* pixels() { return buffer_; }

 private:
  void drawTextScaled(const Font& font, const char* text, int x, int y,
                      Datum datum, uint16_t color, float scale);
  void blendPixel(int x, int y, uint16_t color, float coverage);
  void solidPixel(int x, int y, uint16_t color);
  uint16_t* buffer_ = nullptr;
};

}  // namespace gfx
