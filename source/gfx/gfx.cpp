#include "gfx.hpp"

#include <ft2build.h>
#include FT_FREETYPE_H
#include <algorithm>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <malloc.h>
#include <cctype>
#include <cmath>

namespace gfx {

namespace {
static inline uint32_t packColor(const Color& c){
    return (uint32_t(c.a)<<24) | (uint32_t(c.b)<<16) | (uint32_t(c.g)<<8) | uint32_t(c.r);
}

static FT_Library g_ftlib = nullptr;
}

bool Texture::valid() const {
    return w>0 && h>0 && pixels.size()==static_cast<size_t>(w*h);
}

void Texture::scaleTo(int newW, int newH){
    if(!valid() || newW<=0 || newH<=0) return;
    if(newW==w && newH==h) return;
    std::vector<uint32_t> scaled(static_cast<size_t>(newW)*newH);
    for(int y=0;y<newH;++y){
        int srcY = y * h / newH;
        for(int x=0;x<newW;++x){
            int srcX = x * w / newW;
            scaled[static_cast<size_t>(y)*newW + x] = pixels[static_cast<size_t>(srcY)*w + srcX];
        }
    }
    pixels.swap(scaled);
    w=newW;
    h=newH;
}

void Canvas::init(int width, int height){
    w = width; h = height;
    pixels.assign(static_cast<size_t>(w)*h, 0);
}

void Canvas::clear(Color c){
    std::fill(pixels.begin(), pixels.end(), packColor(c));
}

void Canvas::fillRect(int x,int y,int rw,int rh, Color c){
    if(rw<=0 || rh<=0) return;
    int x0 = std::max(0,x);
    int y0 = std::max(0,y);
    int x1 = std::min(w, x+rw);
    int y1 = std::min(h, y+rh);
    if(x1<=x0 || y1<=y0) return;
    if(c.a>=250){
        uint32_t v = packColor(c);
        for(int yy=y0; yy<y1; ++yy){
            uint32_t* row = &pixels[static_cast<size_t>(yy)*w + x0];
            std::fill(row, row + (x1 - x0), v);
        }
    }else{
        for(int yy=y0; yy<y1; ++yy){
            for(int xx=x0; xx<x1; ++xx){
                plot(xx, yy, c, c.a);
            }
        }
    }
}

void Canvas::fillRoundedRect(int x,int y,int rw,int rh,int radius, Color c){
    if(rw<=0 || rh<=0) return;
    if(radius<=0){
        fillRect(x,y,rw,rh,c);
        return;
    }
    radius = std::min(radius, std::min(rw/2, rh/2));
    fillRect(x, y+radius, rw, rh-2*radius, c);
    fillRect(x+radius, y, rw-2*radius, radius, c);
    fillRect(x+radius, y+rh-radius, rw-2*radius, radius, c);
    for(int dy=0; dy<radius; ++dy){
        int dx = (int)std::round(std::sqrt((double)radius*radius - (double)dy*dy));
        int span = dx*2;
        fillRect(x+radius-dx, y+dy, span, 1, c);
        fillRect(x+radius-dx, y+rh-dy-1, span, 1, c);
    }
}

void Canvas::blit(const Texture& tex, int dx, int dy){
    if(!tex.valid()) return;
    for(int y=0;y<tex.h;++y){
        int ty = dy + y;
        if(ty<0 || ty>=h) continue;
        for(int x=0;x<tex.w;++x){
            int tx = dx + x;
            if(tx<0 || tx>=w) continue;
            uint32_t src = tex.pixels[static_cast<size_t>(y)*tex.w + x];
            uint8_t a = (src>>24)&0xFF;
            Color c{ (uint8_t)((src>>16)&0xFF), (uint8_t)((src>>8)&0xFF), (uint8_t)(src&0xFF), a };
            plot(tx, ty, c, a);
        }
    }
}

void Canvas::blitScaled(const Texture& tex, int dx,int dy,int dstW,int dstH){
    if(!tex.valid() || dstW<=0 || dstH<=0) return;
    for(int y=0;y<dstH;++y){
        int ty = dy + y;
        if(ty<0 || ty>=h) continue;
        int srcY = y * tex.h / dstH;
        for(int x=0;x<dstW;++x){
            int tx = dx + x;
            if(tx<0 || tx>=w) continue;
            int srcX = x * tex.w / dstW;
            uint32_t src = tex.pixels[static_cast<size_t>(srcY)*tex.w + srcX];
            uint8_t a = (src>>24)&0xFF;
            Color c{ (uint8_t)((src>>16)&0xFF), (uint8_t)((src>>8)&0xFF), (uint8_t)(src&0xFF), a };
            plot(tx, ty, c, a);
        }
    }
}

std::vector<uint32_t>& Canvas::data(){ return pixels; }
const std::vector<uint32_t>& Canvas::data() const { return pixels; }
int Canvas::width() const { return w; }
int Canvas::height() const { return h; }

void Canvas::plot(int x,int y, Color c, uint8_t coverage){
    if(x<0 || y<0 || x>=w || y>=h || coverage==0) return;
    size_t idx = static_cast<size_t>(y)*w + x;
    blendPixel(idx, c, coverage);
}

void Canvas::blendPixel(size_t idx, Color c, uint8_t coverage){
    if(idx>=pixels.size()) return;
    uint32_t dst = pixels[idx];
    uint8_t sa = (uint8_t)((uint16_t)c.a * coverage / 255);
    if(sa==255){
        pixels[idx] = packColor(Color{c.r,c.g,c.b,255});
        return;
    }
    uint8_t sr = c.r, sg = c.g, sb = c.b;
    uint8_t dr = (dst>>16)&0xFF;
    uint8_t dg = (dst>>8)&0xFF;
    uint8_t db = dst&0xFF;
    uint8_t da = (dst>>24)&0xFF;
    uint8_t inv = 255 - sa;
    uint8_t outR = (uint8_t)((sr * sa + dr * inv) / 255);
    uint8_t outG = (uint8_t)((sg * sa + dg * inv) / 255);
    uint8_t outB = (uint8_t)((sb * sa + db * inv) / 255);
    uint8_t outA = (uint8_t)(sa + da * inv / 255);
    pixels[idx] = (uint32_t(outA)<<24) | (uint32_t(outR)<<16) | (uint32_t(outG)<<8) | outB;
}

bool InitFreeType(){
    if(g_ftlib) return true;
    return FT_Init_FreeType(&g_ftlib)==0;
}

void ShutdownFreeType(){
    if(g_ftlib){
        FT_Done_FreeType(g_ftlib);
        g_ftlib=nullptr;
    }
}

bool Font::load(const std::string& path, int pixelHeight, std::string& err){
    std::vector<uint8_t> data;
    if(!readFile(path, data)){
        err += "[font] load fail: " + path + "\n";
        return false;
    }
    if(!InitFreeType()){
        err += "[font] freetype init fail\n";
        return false;
    }
    FT_Face face{};
    if(FT_New_Memory_Face(g_ftlib, data.data(), data.size(), 0, &face)!=0){
        err += "[font] FT_New_Memory_Face fail\n";
        return false;
    }
    FT_Set_Pixel_Sizes(face, 0, pixelHeight);
    ascent  = face->size->metrics.ascender  >> 6;
    descent = face->size->metrics.descender >> 6;
    lineGap = face->size->metrics.height    >> 6;
    for(size_t ch=0; ch<glyphs.size(); ++ch){
        char c = static_cast<char>(ch+32);
        if(FT_Load_Char(face, c, FT_LOAD_RENDER)!=0){
            err += "[font] FT_Load_Char fail\n";
            continue;
        }
        FT_GlyphSlot slot = face->glyph;
        Glyph& g = glyphs[ch];
        g.width = slot->bitmap.width;
        g.height = slot->bitmap.rows;
        g.bearingX = slot->bitmap_left;
        g.bearingY = slot->bitmap_top;
        g.advance  = slot->advance.x >> 6;
        g.bitmap.assign(slot->bitmap.buffer, slot->bitmap.buffer + g.width*g.height);
    }
    FT_Done_Face(face);
    loaded = true;
    return true;
}

void Font::draw(Canvas& canvas, const std::string& text, int x, int y, Color color) const{
    if(!loaded) return;
    int penX = x;
    int baseline = y + ascent;
    for(char ch : text){
        if(ch=='\n'){
            penX = x;
            baseline += (ascent + (-descent) + lineGap);
            continue;
        }
        const Glyph& g = glyphFor(ch);
        int gx = penX + g.bearingX;
        int gy = baseline - g.bearingY;
        for(int row=0; row<g.height; ++row){
            int ty = gy + row;
            if(ty<0 || ty>=canvas.height()) continue;
            for(int col=0; col<g.width; ++col){
                int tx = gx + col;
                if(tx<0 || tx>=canvas.width()) continue;
                uint8_t cov = g.bitmap[row * g.width + col];
                if(!cov) continue;
                canvas.plot(tx, ty, color, cov);
            }
        }
        penX += g.advance;
    }
}

bool Font::ready() const { return loaded; }

int Font::textWidth(const std::string& text) const{
    if(!loaded) return 0;
    int penX = 0;
    for(char ch : text){
        if(ch=='\n') break;
        penX += glyphFor(ch).advance;
    }
    return penX;
}

const Font::Glyph& Font::glyphFor(char ch) const{
    if(ch<32 || ch>=128) ch='?';
    return glyphs[static_cast<size_t>(ch-32)];
}

bool Font::readFile(const std::string& path, std::vector<uint8_t>& out){
    FILE* f = fopen(path.c_str(), "rb");
    if(!f) return false;
    fseek(f,0,SEEK_END);
    long sz = ftell(f);
    fseek(f,0,SEEK_SET);
    if(sz <= 0){ fclose(f); return false; }
    out.resize(sz);
    size_t rd = fread(out.data(),1,sz,f);
    fclose(f);
    return rd==(size_t)sz;
}

void DekoPresenter::setLogBuffer(std::string* out){ logOut = out; }

bool DekoPresenter::init(NWindow* win, uint32_t width, uint32_t height){
    nwindowSetDimensions(win, width, height);
    Result rc = framebufferCreate(&fb, win, width, height, PIXEL_FORMAT_RGBA_8888, 2);
    if(R_FAILED(rc)){
        logf("[gfx] framebufferCreate rc=0x%08X\n", rc);
        return false;
    }
    framebufferMakeLinear(&fb);
    fbW = width;
    fbH = height;
    logLine("[gfx] presenter init ok\n");
    return true;
}

void DekoPresenter::shutdown(){
    if(fbW || fbH){
        framebufferClose(&fb);
        fbW = fbH = 0;
    }
}

void DekoPresenter::present(const void* pixels, size_t numBytes){
    if(fbW==0 || fbH==0 || !pixels) return;
    size_t expected = static_cast<size_t>(fbW) * fbH * sizeof(uint32_t);
    if(numBytes < expected) return;
    u32 strideBytes = 0;
    uint8_t* dst = static_cast<uint8_t*>(framebufferBegin(&fb, &strideBytes));
    if(!dst) return;
    const uint8_t* src = static_cast<const uint8_t*>(pixels);
    size_t rowBytes = static_cast<size_t>(fbW) * sizeof(uint32_t);
    if(strideBytes == rowBytes){
        memcpy(dst, src, expected);
    }else{
        for(uint32_t y=0; y<fbH; ++y){
            memcpy(dst + static_cast<size_t>(y)*strideBytes,
                   src + static_cast<size_t>(y)*rowBytes,
                   rowBytes);
        }
    }
    framebufferEnd(&fb);
}

void DekoPresenter::logLine(const char* msg){
    if(logOut) logOut->append(msg);
}

void DekoPresenter::logf(const char* fmt, ...){
    if(!logOut) return;
    char buf[256];
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    logOut->append(buf);
}

void wrapTextLines(const std::string& text_in, int maxWidth, const Font& fontPrimary,
                   const Font& fontSmall, bool smallFont, std::vector<std::string>& out){
    out.clear();
    if(maxWidth <= 0) return;
    const Font& font = smallFont ? fontSmall : fontPrimary;
    if(!font.ready()) return;
    std::string word;
    std::string line;
    auto flushLine = [&](){
        if(line.empty()) return;
        out.push_back(line);
        line.clear();
    };
    auto appendWord = [&](const std::string& w){
        if(w.empty()) return;
        std::string candidate = line.empty() ? w : line + " " + w;
        if(font.textWidth(candidate) > maxWidth && !line.empty()){
            flushLine();
            line = w;
        }else{
            line = candidate;
        }
    };
    std::string text = text_in;
    for(size_t i=0;i<=text.size();++i){
        char c = (i<text.size()) ? text[i] : '\n';
        if(c=='\r') continue;
        if(c=='\n'){
            appendWord(word);
            word.clear();
            flushLine();
            continue;
        }
        if(isspace((unsigned char)c)){
            appendWord(word);
            word.clear();
        }else{
            word.push_back(c);
        }
    }
    appendWord(word);
    flushLine();
}

void drawWrappedText(Canvas& canvas, const std::string& text_in, int x, int y, int maxWidth,
                     Color color, const Font& fontPrimary, const Font& fontSmall, bool forceSmall){
    bool small = forceSmall ? true : fontSmall.ready();
    const Font& font = small ? fontSmall : fontPrimary;
    if(!font.ready()) return;
    std::vector<std::string> lines;
    wrapTextLines(text_in, maxWidth, fontPrimary, fontSmall, small, lines);
    const int lineH = small ? 22 : 28;
    for(const auto& line : lines){
        font.draw(canvas, line, x, y, color);
        y += lineH;
    }
}

} // namespace gfx
