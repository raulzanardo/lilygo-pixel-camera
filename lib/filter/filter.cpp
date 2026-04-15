#include "filter.h"
#include <esp_heap_caps.h>

namespace
{
    uint16_t *g_multiExposureFrames = nullptr;
    size_t g_multiExposurePixelCount = 0;
    int g_multiExposureSlotCount = 0;
    int g_multiExposureStoredFrames = 0;
    int g_multiExposureNextSlot = 0;

    void releaseMultiExposureBuffer()
    {
        if (g_multiExposureFrames)
        {
            heap_caps_free(g_multiExposureFrames);
            g_multiExposureFrames = nullptr;
        }
        g_multiExposurePixelCount = 0;
        g_multiExposureSlotCount = 0;
        g_multiExposureStoredFrames = 0;
        g_multiExposureNextSlot = 0;
    }

    bool ensureMultiExposureBuffer(size_t pixelCount, int frameCount)
    {
        if (pixelCount == 0 || frameCount < 2)
        {
            releaseMultiExposureBuffer();
            return false;
        }

        if (g_multiExposureFrames && g_multiExposurePixelCount == pixelCount && g_multiExposureSlotCount == frameCount)
        {
            return true;
        }

        releaseMultiExposureBuffer();

        size_t bytes = pixelCount * static_cast<size_t>(frameCount) * sizeof(uint16_t);
        g_multiExposureFrames = static_cast<uint16_t *>(heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!g_multiExposureFrames)
        {
            g_multiExposureFrames = static_cast<uint16_t *>(heap_caps_malloc(bytes, MALLOC_CAP_DEFAULT | MALLOC_CAP_8BIT));
        }
        if (!g_multiExposureFrames)
        {
            return false;
        }

        g_multiExposurePixelCount = pixelCount;
        g_multiExposureSlotCount = frameCount;
        memset(g_multiExposureFrames, 0, bytes);
        return true;
    }
} // namespace

//////////////////////////////////////////////////////////////////////////////////////////

/**
 * Apply dithering directly to camera frame buffer
 *
 * @param cameraFb Pointer to camera frame buffer
 * @param redBits Number of bits for red channel
 * @param greenBits Number of bits for green channel
 * @param blueBits Number of bits for blue channel
 * @param grayscale Whether to convert to grayscale
 * @param algorithm Dithering algorithm: 0 = Floyd-Steinberg, 1 = Bayer
 * @param bayerSize Bayer matrix size (2, 4, or 8) - only used when algorithm = 1
 */
void applyDithering(camera_fb_t *cameraFb, int redBits, int greenBits, int blueBits, bool grayscale, int algorithm, int bayerSize)
{
    if (!psramFound() || !cameraFb)
        return;

    int width = cameraFb->width;
    int height = cameraFb->height;
    uint16_t *frameBuffer = (uint16_t *)cameraFb->buf;

    if (grayscale)
    {
        int minBits = min(min(redBits, greenBits), blueBits);
        redBits = greenBits = blueBits = minBits;
    }

    int redMax = (1 << redBits) - 1;
    int greenMax = (1 << greenBits) - 1;
    int blueMax = (1 << blueBits) - 1;

    // Precompute per-channel quantization LUTs (256 entries each).
    // Maps any 8-bit value to its nearest quantized output at the target bit depth.
    // Replaces per-pixel float division + round() calls with a single array lookup.
    uint8_t rQuantLUT[256], gQuantLUT[256], bQuantLUT[256];
    for (int v = 0; v < 256; v++)
    {
        rQuantLUT[v] = (uint8_t)(((v * redMax + 127) / 255) * 255 / redMax);
        gQuantLUT[v] = (uint8_t)(((v * greenMax + 127) / 255) * 255 / greenMax);
        bQuantLUT[v] = (uint8_t)(((v * blueMax + 127) / 255) * 255 / blueMax);
    }

    // GC0308 outputs RGB565 little-endian frames, so byte-swap on every read/write.
    const bool swapBytes = true;

    const int bayer2x2[2][2] = {
        {0, 2},
        {3, 1}};
    const int bayer4x4[4][4] = {
        {0, 8, 2, 10},
        {12, 4, 14, 6},
        {3, 11, 1, 9},
        {15, 7, 13, 5}};
    const int bayer8x8[8][8] = {
        {0, 32, 8, 40, 2, 34, 10, 42},
        {48, 16, 56, 24, 50, 18, 58, 26},
        {12, 44, 4, 36, 14, 46, 6, 38},
        {60, 28, 52, 20, 62, 30, 54, 22},
        {3, 35, 11, 43, 1, 33, 9, 41},
        {51, 19, 59, 27, 49, 17, 57, 25},
        {15, 47, 7, 39, 13, 45, 5, 37},
        {63, 31, 55, 23, 61, 29, 53, 21}};

    if (bayerSize != 2 && bayerSize != 4 && bayerSize != 8)
        bayerSize = 4;
    int bayerDivisor = (bayerSize == 2) ? 4 : (bayerSize == 4) ? 16
                                                               : 64;

    if (algorithm == 0)
    {
        // Floyd-Steinberg: int16_t error buffers replace 3× float buffers + 3× uint8 channel
        // buffers + 1× uint16 output buffer — saving ~11 bytes/pixel for HQVGA (~464 KB).
        // Error values stay well within int16_t range because pixel values are clamped to
        // [0, 255] before each quantization step.
        int16_t *redErr = (int16_t *)ps_malloc(width * height * sizeof(int16_t));
        int16_t *greenErr = (int16_t *)ps_malloc(width * height * sizeof(int16_t));
        int16_t *blueErr = (int16_t *)ps_malloc(width * height * sizeof(int16_t));

        if (!redErr || !greenErr || !blueErr)
        {
            free(redErr);
            free(greenErr);
            free(blueErr);
            return;
        }

        // Initialize error buffers from frame pixels
        for (int i = 0; i < width * height; i++)
        {
            uint16_t px = frameBuffer[i];
            if (swapBytes)
                px = (uint16_t)((px << 8) | (px >> 8));
            int16_t r = (int16_t)(((px >> 11) & 0x1F) << 3);
            int16_t g = (int16_t)(((px >> 5) & 0x3F) << 2);
            int16_t b = (int16_t)((px & 0x1F) << 3);
            if (grayscale)
            {
                int16_t gray = (int16_t)((r * 30 + g * 59 + b * 11) / 100);
                r = g = b = gray;
            }
            redErr[i] = r;
            greenErr[i] = g;
            blueErr[i] = b;
        }

        // Serpentine Floyd-Steinberg scan with fixed-point integer error distribution.
        // Fractions 7/16, 3/16, 5/16, 1/16 computed via bit-shift; residual absorbed
        // by the 1/16 neighbor to prevent cumulative drift.
        for (int y = 0; y < height; y++)
        {
            bool leftToRight = (y % 2 == 0);
            int xStart = leftToRight ? 0 : width - 1;
            int xEnd = leftToRight ? width : -1;
            int xStep = leftToRight ? 1 : -1;

            for (int x = xStart; x != xEnd; x += xStep)
            {
                int idx = y * width + x;

                int16_t oldR = (int16_t)constrain((int)redErr[idx], 0, 255);
                int16_t oldG = (int16_t)constrain((int)greenErr[idx], 0, 255);
                int16_t oldB = (int16_t)constrain((int)blueErr[idx], 0, 255);

                int16_t newR = (int16_t)rQuantLUT[oldR];
                int16_t newG = (int16_t)gQuantLUT[oldG];
                int16_t newB = (int16_t)bQuantLUT[oldB];

                // Write quantized pixel directly to frameBuffer — no outputBuffer needed
                uint16_t color = ((uint16_t)((uint8_t)newR >> 3) << 11) |
                                 ((uint16_t)((uint8_t)newG >> 2) << 5) |
                                 (uint16_t)((uint8_t)newB >> 3);
                if (swapBytes)
                    color = (uint16_t)((color << 8) | (color >> 8));
                frameBuffer[idx] = color;

                int16_t eR = (int16_t)(oldR - newR);
                int16_t eG = (int16_t)(oldG - newG);
                int16_t eB = (int16_t)(oldB - newB);

                int16_t eR7 = (int16_t)((eR * 7) >> 4), eR3 = (int16_t)((eR * 3) >> 4),
                        eR5 = (int16_t)((eR * 5) >> 4), eR1 = (int16_t)(eR - eR7 - eR3 - eR5);
                int16_t eG7 = (int16_t)((eG * 7) >> 4), eG3 = (int16_t)((eG * 3) >> 4),
                        eG5 = (int16_t)((eG * 5) >> 4), eG1 = (int16_t)(eG - eG7 - eG3 - eG5);
                int16_t eB7 = (int16_t)((eB * 7) >> 4), eB3 = (int16_t)((eB * 3) >> 4),
                        eB5 = (int16_t)((eB * 5) >> 4), eB1 = (int16_t)(eB - eB7 - eB3 - eB5);

                if (leftToRight)
                {
                    if (x + 1 < width)
                    {
                        redErr[idx + 1] += eR7;
                        greenErr[idx + 1] += eG7;
                        blueErr[idx + 1] += eB7;
                    }
                    if (y + 1 < height)
                    {
                        int nr = (y + 1) * width;
                        if (x - 1 >= 0)
                        {
                            redErr[nr + x - 1] += eR3;
                            greenErr[nr + x - 1] += eG3;
                            blueErr[nr + x - 1] += eB3;
                        }
                        redErr[nr + x] += eR5;
                        greenErr[nr + x] += eG5;
                        blueErr[nr + x] += eB5;
                        if (x + 1 < width)
                        {
                            redErr[nr + x + 1] += eR1;
                            greenErr[nr + x + 1] += eG1;
                            blueErr[nr + x + 1] += eB1;
                        }
                    }
                }
                else
                {
                    if (x - 1 >= 0)
                    {
                        redErr[idx - 1] += eR7;
                        greenErr[idx - 1] += eG7;
                        blueErr[idx - 1] += eB7;
                    }
                    if (y + 1 < height)
                    {
                        int nr = (y + 1) * width;
                        if (x + 1 < width)
                        {
                            redErr[nr + x + 1] += eR3;
                            greenErr[nr + x + 1] += eG3;
                            blueErr[nr + x + 1] += eB3;
                        }
                        redErr[nr + x] += eR5;
                        greenErr[nr + x] += eG5;
                        blueErr[nr + x] += eB5;
                        if (x - 1 >= 0)
                        {
                            redErr[nr + x - 1] += eR1;
                            greenErr[nr + x - 1] += eG1;
                            blueErr[nr + x - 1] += eB1;
                        }
                    }
                }
            }
        }

        free(redErr);
        free(greenErr);
        free(blueErr);
    }
    else if (algorithm == 1)
    {
        // Bayer: precompute integer offset table once (same approach as applyColorPalette).
        // Eliminates per-pixel float threshold calculation and branch-on-matrix-size.
        // Offset = (bv * 255 / divisor) - 127, centering the range around 0.
        int bayerIntOffsets[8][8] = {};
        for (int by = 0; by < bayerSize; by++)
            for (int bx = 0; bx < bayerSize; bx++)
            {
                int bv = (bayerSize == 2)   ? bayer2x2[by][bx]
                         : (bayerSize == 4) ? bayer4x4[by][bx]
                                            : bayer8x8[by][bx];
                bayerIntOffsets[by][bx] = (bv * 255 / bayerDivisor) - 127;
            }

        // Process in-place directly on frameBuffer — no output buffer or channel buffers needed
        for (int y = 0; y < height; y++)
        {
            for (int x = 0; x < width; x++)
            {
                int idx = y * width + x;
                uint16_t px = frameBuffer[idx];
                if (swapBytes)
                    px = (uint16_t)((px << 8) | (px >> 8));
                int r = ((px >> 11) & 0x1F) << 3;
                int g = ((px >> 5) & 0x3F) << 2;
                int b = (px & 0x1F) << 3;
                if (grayscale)
                {
                    int gray = (r * 30 + g * 59 + b * 11) / 100;
                    r = g = b = gray;
                }

                int offset = bayerIntOffsets[y % bayerSize][x % bayerSize];
                r = constrain(r + offset, 0, 255);
                g = constrain(g + offset, 0, 255);
                b = constrain(b + offset, 0, 255);

                uint16_t color = ((uint16_t)(rQuantLUT[r] >> 3) << 11) |
                                 ((uint16_t)(gQuantLUT[g] >> 2) << 5) |
                                 (uint16_t)(bQuantLUT[b] >> 3);
                if (swapBytes)
                    color = (uint16_t)((color << 8) | (color >> 8));
                frameBuffer[idx] = color;
            }
        }
    }
}

