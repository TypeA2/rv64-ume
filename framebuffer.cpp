#include "framebuffer.h"

#include <SDL2/SDL_render.h>
#include <iostream>
#include <thread>

#include "util.h"

static constexpr SDL_PixelFormatEnum gfx_to_sdl_mode[DisplayModes::NMODES] {
    SDL_PIXELFORMAT_RGBA8888,
    SDL_PIXELFORMAT_RGBA8888,
    SDL_PIXELFORMAT_RGB332,
    SDL_PIXELFORMAT_RGB555,
    SDL_PIXELFORMAT_RGB24,
    SDL_PIXELFORMAT_RGBA8888
};

static constexpr uint8_t bytes_per_pixel[DisplayModes::NMODES] {
    1, 1, 1, 2, 3, 4,
};

RenderContext::RenderContext(uint32_t mode, uint32_t width, uint32_t height)
    : _mode { mode }, _width { width }, _height { height } {
    if (SDL_CreateWindowAndRenderer(_width, _height,
                                    0 , &_window, &_renderer) != 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                   "Couldn't create window/renderer: %s", SDL_GetError());
        std::exit(ExitCodes::FramebufferError);
    }

    SDL_SetWindowTitle(_window, "rv64-ume");
    _texture = SDL_CreateTexture(_renderer, gfx_to_sdl_mode[_mode],
                                SDL_TEXTUREACCESS_STREAMING,
                                _width, _height);

    if (!_texture) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                   "Couldn't create texture: %s", SDL_GetError());
        std::exit(ExitCodes::FramebufferError);
    }
}

RenderContext::~RenderContext() {
    if (_texture)  SDL_DestroyTexture(_texture);
    if (_renderer) SDL_DestroyRenderer(_renderer);
    if (_window)   SDL_DestroyWindow(_window);
}

void RenderContext::redraw(std::span<uint32_t, 256> palette) {
    switch (_mode) {
        case GFX_RGB332:
        case GFX_RGB555:
        case GFX_RGB24:
        case GFX_RGBA32:
            SDL_UpdateTexture(_texture, NULL,
                              reinterpret_cast<char*>(fb_addr), _width * bytes_per_pixel[_mode]);
            break;

        case GFX_Y8:
        case GFX_INDEXED: {
            /* These two are implemented using RGBA8888 */
            uint8_t* pixels;
            int pitch;
            SDL_LockTexture(_texture, nullptr, reinterpret_cast<void**>(&pixels), &pitch);

            for (uint32_t y = 0; y < _height; ++y) {
                for (uint32_t x = 0; x < _width; ++x) {
                    uint32_t* pixel = reinterpret_cast<uint32_t*>(&pixels[pitch * y + x * sizeof(uint32_t)]);

                    uint32_t raw = reinterpret_cast<uint8_t*>(fb_addr)[y * _width + x];
                    if (_mode == GFX_Y8) {
                        *pixel = (raw << 24) | (raw << 16) | (raw << 8) | 0xFF;
                    } else {
                        *pixel = palette[raw];
                    }
                }
            }

            SDL_UnlockTexture(_texture);
        }
    }

    SDL_RenderCopy(_renderer, _texture, 0, 0);
    SDL_RenderPresent(_renderer);
}

bool Framebuffer::handle_write(uintptr_t addr, uint8_t size, uint64_t val) {
    if (addr >= control_addr && (addr + size) <= (control_addr + sizeof(_control) + sizeof(_palette))) {
        /* The original implementation allows probing with size=0, but that's impossible on real hardware */
        if (size != sizeof(uint32_t) || (addr % sizeof(uint32_t) != 0)) {
            crash_and_burn("Only aligned 4-byte access allowed");
        }

        val &= 0xFFFFFFFF;

        uintptr_t offset = addr - control_addr;
        switch (offset) {
            case 0x0:
                _control.enable = val;
                /* Optionally wait for window to open
                 * Leaving this disabled means the final execution time is not influenced
                 * by GUI stuff. This can be used to see how i.e. hardware float is faster
                 */
                // while (!_ctx);
                break;

            case 0x4: _control.mode   = val; break;
            case 0x8: _control.resx   = val; break;
            case 0xc: _control.resy   = val; break;
            default: {
                /* Write into pallette */
                size_t idx = (offset - sizeof(_control)) >> 2;
                _palette[idx] = val;
            }
        }

        return true;
    }

    /* Not handled */
    return false;
}

bool Framebuffer::handle_read(uintptr_t addr, uint8_t size, uint64_t& val) {
    if (addr >= control_addr && (addr + size) <= (control_addr + sizeof(_control) + sizeof(_palette))) {
        /* The original implementation allows probing with size=0, but that's impossible on real hardware */
        if (size != sizeof(uint32_t) || (addr % sizeof(uint32_t) != 0)) {
            crash_and_burn("Only aligned 4-byte access allowed");
        }

        uintptr_t offset = addr - control_addr;
        switch (offset) {
            case 0x0: val = _control.enable; break;
            case 0x4: val = _control.mode;   break;
            case 0x8: val = _control.resx;   break;
            case 0xc: val = _control.resy;   break;
            default: {
                /* Read from pallette */
                size_t idx = (offset - sizeof(_control)) >> 2;
                val = _palette[idx];
            }
        }

        return true;
    }
    
    /* Not handled */
    return false;
}

void Framebuffer::entry(std::stop_token stop) {
    /* Just spinlock until the window is enabled */
    while (_control.enable == 0) {
        if (stop.stop_requested()) {
            return;
        }
    }

    _ctx = std::make_unique<RenderContext>(_control.mode, _control.resx, _control.resy);

    /* If a stop is requested, wait until the window is closed */
    while (_ctx) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            switch (event.type) {
                case SDL_KEYUP:
                    switch (event.key.keysym.sym) {
                        case SDLK_ESCAPE:
                        case SDLK_q:
                            _control.enable = 0;
                            _ctx.reset();

                            if (stop.stop_requested()) {
                                return;
                            }

                            break;
                    }
                    break;
            }
        }

        if (_ctx) {
            _ctx->redraw(_palette);
            /* Constant redraws aren't really necessary */
            // SDL_Delay(20);
        }
    }
}
