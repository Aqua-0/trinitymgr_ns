#pragma once

#include <switch.h>
#include <switch/display/framebuffer.h>
#include <cstdint>
#include <string>
#include <vector>
#include <array>

namespace gfx {

class Canvas;
class Font;

struct Color {
    uint8_t r=0, g=0, b=0, a=255;
};

struct Texture {
    int w=0;
    int h=0;
    std::vector<uint32_t> pixels;
    bool valid() const;
    void scaleTo(int newW, int newH);
};

void wrapTextLines(const std::string& text_in, int maxWidth, const Font& fontPrimary,
                   const Font& fontSmall, bool smallFont, std::vector<std::string>& out);
void drawWrappedText(Canvas& canvas, const std::string& text_in, int x, int y, int maxWidth,
                     Color color, const Font& fontPrimary, const Font& fontSmall, bool forceSmall=false);

class Canvas {
public:
    void init(int width, int height);
    void clear(Color c);
    void fillRect(int x,int y,int rw,int rh, Color c);
    void fillRoundedRect(int x,int y,int rw,int rh,int radius, Color c);
    void blit(const Texture& tex, int dx, int dy);
    void blitScaled(const Texture& tex, int dx,int dy,int dstW,int dstH);
    std::vector<uint32_t>& data();
    const std::vector<uint32_t>& data() const;
    int width() const;
    int height() const;
    void plot(int x,int y, Color c, uint8_t coverage);
private:
    void blendPixel(size_t idx, Color c, uint8_t coverage);
    int w=0;
    int h=0;
    std::vector<uint32_t> pixels;
};

bool InitFreeType();
void ShutdownFreeType();

class Font {
public:
    bool load(const std::string& path, int pixelHeight, std::string& err);
    void draw(Canvas& canvas, const std::string& text, int x, int y, Color color) const;
    bool ready() const;
    int textWidth(const std::string& text) const;
private:
    struct Glyph {
        int width=0;
        int height=0;
        int bearingX=0;
        int bearingY=0;
        int advance=0;
        std::vector<uint8_t> bitmap;
    };
    const Glyph& glyphFor(char ch) const;
    static bool readFile(const std::string& path, std::vector<uint8_t>& out);
    std::array<Glyph,96> glyphs{};
    int ascent=0;
    int descent=0;
    int lineGap=0;
    bool loaded=false;
};

class DekoPresenter {
public:
    void setLogBuffer(std::string* out);
    bool init(NWindow* win, uint32_t width, uint32_t height);
    void shutdown();
    void present(const void* pixels, size_t numBytes);
private:
    Framebuffer fb{};
    uint32_t fbW=0, fbH=0;
    std::string* logOut=nullptr;
    void logLine(const char* msg);
    void logf(const char* fmt, ...);
};

} // namespace gfx
