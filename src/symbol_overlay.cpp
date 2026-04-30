#include "symbol_overlay.hpp"

#include <array>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>

namespace {

constexpr uint16_t kPsf1Magic = 0x0436;
constexpr int kPsf1CharWidth = 8;
constexpr size_t kPsf1HeaderSize = 4;
constexpr size_t kUnicodeTableOffset = 0x2000;
constexpr uint16_t kUnicodeTableDelim = 0xffff;

constexpr int kFretHeight = 4;
constexpr int kCellPadding = 1;
constexpr int kCharPadding = 1;
constexpr int kNumBeltRows = 1;
constexpr int kNumRows = 3;
constexpr int kNumCols = 10;

using ThreeKeymap = std::unordered_map<int, std::tuple<uint16_t, uint16_t, uint16_t>>;

const std::array<std::array<std::pair<int, uint16_t>, kNumCols>, kNumRows> kSymkeyAlphaTable = {{
    {{
        {16, 'Q'}, {17, 'W'}, {18, 'E'}, {19, 'R'}, {20, 'T'},
        {21, 'Y'}, {22, 'U'}, {23, 'I'}, {24, 'O'}, {25, 'P'},
    }},
    {{
        {30, 'A'}, {31, 'S'}, {32, 'D'}, {33, 'F'}, {34, 'G'},
        {35, 'H'}, {36, 'J'}, {37, 'K'}, {38, 'L'}, {0, '\0'},
    }},
    {{
        {0, '\0'}, {44, 'Z'}, {45, 'X'}, {46, 'C'}, {47, 'V'},
        {48, 'B'}, {49, 'N'}, {50, 'M'}, {113, '$'}, {0, '\0'},
    }},
}};

const std::array<std::pair<const char*, const char*>, 5> kBeltLabels = {{
    {"Ctrl", "Call"},
    {"Meta", "Berry"},
    {"", ""},
    {"Esc", "Back"},
    {"Tmux", "End"},
}};

uint16_t read_le16(const uint8_t* ptr) {
    return static_cast<uint16_t>(ptr[0]) |
           static_cast<uint16_t>(static_cast<uint16_t>(ptr[1]) << 8);
}

std::vector<uint8_t> read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("failed to open overlay font: " + path.string());
    }

    input.seekg(0, std::ios::end);
    const auto size = input.tellg();
    if (size < 0) {
        throw std::runtime_error("failed to size overlay font: " + path.string());
    }
    input.seekg(0, std::ios::beg);

    std::vector<uint8_t> data(static_cast<size_t>(size));
    if (!data.empty()) {
        input.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size()));
        if (!input) {
            throw std::runtime_error("failed to read overlay font: " + path.string());
        }
    }
    return data;
}

class PsfFont {
public:
    explicit PsfFont(std::vector<uint8_t> data) : data_(std::move(data)) {
        if (data_.size() <= kUnicodeTableOffset) {
            throw std::runtime_error("overlay font is too small");
        }

        magic_ = read_le16(data_.data());
        char_size_ = data_[3];
        if (magic_ != kPsf1Magic) {
            throw std::runtime_error("overlay font has invalid PSF1 magic");
        }
        if (char_size_ == 0) {
            throw std::runtime_error("overlay font reports zero charsize");
        }

        build_utf16_table();
    }

    int width() const {
        return kPsf1CharWidth;
    }

    int height() const {
        return char_size_;
    }

    void draw_utf16(uint16_t utf16, std::vector<uint8_t>& buf, int buf_width,
                    int buf_height, int x, int y, int scale) const {
        const auto table_it = table_.find(utf16);
        if (table_it == table_.end()) {
            return;
        }

        if (scale <= 0) {
            return;
        }

        const uint16_t idx = table_it->second;
        const size_t glyph_offset = kPsf1HeaderSize + (static_cast<size_t>(char_size_) * idx);
        if ((glyph_offset + static_cast<size_t>(char_size_)) > data_.size()) {
            throw std::runtime_error("overlay font glyph index out of range");
        }
        const uint8_t* glyph = data_.data() + glyph_offset;

        y = (y < 0) ? buf_height + y : y;
        x = (x < 0) ? buf_width + x : x;
        if (x >= buf_width || y >= buf_height) {
            return;
        }

        for (int sy = 0; sy < char_size_; ++sy) {
            const int dy = y + (scale * sy);
            if (dy >= buf_height) {
                break;
            }
            if ((dy + scale) <= 0) {
                continue;
            }

            for (int sx = 0; sx < kPsf1CharWidth; ++sx) {
                const int dx = x + (scale * sx);
                if (dx >= buf_width) {
                    break;
                }
                if ((dx + scale) <= 0) {
                    continue;
                }

                const uint8_t pixel = ((glyph[sy] >> (7 - sx)) & 1) ? 0x00 : 0xff;
                for (int dsy = 0; dsy < scale; ++dsy) {
                    const int py = dy + dsy;
                    if (py < 0 || py >= buf_height) {
                        continue;
                    }
                    for (int dsx = 0; dsx < scale; ++dsx) {
                        const int px = dx + dsx;
                        if (px < 0 || px >= buf_width) {
                            continue;
                        }
                        buf[(py * buf_width) + px] = pixel;
                    }
                }
            }
        }
    }

private:
    void build_utf16_table() {
        uint16_t psf_idx = 0;
        for (size_t offset = kUnicodeTableOffset;
             (offset + sizeof(uint16_t)) <= data_.size();
             offset += sizeof(uint16_t)) {
            const uint16_t unicode_val = read_le16(data_.data() + offset);
            if (unicode_val == kUnicodeTableDelim) {
                ++psf_idx;
            } else {
                table_[unicode_val] = psf_idx;
            }
        }
    }

