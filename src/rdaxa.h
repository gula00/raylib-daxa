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

#ifdef __cplusplus
extern "C" {
#endif

bool rdaxaInit(rdaxa_WindowHandle window, int width, int height);
void rdaxaShutdown(void);
void rdaxaResize(int width, int height);
bool rdaxaPresent(const void *rgbaPixels, int width, int height);
unsigned int rdaxaGetPresentedFrameCount(void);

#ifdef __cplusplus
}
#endif

#endif // RDAXA_H
