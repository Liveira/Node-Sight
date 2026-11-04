// Platform-neutral terminal renderer shared by Win32/GDI and Linux/GTK+Cairo.
#pragma once
#include "monitor.h"
#include <cstdint>

using Color = uint32_t; // 0xRRGGBB, never a platform-specific COLORREF.
namespace Palette {
constexpr Color background = 0x000000, border = 0x2b2f2a, text = 0xdee6da, muted = 0x9ba492;
constexpr Color dim = 0x656d60, green = 0x97c579, cyan = 0x66cbd9, purple = 0xa083cb, gold = 0xd2c96f,
                red = 0xd36f6e;
} // namespace Palette
/** All coordinates are logical pixels. Native backends own fonts and scaling. */
class Canvas {
  public:
    virtual ~Canvas() = default;
    virtual void fill(int x, int y, int w, int h, Color color) = 0;
    virtual void line(int x, int y, int xx, int yy, Color color) = 0;
    virtual void text(int x, int y, int w, int h, const std::wstring& value, int font, Color color,
                      bool right = false) = 0;
};
// Shared hit targets keep both native frontends aligned with the compact terminal header.
constexpr int WIDGET_WIDTH = 360, HEADER_HEIGHT = 54, FOOTER_HEIGHT = 50;
constexpr int HEADER_PIN = 204, HEADER_SETTINGS = 260, HEADER_HIDE = 315, HEADER_CLOSE = 335;
struct HostHit {
    std::wstring id;
    int top = 0, bottom = 0;
};
struct DashboardLayout {
    int height = 120;
    std::vector<HostHit> hosts;
};
/** Compute the same geometry used for hit testing, scrolling and drawing. */
DashboardLayout dashboardLayout(const DashboardState& state, const Config& config);
void drawDashboard(Canvas& canvas, const DashboardState& state, const Config& config, int scroll = 0,
                   bool demo = false, int viewportHeight = 0);
void drawFooter(Canvas& canvas, const DashboardState& state, int height);
std::wstring formatNumber(double value, int precision = 1);
std::wstring formatBytes(double bytes);
std::wstring formatDuration(double seconds);
