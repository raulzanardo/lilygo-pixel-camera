#include <Arduino.h>

// Palette 1: Warm sunset tones (from your first image)
const uint32_t PALETTE_SUNSET[] = {
  0x1a2f3f, // Dark blue
  0x2d4a5a, // Medium dark blue
  0x5a6b7a, // Blue-gray
  0x8b7a7a, // Brownish-gray
  0xc8855a, // Light brown/orange
  0xf0a860, // Orange
  0xf5c878, // Light orange
  0xf5e8c8  // Cream
};
const int PALETTE_SUNSET_SIZE = 8;

// Palette 2: Yellow/Brown tones (from your second image)
const uint32_t PALETTE_YELLOW_BROWN[] = {
  0xf5d428, // Bright yellow
  0xc89050, // Light brown
  0x8b5a28, // Medium brown
  0x503020, // Dark brown
  0xc8a078, // Tan
  0x787878, // Gray
  0x505050, // Dark gray
  0x282828  // Very dark gray/black
};
const int PALETTE_YELLOW_BROWN_SIZE = 8;

// Palette 3: Grayscale/Blue tones (from your third image)
const uint32_t PALETTE_GRAYSCALE[] = {
  0x000000, // Black
  0x282838, // Very dark blue-gray
  0x404858, // Dark blue-gray
  0x606878, // Medium blue-gray
  0x8088a0, // Light blue-gray
  0xa0a8c0, // Lighter blue-gray
  0xc0c8d8, // Very light blue-gray
  0xe0e8f0  // Almost white
};
const int PALETTE_GRAYSCALE_SIZE = 8;


// Palette 4: Retro Gaming (Game Boy inspired)
const uint32_t PALETTE_GAMEBOY[] = {
  0x0f380f, // Dark green
  0x306230, // Medium dark green
  0x8bac0f, // Light green
  0x9bbc0f  // Bright green
};
const int PALETTE_GAMEBOY_SIZE = 4;

// Palette 5: Cyberpunk Neon
const uint32_t PALETTE_CYBERPUNK[] = {
  0x0a0a1a, // Very dark blue
  0x1a1a3a, // Dark blue
  0xff006e, // Hot pink
  0x00f5ff, // Cyan
  0xfb5607, // Orange
  0xffbe0b, // Yellow
  0x8338ec, // Purple
  0x3a86ff  // Blue
};
const int PALETTE_CYBERPUNK_SIZE = 8;

// Palette 6: Autumn Forest
const uint32_t PALETTE_AUTUMN[] = {
  0x2d1b00, // Dark brown
  0x5a3a1a, // Brown
  0x8b4513, // Saddle brown
  0xd2691e, // Chocolate
  0xff8c00, // Dark orange
  0xffa500, // Orange
  0xffd700, // Gold
  0xffebcd  // Blanched almond
};
const int PALETTE_AUTUMN_SIZE = 8;

// Palette 7: Ocean Deep
const uint32_t PALETTE_OCEAN[] = {
  0x001219, // Very dark blue
  0x005f73, // Dark cyan
  0x0a9396, // Teal
  0x94d2bd, // Light teal
  0xe9d8a6, // Beige
  0xee9b00, // Orange
  0xca6702, // Dark orange
  0xbb3e03  // Red-orange
};
const int PALETTE_OCEAN_SIZE = 8;

// Palette 8: Vaporwave
const uint32_t PALETTE_VAPORWAVE[] = {
  0x2d00f7, // Deep blue
  0x6a00f4, // Purple
  0x8900f2, // Violet
  0xa100f2, // Light violet
  0xb100e8, // Magenta
  0xd100d1, // Pink
  0xf72585, // Hot pink
  0xff8fab  // Light pink
};
const int PALETTE_VAPORWAVE_SIZE = 8;

// Palette 9: Desert Sand
const uint32_t PALETTE_DESERT[] = {
  0x3d2817, // Dark brown
  0x6b4423, // Brown
  0x8b6f47, // Light brown
  0xc19a6b, // Tan
  0xd4a574, // Sand
  0xe6b89c, // Light sand
  0xf4d6a8, // Pale sand
  0xffefd5  // Papaya whip
};
const int PALETTE_DESERT_SIZE = 8;

// Palette 10: Cherry Blossom
const uint32_t PALETTE_SAKURA[] = {
  0x2d132c, // Dark purple
  0x801336, // Dark red
  0xc72c41, // Red
  0xee4540, // Bright red
  0xff6b6b, // Light red
  0xffa5ab, // Pink
  0xffc2d1, // Light pink
  0xffe5ec  // Very light pink
};
const int PALETTE_SAKURA_SIZE = 8;