    std::vector<uint8_t> data_;
    uint16_t magic_ = 0;
    int char_size_ = 0;
    std::unordered_map<uint16_t, uint16_t> table_;
};

template <typename RenderFunc>
SymbolOverlayImage render_map(const char* map_label, const PsfFont& psf, RenderFunc&& render) {
    const int cell_width = 40;
    const int cell_height = kFretHeight + kCharPadding + (2 * psf.height());
    SymbolOverlayImage image{
        .width = 400,
        .height = (kNumBeltRows + kNumRows) * cell_height,
        .pixels = {},
    };
    image.pixels.assign(static_cast<size_t>(image.width * image.height), 0xff);

    const int belt_cell_width = image.width / 5;
    const int row = 0;
    std::memset(image.pixels.data(), 0, static_cast<size_t>(image.width * kFretHeight));

    for (int col = 0; col < 5; ++col) {
        for (int y = kFretHeight; y < cell_height; ++y) {
            for (int x = 0; x < kCellPadding; ++x) {
                const int dx = (col * belt_cell_width) + x;
                const int dy = (row * cell_height) + y;
                image.pixels[(dy * image.width) + dx] = 0;
            }
        }

        const char* label = (col == 2) ? map_label : kBeltLabels[col].first;
        const size_t label_len = std::strlen(label);
        const char* key = kBeltLabels[col].second;
        const size_t key_len = std::strlen(key);

        for (size_t i = 0; i < key_len; ++i) {
            int x = (col * belt_cell_width) + kCharPadding + (static_cast<int>(i) * psf.width());
            if (col < 2) {
                x += kCellPadding + kCharPadding;
            } else {
                x += belt_cell_width -
                     (kCellPadding + kCharPadding + static_cast<int>(key_len) * psf.width());
            }
            psf.draw_utf16(static_cast<uint8_t>(key[i]), image.pixels, image.width, image.height,
                           x, (row * cell_height) + kFretHeight + kCharPadding, 1);
        }

        const int centered = (belt_cell_width - (static_cast<int>(label_len) * psf.width())) / 2;
        for (size_t i = 0; i < label_len; ++i) {
            psf.draw_utf16(static_cast<uint8_t>(label[i]), image.pixels, image.width, image.height,
                           (col * belt_cell_width) + kCharPadding +
                               (static_cast<int>(i) * psf.width()) + centered,
                           (row * cell_height) + kFretHeight + kCharPadding + psf.height(), 1);
        }
    }

    for (int map_row = kNumBeltRows; map_row < kNumBeltRows + kNumRows; ++map_row) {
        const int key_row = map_row - kNumBeltRows;
        std::memset(&image.pixels[(map_row * cell_height) * image.width],
                    0, static_cast<size_t>(image.width * kFretHeight));

        for (int col = 0; col < kNumCols; ++col) {
            for (int y = kFretHeight; y < cell_height; ++y) {
                for (int x = 0; x < kCellPadding; ++x) {
                    const int dx = (col * cell_width) + x;
                    const int dy = (map_row * cell_height) + y;
                    image.pixels[(dy * image.width) + dx] = 0;
                }
            }

            const auto& [symkey, alpha_utf16] = kSymkeyAlphaTable[key_row][col];
            if (symkey == 0) {
                continue;
            }
            if (!render(map_row, col, symkey, image, cell_width, cell_height)) {
                continue;
            }

            psf.draw_utf16(alpha_utf16, image.pixels, image.width, image.height,
                           (col * cell_width) +
                               ((col < 5) ? kCellPadding + kCharPadding
                                          : cell_width - (kCharPadding + psf.width())),
                           (map_row * cell_height) + kFretHeight + kCharPadding, 1);
        }
    }

    return image;
}

