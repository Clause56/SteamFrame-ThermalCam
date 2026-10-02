#include "display.h"

#ifdef HAVE_SDL2
#include <SDL.h>

namespace {

class DesktopDisplay : public Display {
public:
    ~DesktopDisplay() override {
        if (tex_) SDL_DestroyTexture(tex_);
        if (ren_) SDL_DestroyRenderer(ren_);
        if (win_) SDL_DestroyWindow(win_);
        SDL_Quit();
    }

    bool init(std::string& err) override {
        if (SDL_Init(SDL_INIT_VIDEO) != 0) {
            err = SDL_GetError();
            return false;
        }
        win_ = SDL_CreateWindow("Thermal Viewer", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 768, 576,
                                SDL_WINDOW_RESIZABLE);
        if (!win_) {
            err = SDL_GetError();
            return false;
        }
        ren_ = SDL_CreateRenderer(win_, -1, SDL_RENDERER_PRESENTVSYNC);
        if (!ren_) ren_ = SDL_CreateRenderer(win_, -1, SDL_RENDERER_SOFTWARE);
        if (!ren_) {
            err = SDL_GetError();
            return false;
        }
        SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");
        return true;
    }

    void present(const thermal::RgbaImage& img) override {
        if (!tex_ || img.width != tw_ || img.height != th_) {
            if (tex_) SDL_DestroyTexture(tex_);
            tex_ = SDL_CreateTexture(ren_, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING, img.width, img.height);
            tw_ = img.width;
            th_ = img.height;
            SDL_RenderSetLogicalSize(ren_, tw_, th_);
        }
        SDL_UpdateTexture(tex_, nullptr, img.px.data(), img.width * 4);
        SDL_RenderClear(ren_);
        SDL_RenderCopy(ren_, tex_, nullptr, nullptr);
        SDL_RenderPresent(ren_);
    }

    DisplayEvents poll() override {
        DisplayEvents ev;
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) ev.quit = true;
            if (e.type != SDL_KEYDOWN) continue;
            switch (e.key.keysym.sym) {
                case SDLK_q:
                case SDLK_ESCAPE: ev.quit = true; break;
                case SDLK_p: ev.nextPalette = true; break;
                case SDLK_g: ev.nextGain = true; break;
                case SDLK_s: ev.snapshot = true; break;
                case SDLK_u: ev.toggleUnits = true; break;
                case SDLK_h: ev.toggleHud = true; break;
                case SDLK_r: ev.rotate = true; break;
                case SDLK_f: {
                    bool fs = SDL_GetWindowFlags(win_) & SDL_WINDOW_FULLSCREEN_DESKTOP;
                    SDL_SetWindowFullscreen(win_, fs ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
                    break;
                }
                default: break;
            }
        }
        return ev;
    }

    const char* name() const override { return "desktop"; }

private:
    SDL_Window* win_ = nullptr;
    SDL_Renderer* ren_ = nullptr;
    SDL_Texture* tex_ = nullptr;
    int tw_ = 0, th_ = 0;
};

}  // namespace

std::unique_ptr<Display> makeDesktopDisplay() { return std::make_unique<DesktopDisplay>(); }
#else
std::unique_ptr<Display> makeDesktopDisplay() { return nullptr; }
#endif