// Palette 11: Mint Ice Cream
const uint32_t PALETTE_MINT[] = {
  0x0d3b66, // Dark blue
  0x1a5490, // Blue
  0x2ec4b6, // Turquoise
  0x7ae582, // Light green
  0xa8e6cf, // Mint
  0xdcedc1, // Light mint
  0xffd3b6, // Peach
  0xffaaa5  // Light coral
};
const int PALETTE_MINT_SIZE = 8;

// Palette 12: Fire and Ash
const uint32_t PALETTE_FIRE[] = {
  0x0c0a09, // Almost black
  0x1c1614, // Very dark gray
  0x3d2b1f, // Dark brown
  0x6a4c3a, // Brown
  0xb85042, // Red-brown
  0xe63946, // Red
  0xff6f59, // Orange-red
  0xffb703  // Yellow-orange
};
const int PALETTE_FIRE_SIZE = 8;

// Palette 13: Arctic Ice
const uint32_t PALETTE_ARCTIC[] = {
  0x03045e, // Navy blue
  0x023e8a, // Dark blue
  0x0077b6, // Blue
  0x0096c7, // Light blue
  0x00b4d8, // Cyan
  0x48cae4, // Light cyan
  0x90e0ef, // Very light cyan
  0xcaf0f8  // Almost white
};
const int PALETTE_ARCTIC_SIZE = 8;

// Palette 14: Vintage Sepia
const uint32_t PALETTE_SEPIA[] = {
  0x1a1110, // Very dark brown
  0x3d2817, // Dark brown
  0x5c4033, // Brown
  0x8b6f47, // Light brown
  0xa0826d, // Tan
  0xc9b7a2, // Light tan
  0xe3d5ca, // Very light tan
  0xf5ebe0  // Cream
};
const int PALETTE_SEPIA_SIZE = 8;

// Palette 15: Neon Night
const uint32_t PALETTE_NEON[] = {
  0x000000, // Black
  0x1a1a2e, // Very dark blue
  0x16213e, // Dark blue
  0x0f3460, // Blue
  0xe94560, // Red
  0xff006e, // Hot pink
  0x00f5ff, // Cyan
  0xffd60a  // Yellow
};
const int PALETTE_NEON_SIZE = 8;

// Palette 16: Pastel Dreams
const uint32_t PALETTE_PASTEL[] = {
  0xffd6e8, // Light pink
  0xffabe1, // Pink
  0xc9a0dc, // Lavender
  0xa0c4ff, // Light blue
  0x9bf6ff, // Cyan
  0xbdb2ff, // Light purple
  0xffc6ff, // Light magenta
  0xfffffc  // White
};
const int PALETTE_PASTEL_SIZE = 8;

// Palette 17: Black & White (2 colors)
const uint32_t PALETTE_BW[] = {
  0x000000, // Black
  0xffffff  // White
};
const int PALETTE_BW_SIZE = 2;

// Palette 18: 4 Colors (CGA inspired)
const uint32_t PALETTE_4COLOR[] = {
  0x000000, // Black
  0x00aaaa, // Cyan
  0xaa00aa, // Magenta
  0xaaaaaa  // White
};
const int PALETTE_4COLOR_SIZE = 4;

// Palette 19: 16 Colors (VGA inspired)
const uint32_t PALETTE_16COLOR[] = {
  0x000000, // Black
  0x0000aa, // Blue
  0x00aa00, // Green
  0x00aaaa, // Cyan
  0xaa0000, // Red
  0xaa00aa, // Magenta
  0xaa5500, // Brown
  0xaaaaaa, // Light Gray
  0x555555, // Dark Gray
  0x5555ff, // Light Blue
  0x55ff55, // Light Green
  0x55ffff, // Light Cyan
  0xff5555, // Light Red
  0xff55ff, // Light Magenta
  0xffff55, // Yellow
  0xffffff  // White
};
const int PALETTE_16COLOR_SIZE = 16;


const uint32_t PALETTE_FRESTA[] = {
  0x0a1f2e, // Darkest Blue (from top left edge)
  0x1a4d6f, // Deep Blue
  0x5fb8b8, // Turquoise/Cyan
  0x7bc491, // Bright Green
  0xe8c547, // Golden Yellow
  0xe89659, // Coral/Orange
  0xd49b9b, // Soft Pink
  0xc9b89a, // Warm Beige/Tan
  0x3dd9d9, // Electric Teal
  0xc8e877, // Luminous Yellow-Green
  0xd4f5f5, // Pale Cyan
  0xf5f0e6  // Cream/Off-White
};
const int PALETTE_FRESTA_SIZE = 12;