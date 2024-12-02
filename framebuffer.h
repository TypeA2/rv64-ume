#ifndef FRAMEBUFFER_H
#define FRAMEBUFFER_H

#include <array>
#include <memory>
#include <stop_token>
#include <atomic>
#include <span>

#include <cstdint>

#include <SDL2/SDL.h>

/* Framebuffer compatible with the original one by Koen Putman used in rv64-emu */
enum DisplayModes {
    GFX_Y8 = 0,
    GFX_INDEXED,
    GFX_RGB332,
    GFX_RGB555,
    GFX_RGB24,
    GFX_RGBA32,
    NMODES
};

/* Some semblance of atomicity */
struct ControlInterface {
    std::atomic_uint32_t enable;
    std::atomic_uint32_t mode;
    std::atomic_uint32_t resx;
    std::atomic_uint32_t resy;
};

static constexpr uint32_t max_dim = 4096;
static constexpr uint32_t max_pixel_size = 4;
static constexpr size_t fb_max_size = max_dim * max_dim * max_pixel_size;

static constexpr uintptr_t control_addr = 0x800;
static constexpr uintptr_t palette_addr = control_addr + sizeof(ControlInterface);
static constexpr uintptr_t fb_addr = 0x1000000;

class RenderContext {
    uint32_t _mode;
    uint32_t _width;
    uint32_t _height;

    SDL_Window* _window{};
    SDL_Renderer* _renderer{};
    SDL_Texture* _texture{};

    public:
    RenderContext(uint32_t mode, uint32_t width, uint32_t height);
    ~RenderContext();

    void redraw(std::span<uint32_t, 256> palette);
};

class Framebuffer {
    /* Palette is mapped behind the ControlInterface structure */
    ControlInterface _control{};
    std::array<uint32_t, 256> _palette{};

    std::unique_ptr<RenderContext> _ctx;

    public:
    /* Return true if handled */
    bool handle_write(uintptr_t addr, uint8_t size, uint64_t val);
    bool handle_read(uintptr_t addr, uint8_t size, uint64_t& val);

    /* Entrypoint for rendering thread */
    void entry(std::stop_token stop);
};

#endif /* FRAMEBUFFER_H */