const ThreeKeymap& meta_keymap() {
    static constexpr uint16_t left_arrow = 0x2190;
    static constexpr uint16_t right_arrow = 0x2192;
    static constexpr uint16_t up_arrow = 0x2191;
    static constexpr uint16_t down_arrow = 0x2193;

    static const ThreeKeymap keymap = {
        {16, {'W', 'd', left_arrow}},
        {17, {left_arrow, '\0', '\0'}},
        {18, {up_arrow, '\0', '\0'}},
        {19, {'H', 'o', 'm'}},
        {20, {'T', 'a', 'b'}},
        {21, {}},
        {22, {}},
        {23, {}},
        {24, {'P', 'g', up_arrow}},
        {25, {'P', 'g', down_arrow}},
        {30, {'W', 'd', right_arrow}},
        {31, {down_arrow, '\0', '\0'}},
        {32, {right_arrow, '\0', '\0'}},
        {33, {'E', 'n', 'd'}},
        {34, {}},
        {35, {}},
        {36, {}},
        {37, {}},
        {38, {}},
        {0, {}},
        {44, {}},
        {45, {'C', 't', 'l'}},
        {46, {'A', 'l', 't'}},
        {47, {}},
        {48, {}},
        {49, {'K', 'b', down_arrow}},
        {50, {'K', 'b', up_arrow}},
        {113, {'K', 'b', 't'}},
    };
    return keymap;
}

} // namespace

SymbolOverlayImage render_symbol_overlay(const std::filesystem::path& font_path,
                                          const SymbolOverlayKeymap& keymap) {
    PsfFont psf(read_file(font_path));
    return render_map("SYMBOLS", psf,
        [&psf, &keymap](int row, int col, int symkey, SymbolOverlayImage& image,
                        int cell_width, int cell_height) {
            const auto key_it = keymap.find(symkey);
            if (key_it == keymap.end()) {
                return false;
            }

            psf.draw_utf16(key_it->second, image.pixels, image.width, image.height,
                           (col * cell_width) + ((cell_width / 2) - psf.width()),
                           (row * cell_height) + kFretHeight +
                               ((cell_height / 2) - psf.height()),
                           2);
            return true;
        });
}

SymbolOverlayImage render_meta_overlay(const std::filesystem::path& font_path) {
    PsfFont psf(read_file(font_path));
    auto image = render_map("META", psf,
        [&psf](int row, int col, int symkey, SymbolOverlayImage& image,
               int cell_width, int cell_height) {
            const auto key_it = meta_keymap().find(symkey);
            if (key_it == meta_keymap().end()) {
                return false;
            }

            const auto& [utf16_1, utf16_2, utf16_3] = key_it->second;
            if (utf16_1 == '\0' && utf16_2 == '\0' && utf16_3 == '\0') {
                return false;
            }

            if (utf16_2 == '\0') {
                psf.draw_utf16(utf16_1, image.pixels, image.width, image.height,
                               (col * cell_width) + ((cell_width / 2) - psf.width()),
                               (row * cell_height) + kFretHeight +
                                   ((cell_height / 2) - psf.height()),
                               2);
            } else {
                const int start_at_x = (col * cell_width) +
                    ((col < 5) ? kCellPadding + psf.width() + kCharPadding
                               : cell_width - (4 * psf.width()));
                const int y = (row * cell_height) + kFretHeight +
                    (cell_height / 2) - (psf.height() / 2);
                psf.draw_utf16(utf16_1, image.pixels, image.width, image.height,
                               start_at_x, y, 1);
                psf.draw_utf16(utf16_2, image.pixels, image.width, image.height,
                               start_at_x + psf.width(), y, 1);
                psf.draw_utf16(utf16_3, image.pixels, image.width, image.height,
                               start_at_x + (2 * psf.width()), y, 1);
            }

            return true;
        });

    const int belt_cell_width = image.width / 5;
    const int cell_height = kFretHeight + kCharPadding + (2 * psf.height());
    for (int y = kFretHeight; y < cell_height; ++y) {
        for (int x = belt_cell_width + kCellPadding;
             x < belt_cell_width + kCellPadding + belt_cell_width;
             ++x) {
            uint8_t& pix = image.pixels[(y * image.width) + x];
            pix = pix ? 0x00 : 0xff;
        }
    }

    return image;
}
