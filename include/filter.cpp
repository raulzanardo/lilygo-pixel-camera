#include "filter.h"

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
    {
        return;
    }

    int width = cameraFb->width;
    int height = cameraFb->height;
    uint16_t *frameBuffer = (uint16_t *)cameraFb->buf;

    // If grayscale mode is enabled, use the minimum bit depth for all channels
    if (grayscale)
    {
        int minBits = min(min(redBits, greenBits), blueBits);
        redBits = greenBits = blueBits = minBits;
    }

    // Calculate number of levels for each channel
    int redLevels = 1 << redBits;
    int greenLevels = 1 << greenBits;
    int blueLevels = 1 << blueBits;

    int redMax = redLevels - 1;
    int greenMax = greenLevels - 1;
    int blueMax = blueLevels - 1;

    float redScale = 255.0f / redMax;
    float greenScale = 255.0f / greenMax;
    float blueScale = 255.0f / blueMax;

    // GC0308 outputs RGB565 little-endian frames, so no byte swapping is required.
    const bool swapBytes = true;

    // Bayer matrix definitions
    const int bayer2x2[2][2] = {
        {0, 2},
        {3, 1}
    };

    const int bayer4x4[4][4] = {
        {0, 8, 2, 10},
        {12, 4, 14, 6},
        {3, 11, 1, 9},
        {15, 7, 13, 5}
    };

    const int bayer8x8[8][8] = {
        {0, 32, 8, 40, 2, 34, 10, 42},
        {48, 16, 56, 24, 50, 18, 58, 26},
        {12, 44, 4, 36, 14, 46, 6, 38},
        {60, 28, 52, 20, 62, 30, 54, 22},
        {3, 35, 11, 43, 1, 33, 9, 41},
        {51, 19, 59, 27, 49, 17, 57, 25},
        {15, 47, 7, 39, 13, 45, 5, 37},
        {63, 31, 55, 23, 61, 29, 53, 21}
    };

    // Clamp bayerSize to valid values
    if (bayerSize != 2 && bayerSize != 4 && bayerSize != 8)
    {
        bayerSize = 4; // Default to 4x4
    }

    int bayerDivisor = (bayerSize == 2) ? 4 : (bayerSize == 4) ? 16 : 64;

    // Allocate memory using PSRAM
    uint8_t *redBuffer = (uint8_t *)ps_malloc(width * height * sizeof(uint8_t));
    uint8_t *greenBuffer = (uint8_t *)ps_malloc(width * height * sizeof(uint8_t));
    uint8_t *blueBuffer = (uint8_t *)ps_malloc(width * height * sizeof(uint8_t));
    float *redErrorBuffer = nullptr;
    float *greenErrorBuffer = nullptr;
    float *blueErrorBuffer = nullptr;
    uint16_t *outputBuffer = (uint16_t *)ps_malloc(width * height * sizeof(uint16_t));

    // Only allocate error buffers for Floyd-Steinberg
    if (algorithm == 0)
    {
        redErrorBuffer = (float *)ps_malloc(width * height * sizeof(float));
        greenErrorBuffer = (float *)ps_malloc(width * height * sizeof(float));
        blueErrorBuffer = (float *)ps_malloc(width * height * sizeof(float));

        if (!redErrorBuffer || !greenErrorBuffer || !blueErrorBuffer)
        {
            if (redBuffer)
                free(redBuffer);
            if (greenBuffer)
                free(greenBuffer);
            if (blueBuffer)
                free(blueBuffer);
            if (redErrorBuffer)
                free(redErrorBuffer);
            if (greenErrorBuffer)
                free(greenErrorBuffer);
            if (blueErrorBuffer)
                free(blueErrorBuffer);
            if (outputBuffer)
                free(outputBuffer);
            return;
        }
    }

    if (!redBuffer || !greenBuffer || !blueBuffer || !outputBuffer)
    {
        // Clean up if any allocation failed
        if (redBuffer)
            free(redBuffer);
        if (greenBuffer)
            free(greenBuffer);
        if (blueBuffer)
            free(blueBuffer);
        if (redErrorBuffer)
            free(redErrorBuffer);
        if (greenErrorBuffer)
            free(greenErrorBuffer);
        if (blueErrorBuffer)
            free(blueErrorBuffer);
        if (outputBuffer)
            free(outputBuffer);
        return;
    }

    // Extract RGB components from the image
    for (int i = 0; i < width * height; i++)
    {
        uint16_t pixel = frameBuffer[i];

        if (swapBytes)
        {
            pixel = ((pixel << 8) | (pixel >> 8));
        }

        // Extract RGB components from RGB565 format
        uint8_t r = ((pixel >> 11) & 0x1F) << 3; // 5 bits to 8 bits
        uint8_t g = ((pixel >> 5) & 0x3F) << 2;  // 6 bits to 8 bits
        uint8_t b = (pixel & 0x1F) << 3;         // 5 bits to 8 bits

        if (grayscale)
        {
            // Convert to grayscale using standard luminance formula
            uint8_t gray = (r * 30 + g * 59 + b * 11) / 100;
            r = g = b = gray;
        }

        redBuffer[i] = r;
        greenBuffer[i] = g;
        blueBuffer[i] = b;

        // Initialize error buffers only for Floyd-Steinberg
        if (algorithm == 0)
        {
            redErrorBuffer[i] = r;
            greenErrorBuffer[i] = g;
            blueErrorBuffer[i] = b;
        }
    }

    if (algorithm == 0)
    {
        // Floyd-Steinberg dithering
        // Precalculate error distribution factors
        const float f7_16 = 7.0f / 16.0f;
        const float f3_16 = 3.0f / 16.0f;
        const float f5_16 = 5.0f / 16.0f;
        const float f1_16 = 1.0f / 16.0f;

        // Apply Floyd-Steinberg dithering with serpentine scanning
        for (int y = 0; y < height; y++)
        {
            bool leftToRight = (y % 2 == 0);
            int xStart = leftToRight ? 0 : width - 1;
            int xEnd = leftToRight ? width : -1;
            int xStep = leftToRight ? 1 : -1;

            for (int x = xStart; x != xEnd; x += xStep)
            {
                int idx = y * width + x;

                // Get current pixel with accumulated error for each channel
                float oldR = redErrorBuffer[idx];
                float oldG = greenErrorBuffer[idx];
                float oldB = blueErrorBuffer[idx];

                // Quantize to the target bit depth
                uint8_t newR = round(round(oldR / redScale) * redScale);
                uint8_t newG = round(round(oldG / greenScale) * greenScale);
                uint8_t newB = round(round(oldB / blueScale) * blueScale);

                // Calculate quantization error
                float errorR = oldR - newR;
                float errorG = oldG - newG;
                float errorB = oldB - newB;

                // Store the quantized value
                redBuffer[idx] = newR;
                greenBuffer[idx] = newG;
                blueBuffer[idx] = newB;

                // Distribute error to neighboring pixels
                if (leftToRight)
                {
                    // Left to right pattern
                    if (x + 1 < width)
                    {
                        redErrorBuffer[idx + 1] += errorR * f7_16;
                        greenErrorBuffer[idx + 1] += errorG * f7_16;
                        blueErrorBuffer[idx + 1] += errorB * f7_16;
                    }

                    if (y + 1 < height)
                    {
                        int nextRow = (y + 1) * width;

                        if (x - 1 >= 0)
                        {
                            redErrorBuffer[nextRow + x - 1] += errorR * f3_16;
                            greenErrorBuffer[nextRow + x - 1] += errorG * f3_16;
                            blueErrorBuffer[nextRow + x - 1] += errorB * f3_16;
                        }

                        redErrorBuffer[nextRow + x] += errorR * f5_16;
                        greenErrorBuffer[nextRow + x] += errorG * f5_16;
                        blueErrorBuffer[nextRow + x] += errorB * f5_16;

                        if (x + 1 < width)
                        {
                            redErrorBuffer[nextRow + x + 1] += errorR * f1_16;
                            greenErrorBuffer[nextRow + x + 1] += errorG * f1_16;
                            blueErrorBuffer[nextRow + x + 1] += errorB * f1_16;
                        }
                    }
                }
                else
                {
                    // Right to left pattern
                    if (x - 1 >= 0)
                    {
                        redErrorBuffer[idx - 1] += errorR * f7_16;
                        greenErrorBuffer[idx - 1] += errorG * f7_16;
                        blueErrorBuffer[idx - 1] += errorB * f7_16;
                    }

                    if (y + 1 < height)
                    {
                        int nextRow = (y + 1) * width;

                        if (x + 1 < width)
                        {
                            redErrorBuffer[nextRow + x + 1] += errorR * f3_16;
                            greenErrorBuffer[nextRow + x + 1] += errorG * f3_16;
                            blueErrorBuffer[nextRow + x + 1] += errorB * f3_16;
                        }

                        redErrorBuffer[nextRow + x] += errorR * f5_16;
                        greenErrorBuffer[nextRow + x] += errorG * f5_16;
                        blueErrorBuffer[nextRow + x] += errorB * f5_16;

                        if (x - 1 >= 0)
                        {
                            redErrorBuffer[nextRow + x - 1] += errorR * f1_16;
                            greenErrorBuffer[nextRow + x - 1] += errorG * f1_16;
                            blueErrorBuffer[nextRow + x - 1] += errorB * f1_16;
                        }
                    }
                }
            }
        }
    }
    else if (algorithm == 1)
    {
        // Bayer dithering
        for (int y = 0; y < height; y++)
        {
            for (int x = 0; x < width; x++)
            {
                int idx = y * width + x;

                // Get current pixel color
                float oldR = redBuffer[idx];
                float oldG = greenBuffer[idx];
                float oldB = blueBuffer[idx];

                // Get Bayer threshold value based on matrix size
                int bayerX = x % bayerSize;
                int bayerY = y % bayerSize;
                int bayerValue;

                if (bayerSize == 2)
                {
                    bayerValue = bayer2x2[bayerY][bayerX];
                }
                else if (bayerSize == 4)
                {
                    bayerValue = bayer4x4[bayerY][bayerX];
                }
                else // bayerSize == 8
                {
                    bayerValue = bayer8x8[bayerY][bayerX];
                }

                // Calculate threshold (normalize to 0-255 range)
                float threshold = (bayerValue / (float)bayerDivisor) * 255.0f;

                // Apply threshold and quantize
                float thresholdedR = oldR + threshold - 127.5f;
                float thresholdedG = oldG + threshold - 127.5f;
                float thresholdedB = oldB + threshold - 127.5f;

                // Clamp to valid range
                thresholdedR = constrain(thresholdedR, 0, 255);
                thresholdedG = constrain(thresholdedG, 0, 255);
                thresholdedB = constrain(thresholdedB, 0, 255);

                // Quantize to the target bit depth
                uint8_t newR = round(round(thresholdedR / redScale) * redScale);
                uint8_t newG = round(round(thresholdedG / greenScale) * greenScale);
                uint8_t newB = round(round(thresholdedB / blueScale) * blueScale);

                // Store the quantized value
                redBuffer[idx] = newR;
                greenBuffer[idx] = newG;
                blueBuffer[idx] = newB;
            }
        }
    }

    // Convert back to RGB565 format
    for (int i = 0; i < width * height; i++)
    {
        uint8_t r = redBuffer[i];
        uint8_t g = greenBuffer[i];
        uint8_t b = blueBuffer[i];

        // Convert 8-bit to RGB565 format
        uint8_t r5 = r >> 3;
        uint8_t g6 = g >> 2;
        uint8_t b5 = b >> 3;
        uint16_t color = (r5 << 11) | (g6 << 5) | b5;

        if (swapBytes)
        {
            color = ((color << 8) | (color >> 8));
        }

        outputBuffer[i] = color;
    }

    // Copy the processed image back to the camera frame buffer
    memcpy(frameBuffer, outputBuffer, width * height * sizeof(uint16_t));

    // Free allocated memory
    free(redBuffer);
    free(greenBuffer);
    free(blueBuffer);
    if (algorithm == 0)
    {
        free(redErrorBuffer);
        free(greenErrorBuffer);
        free(blueErrorBuffer);
    }
    free(outputBuffer);
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
 * @param dithering Dithering algorithm: 0=OFF, 1=Floyd-Steinberg, 2=Bayer
 * @param pixelSize Pixelation size (1 = no pixelation)
 * @param bayerSize Bayer matrix size (2, 4, or 8) - only used when dithering = 2
 */
