#define _CRT_SECURE_NO_WARNINGS
#include <SDL.h>
#include <stdio.h>
#include <SDL_ttf.h>
#include <string.h>

int main(int argc, char* argv[]) {
    SDL_Init(SDL_INIT_VIDEO);

    if (TTF_Init() == -1) {
        printf("TTF_Init error: %s\n", TTF_GetError());
        return 1;
    }

    SDL_Window* window = SDL_CreateWindow("AI Terminal", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 800, 600, SDL_WINDOW_SHOWN);
    SDL_Renderer* renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);

    TTF_Font* font = TTF_OpenFont("consola.ttf", 24);
    if (!font) {
        printf("ERROR: Font not found! SDL_ttf error: %s\n", TTF_GetError());
        return 1;
    }

    SDL_Color white = { 255, 255, 255, 255 };
    char inputText[100] = "";
    int running = 1;
    SDL_Event e;

    SDL_StartTextInput();

    while (running) {
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) running = 0;

            if (e.type == SDL_TEXTINPUT) {
                if (strlen(inputText) + strlen(e.text.text) < 99) {
                    strcat(inputText, e.text.text);
                }
            }
            else if (e.type == SDL_KEYDOWN) {
                if (e.key.keysym.sym == SDLK_BACKSPACE && strlen(inputText) > 0) {
                    inputText[strlen(inputText) - 1] = '\0';
                }
                // Нажми Enter, чтобы очистить строку (для теста)
                if (e.key.keysym.sym == SDLK_RETURN) {
                    inputText[0] = '\0';
                }
            }
        }

        // --- ОТРИСОВКА ---
        SDL_SetRenderDrawColor(renderer, 20, 20, 25, 255); // Темный фон терминала
        SDL_RenderClear(renderer);

        // Рисуем текст и вычисляем положение курсора
        int cursorX = 50; // Начальная позиция X
        int cursorY = 50; // Начальная позиция Y

        if (strlen(inputText) > 0) {
            SDL_Surface* tempSurf = TTF_RenderText_Solid(font, inputText, white);
            if (tempSurf) {
                SDL_Texture* tempTex = SDL_CreateTextureFromSurface(renderer, tempSurf);

                SDL_Rect textRect = { 50, 50, tempSurf->w, tempSurf->h };
                SDL_RenderCopy(renderer, tempTex, NULL, &textRect);

                // Ставим курсор СРАЗУ ПОСЛЕ ширины текста
                cursorX = 50 + tempSurf->w;

                SDL_FreeSurface(tempSurf);
                SDL_DestroyTexture(tempTex);
            }
        }

        // --- РИСУЕМ МИГАЮЩИЙ КУРСОР ---
        Uint32 ticks = SDL_GetTicks();
        if ((ticks % 1000) < 500) {
            // Рисуем курсор там, где закончился текст
            SDL_Rect cursorRect = { cursorX + 2, cursorY, 2, 30 };
            SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
            SDL_RenderFillRect(renderer, &cursorRect);
        }

        SDL_RenderPresent(renderer);
        SDL_Delay(8); // Чуть увеличил задержку, чтобы не грузить процессор
    }

    TTF_CloseFont(font);
    TTF_Quit();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();

    return 0;
}