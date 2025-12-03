#ifndef FILTER_H
#define FILTER_H

#include <Arduino.h>
#include <esp_camera.h>


// Helper functions
int colorDistance(uint8_t r1, uint8_t g1, uint8_t b1, uint8_t r2, uint8_t g2, uint8_t b2);
uint16_t *createSmallDitheredImage(camera_fb_t *cameraFb);

// Main filter functions
void applyDithering(camera_fb_t *cameraFb, int redBits = 1, int greenBits = 1, int blueBits = 1, bool grayscale = false, int algorithm = 0, int bayerSize = 4);
void applyPixelate(camera_fb_t *cameraFb, int blockSize = 8, bool grayscale = false);
void applyColorPalette(uint16_t *imageBuffer, int width, int height, const uint32_t *palette, int paletteSize, int dithering = 1, int pixelSize = 1, int bayerSize = 4);
void reduceResolution(camera_fb_t *cameraFb, int targetWidth, int targetHeight);
void applyColorReduction(camera_fb_t *cameraFb);
void applyEdgeDetection(camera_fb_t *cameraFb, int mode = 1);
void applyAutoAdjust(camera_fb_t *cameraFb);

#endif // FILTER_H