void applyColorPalette(uint16_t *imageBuffer, int width, int height, const uint32_t *palette, int paletteSize, int dithering, int pixelSize, int bayerSize)
{

    if (!psramFound())
    {
        return;
    }

    // GC0308 outputs RGB565 little-endian frames, so no byte swapping is required.
    const bool swapBytes = true;

    // Bayer matrix definitions
    const int bayer2x2[2][2] = {
        {0, 2},
        {3, 1}
    };

    const int bayer4x4[4][4] = {
        {0, 8, 2, 10},
        {12, 4, 14, 6},
        {3, 11, 1, 9},
        {15, 7, 13, 5}
    };

    const int bayer8x8[8][8] = {
        {0, 32, 8, 40, 2, 34, 10, 42},
        {48, 16, 56, 24, 50, 18, 58, 26},
        {12, 44, 4, 36, 14, 46, 6, 38},
        {60, 28, 52, 20, 62, 30, 54, 22},
        {3, 35, 11, 43, 1, 33, 9, 41},
        {51, 19, 59, 27, 49, 17, 57, 25},
        {15, 47, 7, 39, 13, 45, 5, 37},
        {63, 31, 55, 23, 61, 29, 53, 21}
    };

    // Clamp bayerSize to valid values
    if (bayerSize != 2 && bayerSize != 4 && bayerSize != 8)
    {
        bayerSize = 4; // Default to 4x4
    }

    int bayerDivisor = (bayerSize == 2) ? 4 : (bayerSize == 4) ? 16 : 64;

    // Allocate memory using PSRAM for buffers
    uint16_t *outputBuffer = (uint16_t *)ps_malloc(width * height * sizeof(uint16_t));
    float *redErrorBuffer = nullptr;
    float *greenErrorBuffer = nullptr;
    float *blueErrorBuffer = nullptr;

    if (!outputBuffer)
    {
        return;
    }

    // Allocate error buffers only if Floyd-Steinberg dithering is enabled
    if (dithering == 1)
    {
        redErrorBuffer = (float *)ps_malloc(width * height * sizeof(float));
        greenErrorBuffer = (float *)ps_malloc(width * height * sizeof(float));
        blueErrorBuffer = (float *)ps_malloc(width * height * sizeof(float));

        if (!redErrorBuffer || !greenErrorBuffer || !blueErrorBuffer)
        {
            if (outputBuffer)
                free(outputBuffer);
            if (redErrorBuffer)
                free(redErrorBuffer);
            if (greenErrorBuffer)
                free(greenErrorBuffer);
            if (blueErrorBuffer)
                free(blueErrorBuffer);
            return;
        }
    }

    // Initialize error buffers with original pixel values (only if Floyd-Steinberg is enabled)
    if (dithering == 1)
    {
        for (int i = 0; i < width * height; i++)
        {
            uint16_t pixel = imageBuffer[i];

            if (swapBytes)
            {
                pixel = ((pixel << 8) | (pixel >> 8));
            }

            // Extract RGB components from RGB565 format
            redErrorBuffer[i] = ((pixel >> 11) & 0x1F) << 3;  // 5 bits to 8 bits
            greenErrorBuffer[i] = ((pixel >> 5) & 0x3F) << 2; // 6 bits to 8 bits
            blueErrorBuffer[i] = (pixel & 0x1F) << 3;         // 5 bits to 8 bits
        }
    }

    // Precalculate error distribution factors for Floyd-Steinberg dithering
    const float f7_16 = 7.0f / 16.0f;
    const float f3_16 = 3.0f / 16.0f;
    const float f5_16 = 5.0f / 16.0f;
    const float f1_16 = 1.0f / 16.0f;

    // Process each pixel
    for (int y = 0; y < height; y++)
    {
        bool leftToRight = (y % 2 == 0);
        int xStart = leftToRight ? 0 : width - 1;
        int xEnd = leftToRight ? width : -1;
        int xStep = leftToRight ? 1 : -1;

        for (int x = xStart; x != xEnd; x += xStep)
        {
            int idx = y * width + x;

            // Get current pixel color
            uint8_t r, g, b;

            // Determine which pixel to sample (block center for pixelation, or current pixel)
            int sampleIdx = idx;
            if (pixelSize > 1)
            {
                int blockX = (x / pixelSize) * pixelSize + pixelSize / 2;
                int blockY = (y / pixelSize) * pixelSize + pixelSize / 2;
                blockX = constrain(blockX, 0, width - 1);
                blockY = constrain(blockY, 0, height - 1);
                sampleIdx = blockY * width + blockX;
            }

            // Get color from appropriate source (error buffer for Floyd-Steinberg, image buffer otherwise)
            if (dithering == 1)
            {
                // Floyd-Steinberg uses error buffers
                r = constrain(round(redErrorBuffer[sampleIdx]), 0, 255);
                g = constrain(round(greenErrorBuffer[sampleIdx]), 0, 255);
                b = constrain(round(blueErrorBuffer[sampleIdx]), 0, 255);
            }
            else
            {
                // Bayer and no dithering use image buffer directly
                uint16_t pixel = imageBuffer[sampleIdx];

                if (swapBytes)
                {
                    pixel = ((pixel << 8) | (pixel >> 8));
                }

                // Extract RGB components from RGB565 format
                r = ((pixel >> 11) & 0x1F) << 3; // 5 bits to 8 bits
                g = ((pixel >> 5) & 0x3F) << 2;  // 6 bits to 8 bits
                b = (pixel & 0x1F) << 3;         // 5 bits to 8 bits
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
                int bayerValue;

                if (bayerSize == 2)
                {
                    bayerValue = bayer2x2[bayerY][bayerX];
                }
                else if (bayerSize == 4)
                {
                    bayerValue = bayer4x4[bayerY][bayerX];
                }
                else // bayerSize == 8
                {
                    bayerValue = bayer8x8[bayerY][bayerX];
                }

                // Calculate threshold (normalize to 0-255 range)
                float threshold = (bayerValue / (float)bayerDivisor) * 255.0f;

                // Apply threshold
                r = constrain(r + threshold - 127.5f, 0, 255);
                g = constrain(g + threshold - 127.5f, 0, 255);
                b = constrain(b + threshold - 127.5f, 0, 255);
            }

            // Find the closest color in the palette
            int minDistance = INT_MAX;
            int closestIndex = 0;

            for (int j = 0; j < paletteSize; j++)
            {
                uint32_t paletteColor = palette[j];
                uint8_t pr = (paletteColor >> 16) & 0xFF;
                uint8_t pg = (paletteColor >> 8) & 0xFF;
                uint8_t pb = paletteColor & 0xFF;

                int distance = colorDistance(r, g, b, pr, pg, pb);

                if (distance < minDistance)
                {
                    minDistance = distance;
                    closestIndex = j;
                }
            }

            // Get the closest palette color
            uint32_t closestColor = palette[closestIndex];
            uint8_t newR = (closestColor >> 16) & 0xFF;
            uint8_t newG = (closestColor >> 8) & 0xFF;
            uint8_t newB = closestColor & 0xFF;

            // Calculate quantization error (only if Floyd-Steinberg is enabled)
            float errorR = 0, errorG = 0, errorB = 0;
            if (dithering == 1)
            {
                errorR = r - newR;
                errorG = g - newG;
                errorB = b - newB;
            }

            // Convert back to RGB565 format
            uint8_t r5 = newR >> 3; // Convert 8-bit to 5-bit (for red)
            uint8_t g6 = newG >> 2; // Convert 8-bit to 6-bit (for green)
            uint8_t b5 = newB >> 3; // Convert 8-bit to 5-bit (for blue)

            uint16_t newPixel = (r5 << 11) | (g6 << 5) | b5;

            if (swapBytes)
            {
                newPixel = ((newPixel << 8) | (newPixel >> 8));
            }

            outputBuffer[idx] = newPixel;

            // Distribute error to neighboring pixels using Floyd-Steinberg algorithm (only if Floyd-Steinberg is enabled)
            if (dithering == 1)
            {
                if (leftToRight)
                {
                    // Left to right pattern
                    if (x + 1 < width)
                    {
                        redErrorBuffer[idx + 1] += errorR * f7_16;
                        greenErrorBuffer[idx + 1] += errorG * f7_16;
                        blueErrorBuffer[idx + 1] += errorB * f7_16;
                    }

                    if (y + 1 < height)
                    {
                        int nextRow = (y + 1) * width;

                        if (x - 1 >= 0)
                        {
                            redErrorBuffer[nextRow + x - 1] += errorR * f3_16;
                            greenErrorBuffer[nextRow + x - 1] += errorG * f3_16;
                            blueErrorBuffer[nextRow + x - 1] += errorB * f3_16;
                        }

                        redErrorBuffer[nextRow + x] += errorR * f5_16;
                        greenErrorBuffer[nextRow + x] += errorG * f5_16;
                        blueErrorBuffer[nextRow + x] += errorB * f5_16;

                        if (x + 1 < width)
                        {
                            redErrorBuffer[nextRow + x + 1] += errorR * f1_16;
                            greenErrorBuffer[nextRow + x + 1] += errorG * f1_16;
                            blueErrorBuffer[nextRow + x + 1] += errorB * f1_16;
                        }
                    }
                }
                else
                {
                    // Right to left pattern
                    if (x - 1 >= 0)
                    {
                        redErrorBuffer[idx - 1] += errorR * f7_16;
                        greenErrorBuffer[idx - 1] += errorG * f7_16;
                        blueErrorBuffer[idx - 1] += errorB * f7_16;
                    }

                    if (y + 1 < height)
                    {
                        int nextRow = (y + 1) * width;

                        if (x + 1 < width)
                        {
                            redErrorBuffer[nextRow + x + 1] += errorR * f3_16;
                            greenErrorBuffer[nextRow + x + 1] += errorG * f3_16;
                            blueErrorBuffer[nextRow + x + 1] += errorB * f3_16;
                        }

                        redErrorBuffer[nextRow + x] += errorR * f5_16;
                        greenErrorBuffer[nextRow + x] += errorG * f5_16;
                        blueErrorBuffer[nextRow + x] += errorB * f5_16;

                        if (x - 1 >= 0)
                        {
                            redErrorBuffer[nextRow + x - 1] += errorR * f1_16;
                            greenErrorBuffer[nextRow + x - 1] += errorG * f1_16;
                            blueErrorBuffer[nextRow + x - 1] += errorB * f1_16;
                        }
                    }
                }
            }
        }
    }

    // Copy the processed image back to the input buffer
    memcpy(imageBuffer, outputBuffer, width * height * sizeof(uint16_t));

    // Free memory
    free(outputBuffer);
    if (dithering == 1)
    {
        free(redErrorBuffer);
        free(greenErrorBuffer);
        free(blueErrorBuffer);
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
        {-1, 0, 1}
    };
    
    // Gy (vertical edges)
    int sobelY[3][3] = {
        {-1, -2, -1},
        { 0,  0,  0},
        { 1,  2,  1}
    };

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
                if (magnitude > 255) magnitude = 255;
                if (magnitude < 0) magnitude = 0;
                
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
                if (mag_r > 255) mag_r = 255;
                if (mag_g > 255) mag_g = 255;
                if (mag_b > 255) mag_b = 255;
                
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
    int threshold1 = totalPixels / 100;  // 1%
    int threshold99 = totalPixels * 99 / 100;  // 99%
    
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
    float gamma = (midTone < 128) ? 1.2f : 0.8f;  // Lighten dark images, darken bright images

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
        
        // Apply contrast and brightness
        float rf = r * contrast + brightness;
        float gf = g * contrast + brightness;
        float bf = b * contrast + brightness;
        
        // Clamp to 0-255
        rf = max(0.0f, min(255.0f, rf));
        gf = max(0.0f, min(255.0f, gf));
        bf = max(0.0f, min(255.0f, bf));
        
        // Apply gamma correction
        rf = pow(rf / 255.0f, gamma) * 255.0f;
        gf = pow(gf / 255.0f, gamma) * 255.0f;
        bf = pow(bf / 255.0f, gamma) * 255.0f;
        
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

//////////////////////////////////////////////////////////////////////////////////////////
