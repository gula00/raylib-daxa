/**********************************************************************************************
*
*   raylib Daxa smoke test
*
*   This test verifies that a Daxa-configured raylib build can create a window,
*   draw a few frames through rlgl's Daxa native batch path, and successfully
*   present them through the Daxa swapchain.
*
**********************************************************************************************/

#include "raylib.h"
#include "rdaxa.h"

#if !defined(GRAPHICS_API_DAXA)
    #error "daxa_smoke must be built with GRAPHICS_API_DAXA"
#endif

int main(void)
{
    SetTraceLogLevel(LOG_WARNING);
    InitWindow(160, 120, "raylib daxa smoke");

    if (!IsWindowReady())
    {
        CloseWindow();
        return 1;
    }

    for (int frame = 0; frame < 8; frame++)
    {
        BeginDrawing();
            ClearBackground((Color){ 20, 28, 36, 255 });
            DrawRectangle(24, 24, 112, 72, (Color){ 0, 228, 140, 255 });
            DrawPixel(80, 60, RAYWHITE);
        EndDrawing();
    }

    unsigned int presentedFrames = rdaxaGetPresentedFrameCount();
    CloseWindow();

    return (presentedFrames > 0)? 0 : 2;
}
