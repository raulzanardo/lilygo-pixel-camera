# Cam Lily

A feature-rich camera application for the LilyGo T-Display S3 Pro with OV3660 camera module, featuring real-time filters, software digital zoom, and advanced image processing capabilities.

**Inspired by:** Carlo Andreini's [Pixless Camera](https://www.kickstarter.com/projects/carloandreini/pixless-camera) - a 0.03MP camera that captures charming pixel-art style photos, reminiscent of the iconic Game Boy Camera.

## Hardware

**Required:**
- LilyGo T-Display S3 Pro (ESP32-S3, 222x480 TFT display)
- OV3660 Camera Module (not the default camera that comes with the board)
- MicroSD Card (for photo storage)

**Note:** This project uses the OV3660 camera module, which is different from the standard camera module typically bundled with the T-Display S3 Pro. Make sure you have the compatible OV3660 sensor for proper operation.

**Board Features:**
- ESP32-S3 dual-core processor (240MHz)
- 16MB Flash, 8MB PSRAM
- 222x480 IPS LCD display
- Capacitive touch screen
- Battery management (SY6970)
- USB Type-C

## Features

### Camera Capabilities
- **Live Preview**: Real-time camera feed at 240x176 (HQVGA) resolution
- **Software Digital Zoom**: 1x, 2x, and 4x zoom levels with center cropping
- **Photo Capture**: High-quality PNG image output with configurable processing
- **Auto-Adjust**: Automatic contrast, brightness, and gamma correction

### Real-Time Filters
- **Pixelate**: Block-based pixelation effect with adjustable block size
- **Dithering**: Color palette reduction with Floyd-Steinberg or Bayer dithering
- **Edge Detection**: Sobel operator-based edge detection with adjustable threshold
- **CRT Effect**: Retro CRT monitor simulation with RGB channel separation and scanline patterns

### Color Palettes
18 built-in color palettes including:
- Sunset, Cyberpunk, Autumn, Ocean, Desert, Sakura
- Gameboy, Grayscale, Sepia, Fire, Arctic, Neon
- 4-color, 16-color, and custom palettes

### Camera Controls
- Exposure control (AEC/AEC2)
- Gain control (AGC with manual override)
- Manual exposure value adjustment
- Manual gain adjustment via UI sliders

### Storage & Gallery
- PNG image encoding with optimal PSRAM/DRAM allocation
- SD card photo storage with auto-increment naming
- Built-in gallery with touch navigation
- USB Mass Storage mode for direct file access

## Software Architecture

### Core Technologies
- **Platform**: Espressif 32 v6.3.0
- **Framework**: Arduino (ESP-IDF) v3.20009.0 (2.0.9)
- **UI Library**: LVGL v8.3.11
- **Display Driver**: TFT_eSPI v2.5.31
- **Touch Driver**: TouchLib v0.0.2 (CST92xx)
- **Image Encoding**: PNGenc v1.4.0
- **Power Management**: XPowersLib v0.3.2 (SY6970)

### Memory Management
- Strategic use of PSRAM for large buffers
- Efficient RGB565 pixel format throughout pipeline
- Zero-copy buffer strategies where possible
- Custom lodepng memory allocators for PSRAM usage

### Filter Pipeline
All filters operate in-place on RGB565 framebuffers with automatic byte swapping:

```
Camera Frame → Auto-Adjust → Filter → Zoom/Crop → Display
                                    ↓
                            Optional: PNG Encode → SD Card
```

### Project Structure
```
cam_lily/
├── src/
│   ├── main.cpp           # Application entry, setup, loop
│   ├── touch.cpp/h        # Touch screen handling
│   └── utilities.h        # Pin definitions, board config
├── lib/
│   └── ui/                # LVGL UI screens and components
│       ├── ui_HomeScreen  # Main camera screen
│       ├── ui_GalleryScreen
│       └── ui_SettingsScreen
├── include/
│   ├── filter.cpp/h       # Image processing filters
│   └── palettes.h         # Color palette definitions
└── platformio.ini         # Build configuration
```

