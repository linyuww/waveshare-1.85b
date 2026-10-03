// SPDX-License-Identifier: MIT

#include "gfx.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdio>

#include "esp_heap_caps.h"

namespace gfx {
namespace {

inline int32_t readBe32(const uint8_t* p) {
  return static_cast<int32_t>((static_cast<uint32_t>(p[0]) << 24) |
                              (static_cast<uint32_t>(p[1]) << 16) |
                              (static_cast<uint32_t>(p[2]) << 8) |
                              static_cast<uint32_t>(p[3]));
}

inline float clamp01(float value) {
  if (value < 0.0f) return 0.0f;
  if (value > 1.0f) return 1.0f;
  return value;
}

constexpr float kPi = 3.14159265358979323846f;

}  // namespace

bool Font::load(const uint8_t* vlw) {
  if (vlw == nullptr) return false;
  data = vlw;
  glyphCount = readBe32(vlw + 0);
  size = readBe32(vlw + 8);
  ascent = readBe32(vlw + 16);
  descent = readBe32(vlw + 20);
  lineHeight = ascent + descent;
  bitmapBase = vlw + 24 + glyphCount * 28;

  // Layout maths needs the ink height rather than the em box: a Space Mono
  // cap sits well inside its ascent/descent pair, and using the em box would
  // make circular text budgets look far tighter than they really are.
  capHeight = 0;
  for (int i = 0; i < glyphCount; ++i) {
    const uint8_t* record = vlw + 24 + i * 28;
    if (readBe32(record) == 'A') {
      capHeight = readBe32(record + 4);
      break;
    }
  }
  if (capHeight <= 0 && glyphCount > 0) {
    capHeight = readBe32(vlw + 24 + 4);
  }
  return glyphCount > 0;
}

namespace {

// Walks the glyph table for one character. The table is tiny (printable ASCII
// only) so a linear scan is cheaper than an index.
bool findGlyph(const Font& font, uint8_t character, int& advance, int& width,
               int& height, int& topExtent, int& left,
               const uint8_t*& bitmap) {
  int32_t offset = 0;
  for (int i = 0; i < font.glyphCount; ++i) {
    const uint8_t* record = font.data + 24 + i * 28;
    const int32_t unicode = readBe32(record + 0);
    const int32_t glyphHeight = readBe32(record + 4);
    const int32_t glyphWidth = readBe32(record + 8);
    const int32_t xAdvance = readBe32(record + 12);
    const int32_t glyphTopExtent = readBe32(record + 16);
    const int32_t glyphLeft = readBe32(record + 20);
    if (unicode == character) {
      advance = xAdvance;
      width = glyphWidth;
      height = glyphHeight;
      topExtent = glyphTopExtent;
      left = glyphLeft;
      bitmap = font.bitmapBase + offset;
      return true;
    }
    offset += glyphHeight * glyphWidth;
  }
  return false;
}

int glyphAdvance(const Font& font, uint8_t character) {
  int advance = 0;
  int width = 0;
  int height = 0;
  int topExtent = 0;
  int left = 0;
  const uint8_t* bitmap = nullptr;
  if (findGlyph(font, character, advance, width, height, topExtent, left,
                bitmap)) {
    return advance;
  }
  // Space and any other unmapped code point use the monospace advance taken
  // from the first printable glyph.
  int fallbackAdvance = 0;
  int fallbackWidth = 0;
  int fallbackHeight = 0;
  int fallbackTop = 0;
  int fallbackLeft = 0;
  const uint8_t* fallbackBitmap = nullptr;
  if (findGlyph(font, '!', fallbackAdvance, fallbackWidth, fallbackHeight,
                fallbackTop, fallbackLeft, fallbackBitmap)) {
    return fallbackAdvance;
  }
  return font.size / 2;
}

}  // namespace

int Font::textWidth(const char* text) const {
  int width = 0;
  for (const char* cursor = text; cursor != nullptr && *cursor != '\0';
       ++cursor) {
    width += glyphAdvance(*this, static_cast<uint8_t>(*cursor));
  }
  return width;
}

bool Canvas::begin() {
  if (buffer_ != nullptr) return true;
  const size_t bytes = sizeof(uint16_t) * kWidth * kHeight;
  buffer_ = static_cast<uint16_t*>(
      heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (buffer_ == nullptr) {
    buffer_ = static_cast<uint16_t*>(heap_caps_malloc(bytes, MALLOC_CAP_8BIT));
  }
  return buffer_ != nullptr;
}

Canvas::~Canvas() { release(); }

void Canvas::release() {
  heap_caps_free(buffer_);
  buffer_ = nullptr;
}

void Canvas::fillScreen(uint16_t color) {
  const uint16_t stored = swap16(color);
  uint32_t* wide = reinterpret_cast<uint32_t*>(buffer_);
  const uint32_t pair = (static_cast<uint32_t>(stored) << 16) | stored;
  const size_t count = (sizeof(uint16_t) * kWidth * kHeight) / sizeof(uint32_t);
  for (size_t i = 0; i < count; ++i) wide[i] = pair;
}

void Canvas::fillRect(int x, int y, int w, int h, uint16_t color) {
  if (w <= 0 || h <= 0) return;
  for (int row = 0; row < h; ++row) {
    for (int column = 0; column < w; ++column) {
      solidPixel(x + column, y + row, color);
    }
  }
}

void Canvas::solidPixel(int x, int y, uint16_t color) {
  if (x < 0 || y < 0 || x >= kWidth || y >= kHeight) return;
  buffer_[y * kWidth + x] = swap16(color);
}

void Canvas::blendPixel(int x, int y, uint16_t color, float coverage) {
  if (x < 0 || y < 0 || x >= kWidth || y >= kHeight) return;
  if (coverage <= 0.0f) return;
  if (coverage >= 1.0f) {
    buffer_[y * kWidth + x] = swap16(color);
    return;
  }
  const uint16_t background = swap16(buffer_[y * kWidth + x]);
  const int backgroundR = (background >> 11) & 0x1F;
  const int backgroundG = (background >> 5) & 0x3F;
  const int backgroundB = background & 0x1F;
  const int sourceR = (color >> 11) & 0x1F;
  const int sourceG = (color >> 5) & 0x3F;
  const int sourceB = color & 0x1F;
  const int r = backgroundR + static_cast<int>((sourceR - backgroundR) * coverage + 0.5f);
  const int g = backgroundG + static_cast<int>((sourceG - backgroundG) * coverage + 0.5f);
  const int b = backgroundB + static_cast<int>((sourceB - backgroundB) * coverage + 0.5f);
  buffer_[y * kWidth + x] = swap16(static_cast<uint16_t>(
      ((r & 0x1F) << 11) | ((g & 0x3F) << 5) | (b & 0x1F)));
}

void Canvas::fillCircle(int cx, int cy, int r, uint16_t color) {
  if (r <= 0) return;
  const float outer = static_cast<float>(r) + 0.5f;
  const float inner = static_cast<float>(r) - 0.5f;
  for (int y = cy - r; y <= cy + r; ++y) {
    const float dy = static_cast<float>(y) - static_cast<float>(cy);
    const float outerSquared = outer * outer - dy * dy;
    if (outerSquared <= 0.0f) continue;
    const float outerHalf = sqrtf(outerSquared);
    const int outerLeft = static_cast<int>(ceilf(cx - outerHalf));
    const int outerRight = static_cast<int>(floorf(cx + outerHalf));
    if (outerRight < outerLeft) continue;

    const float innerSquared = inner * inner - dy * dy;
    const float innerHalf = innerSquared > 0.0f ? sqrtf(innerSquared) : -1.0f;
    const int solidLeft =
        innerHalf > 0.0f ? static_cast<int>(ceilf(cx - innerHalf)) : 0;
    const int solidRight =
        innerHalf > 0.0f ? static_cast<int>(floorf(cx + innerHalf)) : -1;

    for (int x = solidLeft; x <= solidRight; ++x) {
      if (x < outerLeft || x > outerRight) continue;
      solidPixel(x, y, color);
    }
    for (int x = outerLeft; x < solidLeft; ++x) {
      const float dx = static_cast<float>(x) - static_cast<float>(cx);
      blendPixel(x, y, color, clamp01(outer - sqrtf(dx * dx + dy * dy)));
    }
    for (int x = solidRight + 1; x <= outerRight; ++x) {
      const float dx = static_cast<float>(x) - static_cast<float>(cx);
      blendPixel(x, y, color, clamp01(outer - sqrtf(dx * dx + dy * dy)));
    }
  }
}

void Canvas::fillRingArc(int cx, int cy, int rOuter, int rInner,
                         float startDeg, float endDeg, uint16_t color) {
  float sweep = endDeg - startDeg;
  if (sweep <= 0.0f) return;
  const bool fullCircle = sweep >= 359.5f;
  if (sweep > 360.0f) sweep = 360.0f;

  const float outer = static_cast<float>(rOuter) + 0.5f;
  const float inner = static_cast<float>(rInner) - 0.5f;
  const float outerSquared = outer * outer;
  const float innerSquared = inner > 0.0f ? inner * inner : 0.0f;

  for (int y = cy - rOuter - 1; y <= cy + rOuter + 1; ++y) {
    const float dy = static_cast<float>(y) - static_cast<float>(cy);
    for (int x = cx - rOuter - 1; x <= cx + rOuter + 1; ++x) {
      const float dx = static_cast<float>(x) - static_cast<float>(cx);
      const float distanceSquared = dx * dx + dy * dy;
      if (distanceSquared > outerSquared) continue;
      if (inner > 0.0f && distanceSquared < innerSquared) continue;
      const float distance = sqrtf(distanceSquared);
      float coverage = clamp01(outer - distance);
      if (inner > 0.0f) {
        coverage = fminf(coverage, clamp01(distance - inner));
      }
      if (coverage <= 0.0f) continue;

      if (!fullCircle) {
        float angle = atan2f(dx, -dy) * (180.0f / kPi);
        if (angle < 0.0f) angle += 360.0f;
        float offset = angle - startDeg;
        if (offset < 0.0f) offset += 360.0f;
        if (offset > sweep) continue;
        // Angular edge softening: convert the remaining angular room to a
        // pixel distance so the arc ends stay round instead of aliased.
        const float radius = fmaxf(distance, 1.0f);
        const float roomStart = offset * (kPi / 180.0f) * radius;
        const float roomEnd = (sweep - offset) * (kPi / 180.0f) * radius;
        coverage = fminf(coverage, clamp01(roomStart + 0.5f));
        coverage = fminf(coverage, clamp01(roomEnd + 0.5f));
      }
      blendPixel(x, y, color, coverage);
    }
  }
}

void Canvas::fillSegmentedRing(int cx, int cy, int rOuter, int rInner,
                               int segments, float segmentDegrees,
                               int activeSegments, uint16_t activeColor,
                               uint16_t inactiveColor) {
  if (segments <= 0 || rOuter <= rInner || segmentDegrees <= 0.0f) return;
  const float pitch = 360.0f / static_cast<float>(segments);
  segmentDegrees = fminf(segmentDegrees, pitch);
  activeSegments = std::max(0, std::min(segments, activeSegments));
  const float outer = static_cast<float>(rOuter) + 0.5f;
  const float inner = static_cast<float>(rInner) - 0.5f;
  const float outerSquared = outer * outer;
  const float innerSquared = inner * inner;

  // One annulus traversal matters on the ESP32: calling fillRingArc once per
  // segment would scan almost eight million pixels for a 60-segment ring.
  for (int y = cy - rOuter - 1; y <= cy + rOuter + 1; ++y) {
    const float dy = static_cast<float>(y - cy);
    for (int x = cx - rOuter - 1; x <= cx + rOuter + 1; ++x) {
      const float dx = static_cast<float>(x - cx);
      const float distanceSquared = dx * dx + dy * dy;
      if (distanceSquared > outerSquared || distanceSquared < innerSquared) {
        continue;
      }

      float angle = atan2f(dx, -dy) * (180.0f / kPi);
      if (angle < 0.0f) angle += 360.0f;
      const int segment = std::min(
          segments - 1, static_cast<int>(floorf(angle / pitch)));
      const float within = angle - static_cast<float>(segment) * pitch;
      if (within > segmentDegrees) continue;

      const float distance = sqrtf(distanceSquared);
      float coverage = fminf(clamp01(outer - distance),
                             clamp01(distance - inner));
      const float radius = fmaxf(distance, 1.0f);
      coverage = fminf(coverage,
                       clamp01(within * (kPi / 180.0f) * radius + 0.5f));
      coverage = fminf(
          coverage,
          clamp01((segmentDegrees - within) * (kPi / 180.0f) * radius + 0.5f));
      blendPixel(x, y, segment < activeSegments ? activeColor : inactiveColor,
                 coverage);
    }
  }
}

void Canvas::fillRoundRect(int x, int y, int w, int h, int radius,
                           uint16_t color) {
  if (w <= 0 || h <= 0) return;
  if (radius * 2 > w) radius = w / 2;
  if (radius * 2 > h) radius = h / 2;
  const float left = static_cast<float>(x + radius);
  const float right = static_cast<float>(x + w - radius);
  const float top = static_cast<float>(y + radius);
  const float bottom = static_cast<float>(y + h - radius);
  const float corner = static_cast<float>(radius);

  for (int py = y; py < y + h; ++py) {
    const float fy = static_cast<float>(py) + 0.5f;
    const float dy = fmaxf(fmaxf(top - fy, fy - bottom), 0.0f);
    for (int px = x; px < x + w; ++px) {
      const float fx = static_cast<float>(px) + 0.5f;
      const float dx = fmaxf(fmaxf(left - fx, fx - right), 0.0f);
      const float distance = sqrtf(dx * dx + dy * dy) - corner;
      const float coverage = clamp01(0.5f - distance);
      if (coverage > 0.0f) blendPixel(px, py, color, coverage);
    }
  }
}

void Canvas::fillTriangle(int x0, int y0, int x1, int y1, int x2, int y2,
                          uint16_t color) {
  const int minX = std::min(x0, std::min(x1, x2));
  const int maxX = std::max(x0, std::max(x1, x2));
  const int minY = std::min(y0, std::min(y1, y2));
  const int maxY = std::max(y0, std::max(y1, y2));
  const float area =
      static_cast<float>((x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0));
  if (fabsf(area) < 0.0001f) return;

  for (int py = minY; py <= maxY; ++py) {
    const float fy = static_cast<float>(py) + 0.5f;
    for (int px = minX; px <= maxX; ++px) {
      const float fx = static_cast<float>(px) + 0.5f;
      const float w0 = static_cast<float>((x1 - x0) * (fy - y0) - (fx - x0) * (y1 - y0));
      const float w1 = static_cast<float>((x2 - x1) * (fy - y1) - (fx - x1) * (y2 - y1));
      const float w2 = static_cast<float>((x0 - x2) * (fy - y2) - (fx - x2) * (y0 - y2));
      const bool inside = (w0 >= 0.0f && w1 >= 0.0f && w2 >= 0.0f) ||
                          (w0 <= 0.0f && w1 <= 0.0f && w2 <= 0.0f);
      if (inside) solidPixel(px, py, color);
    }
  }
}

void Canvas::fillPixelRect(int x, int y, int w, int h, uint16_t color, int cut,
                           int step) {
  if (w <= 0 || h <= 0) return;
  if (cut <= 0 || step <= 0) {
    fillRect(x, y, w, h, color);
    return;
  }
  for (int row = 0; row < h; ++row) {
    // A step is measured from whichever edge is nearer, so top and bottom
    // corners come out identical without a second code path.
    const int distance = std::min(row, h - 1 - row);
    const int inset = distance < cut ? step * (cut - distance) : 0;
    for (int column = inset; column < w - inset; ++column) {
      solidPixel(x + column, y + row, color);
    }
  }
}

void Canvas::fillPixelFrame(int x, int y, int w, int h, uint16_t frame,
                            uint16_t fill, int cut, int thickness, int step) {
  fillPixelRect(x, y, w, h, frame, cut, step);
  if (thickness <= 0) return;
  // The inner cut is reduced by the thickness because the inner rectangle
  // starts one border-width further in, and its own steps are correspondingly
  // shallower. Matches pixel_preview.py's pixel_frame().
  fillPixelRect(x + thickness, y + thickness, w - 2 * thickness,
                h - 2 * thickness, fill, std::max(0, cut - thickness), step);
}

void Canvas::drawBitmap565(int x, int y, int w, int h, const uint8_t* source,
                           int sourceWidth) {
  if (source == nullptr || w <= 0 || h <= 0 || sourceWidth <= 0) return;
  // Clip once up front so the copy loop needs no bounds test per row.
  const int firstX = std::max(0, x);
  const int firstY = std::max(0, y);
  const int lastX = std::min(kWidth, x + w);
  const int lastY = std::min(kHeight, y + h);
  if (firstX >= lastX || firstY >= lastY) return;

  const size_t rowBytes = static_cast<size_t>(lastX - firstX) * 2;
  for (int row = firstY; row < lastY; ++row) {
    const uint8_t* line =
        source + (static_cast<size_t>(row - y) * sourceWidth +
                  static_cast<size_t>(firstX - x)) * 2;
    // Both sides are big-endian RGB565, so this is a straight byte copy --
    // which is the whole reason the asset is stored in panel byte order.
    memcpy(reinterpret_cast<uint8_t*>(buffer_ + row * kWidth + firstX), line,
           rowBytes);
  }
}

void Canvas::drawMask(int x, int y, const uint8_t* mask, int w, int h,
                      uint16_t color, float alpha) {
  if (mask == nullptr || w <= 0 || h <= 0 || alpha <= 0.0f) return;
  for (int row = 0; row < h; ++row) {
    for (int column = 0; column < w; ++column) {
      const uint8_t coverage = mask[row * w + column];
      if (coverage == 0) continue;
      blendPixel(x + column, y + row, color,
                 static_cast<float>(coverage) / 255.0f * alpha);
    }
  }
}

void Canvas::drawTexture(int x, int y, const uint16_t* words,
                         const uint8_t* alphas, int w, int h, float alpha) {
  if (words == nullptr || alphas == nullptr || w <= 0 || h <= 0) return;
  if (alpha <= 0.0f) return;
  for (int row = 0; row < h; ++row) {
    const int py = y + row;
    if (py < 0 || py >= kHeight) continue;
    for (int column = 0; column < w; ++column) {
      const int index = row * w + column;
      const int coverage = alphas[index];
      if (coverage == 0) continue;
      const int px = x + column;
      if (px < 0 || px >= kWidth) continue;

      const uint16_t word = words[index];
      if (coverage >= 255 && alpha >= 1.0f) {
        buffer_[py * kWidth + px] = swap16(word);
        continue;
      }

      // Blending is done in 8 bits per channel and then re-quantised, which is
      // the same chain the preview and the panel go through: the texture is
      // stored as RGB565, the preview blends it against an 8-bit background,
      // and the panel finally quantises. Doing the arithmetic in 565 space
      // would drift by a least-significant bit.
      const int amount =
          alpha >= 1.0f ? coverage
                        : static_cast<int>(coverage * alpha + 0.5f);
      const int inverse = 255 - amount;
      const uint16_t background = swap16(buffer_[py * kWidth + px]);
      const int sourceR = ((word >> 11) & 0x1F) << 3;
      const int sourceG = ((word >> 5) & 0x3F) << 2;
      const int sourceB = (word & 0x1F) << 3;
      const int r = std::min(255, sourceR +
                                      ((((background >> 11) & 0x1F) << 3) *
                                           inverse +
                                       127) /
                                          255);
      const int g = std::min(255, sourceG +
                                      ((((background >> 5) & 0x3F) << 2) *
                                           inverse +
                                       127) /
                                          255);
      const int b = std::min(255, sourceB +
                                      (((background & 0x1F) << 3) * inverse +
                                       127) /
                                          255);
      buffer_[py * kWidth + px] = swap16(rgb565(
          static_cast<uint8_t>(r), static_cast<uint8_t>(g),
          static_cast<uint8_t>(b)));
    }
  }
}

void Canvas::drawText(const Font& font, const char* text, int x, int y,
                      Datum datum, uint16_t color) {
  if (font.data == nullptr || text == nullptr) return;
  const int baseline = y + font.baselineOffset();
  int penX = x;
  if (datum == Datum::MiddleCenter) {
    penX = x - font.textWidth(text) / 2;
  }

  for (const char* cursor = text; *cursor != '\0'; ++cursor) {
    const uint8_t character = static_cast<uint8_t>(*cursor);
    int advance = 0;
    int width = 0;
    int height = 0;
    int topExtent = 0;
    int left = 0;
    const uint8_t* bitmap = nullptr;
    if (!findGlyph(font, character, advance, width, height, topExtent, left,
                   bitmap)) {
      penX += glyphAdvance(font, character);
      continue;
    }
    if (width > 0 && height > 0) {
      const int glyphTop = baseline - topExtent;
      const int glyphLeft = penX + left;
      for (int row = 0; row < height; ++row) {
        const uint8_t* source = bitmap + row * width;
        const int py = glyphTop + row;
        for (int column = 0; column < width; ++column) {
          const uint8_t alpha = source[column];
          if (alpha == 0) continue;
          blendPixel(glyphLeft + column, py, color,
                     static_cast<float>(alpha) / 255.0f);
        }
      }
    }
    penX += advance;
  }
}

void Canvas::drawTextInteger(const Font& font, const char* text, int x, int y,
                             Datum datum, uint16_t color, int scale) {
  if (font.data == nullptr || text == nullptr || scale <= 0) return;
  // Deliberately not routed through drawTextScaled(): that path rounds its
  // pen position, while the preview floors it. At integer scales the two
  // differ by a pixel on odd-width strings, and this text is the clock, so
  // the firmware follows the preview exactly rather than approximately.
  const int baseline = y + font.baselineOffset() * scale;
  int penX = x;
  if (datum == Datum::MiddleCenter) {
    penX = x - (font.textWidth(text) * scale) / 2;
  }

  for (const char* cursor = text; *cursor != '\0'; ++cursor) {
    const uint8_t character = static_cast<uint8_t>(*cursor);
    int advance = 0;
    int width = 0;
    int height = 0;
    int topExtent = 0;
    int left = 0;
    const uint8_t* bitmap = nullptr;
    if (!findGlyph(font, character, advance, width, height, topExtent, left,
                   bitmap)) {
      penX += glyphAdvance(font, character) * scale;
      continue;
    }
    if (width > 0 && height > 0) {
      const int glyphTop = baseline - topExtent * scale;
      const int glyphLeft = penX + left * scale;
      for (int row = 0; row < height * scale; ++row) {
        // Nearest-neighbour by integer division: each source pixel becomes a
        // solid scale x scale block, which is what makes an integer upscale
        // bit-exact.
        const uint8_t* source = bitmap + (row / scale) * width;
        const int py = glyphTop + row;
        for (int column = 0; column < width * scale; ++column) {
          const uint8_t alpha = source[column / scale];
          if (alpha == 0) continue;
          blendPixel(glyphLeft + column, py, color,
                     static_cast<float>(alpha) / 255.0f);
        }
      }
    }
    penX += advance * scale;
  }
}

void Canvas::drawTextOutlined(const Font& font, const char* text, int x, int y,
                              Datum datum, uint16_t color, uint16_t outline,
                              int thickness, int scale) {
  if (thickness <= 0) {
    drawTextInteger(font, text, x, y, datum, color, scale);
    return;
  }
  // The halo is the text stamped at every offset in the (2t+1)^2 neighbourhood
  // except the centre, then the text itself on top. Walking the glyphs a few
  // extra times is cheaper than collecting the lit pixels into a buffer, and
  // the strings involved are short.
  for (int dy = -thickness; dy <= thickness; ++dy) {
    for (int dx = -thickness; dx <= thickness; ++dx) {
      if (dx == 0 && dy == 0) continue;
      drawTextInteger(font, text, x + dx, y + dy, datum, outline, scale);
    }
  }
  drawTextInteger(font, text, x, y, datum, color, scale);
}

void Canvas::drawTextFitted(const Font& font, const char* text, int x, int y,
                            Datum datum, uint16_t color, int maxWidth) {
  if (font.data == nullptr || text == nullptr || *text == '\0') return;
  const int width = font.textWidth(text);
  if (width <= 0) return;
  if (maxWidth <= 0) return;
  if (width <= maxWidth) {
    drawText(font, text, x, y, datum, color);
    return;
  }
  float scale = static_cast<float>(maxWidth) / static_cast<float>(width);
  // Below roughly two thirds the glyphs turn to mush; clip rather than shrink
  // into illegibility, so an oversized string can never smear across the dial.
  if (scale < 0.66f) scale = 0.66f;
  drawTextScaled(font, text, x, y, datum, color, scale);
}

void Canvas::drawTextScaled(const Font& font, const char* text, int x, int y,
                            Datum datum, uint16_t color, float scale) {
  if (font.data == nullptr || text == nullptr || scale <= 0.0f) return;

  auto scaledRound = [](float value) -> int {
    return static_cast<int>(value >= 0.0f ? value + 0.5f : value - 0.5f);
  };

  const int baseline =
      y + scaledRound(static_cast<float>(font.baselineOffset()) * scale);
  int penX = x;
  if (datum == Datum::MiddleCenter) {
    penX = x - scaledRound(static_cast<float>(font.textWidth(text)) * scale *
                           0.5f);
  }

  for (const char* cursor = text; *cursor != '\0'; ++cursor) {
    const uint8_t character = static_cast<uint8_t>(*cursor);
    int advance = 0;
    int width = 0;
    int height = 0;
    int topExtent = 0;
    int left = 0;
    const uint8_t* bitmap = nullptr;
    if (!findGlyph(font, character, advance, width, height, topExtent, left,
                   bitmap)) {
      penX += scaledRound(static_cast<float>(glyphAdvance(font, character)) *
                          scale);
      continue;
    }
    if (width > 0 && height > 0) {
      const int destinationWidth =
          std::max(1, scaledRound(static_cast<float>(width) * scale));
      const int destinationHeight =
          std::max(1, scaledRound(static_cast<float>(height) * scale));
      const int glyphTop =
          baseline - scaledRound(static_cast<float>(topExtent) * scale);
      const int glyphLeft = penX + scaledRound(static_cast<float>(left) * scale);
      for (int row = 0; row < destinationHeight; ++row) {
        const int sourceRow = row * height / destinationHeight;
        const uint8_t* source = bitmap + sourceRow * width;
        const int py = glyphTop + row;
        for (int column = 0; column < destinationWidth; ++column) {
          const int sourceColumn = column * width / destinationWidth;
          const uint8_t alpha = source[sourceColumn];
          if (alpha == 0) continue;
          // Nearest-neighbour keeps strokes hard; each destination pixel is
          // covered once, so the alpha is used as-is.
          blendPixel(glyphLeft + column, py, color,
                     static_cast<float>(alpha) / 255.0f);
        }
      }
    }
    penX += scaledRound(static_cast<float>(advance) * scale);
  }
}

}  // namespace gfx