//////////////////////////////////////////////////////////////////////////////////////////
/**
 * Apply pixelation filter directly to camera frame buffer
 *
 * @param cameraFb Pointer to camera frame buffer
 * @param width Image width
 * @param height Image height
 * @param blockSize Size of pixelation blocks
 * @param grayscale Whether to convert to grayscale
 */
void applyPixelate(camera_fb_t *cameraFb, int blockSize, bool grayscale)
{
    if (!psramFound() || !cameraFb)
    {
        return;
    }

    int width = cameraFb->width;
    int height = cameraFb->height;
    uint16_t *frameBuffer = (uint16_t *)cameraFb->buf;

    // GC0308 outputs RGB565 little-endian frames, so no byte swapping is required.
    const bool swapBytes = true;

    // Allocate memory using PSRAM for output buffer
    uint16_t *outputBuffer = (uint16_t *)ps_malloc(width * height * sizeof(uint16_t));

    if (!outputBuffer)
    {
        return;
    }

    // Process image in blocks
    for (int blockY = 0; blockY < height; blockY += blockSize)
    {
        for (int blockX = 0; blockX < width; blockX += blockSize)
        {
            // Calculate block boundaries
            int blockEndY = min(blockY + blockSize, height);
            int blockEndX = min(blockX + blockSize, width);

            // Calculate average color for this block
            long sumR = 0, sumG = 0, sumB = 0;
            int count = 0;

            for (int y = blockY; y < blockEndY; y++)
            {
                for (int x = blockX; x < blockEndX; x++)
                {
                    int idx = y * width + x;
                    uint16_t pixel = frameBuffer[idx];

                    if (swapBytes)
                    {
                        pixel = ((pixel << 8) | (pixel >> 8));
                    }

                    // Extract RGB components from RGB565 format
                    uint8_t r = ((pixel >> 11) & 0x1F) << 3; // 5 bits to 8 bits
                    uint8_t g = ((pixel >> 5) & 0x3F) << 2;  // 6 bits to 8 bits
                    uint8_t b = (pixel & 0x1F) << 3;         // 5 bits to 8 bits

                    sumR += r;
                    sumG += g;
                    sumB += b;
                    count++;
                }
            }

            // Calculate average color
            uint8_t avgR = sumR / count;
            uint8_t avgG = sumG / count;
            uint8_t avgB = sumB / count;

            if (grayscale)
            {
                // Convert to grayscale using standard luminance formula
                uint8_t gray = (avgR * 30 + avgG * 59 + avgB * 11) / 100;
                avgR = avgG = avgB = gray;
            }

            // Convert back to RGB565 format
            uint8_t r5 = avgR >> 3; // Convert 8-bit to 5-bit (for red)
            uint8_t g6 = avgG >> 2; // Convert 8-bit to 6-bit (for green)
            uint8_t b5 = avgB >> 3; // Convert 8-bit to 5-bit (for blue)

            uint16_t avgPixel = (r5 << 11) | (g6 << 5) | b5;

            if (swapBytes)
            {
                avgPixel = ((avgPixel << 8) | (avgPixel >> 8));
            }

            // Fill the entire block with the average color
            for (int y = blockY; y < blockEndY; y++)
            {
                for (int x = blockX; x < blockEndX; x++)
                {
                    int idx = y * width + x;
                    outputBuffer[idx] = avgPixel;
                }
            }
        }
    }

    // Copy the processed image back to the camera frame buffer
    memcpy(frameBuffer, outputBuffer, width * height * sizeof(uint16_t));

    // Free memory
    free(outputBuffer);
}

//////////////////////////////////////////////////////////////////////////////////////////

//////////////////////////////////////////////////////////////////////////////////////////
// Helper function to calculate color distance
int colorDistance(uint8_t r1, uint8_t g1, uint8_t b1, uint8_t r2, uint8_t g2, uint8_t b2)
{
    // Use weighted Euclidean distance (perceptual color difference)
    int dr = r1 - r2;
    int dg = g1 - g2;
    int db = b1 - b2;
    return dr * dr * 2 + dg * dg * 4 + db * db * 3;
}

//////////////////////////////////////////////////////////////////////////////////////////
/**
 * Apply color palette with optional dithering
 *
 * @param imageBuffer Pointer to image buffer
 * @param width Image width
 * @param height Image height
 * @param palette Pointer to palette array
 * @param paletteSize Number of colors in palette
 * @param dithering Dithering algorithm: 0=OFF, 1=Floyd-Steinberg, 2=Bayer, 3=Sierra Lite, 4=Atkinson
 * @param pixelSize Pixelation size (1 = no pixelation)
 * @param bayerSize Bayer matrix size (2, 4, or 8) - only used when dithering = 2
 */
