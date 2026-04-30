#pragma once

#include <cstdint>
#include <filesystem>
#include <unordered_map>
#include <vector>

struct SymbolOverlayImage {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> pixels;
};

using SymbolOverlayKeymap = std::unordered_map<int, uint16_t>;

SymbolOverlayImage render_symbol_overlay(const std::filesystem::path& font_path,
                                          const SymbolOverlayKeymap& keymap);
SymbolOverlayImage render_meta_overlay(const std::filesystem::path& font_path);