## Building

### Prerequisites
- PlatformIO Core or PlatformIO IDE
- ESP32 toolchain (automatically installed by PlatformIO)

### Build Commands
```bash
# Build firmware
platformio run

# Build and upload
platformio run --target upload

# Monitor serial output
platformio device monitor
```

### Configuration
Edit `platformio.ini` to adjust:
- Upload speed
- Monitor speed
- Partition scheme
- Build flags

## Usage

### Basic Operation
1. **Power On**: Camera preview starts automatically
2. **Tap Screen**: Cycle through zoom levels (1x → 2x → 4x)
3. **Flash Button**: Toggle camera LED
4. **Filter Dropdown**: Select real-time filter effect
5. **Palette Dropdown**: Choose color palette (for dithering filter)
6. **Camera Button**: Capture and save photo to SD card

### Settings Screen
- **Dithering Type**: Off, Floyd-Steinberg, or Bayer
- **Pixel Size**: 1x1, 2x2, 4x4, or 8x8 blocks
- **Exposure Control**: Manual AEC value slider
- **Gain Control**: Manual AGC gain slider
- **Auto-Adjust**: Toggle automatic image enhancement

### Gallery Screen
- Browse captured photos
- Delete unwanted images
- View metadata and file information

### USB Mass Storage
Toggle storage switch to enable USB MSC mode for direct SD card access from computer.

## Technical Details

### Image Processing

**Pixelate Filter**
- Averages RGB values within NxN blocks
- Preserves color fidelity while reducing detail
- Configurable block sizes: 1, 2, 4, 8 pixels

**Dithering Algorithm**
- Floyd-Steinberg: Error diffusion for smooth gradients
- Bayer: Ordered dithering with threshold matrix
- Operates on custom color palettes with RGB565 conversion

**Edge Detection**
- Sobel operator (3x3 convolution kernels)
- Separate horizontal and vertical gradient computation
- Configurable threshold for edge sensitivity

**CRT Filter**
- Block-based RGB channel separation
- Scanline-rotating pattern (R,G,B → B,R,G → G,B,R)
- Combined pixelation and color separation effect

### Camera Configuration
- Pixel Format: RGB565
- Frame Size: HQVGA (240x176)
- Frame Buffer: Double-buffered in PSRAM
- Sensor: OV3660 with SCCB interface

### Performance Optimizations
- Hardware SPI for display communication
- DMA transfers where applicable
- Filter algorithms optimized for RGB565
- Strategic frame buffer allocation in PSRAM

## Pin Configuration

See `src/utilities.h` for complete pin mapping.

**Key Pins:**
- I2C (Camera/Touch/PMU): SDA=5, SCL=6
- SPI (Display/SD): MISO=8, MOSI=17, SCK=18
- Camera Data: Y2-Y9 on pins 45,41,40,42,1,3,10,4
- Camera Control: XCLK=11, PCLK=2, VSYNC=7, HREF=15
- LED Flash: GPIO 38

## Memory Usage

**Typical Build:**
- Flash: ~810KB / 6.5MB (12.4%)
- RAM: ~172KB / 320KB (52.6%)
- PSRAM: Used for framebuffers and image processing

## Known Limitations

- Maximum photo resolution limited by PSRAM availability
- USB Mass Storage requires camera stream pause
- Some filters may reduce frame rate on complex scenes

## License

This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details.

This project uses open-source libraries:
- LVGL v8.3.11 (MIT License)
- TFT_eSPI v2.5.31 (FreeBSD License)
- PNGenc v1.4.0 (Apache 2.0)
- XPowersLib v0.3.2 (MIT License)
- TouchLib v0.0.2 (MIT License)

Hardware designed by LilyGo.

## Credits

Developed for the LilyGo T-Display S3 Pro ecosystem.
Built with PlatformIO and the Arduino framework for ESP32.