void applyColorPalette(uint16_t *imageBuffer, int width, int height, const uint32_t *palette, int paletteSize, int dithering, int pixelSize, int bayerSize, bool autoLevels)
{

    if (!psramFound())
    {
        return;
    }

    // GC0308 outputs RGB565 little-endian frames, so no byte swapping is required.
    const bool swapBytes = true;
    const int origWidth = width;
    const int origHeight = height;
    const int downscale = (pixelSize == 2 || pixelSize == 4 || pixelSize == 8) ? pixelSize : 1;

    // Bayer matrix definitions
    const int bayer2x2[2][2] = {
        {0, 2},
        {3, 1}};

    const int bayer4x4[4][4] = {
        {0, 8, 2, 10},
        {12, 4, 14, 6},
        {3, 11, 1, 9},
        {15, 7, 13, 5}};

    const int bayer8x8[8][8] = {
        {0, 32, 8, 40, 2, 34, 10, 42},
        {48, 16, 56, 24, 50, 18, 58, 26},
        {12, 44, 4, 36, 14, 46, 6, 38},
        {60, 28, 52, 20, 62, 30, 54, 22},
        {3, 35, 11, 43, 1, 33, 9, 41},
        {51, 19, 59, 27, 49, 17, 57, 25},
        {15, 47, 7, 39, 13, 45, 5, 37},
        {63, 31, 55, 23, 61, 29, 53, 21}};

    // Clamp bayerSize to valid values
    if (bayerSize != 2 && bayerSize != 4 && bayerSize != 8)
    {
        bayerSize = 4; // Default to 4x4
    }

    int bayerDivisor = (bayerSize == 2) ? 4 : (bayerSize == 4) ? 16
                                                               : 64;

    // Downscale if requested (2x2, 4x4, 8x8)
    uint16_t *workingBuffer = imageBuffer;
    int workWidth = width;
    int workHeight = height;
    bool usedDownscale = false;
    uint16_t *downscaledBuffer = nullptr;

    if (downscale > 1)
    {
        workWidth = (width + downscale - 1) / downscale;
        workHeight = (height + downscale - 1) / downscale;
        downscaledBuffer = (uint16_t *)ps_malloc(workWidth * workHeight * sizeof(uint16_t));
        if (!downscaledBuffer)
        {
            return;
        }

        for (int by = 0; by < height; by += downscale)
        {
            for (int bx = 0; bx < width; bx += downscale)
            {
                uint32_t sumR = 0, sumG = 0, sumB = 0;
                int count = 0;
                for (int dy = 0; dy < downscale && (by + dy) < height; ++dy)
                {
                    for (int dx = 0; dx < downscale && (bx + dx) < width; ++dx)
                    {
                        int srcIdx = (by + dy) * width + (bx + dx);
                        uint16_t px = imageBuffer[srcIdx];
                        if (swapBytes)
                        {
                            px = ((px << 8) | (px >> 8));
                        }
                        uint8_t r = ((px >> 11) & 0x1F) << 3;
                        uint8_t g = ((px >> 5) & 0x3F) << 2;
                        uint8_t b = (px & 0x1F) << 3;
                        sumR += r;
                        sumG += g;
                        sumB += b;
                        ++count;
                    }
                }

                uint8_t avgR = count ? (sumR / count) : 0;
                uint8_t avgG = count ? (sumG / count) : 0;
                uint8_t avgB = count ? (sumB / count) : 0;

                uint16_t avgPixel = ((avgR >> 3) << 11) | ((avgG >> 2) << 5) | (avgB >> 3);
                if (swapBytes)
                {
                    avgPixel = ((avgPixel << 8) | (avgPixel >> 8));
                }

                int dstIdx = (by / downscale) * workWidth + (bx / downscale);
                downscaledBuffer[dstIdx] = avgPixel;
            }
        }

        workingBuffer = downscaledBuffer;
        usedDownscale = true;
        pixelSize = 1; // internal processing uses native resolution
    }

    // Auto-levels: scan workingBuffer to find per-channel min/max, then stretch to 0-255
    uint8_t alMinR = 255, alMaxR = 0, alMinG = 255, alMaxG = 0, alMinB = 255, alMaxB = 0;
    if (autoLevels)
    {
        for (int i = 0; i < workWidth * workHeight; i++)
        {
            uint16_t px = workingBuffer[i];
            if (swapBytes)
                px = ((px << 8) | (px >> 8));
            uint8_t r = ((px >> 11) & 0x1F) << 3;
            uint8_t g = ((px >> 5) & 0x3F) << 2;
            uint8_t b = (px & 0x1F) << 3;
            if (r < alMinR)
                alMinR = r;
            if (r > alMaxR)
                alMaxR = r;
            if (g < alMinG)
                alMinG = g;
            if (g > alMaxG)
                alMaxG = g;
            if (b < alMinB)
                alMinB = b;
            if (b > alMaxB)
                alMaxB = b;
        }
    }

    // Palette-derived data cached across calls — only rebuilt when palette pointer/size changes.
    // This is critical: the LUT build costs ~1M operations for a 16-color palette and was
    // previously executed every frame, consuming ~40ms/frame at 240MHz.
    static const uint32_t *s_cachedPalette = nullptr;
    static int s_cachedPaletteSize = 0;
    static uint8_t s_prArr[256], s_pgArr[256], s_pbArr[256];
    static uint16_t s_palettePixels[256];
    static uint8_t *palLUT = nullptr; // persists in internal SRAM across calls

    if (palette != s_cachedPalette || paletteSize != s_cachedPaletteSize)
    {
        // Palette changed — refresh pre-extracted arrays
        for (int j = 0; j < paletteSize; j++)
        {
            s_prArr[j] = (palette[j] >> 16) & 0xFF;
            s_pgArr[j] = (palette[j] >> 8) & 0xFF;
            s_pbArr[j] = palette[j] & 0xFF;
            uint8_t r5 = s_prArr[j] >> 3;
            uint8_t g6 = s_pgArr[j] >> 2;
            uint8_t b5 = s_pbArr[j] >> 3;
            uint16_t px = (r5 << 11) | (g6 << 5) | b5;
            s_palettePixels[j] = (uint16_t)((px << 8) | (px >> 8));
        }

        // Allocate LUT in internal SRAM if not already done (or if size changed)
        if (!palLUT)
        {
            palLUT = (uint8_t *)heap_caps_malloc(65536 * sizeof(uint8_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
            if (!palLUT)
                palLUT = (uint8_t *)ps_malloc(65536 * sizeof(uint8_t));
        }

        // Rebuild the LUT for the new palette
        if (palLUT)
        {
            for (int r5 = 0; r5 < 32; r5++)
            {
                uint8_t r = (uint8_t)(r5 << 3);
                for (int g6 = 0; g6 < 64; g6++)
                {
                    uint8_t g = (uint8_t)(g6 << 2);
                    for (int b5 = 0; b5 < 32; b5++)
                    {
                        uint8_t b = (uint8_t)(b5 << 3);
                        int minDist = INT_MAX, best = 0;
                        for (int j = 0; j < paletteSize; j++)
                        {
                            int dr = r - s_prArr[j], dg = g - s_pgArr[j], db = b - s_pbArr[j];
                            int dist = dr * dr * 2 + dg * dg * 4 + db * db * 3;
                            if (dist < minDist)
                            {
                                minDist = dist;
                                best = j;
                                if (!dist)
                                    break;
                            }
                        }
                        palLUT[(r5 << 11) | (g6 << 5) | b5] = (uint8_t)best;
                    }
                }
            }
        }

        s_cachedPalette = palette;
        s_cachedPaletteSize = paletteSize;
    }

    // Use cached palette arrays
    const uint8_t *prArr = s_prArr;
    const uint8_t *pgArr = s_pgArr;
    const uint8_t *pbArr = s_pbArr;
    const uint16_t *palettePixels = s_palettePixels;

    // Mode 0 fast path: in-place O(1) per-pixel, no outputBuffer needed.
    if (dithering == 0 && palLUT)
    {
        if (pixelSize <= 1)
        {
            int total = workWidth * workHeight;
            for (int i = 0; i < total; i++)
            {
                uint16_t px = (uint16_t)((workingBuffer[i] << 8) | (workingBuffer[i] >> 8));
                uint8_t r = ((px >> 11) & 0x1F) << 3;
                uint8_t g = ((px >> 5) & 0x3F) << 2;
                uint8_t b = (px & 0x1F) << 3;
                if (autoLevels)
                {
                    if (alMaxR > alMinR)
                        r = (uint8_t)constrain((r - alMinR) * 255 / (alMaxR - alMinR), 0, 255);
                    if (alMaxG > alMinG)
                        g = (uint8_t)constrain((g - alMinG) * 255 / (alMaxG - alMinG), 0, 255);
                    if (alMaxB > alMinB)
                        b = (uint8_t)constrain((b - alMinB) * 255 / (alMaxB - alMinB), 0, 255);
                }
                workingBuffer[i] = palettePixels[palLUT[((uint16_t)(r >> 3) << 11) | ((uint16_t)(g >> 2) << 5) | (b >> 3)]];
            }
        }
        else
        {
            uint16_t *outBuf = (uint16_t *)ps_malloc(workWidth * workHeight * sizeof(uint16_t));
            if (outBuf)
            {
                for (int y = 0; y < workHeight; y++)
                {
                    int bcy = constrain((y / pixelSize) * pixelSize + pixelSize / 2, 0, workHeight - 1);
                    for (int x = 0; x < workWidth; x++)
                    {
                        int bcx = constrain((x / pixelSize) * pixelSize + pixelSize / 2, 0, workWidth - 1);
                        uint16_t raw = workingBuffer[bcy * workWidth + bcx];
                        uint16_t px = (uint16_t)((raw << 8) | (raw >> 8));
                        uint8_t r = ((px >> 11) & 0x1F) << 3;
                        uint8_t g = ((px >> 5) & 0x3F) << 2;
                        uint8_t b = (px & 0x1F) << 3;
                        if (autoLevels)
                        {
                            if (alMaxR > alMinR)
                                r = (uint8_t)constrain((r - alMinR) * 255 / (alMaxR - alMinR), 0, 255);
                            if (alMaxG > alMinG)
                                g = (uint8_t)constrain((g - alMinG) * 255 / (alMaxG - alMinG), 0, 255);
                            if (alMaxB > alMinB)
                                b = (uint8_t)constrain((b - alMinB) * 255 / (alMaxB - alMinB), 0, 255);
                        }
                        outBuf[y * workWidth + x] = palettePixels[palLUT[((uint16_t)(r >> 3) << 11) | ((uint16_t)(g >> 2) << 5) | (b >> 3)]];
                    }
                }
                memcpy(workingBuffer, outBuf, workWidth * workHeight * sizeof(uint16_t));
                free(outBuf);
            }
        }
        // palLUT is a static cache — not freed here
        if (usedDownscale)
        {
            for (int y = 0; y < origHeight; ++y)
            {
                int srcY = y / downscale;
                for (int x = 0; x < origWidth; ++x)
                    imageBuffer[y * origWidth + x] = workingBuffer[srcY * workWidth + (x / downscale)];
            }
            free(downscaledBuffer);
        }
        return;
    }

    // Allocate output buffer for modes 1-4 (or mode 0 fallback if palLUT unavailable)
    uint16_t *outputBuffer = (uint16_t *)ps_malloc(workWidth * workHeight * sizeof(uint16_t));
    int16_t *redErrorBuffer = nullptr;
    int16_t *greenErrorBuffer = nullptr;
    int16_t *blueErrorBuffer = nullptr;

    if (!outputBuffer)
    {
        // palLUT is a static cache — not freed here
        if (downscaledBuffer)
        {
            free(downscaledBuffer);
        }
        return;
    }

    // Allocate error buffers for error-diffusion dithering algorithms
    if (dithering == 1 || dithering == 3 || dithering == 4)
    {
        redErrorBuffer = (int16_t *)ps_malloc(workWidth * workHeight * sizeof(int16_t));
        greenErrorBuffer = (int16_t *)ps_malloc(workWidth * workHeight * sizeof(int16_t));
        blueErrorBuffer = (int16_t *)ps_malloc(workWidth * workHeight * sizeof(int16_t));

        if (!redErrorBuffer || !greenErrorBuffer || !blueErrorBuffer)
        {
            if (outputBuffer)
                if (redErrorBuffer)
                    free(redErrorBuffer);
            if (greenErrorBuffer)
                free(greenErrorBuffer);
            if (blueErrorBuffer)
                free(blueErrorBuffer);
            if (downscaledBuffer)
                free(downscaledBuffer);
            return;
        }
    }

    // Initialize error buffers with original pixel values (for error-diffusion dithering)
    if (dithering == 1 || dithering == 3 || dithering == 4)
    {
        for (int i = 0; i < workWidth * workHeight; i++)
        {
            uint16_t pixel = workingBuffer[i];

            if (swapBytes)
            {
                pixel = ((pixel << 8) | (pixel >> 8));
            }

            // Extract RGB components from RGB565 format
            redErrorBuffer[i] = (int16_t)(((pixel >> 11) & 0x1F) << 3);  // 5 bits to 8 bits
            greenErrorBuffer[i] = (int16_t)(((pixel >> 5) & 0x3F) << 2); // 6 bits to 8 bits
            blueErrorBuffer[i] = (int16_t)((pixel & 0x1F) << 3);         // 5 bits to 8 bits

            if (autoLevels)
            {
                if (alMaxR > alMinR)
                    redErrorBuffer[i] = (int16_t)((redErrorBuffer[i] - alMinR) * 255 / (alMaxR - alMinR));
                if (alMaxG > alMinG)
                    greenErrorBuffer[i] = (int16_t)((greenErrorBuffer[i] - alMinG) * 255 / (alMaxG - alMinG));
                if (alMaxB > alMinB)
                    blueErrorBuffer[i] = (int16_t)((blueErrorBuffer[i] - alMinB) * 255 / (alMaxB - alMinB));
            }
        }
    }

    // Precompute integer Bayer offset table (avoids float arithmetic and branch-on-matrix per pixel)
    int bayerIntOffsets[8][8] = {};
    if (dithering == 2)
    {
        // Bayer in palette mode tends to look harsher/brighter than error-diffusion.
        // Keep the ordered pattern, but reduce amplitude and add a slight dark bias.
        const int bayerStrengthPct = 70;
        const int bayerBias = -6;
        for (int by = 0; by < bayerSize; by++)
            for (int bx = 0; bx < bayerSize; bx++)
            {
                int bv = (bayerSize == 2) ? bayer2x2[by][bx] : (bayerSize == 4) ? bayer4x4[by][bx]
                                                                                : bayer8x8[by][bx];
                // Centered-bin normalization keeps ordered dithering balanced while avoiding
                // the very strong extremes at small matrices (notably 2x2).
                // Formula: ((2*bv + 1)/(2*N^2) - 0.5) * 255
                int baseOffset = (((2 * bv + 1) * 255) / (2 * bayerDivisor)) - 127;
                bayerIntOffsets[by][bx] = (baseOffset * bayerStrengthPct) / 100 + bayerBias;
            }
    }

    // Process each pixel
    for (int y = 0; y < workHeight; y++)
    {
        bool leftToRight = (y % 2 == 0);
        // Sierra Lite and Atkinson are always left-to-right (no serpentine scan)
        if (dithering == 3 || dithering == 4)
            leftToRight = true;
        int xStart = leftToRight ? 0 : workWidth - 1;
        int xEnd = leftToRight ? workWidth : -1;
        int xStep = leftToRight ? 1 : -1;

        for (int x = xStart; x != xEnd; x += xStep)
        {
            int idx = y * workWidth + x;

            // Get current pixel color
            uint8_t r, g, b;

            // Determine which pixel to sample (block center for pixelation, or current pixel)
            int sampleIdx = idx;
            if (pixelSize > 1)
            {
                int blockX = (x / pixelSize) * pixelSize + pixelSize / 2;
                int blockY = (y / pixelSize) * pixelSize + pixelSize / 2;
                blockX = constrain(blockX, 0, workWidth - 1);
                blockY = constrain(blockY, 0, workHeight - 1);
                sampleIdx = blockY * workWidth + blockX;
            }

            // Get color from appropriate source (error buffer for error-diffusion, image buffer otherwise)
            if (dithering == 1 || dithering == 3 || dithering == 4)
            {
                // Error-diffusion algorithms use error buffers
                r = (uint8_t)constrain((int)redErrorBuffer[sampleIdx], 0, 255);
                g = (uint8_t)constrain((int)greenErrorBuffer[sampleIdx], 0, 255);
                b = (uint8_t)constrain((int)blueErrorBuffer[sampleIdx], 0, 255);
            }
            else
            {
                // Bayer and no dithering use image buffer directly
                uint16_t pixel = workingBuffer[sampleIdx];

                if (swapBytes)
                {
                    pixel = ((pixel << 8) | (pixel >> 8));
                }

                // Extract RGB components from RGB565 format
                r = ((pixel >> 11) & 0x1F) << 3; // 5 bits to 8 bits
                g = ((pixel >> 5) & 0x3F) << 2;  // 6 bits to 8 bits
                b = (pixel & 0x1F) << 3;         // 5 bits to 8 bits

                if (autoLevels)
                {
                    if (alMaxR > alMinR)
                        r = constrain((int)((r - alMinR) * 255 / (alMaxR - alMinR)), 0, 255);
                    if (alMaxG > alMinG)
                        g = constrain((int)((g - alMinG) * 255 / (alMaxG - alMinG)), 0, 255);
                    if (alMaxB > alMinB)
                        b = constrain((int)((b - alMinB) * 255 / (alMaxB - alMinB)), 0, 255);
                }
            }

            // Apply Bayer threshold if using Bayer dithering
            if (dithering == 2)
            {
                // Get Bayer threshold value based on matrix size
                // Use block center coordinates for pixelation to ensure uniform blocks
                int bayerX, bayerY;
                if (pixelSize > 1)
                {
                    int blockX = (x / pixelSize) * pixelSize + pixelSize / 2;
                    int blockY = (y / pixelSize) * pixelSize + pixelSize / 2;
                    bayerX = blockX % bayerSize;
                    bayerY = blockY % bayerSize;
                }
                else
                {
                    bayerX = x % bayerSize;
                    bayerY = y % bayerSize;
                }
                // Apply precomputed integer Bayer offset (no float arithmetic)
                int bayerOffset = bayerIntOffsets[bayerY][bayerX];
                r = (uint8_t)constrain((int)r + bayerOffset, 0, 255);
                g = (uint8_t)constrain((int)g + bayerOffset, 0, 255);
                b = (uint8_t)constrain((int)b + bayerOffset, 0, 255);
            }

            // Find the closest palette color: O(1) LUT lookup, or O(paletteSize) fallback
            int closestIndex;
            if (palLUT)
            {
                closestIndex = palLUT[((uint16_t)(r >> 3) << 11) | ((uint16_t)(g >> 2) << 5) | (b >> 3)];
            }
            else
            {
                int minDistance = INT_MAX;
                closestIndex = 0;
                for (int j = 0; j < paletteSize; j++)
                {
                    int dr = (int)r - prArr[j], dg = (int)g - pgArr[j], db = (int)b - pbArr[j];
                    int distance = dr * dr * 2 + dg * dg * 4 + db * db * 3;
                    if (distance < minDistance)
                    {
                        minDistance = distance;
                        closestIndex = j;
                        if (!distance)
                            break;
                    }
                }
            }

            // Get the closest palette color using pre-extracted arrays
            uint8_t newR = prArr[closestIndex];
            uint8_t newG = pgArr[closestIndex];
            uint8_t newB = pbArr[closestIndex];

            // Calculate quantization error (for error-diffusion algorithms)
            int errorR = 0, errorG = 0, errorB = 0;
            if (dithering == 1 || dithering == 3 || dithering == 4)
            {
                errorR = (int)r - newR;
                errorG = (int)g - newG;
                errorB = (int)b - newB;
            }

            outputBuffer[idx] = palettePixels[closestIndex];

            // Distribute error to neighboring pixels (integer arithmetic)
            if (dithering == 1)
            {
                // Floyd-Steinberg: 7/16, 3/16, 5/16, 1/16
                if (leftToRight)
                {
                    if (x + 1 < workWidth)
                    {
                        redErrorBuffer[idx + 1] += (int16_t)((errorR * 7) >> 4);
                        greenErrorBuffer[idx + 1] += (int16_t)((errorG * 7) >> 4);
                        blueErrorBuffer[idx + 1] += (int16_t)((errorB * 7) >> 4);
                    }
                    if (y + 1 < workHeight)
                    {
                        int nextRow = (y + 1) * workWidth;
                        if (x - 1 >= 0)
                        {
                            redErrorBuffer[nextRow + x - 1] += (int16_t)((errorR * 3) >> 4);
                            greenErrorBuffer[nextRow + x - 1] += (int16_t)((errorG * 3) >> 4);
                            blueErrorBuffer[nextRow + x - 1] += (int16_t)((errorB * 3) >> 4);
                        }
                        redErrorBuffer[nextRow + x] += (int16_t)((errorR * 5) >> 4);
                        greenErrorBuffer[nextRow + x] += (int16_t)((errorG * 5) >> 4);
                        blueErrorBuffer[nextRow + x] += (int16_t)((errorB * 5) >> 4);
                        if (x + 1 < workWidth)
                        {
                            redErrorBuffer[nextRow + x + 1] += (int16_t)(errorR >> 4);
                            greenErrorBuffer[nextRow + x + 1] += (int16_t)(errorG >> 4);
                            blueErrorBuffer[nextRow + x + 1] += (int16_t)(errorB >> 4);
                        }
                    }
                }
                else
                {
                    if (x - 1 >= 0)
                    {
                        redErrorBuffer[idx - 1] += (int16_t)((errorR * 7) >> 4);
                        greenErrorBuffer[idx - 1] += (int16_t)((errorG * 7) >> 4);
                        blueErrorBuffer[idx - 1] += (int16_t)((errorB * 7) >> 4);
                    }
                    if (y + 1 < workHeight)
                    {
                        int nextRow = (y + 1) * workWidth;
                        if (x + 1 < workWidth)
                        {
                            redErrorBuffer[nextRow + x + 1] += (int16_t)((errorR * 3) >> 4);
                            greenErrorBuffer[nextRow + x + 1] += (int16_t)((errorG * 3) >> 4);
                            blueErrorBuffer[nextRow + x + 1] += (int16_t)((errorB * 3) >> 4);
                        }
                        redErrorBuffer[nextRow + x] += (int16_t)((errorR * 5) >> 4);
                        greenErrorBuffer[nextRow + x] += (int16_t)((errorG * 5) >> 4);
                        blueErrorBuffer[nextRow + x] += (int16_t)((errorB * 5) >> 4);
                        if (x - 1 >= 0)
                        {
                            redErrorBuffer[nextRow + x - 1] += (int16_t)(errorR >> 4);
                            greenErrorBuffer[nextRow + x - 1] += (int16_t)(errorG >> 4);
                            blueErrorBuffer[nextRow + x - 1] += (int16_t)(errorB >> 4);
                        }
                    }
                }
            }
            else if (dithering == 3)
            {
                // Sierra Filter Lite: right 1/2, below-left 1/4, below 1/4
                if (x + 1 < workWidth)
                {
                    redErrorBuffer[idx + 1] += (int16_t)(errorR >> 1);
                    greenErrorBuffer[idx + 1] += (int16_t)(errorG >> 1);
                    blueErrorBuffer[idx + 1] += (int16_t)(errorB >> 1);
                }
                if (y + 1 < workHeight)
                {
                    int nextRow = (y + 1) * workWidth;
                    if (x - 1 >= 0)
                    {
                        redErrorBuffer[nextRow + x - 1] += (int16_t)(errorR >> 2);
                        greenErrorBuffer[nextRow + x - 1] += (int16_t)(errorG >> 2);
                        blueErrorBuffer[nextRow + x - 1] += (int16_t)(errorB >> 2);
                    }
                    redErrorBuffer[nextRow + x] += (int16_t)(errorR >> 2);
                    greenErrorBuffer[nextRow + x] += (int16_t)(errorG >> 2);
                    blueErrorBuffer[nextRow + x] += (int16_t)(errorB >> 2);
                }
            }
            else if (dithering == 4)
            {
                // Atkinson: 1/8 each to 6 neighbors (6/8 total)
                if (x + 1 < workWidth)
                {
                    redErrorBuffer[idx + 1] += (int16_t)(errorR >> 3);
                    greenErrorBuffer[idx + 1] += (int16_t)(errorG >> 3);
                    blueErrorBuffer[idx + 1] += (int16_t)(errorB >> 3);
                }
                if (x + 2 < workWidth)
                {
                    redErrorBuffer[idx + 2] += (int16_t)(errorR >> 3);
                    greenErrorBuffer[idx + 2] += (int16_t)(errorG >> 3);
                    blueErrorBuffer[idx + 2] += (int16_t)(errorB >> 3);
                }
                if (y + 1 < workHeight)
                {
                    int nextRow = (y + 1) * workWidth;
                    if (x - 1 >= 0)
                    {
                        redErrorBuffer[nextRow + x - 1] += (int16_t)(errorR >> 3);
                        greenErrorBuffer[nextRow + x - 1] += (int16_t)(errorG >> 3);
                        blueErrorBuffer[nextRow + x - 1] += (int16_t)(errorB >> 3);
                    }
                    redErrorBuffer[nextRow + x] += (int16_t)(errorR >> 3);
                    greenErrorBuffer[nextRow + x] += (int16_t)(errorG >> 3);
                    blueErrorBuffer[nextRow + x] += (int16_t)(errorB >> 3);
                    if (x + 1 < workWidth)
                    {
                        redErrorBuffer[nextRow + x + 1] += (int16_t)(errorR >> 3);
                        greenErrorBuffer[nextRow + x + 1] += (int16_t)(errorG >> 3);
                        blueErrorBuffer[nextRow + x + 1] += (int16_t)(errorB >> 3);
                    }
                }
                if (y + 2 < workHeight)
                {
                    int nextNextRow = (y + 2) * workWidth;
                    redErrorBuffer[nextNextRow + x] += (int16_t)(errorR >> 3);
                    greenErrorBuffer[nextNextRow + x] += (int16_t)(errorG >> 3);
                    blueErrorBuffer[nextNextRow + x] += (int16_t)(errorB >> 3);
                }
            }
        }
    }

    // Copy the processed image back to the input buffer (with optional upscale)
    if (usedDownscale)
    {
        for (int y = 0; y < origHeight; ++y)
        {
            int srcY = y / downscale;
            for (int x = 0; x < origWidth; ++x)
            {
                int srcX = x / downscale;
                int dstIdx = y * origWidth + x;
                int srcIdx = srcY * workWidth + srcX;
                imageBuffer[dstIdx] = outputBuffer[srcIdx];
            }
        }
    }
    else
    {
        memcpy(imageBuffer, outputBuffer, workWidth * workHeight * sizeof(uint16_t));
    }

    // Free memory (palLUT is a static cache — not freed here)
    free(outputBuffer);
    if (dithering == 1 || dithering == 3 || dithering == 4)
    {
        free(redErrorBuffer);
        free(greenErrorBuffer);
        free(blueErrorBuffer);
    }
    if (downscaledBuffer)
    {
        free(downscaledBuffer);
    }
}

//////////////////////////////////////////////////////////////////////////////////////////

/**
 * Create a downscaled 128x64 version of the camera image with 1-bit dithering
 *
 * @param cameraFb Pointer to camera frame buffer
 * @return Pointer to newly allocated 128x64 buffer (RGB565 format), caller must free it
 */
uint16_t *createSmallDitheredImage(camera_fb_t *cameraFb)
{
    if (!psramFound() || !cameraFb)
    {
        return nullptr;
    }

    const int targetWidth = 128;
    const int targetHeight = 64;
    int srcWidth = cameraFb->width;
    int srcHeight = cameraFb->height;
    uint16_t *srcBuffer = (uint16_t *)cameraFb->buf;

    // GC0308 outputs RGB565 little-endian frames, so no byte swapping is required.
    const bool swapBytes = true;

    // Allocate buffers
    uint16_t *outputBuffer = (uint16_t *)ps_malloc(targetWidth * targetHeight * sizeof(uint16_t));
    float *errorBuffer = (float *)ps_malloc(targetWidth * targetHeight * sizeof(float));

    if (!outputBuffer || !errorBuffer)
    {
        if (outputBuffer)
            free(outputBuffer);
        if (errorBuffer)
            free(errorBuffer);
        return nullptr;
    }

    // Calculate scaling factors
    float scaleX = (float)srcWidth / targetWidth;
    float scaleY = (float)srcHeight / targetHeight;

    // Downsample and convert to grayscale, storing in error buffer
    for (int y = 0; y < targetHeight; y++)
    {
        for (int x = 0; x < targetWidth; x++)
        {
            // Calculate source coordinates (center of the scaled region)
            int srcX = (int)((x + 0.5f) * scaleX);
            int srcY = (int)((y + 0.5f) * scaleY);

            // Clamp to valid range
            srcX = constrain(srcX, 0, srcWidth - 1);
            srcY = constrain(srcY, 0, srcHeight - 1);

            int srcIdx = srcY * srcWidth + srcX;
            uint16_t pixel = srcBuffer[srcIdx];

            if (swapBytes)
            {
                pixel = ((pixel << 8) | (pixel >> 8));
            }

            // Extract RGB and convert to grayscale
            uint8_t r = ((pixel >> 11) & 0x1F) << 3;
            uint8_t g = ((pixel >> 5) & 0x3F) << 2;
            uint8_t b = (pixel & 0x1F) << 3;

            // Standard luminance formula
            float gray = (r * 0.299f + g * 0.587f + b * 0.114f);

            int idx = y * targetWidth + x;
            errorBuffer[idx] = gray;
        }
    }

    // Apply Floyd-Steinberg dithering for 1-bit (black and white)
    const float f7_16 = 7.0f / 16.0f;
    const float f3_16 = 3.0f / 16.0f;
    const float f5_16 = 5.0f / 16.0f;
    const float f1_16 = 1.0f / 16.0f;
    const float threshold = 128.0f;

    for (int y = 0; y < targetHeight; y++)
    {
        bool leftToRight = (y % 2 == 0);
        int xStart = leftToRight ? 0 : targetWidth - 1;
        int xEnd = leftToRight ? targetWidth : -1;
        int xStep = leftToRight ? 1 : -1;

        for (int x = xStart; x != xEnd; x += xStep)
        {
            int idx = y * targetWidth + x;

            float oldPixel = errorBuffer[idx];

            // Quantize to 1-bit (0 or 255)
            uint8_t newPixel = (oldPixel >= threshold) ? 255 : 0;

            // Calculate error
            float error = oldPixel - newPixel;

            // Convert to RGB565 (black or white)
            uint16_t color;
            if (newPixel == 255)
            {
                color = 0xFFFF; // White
            }
            else
            {
                color = 0x0000; // Black
            }

            if (swapBytes)
            {
                color = ((color << 8) | (color >> 8));
            }

            outputBuffer[idx] = color;

            // Distribute error to neighboring pixels
            if (leftToRight)
            {
                if (x + 1 < targetWidth)
                {
                    errorBuffer[idx + 1] += error * f7_16;
                }

                if (y + 1 < targetHeight)
                {
                    int nextRow = (y + 1) * targetWidth;

                    if (x - 1 >= 0)
                    {
                        errorBuffer[nextRow + x - 1] += error * f3_16;
                    }

                    errorBuffer[nextRow + x] += error * f5_16;

                    if (x + 1 < targetWidth)
                    {
                        errorBuffer[nextRow + x + 1] += error * f1_16;
                    }
                }
            }
            else
            {
                if (x - 1 >= 0)
                {
                    errorBuffer[idx - 1] += error * f7_16;
                }

                if (y + 1 < targetHeight)
                {
                    int nextRow = (y + 1) * targetWidth;

                    if (x + 1 < targetWidth)
                    {
                        errorBuffer[nextRow + x + 1] += error * f3_16;
                    }

                    errorBuffer[nextRow + x] += error * f5_16;

                    if (x - 1 >= 0)
                    {
                        errorBuffer[nextRow + x - 1] += error * f1_16;
                    }
                }
            }
        }
    }

    // Free error buffer
    free(errorBuffer);

    return outputBuffer;
}

//////////////////////////////////////////////////////////////////////////////////////////

/**
 * Reduce camera framebuffer resolution to specified dimensions in-place
 * Modifies the camera framebuffer directly using nearest neighbor downsampling
 *
 * @param cameraFb Pointer to camera frame buffer (will be modified)
 * @param targetWidth Target width for downsampled image
 * @param targetHeight Target height for downsampled image
 */
void reduceResolution(camera_fb_t *cameraFb, int targetWidth, int targetHeight)
{
    if (!psramFound() || !cameraFb)
    {
        return;
    }

    int srcWidth = cameraFb->width;
    int srcHeight = cameraFb->height;

    // If already at target resolution, nothing to do
    if (srcWidth == targetWidth && srcHeight == targetHeight)
    {
        return;
    }

    uint16_t *srcBuffer = (uint16_t *)cameraFb->buf;

    // Allocate temporary buffer for downsampled image
    uint16_t *outputBuffer = (uint16_t *)ps_malloc(targetWidth * targetHeight * sizeof(uint16_t));

    if (!outputBuffer)
    {
        return;
    }

    // Calculate scaling factors
    float scaleX = (float)srcWidth / targetWidth;
    float scaleY = (float)srcHeight / targetHeight;

    // Downsample using nearest neighbor
    for (int y = 0; y < targetHeight; y++)
    {
        for (int x = 0; x < targetWidth; x++)
        {
            // Calculate source coordinates (center of the scaled region)
            int srcX = (int)((x + 0.5f) * scaleX);
            int srcY = (int)((y + 0.5f) * scaleY);

            // Clamp to valid range
            srcX = constrain(srcX, 0, srcWidth - 1);
            srcY = constrain(srcY, 0, srcHeight - 1);

            int srcIdx = srcY * srcWidth + srcX;
            int dstIdx = y * targetWidth + x;

            // Copy pixel directly to maintain same format as source
            outputBuffer[dstIdx] = srcBuffer[srcIdx];
        }
    }

    // Copy downsampled image back to framebuffer
    memcpy(srcBuffer, outputBuffer, targetWidth * targetHeight * sizeof(uint16_t));

    // Update framebuffer dimensions
    cameraFb->width = targetWidth;
    cameraFb->height = targetHeight;
    cameraFb->len = targetWidth * targetHeight * sizeof(uint16_t);

    // Free temporary buffer
    free(outputBuffer);
}

//////////////////////////////////////////////////////////////////////////////////////////
/**
 * Apply color reduction to 8 colors
 * Extracts the 8 most dominant colors from the image using k-means clustering,
 * then replaces all pixels with their nearest dominant color
 *
 * @param cameraFb Pointer to camera frame buffer
 */
void applyColorReduction(camera_fb_t *cameraFb)
{
    if (!psramFound() || !cameraFb)
    {
        return;
    }

    int width = cameraFb->width;
    int height = cameraFb->height;
    uint16_t *frameBuffer = (uint16_t *)cameraFb->buf;
    int totalPixels = width * height;

    // GC0308 outputs RGB565 little-endian frames, so no byte swapping is required.
    const bool swapBytes = true;

    const int numColors = 8;

    // Step 1: Initialize k-means centroids with evenly spaced pixels from the image
    uint32_t centroids[8];
    int step = totalPixels / numColors;

    for (int i = 0; i < numColors; i++)
    {
        int pixelIdx = (i * step + step / 2) % totalPixels;
        uint16_t pixel = frameBuffer[pixelIdx];

        if (swapBytes)
        {
            pixel = ((pixel << 8) | (pixel >> 8));
        }

        uint8_t r = ((pixel >> 11) & 0x1F) << 3;
        uint8_t g = ((pixel >> 5) & 0x3F) << 2;
        uint8_t b = (pixel & 0x1F) << 3;

        centroids[i] = (r << 16) | (g << 8) | b;
    }

    // Allocate buffer for pixel assignments
    uint8_t *assignments = (uint8_t *)ps_malloc(totalPixels);
    if (!assignments)
    {
        return;
    }

    // Step 2: K-means clustering (3 iterations for speed)
    for (int iteration = 0; iteration < 3; iteration++)
    {
        // Assign each pixel to nearest centroid
        for (int i = 0; i < totalPixels; i++)
        {
            uint16_t pixel = frameBuffer[i];

            if (swapBytes)
            {
                pixel = ((pixel << 8) | (pixel >> 8));
            }

            uint8_t r = ((pixel >> 11) & 0x1F) << 3;
            uint8_t g = ((pixel >> 5) & 0x3F) << 2;
            uint8_t b = (pixel & 0x1F) << 3;

            int nearestIdx = 0;
            int minDist = INT_MAX;

            for (int c = 0; c < numColors; c++)
            {
                uint8_t cr = (centroids[c] >> 16) & 0xFF;
                uint8_t cg = (centroids[c] >> 8) & 0xFF;
                uint8_t cb = centroids[c] & 0xFF;

                int dist = colorDistance(r, g, b, cr, cg, cb);
                if (dist < minDist)
                {
                    minDist = dist;
                    nearestIdx = c;
                }
            }

            assignments[i] = nearestIdx;
        }

        // Recalculate centroids as average of assigned pixels
        uint32_t sumR[8] = {0};
        uint32_t sumG[8] = {0};
        uint32_t sumB[8] = {0};
        uint32_t count[8] = {0};

        for (int i = 0; i < totalPixels; i++)
        {
            uint16_t pixel = frameBuffer[i];

            if (swapBytes)
            {
                pixel = ((pixel << 8) | (pixel >> 8));
            }

            uint8_t r = ((pixel >> 11) & 0x1F) << 3;
            uint8_t g = ((pixel >> 5) & 0x3F) << 2;
            uint8_t b = (pixel & 0x1F) << 3;

            int cluster = assignments[i];
            sumR[cluster] += r;
            sumG[cluster] += g;
            sumB[cluster] += b; // Add the RGB values to the sum
            count[cluster]++;
        }

        // Update centroids (avoid division by zero)
        for (int c = 0; c < numColors; c++)
        {
            if (count[c] > 0)
            {
                uint8_t avgR = sumR[c] / count[c];
                uint8_t avgG = sumG[c] / count[c];
                uint8_t avgB = sumB[c] / count[c];
                centroids[c] = (avgR << 16) | (avgG << 8) | avgB;
            }
        }
    }

    // Step 3: Apply the 8 dominant colors directly to the image
    for (int i = 0; i < totalPixels; i++)
    {
        uint8_t dominantColorIdx = assignments[i];
        uint32_t dominantColor = centroids[dominantColorIdx];

        // Extract RGB from palette color
        uint8_t r = (dominantColor >> 16) & 0xFF;
        uint8_t g = (dominantColor >> 8) & 0xFF;
        uint8_t b = dominantColor & 0xFF;

        // Convert to RGB565
        uint16_t rgb565 = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);

        if (swapBytes)
        {
            rgb565 = ((rgb565 << 8) | (rgb565 >> 8));
        }

        frameBuffer[i] = rgb565;
    }

    // Free temporary buffer
    free(assignments);
}

//////////////////////////////////////////////////////////////////////////////////////////
/**
 * Apply Sobel edge detection filter to the camera frame buffer
 * Detects edges by computing gradients in X and Y directions
 *
 * @param cameraFb Pointer to camera frame buffer
 * @param mode Edge detection mode: 1=Grayscale, 2=Color
 */
void applyEdgeDetection(camera_fb_t *cameraFb, int mode)
{
    if (!psramFound() || !cameraFb)
    {
        return;
    }

    int width = cameraFb->width;
    int height = cameraFb->height;
    uint16_t *frameBuffer = (uint16_t *)cameraFb->buf;
    int totalPixels = width * height;

    // GC0308 outputs RGB565 little-endian frames, so no byte swapping is required.
    const bool swapBytes = true;

    // Allocate temporary buffer for edge-detected image
    uint16_t *edgeBuffer = (uint16_t *)ps_malloc(totalPixels * sizeof(uint16_t));
    if (!edgeBuffer)
    {
        return;
    }

    // Sobel kernels for edge detection
    // Gx (horizontal edges)
    int sobelX[3][3] = {
        {-1, 0, 1},
        {-2, 0, 2},
        {-1, 0, 1}};

    // Gy (vertical edges)
    int sobelY[3][3] = {
        {-1, -2, -1},
        {0, 0, 0},
        {1, 2, 1}};

    // Process each pixel
    for (int y = 0; y < height; y++)
    {
        for (int x = 0; x < width; x++)
        {
            int idx = y * width + x;

            // Skip border pixels
            if (x == 0 || x == width - 1 || y == 0 || y == height - 1)
            {
                edgeBuffer[idx] = 0; // Black border
                continue;
            }

            if (mode == 1)
            {
                // Grayscale edge detection
                int gx = 0, gy = 0;

                // Apply Sobel kernels
                for (int ky = -1; ky <= 1; ky++)
                {
                    for (int kx = -1; kx <= 1; kx++)
                    {
                        int pixelIdx = (y + ky) * width + (x + kx);
                        uint16_t pixel = frameBuffer[pixelIdx];

                        if (swapBytes)
                        {
                            pixel = ((pixel << 8) | (pixel >> 8));
                        }

                        // Convert to grayscale using luminance formula
                        uint8_t r = ((pixel >> 11) & 0x1F) << 3;
                        uint8_t g = ((pixel >> 5) & 0x3F) << 2;
                        uint8_t b = (pixel & 0x1F) << 3;
                        uint8_t gray = (r * 30 + g * 59 + b * 11) / 100;

                        // Apply kernel weights
                        gx += gray * sobelX[ky + 1][kx + 1];
                        gy += gray * sobelY[ky + 1][kx + 1];
                    }
                }

                // Calculate gradient magnitude
                int magnitude = (int)sqrt(gx * gx + gy * gy);

                // Clamp to 0-255
                if (magnitude > 255)
                    magnitude = 255;
                if (magnitude < 0)
                    magnitude = 0;

                // Black edges on white background
                uint8_t edgeValue = magnitude;

                // Convert grayscale to RGB565
                uint8_t r5 = edgeValue >> 3;
                uint8_t g6 = edgeValue >> 2;
                uint8_t b5 = edgeValue >> 3;
                uint16_t rgb565 = (r5 << 11) | (g6 << 5) | b5;

                if (swapBytes)
                {
                    rgb565 = ((rgb565 << 8) | (rgb565 >> 8));
                }

                edgeBuffer[idx] = rgb565;
            }
            else if (mode == 2)
            {
                // Color edge detection - apply Sobel to each channel separately
                int gx_r = 0, gy_r = 0;
                int gx_g = 0, gy_g = 0;
                int gx_b = 0, gy_b = 0;

                // Apply Sobel kernels to each color channel
                for (int ky = -1; ky <= 1; ky++)
                {
                    for (int kx = -1; kx <= 1; kx++)
                    {
                        int pixelIdx = (y + ky) * width + (x + kx);
                        uint16_t pixel = frameBuffer[pixelIdx];

                        if (swapBytes)
                        {
                            pixel = ((pixel << 8) | (pixel >> 8));
                        }

                        // Extract RGB channels
                        uint8_t r = ((pixel >> 11) & 0x1F) << 3;
                        uint8_t g = ((pixel >> 5) & 0x3F) << 2;
                        uint8_t b = (pixel & 0x1F) << 3;

                        int weight_x = sobelX[ky + 1][kx + 1];
                        int weight_y = sobelY[ky + 1][kx + 1];

                        // Apply kernel weights to each channel
                        gx_r += r * weight_x;
                        gy_r += r * weight_y;
                        gx_g += g * weight_x;
                        gy_g += g * weight_y;
                        gx_b += b * weight_x;
                        gy_b += b * weight_y;
                    }
                }

                // Calculate gradient magnitude for each channel
                int mag_r = (int)sqrt(gx_r * gx_r + gy_r * gy_r);
                int mag_g = (int)sqrt(gx_g * gx_g + gy_g * gy_g);
                int mag_b = (int)sqrt(gx_b * gx_b + gy_b * gy_b);

                // Clamp to 0-255
                if (mag_r > 255)
                    mag_r = 255;
                if (mag_g > 255)
                    mag_g = 255;
                if (mag_b > 255)
                    mag_b = 255;

                // Black edges on white background
                uint8_t edge_r = mag_r;
                uint8_t edge_g = mag_g;
                uint8_t edge_b = mag_b;

                // Convert to RGB565
                uint8_t r5 = edge_r >> 3;
                uint8_t g6 = edge_g >> 2;
                uint8_t b5 = edge_b >> 3;
                uint16_t rgb565 = (r5 << 11) | (g6 << 5) | b5;

                if (swapBytes)
                {
                    rgb565 = ((rgb565 << 8) | (rgb565 >> 8));
                }

                edgeBuffer[idx] = rgb565;
            }
        }
    }

    // Copy edge-detected image back to frame buffer
    memcpy(frameBuffer, edgeBuffer, totalPixels * sizeof(uint16_t));

    // Free temporary buffer
    free(edgeBuffer);
}

//////////////////////////////////////////////////////////////////////////////////////////
/**
 * Auto-adjust brightness, contrast, and gamma based on histogram analysis
 * Analyzes the image and applies optimal adjustments
 *
 * @param cameraFb Pointer to camera frame buffer
 */
void applyAutoAdjust(camera_fb_t *cameraFb)
{
    if (!psramFound() || !cameraFb)
    {
        return;
    }

    int width = cameraFb->width;
    int height = cameraFb->height;
    uint16_t *frameBuffer = (uint16_t *)cameraFb->buf;
    int totalPixels = width * height;

    // Detect byte swapping
    bool swapBytes = true;
    // Build histogram for luminance
    int histogram[256] = {0};

    for (int i = 0; i < totalPixels; i++)
    {
        uint16_t pixel = frameBuffer[i];

        if (swapBytes)
        {
            pixel = ((pixel << 8) | (pixel >> 8));
        }

        // Extract RGB and calculate luminance
        uint8_t r = ((pixel >> 11) & 0x1F) << 3;
        uint8_t g = ((pixel >> 5) & 0x3F) << 2;
        uint8_t b = (pixel & 0x1F) << 3;
        uint8_t lum = (r * 30 + g * 59 + b * 11) / 100;

        histogram[lum]++;
    }

    // Find min and max values (1% and 99% percentiles to ignore outliers)
    int cumulative = 0;
    int minVal = 0, maxVal = 255;
    int threshold1 = totalPixels / 100;       // 1%
    int threshold99 = totalPixels * 99 / 100; // 99%

    for (int i = 0; i < 256; i++)
    {
        cumulative += histogram[i];
        if (cumulative >= threshold1 && minVal == 0)
        {
            minVal = i;
        }
        if (cumulative >= threshold99)
        {
            maxVal = i;
            break;
        }
    }

    // Prevent division by zero
    if (maxVal <= minVal)
    {
        maxVal = minVal + 1;
    }

    // Calculate contrast and brightness adjustments
    float contrast = 255.0f / (maxVal - minVal);
    float brightness = -minVal * contrast;

    // Auto gamma (aim for mid-tone at 128)
    float midTone = (minVal + maxVal) / 2.0f;
    float gamma = (midTone < 128) ? 1.2f : 0.8f; // Lighten dark images, darken bright images

    // Precompute gamma-adjusted lookup to avoid per-pixel powf
    uint8_t gamma_lut[256];
    for (int i = 0; i < 256; ++i)
    {
        float v = i * contrast + brightness;
        v = max(0.0f, min(255.0f, v));
        v = pow(v / 255.0f, gamma) * 255.0f;
        gamma_lut[i] = static_cast<uint8_t>(v + 0.5f);
    }

    // Apply adjustments to each pixel
    for (int i = 0; i < totalPixels; i++)
    {
        uint16_t pixel = frameBuffer[i];

        if (swapBytes)
        {
            pixel = ((pixel << 8) | (pixel >> 8));
        }

        // Extract RGB
        uint8_t r = ((pixel >> 11) & 0x1F) << 3;
        uint8_t g = ((pixel >> 5) & 0x3F) << 2;
        uint8_t b = (pixel & 0x1F) << 3;

        // Apply contrast/brightness + gamma via LUT
        uint8_t rf = gamma_lut[r];
        uint8_t gf = gamma_lut[g];
        uint8_t bf = gamma_lut[b];

        // Convert back to RGB565
        uint8_t r5 = (uint8_t)rf >> 3;
        uint8_t g6 = (uint8_t)gf >> 2;
        uint8_t b5 = (uint8_t)bf >> 3;
        uint16_t rgb565 = (r5 << 11) | (g6 << 5) | b5;

        if (swapBytes)
        {
            rgb565 = ((rgb565 << 8) | (rgb565 >> 8));
        }

        frameBuffer[i] = rgb565;
    }
}

/**
 * Apply CRT filter - pixelates and separates RGB channels across blocks
 * Block 0: red only, Block 1: green only, Block 2: blue only, repeat
 * @param cameraFb Pointer to camera frame buffer
 * @param pixelSize Size of blocks (1, 2, 4, or 8)
 */
void applyCRT(camera_fb_t *cameraFb, int pixelSize)
{
    if (!psramFound() || !cameraFb)
    {
        return;
    }

    int width = cameraFb->width;
    int height = cameraFb->height;
    uint16_t *frameBuffer = (uint16_t *)cameraFb->buf;

    const bool swapBytes = true;

    // Process image in blocks
    for (int by = 0; by < height; by += pixelSize)
    {
        for (int bx = 0; bx < width; bx += pixelSize)
        {
            // Determine channel for this block based on scanline rotation
            // Line 0: R,G,B,R,G,B... (offset 0)
            // Line 1: B,R,G,B,R,G... (offset 2)
            // Line 2: G,B,R,G,B,R... (offset 1)
            int blockX = bx / pixelSize;
            int blockY = by / pixelSize;
            int lineOffset = (blockY % 3) * 2;
            int channel = (blockX + lineOffset) % 3;

            // First pass: Calculate average color for this block
            uint32_t sumR = 0, sumG = 0, sumB = 0;
            int pixelCount = 0;

            for (int dy = 0; dy < pixelSize && (by + dy) < height; dy++)
            {
                for (int dx = 0; dx < pixelSize && (bx + dx) < width; dx++)
                {
                    int x = bx + dx;
                    int y = by + dy;
                    int i = y * width + x;

                    uint16_t pixel = frameBuffer[i];

                    if (swapBytes)
                    {
                        pixel = ((pixel << 8) | (pixel >> 8));
                    }

                    // Extract RGB565 components and accumulate
                    sumR += (pixel >> 11) & 0x1F;
                    sumG += (pixel >> 5) & 0x3F;
                    sumB += pixel & 0x1F;
                    pixelCount++;
                }
            }

            // Calculate average RGB values
            uint8_t avgR5 = sumR / pixelCount;
            uint8_t avgG6 = sumG / pixelCount;
            uint8_t avgB5 = sumB / pixelCount;

            // Apply channel filter based on block
            if (channel == 0)
            {
                // Keep red only
                avgG6 = 0;
                avgB5 = 0;
            }
            else if (channel == 1)
            {
                // Keep green only
                avgR5 = 0;
                avgB5 = 0;
            }
            else // channel == 2
            {
                // Keep blue only
                avgR5 = 0;
                avgG6 = 0;
            }

            // Reconstruct averaged and filtered RGB565
            uint16_t blockColor = (avgR5 << 11) | (avgG6 << 5) | avgB5;

            if (swapBytes)
            {
                blockColor = ((blockColor << 8) | (blockColor >> 8));
            }

            // Determine if this block row should be darkened (odd block rows)
            int blockRowIndex = by / pixelSize;
            bool darkenBlock = (blockRowIndex % 2 == 1);

            // Apply darkening to the block color if it's on an odd block row
            uint16_t finalBlockColor = blockColor;
            if (darkenBlock)
            {
                // Extract RGB components
                uint16_t tempPixel = blockColor;
                if (swapBytes)
                {
                    tempPixel = ((tempPixel << 8) | (tempPixel >> 8));
                }

                // Reduce brightness to 25% for scanlines
                uint8_t r5 = ((tempPixel >> 11) & 0x1F) * 0.25;
                uint8_t g6 = ((tempPixel >> 5) & 0x3F) * 0.25;
                uint8_t b5 = (tempPixel & 0x1F) * 0.25;

                // Reconstruct pixel
                finalBlockColor = (r5 << 11) | (g6 << 5) | b5;

                if (swapBytes)
                {
                    finalBlockColor = ((finalBlockColor << 8) | (finalBlockColor >> 8));
                }
            }

            // Second pass: Fill entire block with the (possibly darkened) filtered color
            for (int dy = 0; dy < pixelSize && (by + dy) < height; dy++)
            {
                for (int dx = 0; dx < pixelSize && (bx + dx) < width; dx++)
                {
                    int x = bx + dx;
                    int y = by + dy;
                    int i = y * width + x;

                    frameBuffer[i] = finalBlockColor;
                }
            }
        }
    }
}

void resetMultipleExposure()
{
    releaseMultiExposureBuffer();
}

void applyMultipleExposure(camera_fb_t *cameraFb, int frameCount, int blendMode)
{
    if (!cameraFb || !cameraFb->buf || frameCount < 2)
    {
        return;
    }

    size_t pixelCount = static_cast<size_t>(cameraFb->width) * cameraFb->height;
    if (!ensureMultiExposureBuffer(pixelCount, frameCount))
    {
        return;
    }

    uint16_t *frameBuffer = reinterpret_cast<uint16_t *>(cameraFb->buf);
    uint16_t *currentSlot = g_multiExposureFrames + (static_cast<size_t>(g_multiExposureNextSlot) * pixelCount);
    memcpy(currentSlot, frameBuffer, pixelCount * sizeof(uint16_t));

    if (g_multiExposureStoredFrames < frameCount)
    {
        g_multiExposureStoredFrames++;
    }

    const bool swapBytes = true;
    for (size_t index = 0; index < pixelCount; ++index)
    {
        uint32_t redSum = 0;
        uint32_t greenSum = 0;
        uint32_t blueSum = 0;
        uint32_t weightSum = 0;

        for (int frameOffset = 0; frameOffset < g_multiExposureStoredFrames; ++frameOffset)
        {
            int slot = (g_multiExposureNextSlot - 1 - frameOffset + frameCount) % frameCount;
            uint16_t px = g_multiExposureFrames[static_cast<size_t>(slot) * pixelCount + index];
            if (swapBytes)
            {
                px = static_cast<uint16_t>((px << 8) | (px >> 8));
            }

            uint32_t weight = 1;
            if (blendMode == 1)
            {
                weight = static_cast<uint32_t>(g_multiExposureStoredFrames - frameOffset);
            }

            redSum += (((px >> 11) & 0x1F) << 3) * weight;
            greenSum += (((px >> 5) & 0x3F) << 2) * weight;
            blueSum += ((px & 0x1F) << 3) * weight;
            weightSum += weight;
        }

        if (weightSum == 0)
        {
            continue;
        }

        uint8_t red = static_cast<uint8_t>(redSum / weightSum);
        uint8_t green = static_cast<uint8_t>(greenSum / weightSum);
        uint8_t blue = static_cast<uint8_t>(blueSum / weightSum);

        uint16_t blended = (static_cast<uint16_t>(red >> 3) << 11) |
                           (static_cast<uint16_t>(green >> 2) << 5) |
                           static_cast<uint16_t>(blue >> 3);
        if (swapBytes)
        {
            blended = static_cast<uint16_t>((blended << 8) | (blended >> 8));
        }
        frameBuffer[index] = blended;
    }

    g_multiExposureNextSlot = (g_multiExposureNextSlot + 1) % frameCount;
}

//////////////////////////////////////////////////////////////////////////////////////////

/**
 * Apply chromatic aberration to camera frame buffer.
 * Shifts the red channel right and the blue channel left by `shift` pixels,
 * leaving the green channel in place.
 *
 * @param cameraFb Pointer to camera frame buffer (RGB565, little-endian)
 * @param shift Number of pixels to offset each colour channel
 */
void applyChromaAberration(camera_fb_t *cameraFb, int shift)
{
    if (!cameraFb || shift <= 0)
        return;

    int width = cameraFb->width;
    int height = cameraFb->height;
    uint16_t *frameBuffer = (uint16_t *)cameraFb->buf;
    size_t pixelCount = (size_t)width * height;

    // Work on a copy so channel reads are independent of writes
    uint16_t *src = (uint16_t *)heap_caps_malloc(pixelCount * sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!src)
        src = (uint16_t *)heap_caps_malloc(pixelCount * sizeof(uint16_t), MALLOC_CAP_DEFAULT | MALLOC_CAP_8BIT);
    if (!src)
        return;

    memcpy(src, frameBuffer, pixelCount * sizeof(uint16_t));

    for (int y = 0; y < height; y++)
    {
        for (int x = 0; x < width; x++)
        {
            int xRed  = x + shift;
            int xBlue = x - shift;

            // Clamp to edge
            if (xRed  >= width)  xRed  = width  - 1;
            if (xBlue < 0)       xBlue = 0;

            // Read pixels (swap bytes: GC0308 outputs little-endian RGB565)
            uint16_t pxG    = src[y * width + x];
            uint16_t pxR    = src[y * width + xRed];
            uint16_t pxB    = src[y * width + xBlue];

            pxG = (uint16_t)((pxG << 8) | (pxG >> 8));
            pxR = (uint16_t)((pxR << 8) | (pxR >> 8));
            pxB = (uint16_t)((pxB << 8) | (pxB >> 8));

            uint8_t r = (uint8_t)((pxR >> 11) & 0x1F);
            uint8_t g = (uint8_t)((pxG >>  5) & 0x3F);
            uint8_t b = (uint8_t)( pxB        & 0x1F);

            uint16_t out = ((uint16_t)r << 11) | ((uint16_t)g << 5) | b;
            out = (uint16_t)((out << 8) | (out >> 8));
            frameBuffer[y * width + x] = out;
        }
    }

    heap_caps_free(src);
}

//////////////////////////////////////////////////////////////////////////////////////////
