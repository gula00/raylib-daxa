/**********************************************************************************************
*
*   rdaxa - Experimental Daxa presentation backend for raylib software rendering
*
**********************************************************************************************/

#ifndef RDAXA_H
#define RDAXA_H

#include <stdbool.h>

#if defined(_WIN32)
typedef struct HWND__ *rdaxa_WindowHandle;
#else
typedef void *rdaxa_WindowHandle;
#endif

typedef struct rdaxa_DrawCall {
    int mode;
    int vertexCount;
    int vertexAlignment;
    unsigned int textureId;
} rdaxa_DrawCall;

typedef struct rdaxa_BatchData {
    const float *vertices;
    const float *texcoords;
    const unsigned char *colors;
    int vertexCounter;
    const rdaxa_DrawCall *draws;
    int drawCounter;
    const float *mvp;
    int framebufferWidth;
    int framebufferHeight;
} rdaxa_BatchData;

#ifdef __cplusplus
extern "C" {
#endif

bool rdaxaInit(rdaxa_WindowHandle window, int width, int height);
void rdaxaShutdown(void);
void rdaxaResize(int width, int height);
void rdaxaSetClearColor(unsigned char r, unsigned char g, unsigned char b, unsigned char a);
bool rdaxaDrawBatch(const rdaxa_BatchData *batch);
bool rdaxaPresent(const void *rgbaPixels, int width, int height);
unsigned int rdaxaGetPresentedFrameCount(void);

#ifdef __cplusplus
}
#endif

#endif // RDAXA_H
