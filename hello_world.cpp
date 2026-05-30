#include <iostream>
#include <vector>
#include <fstream>
#include <string>
#include <cstdint>
#include <iomanip>
#include <cstring>
#include <SDL3/SDL.h>

// Dimensiones de la Game Boy
const int SCREEN_WIDTH = 160;
const int SCREEN_HEIGHT = 144;

// Direcciones de Registros I/O comunes
enum IORegisters : uint16_t {
    REG_JOYP = 0xFF00, REG_SB   = 0xFF01, REG_SC   = 0xFF02,
    REG_DIV  = 0xFF04, REG_TIMA = 0xFF05, REG_TMA  = 0xFF06, REG_TAC  = 0xFF07,
    REG_IF   = 0xFF0F,
    REG_LCDC = 0xFF40, REG_STAT = 0xFF41, REG_SCY  = 0xFF42, REG_SCX  = 0xFF43,
    REG_LY   = 0xFF44, REG_LYC  = 0xFF45, REG_DMA  = 0xFF46,
    REG_BGP  = 0xFF47, REG_OBP0 = 0xFF48, REG_OBP1 = 0xFF49,
    REG_WY   = 0xFF4A, REG_WX   = 0xFF4B,
    REG_IE   = 0xFFFF
};

// Modos de la PPU
enum PPUMode {
    MODE_HBLANK = 0,
    MODE_VBLANK = 1,
    MODE_OAM    = 2,
    MODE_VRAM   = 3
};

// ===================== CLASE CPU Z80 SIMPLIFICADA =====================
class CPU {
public:
    // Registros de 8 bits
    uint8_t A, B, C, D, E, H, L;
    
    // Registros de 16 bits
    uint16_t SP, PC;
    
    // Flags (Z, N, H, C)
    uint8_t flagZ, flagN, flagH, flagC;
    
    // Ciclos de máquina y tiempo
    int cycles;
    bool halted;
    bool ime;           // Master Interrupt Enable
    int ei_delay;       // Retraso para la instrucción EI

    CPU() : A(0x01), B(0), C(0x13), D(0), E(0xD8), H(0x01), L(0x4D), 
            SP(0xFFFE), PC(0x0100), 
            flagZ(1), flagN(0), flagH(1), flagC(1),
            cycles(0), halted(false), ime(false), ei_delay(0) {}

    // Pares de registros como direcciones de 16 bits
    uint16_t getBC() const { return (static_cast<uint16_t>(B) << 8) | C; }
    uint16_t getDE() const { return (static_cast<uint16_t>(D) << 8) | E; }
    uint16_t getHL() const { return (static_cast<uint16_t>(H) << 8) | L; }
    uint16_t getAF() const {
        return (static_cast<uint16_t>(A) << 8) | ((flagZ << 7) | (flagN << 6) | (flagH << 5) | (flagC << 4));
    }

    void setBC(uint16_t value) { B = (value >> 8) & 0xFF; C = value & 0xFF; }
    void setDE(uint16_t value) { D = (value >> 8) & 0xFF; E = value & 0xFF; }
    void setHL(uint16_t value) { H = (value >> 8) & 0xFF; L = value & 0xFF; }
    void setAF(uint16_t value) { 
        A = (value >> 8) & 0xFF;
        uint8_t f = value & 0xF0; // Los 4 bits bajos de F siempre son 0 en Game Boy
        flagZ = (f >> 7) & 1;
        flagN = (f >> 6) & 1;
        flagH = (f >> 5) & 1;
        flagC = (f >> 4) & 1;
    }

    // Setear flags basado en operaciones (Re-agregado para XOR/OR)
    void setFlagsZN(uint8_t result) {
        flagZ = (result == 0) ? 1 : 0;
        flagN = 0;
        flagH = 0;
    }
};

// ===================== CLASE PPU (PICTURE PROCESSING UNIT) =====================
class PPU {
private:
    SDL_Window* window;
    SDL_Renderer* renderer;
    SDL_Texture* texture;
    uint32_t pixels[SCREEN_WIDTH * SCREEN_HEIGHT];
    uint32_t colors[4]; // Paleta de colores parametrizada

public:
    uint8_t ly;        // Scanline actual (0xFF44)
    uint8_t stat;      // PPU Status (0xFF41)
    uint8_t bgp, obp0, obp1; // Paletas (0xFF47, 0xFF48, 0xFF49)
    uint8_t lcdc;      // LCD Control (0xFF40)
    uint8_t scy, scx;  // Scroll Y, X
    uint8_t wy, wx;    // Window Y, X (0xFF4A, 0xFF4B)
    int cycles_count;  // Contador para sincronizar con la CPU
    int window_line_counter; // Contador interno para la Window

    PPU() : ly(0), stat(0), bgp(0xFC), obp0(0xFF), obp1(0xFF), lcdc(0x91), scy(0), scx(0), wy(0), wx(0), cycles_count(456), window_line_counter(0) {
        SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO);
        window = SDL_CreateWindow("GameBoy", SCREEN_WIDTH * 3, SCREEN_HEIGHT * 3, 0);
        renderer = SDL_CreateRenderer(window, NULL);
        texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, SCREEN_WIDTH, SCREEN_HEIGHT);
        SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_NEAREST);

        // Establecer la resolución lógica y mantener el aspect ratio con bandas negras (Letterbox)
        SDL_SetRenderLogicalPresentation(renderer, SCREEN_WIDTH, SCREEN_HEIGHT, 
                                         SDL_LOGICAL_PRESENTATION_LETTERBOX);

        // Inicializar con paleta DMG (Grises) por defecto
        set_palette(0xFFecf2cb, 0xFF98d8b1, 0xFF4b849a, 0xFF1f285d);
    }

    // Método para cambiar la paleta de colores fácilmente
    void set_palette(uint32_t c0, uint32_t c1, uint32_t c2, uint32_t c3) {
        colors[0] = c0; colors[1] = c1; colors[2] = c2; colors[3] = c3;
    }

    // Alternar entre pantalla completa y modo ventana
    void toggle_fullscreen() {
        Uint64 flags = SDL_GetWindowFlags(window);
        SDL_SetWindowFullscreen(window, !(flags & SDL_WINDOW_FULLSCREEN));
    }

    ~PPU() {
        SDL_DestroyTexture(texture);
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
    }

    void render_scanline(const std::vector<uint8_t>& memory) {
        // Determinar si la Window está activa en esta línea
        bool window_enabled = (lcdc & 0x20) && (ly >= wy) && (wx <= 166);
        bool drew_window_pixel = false;

        auto get_color = [&](int color_id) {
            int shade = (bgp >> (color_id * 2)) & 0x03;
            return colors[shade];
        };

        uint32_t bg_color_0 = get_color(0);

        // 1. RENDERIZAR FONDO Y WINDOW
        if (lcdc & 0x01) { 
        uint8_t y_pos = ly + scy;

        for (int x = 0; x < SCREEN_WIDTH; x++) {
            uint16_t tile_map_start;
            uint8_t current_x, current_y;

            // Lógica de cambio entre Background y Window
            if (window_enabled && x >= (wx - 7)) {
                tile_map_start = (lcdc & 0x40) ? 0x9C00 : 0x9800;
                current_x = x - (wx - 7);
                current_y = window_line_counter;
                drew_window_pixel = true;
            } else {
                tile_map_start = (lcdc & 0x08) ? 0x9C00 : 0x9800;
                current_x = x + scx;
                current_y = ly + scy;
            }

            uint16_t tile_data_start = (lcdc & 0x10) ? 0x8000 : 0x8800;
            bool unsigned_indices = (lcdc & 0x10);

            uint16_t tile_row = (current_y / 8) * 32;
            uint16_t tile_col = current_x / 8;
            uint16_t tile_address = tile_map_start + tile_row + tile_col;
            
            int16_t tile_index;
            if (unsigned_indices) tile_index = memory[tile_address];
            else tile_index = (int8_t)memory[tile_address];

            uint16_t tile_data_address;
            if (unsigned_indices) tile_data_address = tile_data_start + (tile_index * 16);
            else tile_data_address = tile_data_start + ((tile_index + 128) * 16);

            uint8_t line = (current_y % 8) * 2;
            uint8_t byte1 = memory[tile_data_address + line];
            uint8_t byte2 = memory[tile_data_address + line + 1];

            int color_bit = 7 - (current_x % 8);
            int color_id = ((byte1 >> color_bit) & 1) | (((byte2 >> color_bit) & 1) << 1);

            pixels[ly * SCREEN_WIDTH + x] = get_color(color_id);
        }
        }
        else {
            // Si el fondo está desactivado (DMG), la pantalla se llena con el color 0
            for (int x = 0; x < SCREEN_WIDTH; x++) {
                pixels[ly * SCREEN_WIDTH + x] = bg_color_0;
            }
        }

        if (drew_window_pixel) window_line_counter++;

        // 2. RENDERIZAR SPRITES (Objects)
        if (lcdc & 0x02) {
            bool use_8x16 = (lcdc & 0x04);

            // Iterar en reversa: el sprite en el índice 0 tiene la mayor prioridad
            // y debe dibujarse al final para quedar encima de los demás.
            for (int i = 39; i >= 0; i--) {
                uint16_t oam_addr = 0xFE00 + (i * 4);
                int sprite_y = memory[oam_addr] - 16;
                int sprite_x = memory[oam_addr + 1] - 8;
                uint8_t tile_idx = memory[oam_addr + 2];
                uint8_t attributes = memory[oam_addr + 3];

                bool y_flip = attributes & 0x40;
                bool x_flip = attributes & 0x20;
                uint8_t palette = (attributes & 0x10) ? obp1 : obp0;

                int height = use_8x16 ? 16 : 8;

                // ¿Está el sprite en esta línea (ly)?
                if (ly >= sprite_y && ly < (sprite_y + height)) {
                    int line = ly - sprite_y;
                    if (y_flip) line = height - 1 - line;

                    // Para 8x16, el bit más bajo del índice se ignora
                    if (use_8x16) tile_idx &= 0xFE;

                    uint16_t tile_data_addr = 0x8000 + (tile_idx * 16) + (line * 2);
                    uint8_t byte1 = memory[tile_data_addr];
                    uint8_t byte2 = memory[tile_data_addr + 1];

                    for (int tile_x = 0; tile_x < 8; tile_x++) {
                        int pixel_x = sprite_x + tile_x;
                        if (pixel_x < 0 || pixel_x >= SCREEN_WIDTH) continue;

                        int color_bit = x_flip ? tile_x : (7 - tile_x);
                        int color_id = ((byte1 >> color_bit) & 1) | (((byte2 >> color_bit) & 1) << 1);

                        // Color 0 es transparente para sprites
                        if (color_id == 0) continue;

                        // Prioridad: Si el bit 7 de atributos es 1, el sprite se dibuja 
                        // detrás del fondo (a menos que el fondo sea color 0)
                        bool bg_over_obj = attributes & 0x80;
                        // Nota: Si el fondo está desactivado (lcdc & 0x01 == 0), la prioridad se ignora.
                        if (bg_over_obj && (lcdc & 0x01) && pixels[ly * SCREEN_WIDTH + pixel_x] != bg_color_0) {
                            continue;
                        }

                        // Aplicar paleta del objeto
                        int shade = (palette >> (color_id * 2)) & 0x03;
                        pixels[ly * SCREEN_WIDTH + pixel_x] = colors[shade];
                    }
                }
                // Límite de hardware: solo 10 sprites por línea (Omitido por simplicidad técnica aquí)
            }
        }
    }

    void update(int cycles, std::vector<uint8_t>& memory) {
        // Si el LCD está apagado, no hacemos nada
        if (!(lcdc & 0x80)) {
            ly = 0;
            cycles_count = 456;
            stat = (stat & ~0x03) | 0x00; 
            return;
        }

        cycles_count -= cycles;

        // 1. Actualización de Modos (Solo si estamos en líneas de dibujo)
        if (ly < SCREEN_HEIGHT) {
            PPUMode current_mode = static_cast<PPUMode>(stat & 0x03);
            PPUMode new_mode;
            int ticks = 456 - cycles_count;

            if (ticks < 80)      new_mode = MODE_OAM;
            else if (ticks < 252) new_mode = MODE_VRAM;
            else                 new_mode = MODE_HBLANK;

            if (new_mode != current_mode) {
                stat = (stat & 0xFC) | static_cast<uint8_t>(new_mode);
                // Solicitar interrupción STAT si el bit de selección de modo está activo
                bool interrupt = false;
                if (new_mode == MODE_HBLANK && (stat & 0x08)) interrupt = true;
                if (new_mode == MODE_OAM    && (stat & 0x20)) interrupt = true;
                if (interrupt) memory[REG_IF] |= 0x02;
            }
        } else {
            // Asegurar que el modo sea 1 durante todo el VBlank
            if ((stat & 0x03) != MODE_VBLANK) {
                stat = (stat & 0xFC) | MODE_VBLANK;
                if (stat & 0x10) memory[REG_IF] |= 0x02; // Interrupción STAT Modo 1
            }
        }

        // 2. Cambio de scanline
        if (cycles_count <= 0) {
            if (ly < SCREEN_HEIGHT) render_scanline(memory);
            ly++;
            cycles_count = 456;

            // Comparación LYC (0xFF45)
            uint8_t lyc = memory[REG_LYC];
            if (ly == lyc) {
                stat |= 0x04; // Set coincidence flag (bit 2)
                if (stat & 0x40) memory[REG_IF] |= 0x02; // Interrupción STAT por coincidencia
            } else {
                stat &= ~0x04;
            }

            if (ly == 144) {
                memory[REG_IF] |= 0x01;      // Interrupción VBlank

                // Actualizar pantalla al inicio del VBlank
                SDL_UpdateTexture(texture, NULL, pixels, SCREEN_WIDTH * sizeof(uint32_t));
                SDL_RenderClear(renderer);
                SDL_RenderTexture(renderer, texture, NULL, NULL);
                SDL_RenderPresent(renderer);
            }

            if (ly > 153) {
                ly = 0;
                window_line_counter = 0;
                // Al reiniciar LY a 0, también chequear coincidencia LYC
                if (ly == lyc) {
                    stat |= 0x04;
                    if (stat & 0x40) memory[REG_IF] |= 0x02;
                } else {
                    stat &= ~0x04;
                }
            }
        }
    }
};

// ===================== CLASE APU (AUDIO PROCESSING UNIT) =====================
class APU {
private:
    uint8_t regs[0x30]; // Registros 0xFF10 - 0xFF3F
    float sample_rate = 44100.0f;
    float cycles_per_sample = 4194304.0f / 44100.0f;
    float cycle_accumulator = 0;

    // Frame Sequencer (512Hz) para Envelopes y Lengths
    float sequencer_accumulator = 0;
    int sequencer_step = 0;

    // Estado interno de los canales
    bool ch1_enabled = false, ch2_enabled = false, ch3_enabled = false, ch4_enabled = false;
    int ch1_length = 0, ch2_length = 0, ch3_length = 0, ch4_length = 0;
    bool ch1_len_enabled = false, ch2_len_enabled = false, ch3_len_enabled = false, ch4_len_enabled = false;

    int ch1_env_vol = 0, ch2_env_vol = 0, ch4_env_vol = 0;
    int ch1_env_ticks = 0, ch2_env_ticks = 0, ch4_env_ticks = 0;

    // Estado del Sweep (Canal 1)
    int ch1_sweep_timer = 0;
    uint16_t ch1_shadow_freq = 0;
    bool ch1_sweep_enabled = false;

    float ch1_phase = 0;
    float ch2_phase = 0;
    float ch3_phase = 0;
    float ch4_phase = 0;
    uint16_t ch4_lfsr = 0x7FFF; // Registro de desplazamiento para ruido

    // Estado del Filtro de Paso Alto (HPF) para eliminar DC Offset
    float prev_sample_l = 0, prev_sample_r = 0;
    float prev_out_l = 0, prev_out_r = 0;

    // Estado del Filtro de Paso Bajo (LPF) para un sonido más suave (Filtro Linear)
    float lpf_l = 0, lpf_r = 0;

    float audio_buffer[256];
    int buffer_ptr = 0;

public:
    SDL_AudioStream* stream = nullptr;

    APU() {
        SDL_AudioSpec spec = { SDL_AUDIO_F32, 2, 44100 };
        stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
        if (stream) {
            SDL_ResumeAudioDevice(SDL_GetAudioStreamDevice(stream));
        }
        std::memset(regs, 0, sizeof(regs));
    }

    ~APU() {
        if (stream) SDL_DestroyAudioStream(stream);
    }

    void write(uint16_t addr, uint8_t val) {
        if (addr == 0xFF26) {
            uint8_t old_power = regs[0x16] & 0x80;
            uint8_t new_power = val & 0x80;
            if (!(val & 0x80)) {
                // Apagar todos los canales y limpiar registros
                std::memset(regs, 0, 0x16); // 0xFF10 - 0xFF25
                ch1_enabled = ch2_enabled = ch3_enabled = ch4_enabled = false;
                ch1_phase = ch2_phase = ch3_phase = ch4_phase = 0;
                prev_sample_l = prev_sample_r = prev_out_l = prev_out_r = 0;
                lpf_l = lpf_r = 0;
            }
            regs[0x16] = new_power; 
            return;
        }

        // Si el sonido maestro está apagado, ignorar escrituras a otros registros
        if (!(regs[0x16] & 0x80)) return;

        uint8_t reg_idx = addr - 0xFF10;
        if (reg_idx < 0x30) regs[reg_idx] = val;

        // Lógica de Length Counters y Triggers
        switch(addr) {
            case 0xFF1A: { // NR30 DAC Power
                bool dac_on = val & 0x80;
                if (!dac_on) ch3_enabled = false;
                regs[0x0A] = val; 
                break;
            }
            case 0xFF11: ch1_length = 64 - (val & 0x3F); break;
            case 0xFF14: 
                ch1_len_enabled = val & 0x40;
                if (val & 0x80) trigger_ch1();
                break;
            case 0xFF16: ch2_length = 64 - (val & 0x3F); break;
            case 0xFF19: 
                ch2_len_enabled = val & 0x40;
                if (val & 0x80) trigger_ch2(); 
                break;
            case 0xFF1B: ch3_length = 256 - val; break;
            case 0xFF1E: 
                ch3_len_enabled = val & 0x40;
                if ((val & 0x80) && (regs[0x1A - 0xFF10] & 0x80)) { 
                    ch3_enabled = true; 
                    ch3_phase = 0; // Reiniciar la onda para que la nota empiece limpia
                    if(ch3_length == 0) ch3_length = 256; 
                }
                break;
            case 0xFF20: ch4_length = 64 - (val & 0x3F); break;
            case 0xFF23: 
                ch4_len_enabled = val & 0x40;
                if (val & 0x80) trigger_ch4(); 
                break;
        }
    }

    void trigger_ch1() {
        // El canal solo se activa si el DAC tiene energía
        if ((regs[0x02] & 0xF8) != 0) {
            ch1_enabled = true;
            ch1_phase = 0;
            ch1_env_vol = regs[0x02] >> 4;
            ch1_env_ticks = regs[0x02] & 0x07;
            if (ch1_length == 0) ch1_length = 64;
        }

        // Inicializar Sweep
        ch1_shadow_freq = regs[0x03] | ((regs[0x04] & 0x07) << 8);
        uint8_t nr10 = regs[0x00];
        int sweep_period = (nr10 >> 4) & 0x07;
        int sweep_shift = nr10 & 0x07;
        ch1_sweep_timer = (sweep_period == 0) ? 8 : sweep_period;
        ch1_sweep_enabled = (sweep_period != 0) || (sweep_shift != 0);
    }

    void update_sweep() {
        if (ch1_sweep_timer > 0) ch1_sweep_timer--;

        if (ch1_sweep_timer == 0) {
            uint8_t nr10 = regs[0x00];
            int sweep_period = (nr10 >> 4) & 0x07;
            ch1_sweep_timer = (sweep_period == 0) ? 8 : sweep_period;

            if (ch1_sweep_enabled && sweep_period > 0) {
                int sweep_shift = nr10 & 0x07;
                bool negate = nr10 & 0x08;
                uint16_t delta = ch1_shadow_freq >> sweep_shift;
                uint16_t new_freq = negate ? (ch1_shadow_freq - delta) : (ch1_shadow_freq + delta);

                if (new_freq > 2047) {
                    ch1_enabled = false;
                } else if (sweep_shift > 0) {
                    ch1_shadow_freq = new_freq;
                    regs[0x03] = new_freq & 0xFF;
                    regs[0x04] = (regs[0x04] & 0xF8) | ((new_freq >> 8) & 0x07);
                }
            }
        }
    }

    void trigger_ch2() {
        if ((regs[0x07] & 0xF8) != 0) {
            ch2_enabled = true;
            ch2_env_vol = regs[0x07] >> 4;
            ch2_env_ticks = regs[0x07] & 0x07;
            if (ch2_length == 0) ch2_length = 64;
        }
    }

    void trigger_ch4() {
        if ((regs[0x11] & 0xF8) != 0) {
            ch4_enabled = true;
            ch4_env_vol = regs[0x11] >> 4;
            ch4_env_ticks = regs[0x11] & 0x07;
            if (ch4_length == 0) ch4_length = 64;
        }
    }

    uint8_t read(uint16_t addr) {
        if (addr == 0xFF26) { // NR52: Retornar el estado real de los canales
            uint8_t power = regs[0x16] & 0x80;
            if (!power) return 0x70; // Si no hay energía, los estados son 0
            // Se añade cast a uint8_t para evitar el warning C4805
            return power | (ch4_enabled << 3) | (ch3_enabled << 2) | (ch2_enabled << 1) | (uint8_t)ch1_enabled | 0x70; 
        }
        if (addr >= 0xFF30 && addr <= 0xFF3F) {
            return regs[addr - 0xFF10]; // Wave RAM
        }
        return regs[addr - 0xFF10] | 0xFF;
    }

    void tick(int cycles) {
        cycle_accumulator += cycles;
        sequencer_accumulator += cycles;

        // El Frame Sequencer corre a 512Hz
        if (sequencer_accumulator >= 8192) { // 4MHz / 512 = 8192
            sequencer_accumulator -= 8192;
            step_sequencer();
        }

        while (cycle_accumulator >= cycles_per_sample) {
            generate_sample();
            cycle_accumulator -= cycles_per_sample;
        }
    }

    void step_sequencer() {
        sequencer_step = (sequencer_step + 1) % 8;
        
        // Contadores de Longitud (256Hz) - Se ejecutan en pasos pares
        if (sequencer_step % 2 == 0) {
            if (ch1_len_enabled && ch1_length > 0) { if (--ch1_length == 0) ch1_enabled = false; }
            if (ch2_len_enabled && ch2_length > 0) { if (--ch2_length == 0) ch2_enabled = false; }
            if (ch3_len_enabled && ch3_length > 0) { if (--ch3_length == 0) ch3_enabled = false; }
            if (ch4_len_enabled && ch4_length > 0) { if (--ch4_length == 0) ch4_enabled = false; }
        }

        // Sweep (128Hz) - Pasos 2 y 6
        if (sequencer_step == 2 || sequencer_step == 6) {
            update_sweep();
        }

        // Cada 64Hz (paso 7) actualizamos los Envelopes
        if (sequencer_step == 7) {
            auto update_env = [](uint8_t reg, int& vol, int& ticks) {
                int sweep_pace = reg & 0x07;
                if (sweep_pace == 0) return;
                if (ticks > 0) {
                    ticks--;
                    if (ticks == 0) {
                        ticks = sweep_pace;
                        bool inc = reg & 0x08;
                        if (inc && vol < 15) vol++;
                        else if (!inc && vol > 0) vol--;
                    }
                }
            };
            update_env(regs[0x02], ch1_env_vol, ch1_env_ticks);
            update_env(regs[0x07], ch2_env_vol, ch2_env_ticks);
            update_env(regs[0x11], ch4_env_vol, ch4_env_ticks);
        }
    }

    void generate_sample() {
        if (!stream) return;

        float left = 0.0f, right = 0.0f;
        const float duty_lookup[] = { 0.125f, 0.25f, 0.50f, 0.75f };

        // NR50 (0xFF24) Control de volumen maestro
        float master_vol_l = ((regs[0x14] >> 4) & 0x07) / 7.0f;
        float master_vol_r = (regs[0x14] & 0x07) / 7.0f;

        // NR51 (0xFF25) Panning
        uint8_t panning = regs[0x15];

        // NR52 (0xFF26) Bit 7 es el interruptor maestro de sonido
        if (regs[0x16] & 0x80) {
            // Canal 1
            uint16_t freq1 = regs[0x03] | ((regs[0x04] & 0x07) << 8);
            if (ch1_enabled && freq1 > 0) {
                float f = 131072.0f / (2048.0f - freq1);
                ch1_phase += f / sample_rate;
                if (ch1_phase > 1.0f) ch1_phase -= 1.0f;
                float duty = duty_lookup[regs[0x01] >> 6];
                float val = (ch1_phase < duty ? 1.0f : -1.0f) * (ch1_env_vol / 15.0f) * 0.1f;
                if (panning & 0x10) left += val;
                if (panning & 0x01) right += val;
            }
            // Canal 2
            uint16_t freq2 = regs[0x08] | ((regs[0x09] & 0x07) << 8);
            if (ch2_enabled && freq2 > 0) {
                float f = 131072.0f / (2048.0f - freq2);
                ch2_phase += f / sample_rate;
                if (ch2_phase > 1.0f) ch2_phase -= 1.0f;
                float duty = duty_lookup[regs[0x06] >> 6];
                float val = (ch2_phase < duty ? 1.0f : -1.0f) * (ch2_env_vol / 15.0f) * 0.1f;
                if (panning & 0x20) left += val;
                if (panning & 0x02) right += val;
            }
            // Canal 3 (Bajo / Wave RAM) - Corrección de DC Offset
            uint8_t nr30 = regs[0x0A]; // NR30 (0xFF1A)
            if (ch3_enabled && (nr30 & 0x80)) {
                uint16_t freq3 = regs[0x0D] | ((regs[0x0E] & 0x07) << 8); // 0xFF1D y 0xFF1E
                if (freq3 < 2048) {
                    float f = 2097152.0f / (2048.0f - freq3);
                    ch3_phase += (f / 32.0f) / sample_rate;
                    if (ch3_phase > 1.0f) ch3_phase -= 1.0f;
                    
                    int sample_idx = (int)(ch3_phase * 32.0f) & 31;

                    uint8_t wave_byte = regs[(0xFF30 + (sample_idx / 2)) - 0xFF10];
                    uint8_t wave_sample = (sample_idx % 2 == 0) ? (wave_byte >> 4) : (wave_byte & 0x0F);
                    
                    // 1. Centrar la muestra (0..15) a (-1.0..1.0) PRIMERO
                    float centered_sample = ((float)wave_sample - 7.5f) / 7.5f;

                    // 2. Aplicar el volumen (shift) de forma multiplicativa
                    int shift = (regs[0x0C] >> 5) & 0x03; // NR32 (0xFF1C)
                    float volume = 0.0f;
                    if (shift == 1)      volume = 1.0f;  // 100%
                    else if (shift == 2) volume = 0.5f;  // 50%
                    else if (shift == 3) volume = 0.25f; // 25%
                    
                    if (volume > 0) {
                        float val = (centered_sample * volume) * 0.10f;
                        if (panning & 0x40) left += val;
                        if (panning & 0x04) right += val;
                    }
                }
            }
            // Canal 4
            uint8_t nr43 = regs[0xFF22 - 0xFF10];
            float s = (nr43 >> 4) & 0x0F;
            float r = (nr43 & 0x07);
            if (r == 0) r = 0.5f;
            float f_noise = 524288.0f / r / (float)(1 << (int)(s + 1));

            ch4_phase += f_noise / sample_rate;
            while (ch4_phase > 1.0f) {
                ch4_phase -= 1.0f;
                // Lógica de LFSR para generar ruido blanco
                uint16_t result = (ch4_lfsr & 1) ^ ((ch4_lfsr >> 1) & 1);
                ch4_lfsr = (ch4_lfsr >> 1) | (result << 14);
                if (nr43 & 0x08) { // Modo de paso corto (7 bits)
                    ch4_lfsr = (ch4_lfsr & ~0x40) | (result << 6);
                }
            }
            if (ch4_enabled && ch4_env_vol > 0) {
                float val = ((ch4_lfsr & 1) ? 1.0f : -1.0f) * (ch4_env_vol / 15.0f) * 0.05f;
                if (panning & 0x80) left += val;
                if (panning & 0x08) right += val;
            }
        }

        float mixed_l = left * master_vol_l;
        float mixed_r = right * master_vol_r;

        // Aplicar Filtro de Paso Alto (capacitor de salida)
        // Esto centra la onda en el cero dinámicamente, eliminando el zumbido de DC.
        prev_out_l = mixed_l - prev_sample_l + 0.996f * prev_out_l;
        prev_out_r = mixed_r - prev_sample_r + 0.996f * prev_out_r;
        prev_sample_l = mixed_l;
        prev_sample_r = mixed_r;

        // Aplicar Filtro de Paso Bajo (LPF) - Suavizado linear
        // Esto elimina los armónicos más agudos y estridentes, dando un tono más analógico.
        lpf_l += (prev_out_l - lpf_l) * 0.6f;
        lpf_r += (prev_out_r - lpf_r) * 0.6f;

        audio_buffer[buffer_ptr++] = lpf_l;
        audio_buffer[buffer_ptr++] = lpf_r;
        if (buffer_ptr >= 256) {
            // Sincronización: si la cola de SDL tiene más de ~20ms de audio, esperamos
            // 44100 muestras/seg * 2 canales * 4 bytes/muestra * 0.020 seg = ~7056 bytes
            while (SDL_GetAudioStreamQueued(stream) > 7056) {
                SDL_Delay(1);
            }
            SDL_PutAudioStreamData(stream, audio_buffer, buffer_ptr * sizeof(float));
            buffer_ptr = 0;
        }
    }
};

// ===================== CLASE TIMER =====================
class Timer {
public:
    uint16_t div_counter;
    int tima_counter;
    uint8_t tima, tma, tac;

    Timer() : div_counter(0), tima_counter(0), tima(0), tma(0), tac(0) {}

    void update(int cycles, std::vector<uint8_t>& memory) {
        // DIV siempre incrementa a 16384Hz (cada 256 ciclos de CPU)
        div_counter += cycles;

        if (tac & 0x04) { // Timer enable
            tima_counter += cycles;
            int threshold = 0;
            switch (tac & 0x03) {
                case 0: threshold = 1024; break; // 4096 Hz
                case 1: threshold = 16;   break; // 262144 Hz
                case 2: threshold = 64;   break; // 65536 Hz
                case 3: threshold = 256;  break; // 16384 Hz
            }

            while (tima_counter >= threshold) {
                tima_counter -= threshold;
                if (tima == 0xFF) {
                    tima = tma;
                    memory[0xFF0F] |= 0x04; // Solicitar interrupción de Timer (bit 2)
                } else {
                    tima++;
                }
            }
        }
    }
};

// ===================== CLASE GAMEBOY =====================
class GameBoy {
private:
    // Memoria del sistema (64 KB)
    std::vector<uint8_t> memory;
    // Buffer para la ROM (soporta hasta 64KB para este test)
    std::vector<uint8_t> rom;
    uint8_t current_bank; // Para soporte básico de MBC1
    
    // Estado del Joypad (Bits 0-3, 0 = presionado)
    uint8_t joypad_buttons;    // A, B, Select, Start
    uint8_t joypad_directions; // Right, Left, Up, Down

    // Componentes
    CPU cpu;
    PPU ppu;
    APU apu;
    Timer timer;

    // Sincronización de ciclos
    int frame_cycles = 0;

    // Actualiza los componentes del sistema y rastrea los ciclos del frame
    void tick(int cycles) {
        ppu.update(cycles, memory);
        timer.update(cycles, memory);
        apu.tick(cycles);
        frame_cycles += cycles;
    }
    
    // Maneja la entrada de teclado mapeando a los botones de GB
    void manejar_teclado(SDL_Keycode key, bool pressed) {
        uint8_t bit = 0xFF;
        bool is_direction = false;
        switch (key) {
            case SDLK_RIGHT:  bit = 0; is_direction = true; break;
            case SDLK_LEFT:   bit = 1; is_direction = true; break;
            case SDLK_UP:     bit = 2; is_direction = true; break;
            case SDLK_DOWN:   bit = 3; is_direction = true; break;
            case SDLK_Z:      bit = 0; is_direction = false; break; // Botón A
            case SDLK_X:      bit = 1; is_direction = false; break; // Botón B
            case SDLK_SPACE:  bit = 2; is_direction = false; break; // Select
            case SDLK_RETURN: bit = 3; is_direction = false; break; // Start
        }

        if (bit != 0xFF) {
            if (is_direction) {
                if (pressed) joypad_directions &= ~(1 << bit);
                else joypad_directions |= (1 << bit);
            } else {
                if (pressed) joypad_buttons &= ~(1 << bit);
                else joypad_buttons |= (1 << bit);
            }
            // Tetris no usa interrupción de Joypad, prefiere polling.
        }
    }

    // Para debug
    std::vector<uint8_t> unsupported_opcodes;

    // Lee un valor de 8 bits de memoria
    uint8_t readMem(uint16_t addr) {
        // Redirección de registros de la APU (0xFF10 - 0xFF3F)
        if (addr >= 0xFF10 && addr <= 0xFF3F) return apu.read(addr);

        // 1. Manejo de Registros de Entrada/Salida (I/O)
        if (addr >= 0xFF00 && addr <= 0xFF7F) {
            switch (addr) {
                case REG_JOYP: {
                    uint8_t select = memory[REG_JOYP] & 0x30; 
            uint8_t res = 0x0F;
            if (!(select & 0x10)) res &= joypad_directions; // P14 seleccionado
            if (!(select & 0x20)) res &= joypad_buttons;    // P15 seleccionado
            return 0xC0 | select | res;
        }
                // DIV incrementa cada 256 T-states (16384Hz)
                case REG_DIV:  return (timer.div_counter >> 8) & 0xFF;
                case REG_TIMA: return timer.tima;
                case REG_TMA:  return timer.tma;
                case REG_TAC:  return timer.tac;
                case REG_IF:   return memory[REG_IF] | 0xE0; // Bits 5-7 son siempre 1
                case REG_LCDC: return ppu.lcdc;
                case REG_STAT: return ppu.stat;
                case REG_SCY:  return ppu.scy;
                case REG_SCX:  return ppu.scx;
                case REG_LY:   return ppu.ly;
                case REG_LYC:  return memory[REG_LYC];
                case REG_BGP:  return ppu.bgp;
                case REG_OBP0: return ppu.obp0;
                case REG_OBP1: return ppu.obp1;
                case REG_WY:   return ppu.wy;
                case REG_WX:   return ppu.wx;
                default:       return memory[addr];
            }
        }

        // 2. Registro de Habilitación de Interrupciones
        if (addr == REG_IE) return memory[REG_IE]; // No enmascarar IE para tests de Blargg

        // 3. Mapeo de Memoria (ROM / RAM)
        if (addr <= 0x3FFF) return rom[addr];
        if (addr >= 0x4000 && addr <= 0x7FFF) {
            uint32_t offset = (addr - 0x4000) + (current_bank * 0x4000);
            return rom[offset % rom.size()];
        }

        if (addr >= 0x8000 && addr <= 0xFFFF) {
            return memory[addr];
        }
        return 0xFF;
    }

    // Escribe un valor de 8 bits a memoria
    void writeMem(uint16_t addr, uint8_t value) {
        // Redirección de registros de la APU (0xFF10 - 0xFF3F)
        if (addr >= 0xFF10 && addr <= 0xFF3F) { apu.write(addr, value); return; }

        // 1. Manejo de Registros I/O
        if (addr >= 0xFF00 && addr <= 0xFF7F) {
            switch (addr) {
                case REG_JOYP: memory[REG_JOYP] = (value & 0x30); break;
                case REG_SB:   memory[REG_SB] = value; break;
                case REG_SC: {
                    // Restaurar salida de depuración para ver resultados de Blargg
                    if (value == 0x81) {
                        std::cout << (char)memory[REG_SB] << std::flush;
                    }

                    memory[REG_SC] = value;
                    if ((value & 0x81) == 0x81) {
                        memory[REG_SB] = 0xFF;
                        memory[REG_SC] &= 0x7F;
                        memory[REG_IF] |= 0x08;
                    }
                    break;
                }
                case REG_DIV:  timer.div_counter = 0; timer.tima_counter = 0; break; // Escribir en DIV resetea el contador
                case REG_TIMA: timer.tima = value; break;
                case REG_TMA:  timer.tma = value; break;
                case REG_TAC:  timer.tac = value; break;
                case REG_IF:   memory[REG_IF] = value; break;
                case REG_LCDC: ppu.lcdc = value; break;
                case REG_STAT: ppu.stat = (value & 0xF8) | (ppu.stat & 0x07); break;
                case REG_SCY:  ppu.scy = value; break;
                case REG_SCX:  ppu.scx = value; break;
                case REG_LY:   break; // Read-only
                case REG_LYC:  memory[REG_LYC] = value; break;
                case REG_BGP:  ppu.bgp = value; break;
                case REG_OBP0: ppu.obp0 = value; break;
                case REG_OBP1: ppu.obp1 = value; break;
                case REG_WY:   ppu.wy = value; break;
                case REG_WX:   ppu.wx = value; break;
                case REG_DMA: {
            uint16_t source = value << 8;
            for (int i = 0; i < 0xA0; i++) {
                uint8_t data = readMem(source + i);
                memory[0xFE00 + i] = data; 
            }
                    break;
                }
                default: memory[addr] = value; break;
            }
            return;
        }

        // 2. Registro de Habilitación de Interrupciones
        if (addr == REG_IE) { memory[REG_IE] = value; return; }

        // Escritura en área de control MBC1
        if (addr >= 0x2000 && addr <= 0x3FFF) {
            current_bank = value & 0x1F;
            if (current_bank == 0) current_bank = 1;
            return;
        }

        // Mapeo de escritura (No se puede escribir en ROM)
        if (addr >= 0x8000 && addr <= 0xFFFF) {
            memory[addr] = value;
        }
    }

    // Gestiona las interrupciones pendientes
    void manejar_interrupciones() {
        uint8_t ie = readMem(REG_IE);
        uint8_t if_reg = readMem(REG_IF);
        uint8_t pending = ie & if_reg;

        if (pending != 0) {
            cpu.halted = false; // Salir de HALT si hay interrupción pendiente
            if (cpu.ime) {
                for (int i = 0; i < 5; i++) {
                    if (pending & (1 << i)) {
                        servir_interrupcion(i);
                        // El servicio de interrupción consume ciclos
                        tick(20);
                        break;
                    }
                }
            }
        }
    }

    // Salta al vector de interrupción correspondiente
    void servir_interrupcion(int bit) {
        cpu.ime = false;
        uint8_t if_reg = readMem(REG_IF);
        writeMem(REG_IF, if_reg & ~(1 << bit)); // Limpiar el flag en IF
        
        cpu.SP -= 2;
        writeMem16(cpu.SP, cpu.PC);
        
        uint16_t targets[] = {0x40, 0x48, 0x50, 0x58, 0x60};
        cpu.PC = targets[bit];
    }

    // Lee un valor de 16 bits de memoria
    uint16_t readMem16(uint16_t addr) {
        uint8_t lo = readMem(addr);
        uint8_t hi = readMem(addr + 1);
        return (static_cast<uint16_t>(hi) << 8) | lo;
    }

    // Escribe un valor de 16 bits a memoria
    void writeMem16(uint16_t addr, uint16_t value) {
        writeMem(addr, value & 0xFF);
        writeMem(addr + 1, (value >> 8) & 0xFF);
    }

    // Decodificación y ejecución de instrucciones
    void ejecutar_instruccion() {
        if (cpu.halted) {
            cpu.cycles = 4;
            return;
        }

        uint8_t opcode = readMem(cpu.PC);
        uint8_t increment = 1; // Por defecto incrementa 1 byte
        
        // Decodificación de instrucciones Z80
        switch (opcode) {
            // === INSTRUCCIONES DE CONTROL ===
            // NOP
            case 0x00:
                cpu.cycles = 4;
                break;

            // STOP
            case 0x10:
                increment = 2; // STOP es seguido por un byte 00
                cpu.cycles = 4;
                break;

            // === ROTACIONES Y BITS ===
            // RRCA
            case 0x0F: {
                uint8_t carry = cpu.A & 1;
                cpu.A = (cpu.A >> 1) | (carry << 7);
                cpu.flagZ = 0; // En opcodes base, Z siempre es 0
                cpu.flagN = 0;
                cpu.flagH = 0;
                cpu.flagC = carry;
                cpu.cycles = 4;
                break;
            }

            // RLA
            case 0x17: {
                uint8_t carry = (cpu.A >> 7) & 1;
                cpu.A = (cpu.A << 1) | cpu.flagC;
                cpu.flagZ = 0; // En opcodes base, Z siempre es 0
                cpu.flagN = 0;
                cpu.flagH = 0;
                cpu.flagC = carry;
                cpu.cycles = 4;
                break;
            }

            // RRA (Rotate A Right through Carry)
            case 0x1F: {
                uint8_t carry = cpu.A & 1;
                cpu.A = (cpu.A >> 1) | (cpu.flagC << 7);
                cpu.flagZ = 0; // En opcodes base, Z siempre es 0
                cpu.flagN = 0;
                cpu.flagH = 0;
                cpu.flagC = carry;
                cpu.cycles = 4;
                break;
            }

            // === SALTOS RELATIVOS ===
            // JR n (Jump Relative)
            case 0x18: cpu.PC += (int8_t)readMem(cpu.PC + 1) + 2; increment = 0; cpu.cycles = 12; break;
            
            // JR cc, n
            case 0x20: // JR NZ, n
                if (!cpu.flagZ) { cpu.PC += (int8_t)readMem(cpu.PC + 1) + 2; increment = 0; cpu.cycles = 12; } 
                else { increment = 2; cpu.cycles = 8; }
                break;
            case 0x28: // JR Z, n
                if (cpu.flagZ) { cpu.PC += (int8_t)readMem(cpu.PC + 1) + 2; increment = 0; cpu.cycles = 12; } 
                else { increment = 2; cpu.cycles = 8; }
                break;
            case 0x30: // JR NC, n
                if (!cpu.flagC) { cpu.PC += (int8_t)readMem(cpu.PC + 1) + 2; increment = 0; cpu.cycles = 12; } 
                else { increment = 2; cpu.cycles = 8; }
                break;
            case 0x38: // JR C, n
                if (cpu.flagC) { cpu.PC += (int8_t)readMem(cpu.PC + 1) + 2; increment = 0; cpu.cycles = 12; } 
                else { increment = 2; cpu.cycles = 8; }
                break;

            // === CARGAS DE 16 BITS ===
            // LD (HL-), A
            case 0x32:
                writeMem(cpu.getHL(), cpu.A);
                cpu.setHL(cpu.getHL() - 1);
                cpu.cycles = 8;
                break;

            // LD rr, u16
            case 0x01: cpu.setBC(readMem16(cpu.PC + 1)); increment = 3; cpu.cycles = 12; break;
            case 0x11: cpu.setDE(readMem16(cpu.PC + 1)); increment = 3; cpu.cycles = 12; break;
            case 0x21: cpu.setHL(readMem16(cpu.PC + 1)); increment = 3; cpu.cycles = 12; break;
            case 0x31: cpu.SP = readMem16(cpu.PC + 1); increment = 3; cpu.cycles = 12; break;

            // LD (rr), A
            case 0x02: writeMem(cpu.getBC(), cpu.A); cpu.cycles = 8; break;
            case 0x12: writeMem(cpu.getDE(), cpu.A); cpu.cycles = 8; break;

            // INC rr
            case 0x03: cpu.setBC(cpu.getBC() + 1); cpu.cycles = 8; break;
            case 0x13: cpu.setDE(cpu.getDE() + 1); cpu.cycles = 8; break;
            case 0x23: cpu.setHL(cpu.getHL() + 1); cpu.cycles = 8; break;
            case 0x33: cpu.SP++; cpu.cycles = 8; break;

            // === ARITMÉTICA DE 8 BITS ===
            // INC r
            case 0x04: case 0x0C: case 0x14: case 0x1C: case 0x24: case 0x2C: case 0x3C: {
                uint8_t* reg;
                if (opcode == 0x04) reg = &cpu.B;
                else if (opcode == 0x0C) reg = &cpu.C;
                else if (opcode == 0x14) reg = &cpu.D;
                else if (opcode == 0x1C) reg = &cpu.E;
                else if (opcode == 0x24) reg = &cpu.H;
                else if (opcode == 0x2C) reg = &cpu.L;
                else reg = &cpu.A;
                
                uint8_t result = *reg + 1;
                cpu.flagH = ((*reg & 0x0F) + 1 > 0x0F) ? 1 : 0;
                *reg = result;
                cpu.flagZ = (result == 0) ? 1 : 0;
                cpu.flagN = 0;
                cpu.cycles = 4;
                break;
            }

            // DEC r
            case 0x05: case 0x0D: case 0x15: case 0x1D: case 0x25: case 0x2D: case 0x3D: {
                uint8_t* reg;
                if (opcode == 0x05) reg = &cpu.B;
                else if (opcode == 0x0D) reg = &cpu.C;
                else if (opcode == 0x15) reg = &cpu.D;
                else if (opcode == 0x1D) reg = &cpu.E;
                else if (opcode == 0x25) reg = &cpu.H;
                else if (opcode == 0x2D) reg = &cpu.L;
                else reg = &cpu.A;
                
                uint8_t result = *reg - 1;
                cpu.flagH = ((*reg & 0x0F) == 0) ? 1 : 0;
                *reg = result;
                cpu.flagZ = (result == 0) ? 1 : 0;
                cpu.flagN = 1;
                cpu.cycles = 4;
                break;
            }

            // === CARGAS DE 8 BITS ===
            // LD r, u8
            case 0x06: cpu.B = readMem(cpu.PC + 1); increment = 2; cpu.cycles = 8; break;
            case 0x0E: cpu.C = readMem(cpu.PC + 1); increment = 2; cpu.cycles = 8; break;
            case 0x16: cpu.D = readMem(cpu.PC + 1); increment = 2; cpu.cycles = 8; break;
            case 0x1E: cpu.E = readMem(cpu.PC + 1); increment = 2; cpu.cycles = 8; break;
            case 0x26: cpu.H = readMem(cpu.PC + 1); increment = 2; cpu.cycles = 8; break;
            case 0x2E: cpu.L = readMem(cpu.PC + 1); increment = 2; cpu.cycles = 8; break;
            case 0x3E: cpu.A = readMem(cpu.PC + 1); increment = 2; cpu.cycles = 8; break;

            // RLCA
            case 0x07: {
                uint8_t carry = (cpu.A >> 7) & 1;
                cpu.A = (cpu.A << 1) | carry;
                cpu.flagZ = 0; // En opcodes base, Z siempre es 0
                cpu.flagN = 0;
                cpu.flagH = 0;
                cpu.flagC = carry;
                cpu.cycles = 4;
                break;
            }

            // LD (u16), SP
            case 0x08: {
                uint16_t addr = readMem16(cpu.PC + 1);
                writeMem16(addr, cpu.SP);
                increment = 3;
                cpu.cycles = 20;
                break;
            }

            // ADD HL, rr
            case 0x09: case 0x19: case 0x29: case 0x39: {
                uint16_t hl = cpu.getHL();
                uint16_t val;
                if (opcode == 0x09) val = cpu.getBC();
                else if (opcode == 0x19) val = cpu.getDE();
                else if (opcode == 0x29) val = hl;
                else val = cpu.SP;
                
                uint32_t result = hl + val;
                cpu.setHL(result & 0xFFFF);
                cpu.flagN = 0;
                cpu.flagH = ((hl & 0x0FFF) + (val & 0x0FFF) > 0x0FFF) ? 1 : 0;
                cpu.flagC = (result > 0xFFFF) ? 1 : 0;
                cpu.cycles = 8;
                break;
            }

            // LD A, (rr)
            case 0x0A: cpu.A = readMem(cpu.getBC()); cpu.cycles = 8; break;
            case 0x1A: cpu.A = readMem(cpu.getDE()); cpu.cycles = 8; break;

            // DEC rr
            case 0x0B: cpu.setBC(cpu.getBC() - 1); cpu.cycles = 8; break;
            case 0x1B: cpu.setDE(cpu.getDE() - 1); cpu.cycles = 8; break;
            case 0x2B: cpu.setHL(cpu.getHL() - 1); cpu.cycles = 8; break;
            case 0x3B: cpu.SP--; cpu.cycles = 8; break;

            // INC (HL)
            case 0x34: {
                uint8_t val = readMem(cpu.getHL());
                uint8_t result = val + 1;
                cpu.flagZ = (result == 0) ? 1 : 0;
                cpu.flagN = 0;
                cpu.flagH = ((val & 0x0F) + 1 > 0x0F) ? 1 : 0;
                writeMem(cpu.getHL(), result);
                cpu.cycles = 12;
                break;
            }

            // DEC (HL)
            case 0x35: {
                uint8_t val = readMem(cpu.getHL());
                uint8_t result = val - 1;
                cpu.flagZ = (result == 0) ? 1 : 0;
                cpu.flagN = 1;
                cpu.flagH = ((val & 0x0F) == 0) ? 1 : 0;
                writeMem(cpu.getHL(), result);
                cpu.cycles = 12;
                break;
            }

            // LD (HL), u8
            case 0x36:
                writeMem(cpu.getHL(), readMem(cpu.PC + 1));
                increment = 2;
                cpu.cycles = 12;
                break;

            // LD (HL+), A
            case 0x22:
                writeMem(cpu.getHL(), cpu.A);
                cpu.setHL(cpu.getHL() + 1);
                cpu.cycles = 8;
                break;

            // LD A, (HL+)
            case 0x2A:
                cpu.A = readMem(cpu.getHL());
                cpu.setHL(cpu.getHL() + 1);
                cpu.cycles = 8;
                break;

            // LD r, r
            case 0x40: case 0x41: case 0x42: case 0x43: case 0x44: case 0x45: case 0x47:
            case 0x48: case 0x49: case 0x4A: case 0x4B: case 0x4C: case 0x4D: case 0x4F:
            case 0x50: case 0x51: case 0x52: case 0x53: case 0x54: case 0x55: case 0x57:
            case 0x58: case 0x59: case 0x5A: case 0x5B: case 0x5C: case 0x5D: case 0x5F:
            case 0x60: case 0x61: case 0x62: case 0x63: case 0x64: case 0x65: case 0x67:
            case 0x68: case 0x69: case 0x6A: case 0x6B: case 0x6C: case 0x6D: case 0x6F:
            case 0x78: case 0x79: case 0x7A: case 0x7B: case 0x7C: case 0x7D: case 0x7F:
            {
                // Mapeo seguro de registros: B=0, C=1, D=2, E=3, H=4, L=5, (HL)=6, A=7
                uint8_t* regs[] = {&cpu.B, &cpu.C, &cpu.D, &cpu.E, &cpu.H, &cpu.L, nullptr, &cpu.A};
                uint8_t dst_idx = (opcode >> 3) & 0x07;
                uint8_t src_idx = opcode & 0x07;
                if (regs[dst_idx] && regs[src_idx]) {
                    *regs[dst_idx] = *regs[src_idx];
                }
                cpu.cycles = 4;
                break;
            }

            // LD (HL), r / LD r, (HL)
            case 0x46: cpu.B = readMem(cpu.getHL()); cpu.cycles = 8; break;
            case 0x4E: cpu.C = readMem(cpu.getHL()); cpu.cycles = 8; break;
            case 0x56: cpu.D = readMem(cpu.getHL()); cpu.cycles = 8; break;
            case 0x5E: cpu.E = readMem(cpu.getHL()); cpu.cycles = 8; break;
            case 0x66: cpu.H = readMem(cpu.getHL()); cpu.cycles = 8; break;
            case 0x6E: cpu.L = readMem(cpu.getHL()); cpu.cycles = 8; break;
            case 0x70: writeMem(cpu.getHL(), cpu.B); cpu.cycles = 8; break;
            case 0x71: writeMem(cpu.getHL(), cpu.C); cpu.cycles = 8; break;
            case 0x72: writeMem(cpu.getHL(), cpu.D); cpu.cycles = 8; break;
            case 0x73: writeMem(cpu.getHL(), cpu.E); cpu.cycles = 8; break;
            case 0x74: writeMem(cpu.getHL(), cpu.H); cpu.cycles = 8; break;
            case 0x75: writeMem(cpu.getHL(), cpu.L); cpu.cycles = 8; break;
            case 0x77: writeMem(cpu.getHL(), cpu.A); cpu.cycles = 8; break;
            case 0x7E: cpu.A = readMem(cpu.getHL()); cpu.cycles = 8; break;

            // === OPERACIONES LÓGICO-ARITMÉTICAS ===
            case 0x86: { // ADD A, (HL)
                uint8_t val = readMem(cpu.getHL());
                uint16_t result = cpu.A + val;
                cpu.flagH = ((cpu.A & 0x0F) + (val & 0x0F) > 0x0F) ? 1 : 0;
                cpu.flagC = (result > 0xFF) ? 1 : 0;
                cpu.A = result & 0xFF;
                cpu.flagZ = (cpu.A == 0) ? 1 : 0;
                cpu.flagN = 0;
                cpu.cycles = 8; break;
            }
            case 0x8E: { // ADC A, (HL)
                uint8_t val = readMem(cpu.getHL());
                uint8_t c = cpu.flagC;
                int result = cpu.A + val + c;
                cpu.flagH = ((cpu.A & 0x0F) + (val & 0x0F) + c > 0x0F) ? 1 : 0;
                cpu.flagC = (result > 0xFF) ? 1 : 0;
                cpu.A = result & 0xFF;
                cpu.flagZ = (cpu.A == 0) ? 1 : 0;
                cpu.flagN = 0;
                cpu.cycles = 8; break;
            }
            case 0x96: { // SUB (HL)
                uint8_t val = readMem(cpu.getHL());
                uint8_t result = cpu.A - val;
                cpu.flagH = ((cpu.A & 0x0F) < (val & 0x0F)) ? 1 : 0;
                cpu.flagC = (cpu.A < val) ? 1 : 0;
                cpu.A = result;
                cpu.flagZ = (cpu.A == 0) ? 1 : 0;
                cpu.flagN = 1;
                cpu.cycles = 8; break;
            }
            case 0x9E: { // SBC A, (HL)
                uint8_t val = readMem(cpu.getHL());
                uint8_t c = cpu.flagC;
                int res = cpu.A - val - c;
                cpu.flagH = (static_cast<int>(cpu.A & 0x0F) - static_cast<int>(val & 0x0F) - static_cast<int>(c)) < 0;
                cpu.flagC = res < 0;
                cpu.A = res & 0xFF;
                cpu.flagZ = (cpu.A == 0);
                cpu.flagN = 1;
                cpu.cycles = 8; break;
            }
            case 0xA6: { // AND (HL)
                cpu.A &= readMem(cpu.getHL());
                cpu.flagZ = (cpu.A == 0) ? 1 : 0;
                cpu.flagN = 0; cpu.flagH = 1; cpu.flagC = 0;
                cpu.cycles = 8; break;
            }
            case 0xAE: {
                cpu.A ^= readMem(cpu.getHL());
                cpu.setFlagsZN(cpu.A); cpu.flagH = 0; cpu.flagC = 0;
                cpu.cycles = 8; break;
            }
            case 0xB6: {
                cpu.A |= readMem(cpu.getHL());
                cpu.setFlagsZN(cpu.A); cpu.flagH = 0; cpu.flagC = 0;
                cpu.cycles = 8; break;
            }
            case 0xBE: { // CP (HL)
                uint8_t val = readMem(cpu.getHL());
                uint8_t result = cpu.A - val;
                cpu.flagZ = (result == 0) ? 1 : 0;
                cpu.flagN = 1;
                cpu.flagH = ((cpu.A & 0x0F) < (val & 0x0F)) ? 1 : 0;
                cpu.flagC = (cpu.A < val) ? 1 : 0;
                cpu.cycles = 8; break;
            }

            // ADD A, r
            case 0x80: case 0x81: case 0x82: case 0x83: case 0x84: case 0x85: case 0x87: {
                uint8_t val;
                if (opcode == 0x80) val = cpu.B;
                else if (opcode == 0x81) val = cpu.C;
                else if (opcode == 0x82) val = cpu.D;
                else if (opcode == 0x83) val = cpu.E;
                else if (opcode == 0x84) val = cpu.H;
                else if (opcode == 0x85) val = cpu.L;
                else val = cpu.A;
                
                uint16_t result = cpu.A + val;
                cpu.flagH = ((cpu.A & 0x0F) + (val & 0x0F) > 0x0F) ? 1 : 0;
                cpu.flagC = (result > 0xFF) ? 1 : 0;
                cpu.A = result & 0xFF;
                cpu.flagZ = (cpu.A == 0) ? 1 : 0;
                cpu.flagN = 0;
                cpu.cycles = 4;
                break;
            }

            // ADC A, r
            case 0x88: case 0x89: case 0x8A: case 0x8B: case 0x8C: case 0x8D: case 0x8F: {
                uint8_t val;
                if (opcode == 0x88) val = cpu.B;
                else if (opcode == 0x89) val = cpu.C;
                else if (opcode == 0x8A) val = cpu.D;
                else if (opcode == 0x8B) val = cpu.E;
                else if (opcode == 0x8C) val = cpu.H;
                else if (opcode == 0x8D) val = cpu.L;
                else val = cpu.A;
                uint8_t c = cpu.flagC;
                uint16_t res = cpu.A + val + c;
                cpu.flagH = ((cpu.A & 0xF) + (val & 0xF) + c) > 0xF;
                cpu.flagC = res > 0xFF;
                cpu.A = res & 0xFF;
                cpu.flagZ = (cpu.A == 0);
                cpu.flagN = 0;
                cpu.cycles = 4;
                break;
            }

            // SUB r
            case 0x90: case 0x91: case 0x92: case 0x93: case 0x94: case 0x95: case 0x97: {
                uint8_t val;
                if (opcode == 0x90) val = cpu.B;
                else if (opcode == 0x91) val = cpu.C;
                else if (opcode == 0x92) val = cpu.D;
                else if (opcode == 0x93) val = cpu.E;
                else if (opcode == 0x94) val = cpu.H;
                else if (opcode == 0x95) val = cpu.L;
                else val = cpu.A;
                
                uint8_t result = cpu.A - val;
                cpu.flagH = ((cpu.A & 0x0F) < (val & 0x0F)) ? 1 : 0;
                cpu.flagC = (cpu.A < val) ? 1 : 0;
                cpu.A = result;
                cpu.flagZ = (cpu.A == 0) ? 1 : 0;
                cpu.flagN = 1;
                cpu.cycles = 4;
                break;
            }

            // SBC A, r
            case 0x98: case 0x99: case 0x9A: case 0x9B: case 0x9C: case 0x9D: case 0x9F: {
                uint8_t val;
                if (opcode == 0x98) val = cpu.B;
                else if (opcode == 0x99) val = cpu.C;
                else if (opcode == 0x9A) val = cpu.D;
                else if (opcode == 0x9B) val = cpu.E;
                else if (opcode == 0x9C) val = cpu.H;
                else if (opcode == 0x9D) val = cpu.L;
                else val = cpu.A;
                uint8_t c = cpu.flagC;
                int res = cpu.A - val - c;
                cpu.flagH = (static_cast<int>(cpu.A & 0x0F) - static_cast<int>(val & 0x0F) - static_cast<int>(c)) < 0;
                cpu.flagC = res < 0;
                cpu.A = res & 0xFF;
                cpu.flagZ = (cpu.A == 0);
                cpu.flagN = 1;
                cpu.cycles = 4;
                break;
            }

            // AND r
            case 0xA0: case 0xA1: case 0xA2: case 0xA3: case 0xA4: case 0xA5: case 0xA7: {
                uint8_t val;
                if (opcode == 0xA0) val = cpu.B;
                else if (opcode == 0xA1) val = cpu.C;
                else if (opcode == 0xA2) val = cpu.D;
                else if (opcode == 0xA3) val = cpu.E;
                else if (opcode == 0xA4) val = cpu.H;
                else if (opcode == 0xA5) val = cpu.L;
                else val = cpu.A;
                
                cpu.A &= val;
                cpu.flagZ = (cpu.A == 0) ? 1 : 0;
                cpu.flagN = 0;
                cpu.flagH = 1;
                cpu.flagC = 0;
                cpu.cycles = 4;
                break;
            }

            // XOR r
            case 0xA8: case 0xA9: case 0xAA: case 0xAB: case 0xAC: case 0xAD: case 0xAF: {
                uint8_t val;
                if (opcode == 0xA8) val = cpu.B;
                else if (opcode == 0xA9) val = cpu.C;
                else if (opcode == 0xAA) val = cpu.D;
                else if (opcode == 0xAB) val = cpu.E;
                else if (opcode == 0xAC) val = cpu.H;
                else if (opcode == 0xAD) val = cpu.L;
                else val = cpu.A;
                
                cpu.A ^= val;
                cpu.flagZ = (cpu.A == 0) ? 1 : 0;
                cpu.flagN = 0;
                cpu.flagH = 0;
                cpu.flagC = 0;
                cpu.cycles = 4;
                break;
            }

            // OR r
            case 0xB0: case 0xB1: case 0xB2: case 0xB3: case 0xB4: case 0xB5: case 0xB7: {
                uint8_t val;
                if (opcode == 0xB0) val = cpu.B;
                else if (opcode == 0xB1) val = cpu.C;
                else if (opcode == 0xB2) val = cpu.D;
                else if (opcode == 0xB3) val = cpu.E;
                else if (opcode == 0xB4) val = cpu.H;
                else if (opcode == 0xB5) val = cpu.L;
                else val = cpu.A;
                
                cpu.A |= val;
                cpu.flagZ = (cpu.A == 0) ? 1 : 0;
                cpu.flagN = 0;
                cpu.flagH = 0;
                cpu.flagC = 0;
                cpu.cycles = 4;
                break;
            }

            // CP r (compare)
            case 0xB8: case 0xB9: case 0xBA: case 0xBB: case 0xBC: case 0xBD: case 0xBF: {
                uint8_t val;
                if (opcode == 0xB8) val = cpu.B;
                else if (opcode == 0xB9) val = cpu.C;
                else if (opcode == 0xBA) val = cpu.D;
                else if (opcode == 0xBB) val = cpu.E;
                else if (opcode == 0xBC) val = cpu.H;
                else if (opcode == 0xBD) val = cpu.L;
                else val = cpu.A;
                
                uint8_t result = cpu.A - val;
                cpu.flagZ = (result == 0) ? 1 : 0;
                cpu.flagN = 1;
                cpu.flagH = ((cpu.A & 0x0F) < (val & 0x0F)) ? 1 : 0;
                cpu.flagC = (cpu.A < val) ? 1 : 0;
                cpu.cycles = 4;
                break;
            }

            // === PILA (STACK) ===
            // PUSH rr
            case 0xC5: cpu.SP -= 2; writeMem16(cpu.SP, cpu.getBC()); cpu.cycles = 16; break;
            case 0xD5: cpu.SP -= 2; writeMem16(cpu.SP, cpu.getDE()); cpu.cycles = 16; break;
            case 0xE5: cpu.SP -= 2; writeMem16(cpu.SP, cpu.getHL()); cpu.cycles = 16; break;
            case 0xF5: cpu.SP -= 2; writeMem16(cpu.SP, cpu.getAF()); cpu.cycles = 16; break;

            // POP rr
            case 0xC1: cpu.setBC(readMem16(cpu.SP)); cpu.SP += 2; cpu.cycles = 12; break;
            case 0xD1: cpu.setDE(readMem16(cpu.SP)); cpu.SP += 2; cpu.cycles = 12; break;
            case 0xE1: cpu.setHL(readMem16(cpu.SP)); cpu.SP += 2; cpu.cycles = 12; break;
            case 0xF1: cpu.setAF(readMem16(cpu.SP)); cpu.SP += 2; cpu.cycles = 12; break;

            // === SALTOS ABSOLUTOS ===
            // JP u16 (salto incondicional)
            case 0xC3: {
                uint16_t addr = readMem16(cpu.PC + 1);
                cpu.PC = addr;
                increment = 0;
                cpu.cycles = 16;
                break;
            }

            // JP (HL)
            case 0xE9: {
                cpu.PC = cpu.getHL();
                increment = 0;
                cpu.cycles = 4;
                break;
            }

            // JP Z, u16 / JP NZ, u16 / JP C, u16 / JP NC, u16
            case 0xCA: {
                uint16_t addr = readMem16(cpu.PC + 1);
                if (cpu.flagZ) {
                    cpu.PC = addr;
                    increment = 0;
                } else {
                    increment = 3;
                }
                cpu.cycles = 16;
                break;
            }
            case 0xC2: {
                uint16_t addr = readMem16(cpu.PC + 1);
                if (!cpu.flagZ) {
                    cpu.PC = addr;
                    increment = 0;
                } else {
                    increment = 3;
                }
                cpu.cycles = 16;
                break;
            }
            case 0xDA: {
                uint16_t addr = readMem16(cpu.PC + 1);
                if (cpu.flagC) {
                    cpu.PC = addr;
                    increment = 0;
                } else {
                    increment = 3;
                }
                cpu.cycles = 16;
                break;
            }
            case 0xD2: {
                uint16_t addr = readMem16(cpu.PC + 1);
                if (!cpu.flagC) {
                    cpu.PC = addr;
                    increment = 0;
                } else {
                    increment = 3;
                }
                cpu.cycles = 16;
                break;
            }

            // CALL cc, u16
            case 0xC4: // CALL NZ, u16
            case 0xCC: // CALL Z, u16
            case 0xD4: // CALL NC, u16
            case 0xDC: // CALL C, u16
            {
                uint16_t addr = readMem16(cpu.PC + 1);
                bool condition = (opcode == 0xC4) ? !cpu.flagZ :
                                 (opcode == 0xCC) ? cpu.flagZ :
                                 (opcode == 0xD4) ? !cpu.flagC : cpu.flagC;
                if (condition) {
                    cpu.SP -= 2;
                    writeMem16(cpu.SP, cpu.PC + 3);
                    cpu.PC = addr;
                    increment = 0;
                    cpu.cycles = 24;
                } else {
                    increment = 3;
                    cpu.cycles = 12;
                }
                break;
            }

            // CALL u16
            case 0xCD: {
                uint16_t addr = readMem16(cpu.PC + 1);
                cpu.SP -= 2;
                writeMem16(cpu.SP, cpu.PC + 3);
                cpu.PC = addr;
                increment = 0;
                cpu.cycles = 24;
                break;
            }

            // === RETORNOS ===
            case 0xD9:
                cpu.PC = readMem16(cpu.SP);
                cpu.SP += 2;
                cpu.ime = true; // Habilita interrupciones inmediatamente
                increment = 0;
                cpu.cycles = 16;
                break;

            // RET
            case 0xC9:
                cpu.PC = readMem16(cpu.SP);
                cpu.SP += 2;
                increment = 0;
                cpu.cycles = 16;
                break;

            // RET cc
            case 0xC0: // RET NZ
            case 0xC8: // RET Z
            case 0xD0: // RET NC
            case 0xD8: // RET C
            {
                bool condition = (opcode == 0xC0) ? !cpu.flagZ :
                                 (opcode == 0xC8) ? cpu.flagZ :
                                 (opcode == 0xD0) ? !cpu.flagC : cpu.flagC;
                if (condition) {
                    cpu.PC = readMem16(cpu.SP);
                    cpu.SP += 2;
                    increment = 0;
                    cpu.cycles = 20;
                } else {
                    cpu.cycles = 8;
                }
                break;
            }

            // HALT
            case 0x76:
                cpu.halted = true;
                cpu.cycles = 4;
                break;

            // === AJUSTES ESPECIALES ===
            // DAA (Decimal Adjust Accumulator)
            case 0x27: {
                uint8_t reg = cpu.A;
                uint8_t correction = 0;
                if (!cpu.flagN) {
                    if (cpu.flagC || reg > 0x99) {
                        correction |= 0x60;
                        cpu.flagC = 1;
                    }
                    if (cpu.flagH || (reg & 0x0F) > 0x09) {
                        correction |= 0x06;
                    }
                } else {
                    if (cpu.flagC) correction |= 0x60;
                    if (cpu.flagH) correction |= 0x06;
                }
                if (cpu.flagN) cpu.A -= correction;
                else cpu.A += correction;
                
                cpu.flagZ = (cpu.A == 0);
                cpu.flagH = 0;
                cpu.cycles = 4;
                break;
            }

            // CPL (Complement A)
            case 0x2F:
                cpu.A = ~cpu.A;
                cpu.flagN = 1;
                cpu.flagH = 1;
                cpu.cycles = 4;
                break;

            // SCF (Set Carry Flag)
            case 0x37: cpu.flagC = 1; cpu.flagN = 0; cpu.flagH = 0; cpu.cycles = 4; break;

            // CCF (Complement Carry Flag)
            case 0x3F: 
                cpu.flagH = 0; // En GB, CCF siempre limpia H
                cpu.flagC ^= 1; 
                cpu.flagN = 0; 
                cpu.cycles = 4; 
                break;

            // ADD A, u8
            case 0xC6: {
                uint8_t val = readMem(cpu.PC + 1);
                uint16_t result = cpu.A + val;
                cpu.flagH = ((cpu.A & 0x0F) + (val & 0x0F) > 0x0F) ? 1 : 0;
                cpu.flagC = (result > 0xFF) ? 1 : 0;
                cpu.A = result & 0xFF;
                cpu.flagZ = (cpu.A == 0) ? 1 : 0;
                cpu.flagN = 0;
                increment = 2;
                cpu.cycles = 8;
                break;
            }

            // ADC A, u8
            case 0xCE: {
                uint8_t val = readMem(cpu.PC + 1);
                uint8_t c = cpu.flagC;
                uint16_t full_res = cpu.A + val + c;
                cpu.flagH = ((cpu.A & 0x0F) + (val & 0x0F) + c) > 0x0F;
                cpu.flagC = full_res > 0xFF;
                cpu.A = full_res & 0xFF;
                cpu.flagZ = (cpu.A == 0) ? 1 : 0;
                cpu.flagN = 0;
                increment = 2;
                cpu.cycles = 8;
                break;
            }

            // SBC A, u8
            case 0xDE: {
                uint8_t val = readMem(cpu.PC + 1);
                uint8_t c = cpu.flagC;
                int res = cpu.A - val - c;
                cpu.flagH = (static_cast<int>(cpu.A & 0x0F) - static_cast<int>(val & 0x0F) - static_cast<int>(c)) < 0;
                cpu.flagC = res < 0;
                cpu.A = res & 0xFF;
                cpu.flagZ = (cpu.A == 0);
                cpu.flagN = 1;
                increment = 2;
                cpu.cycles = 8;
                break;
            }

            // SUB u8
            case 0xD6: {
                uint8_t val = readMem(cpu.PC + 1);
                uint8_t result = cpu.A - val;
                cpu.flagH = ((cpu.A & 0x0F) < (val & 0x0F)) ? 1 : 0;
                cpu.flagC = (cpu.A < val) ? 1 : 0;
                cpu.A = result;
                cpu.flagZ = (cpu.A == 0) ? 1 : 0;
                cpu.flagN = 1;
                increment = 2;
                cpu.cycles = 8;
                break;
            }

            // === LÓGICA DE BITS ===
            // AND u8
            case 0xE6: {
                uint8_t val = readMem(cpu.PC + 1);
                cpu.A &= val;
                cpu.flagZ = (cpu.A == 0) ? 1 : 0;
                cpu.flagN = 0;
                cpu.flagH = 1;
                cpu.flagC = 0;
                increment = 2;
                cpu.cycles = 8;
                break;
            }

            // OR u8
            case 0xF6: {
                uint8_t val = readMem(cpu.PC + 1);
                cpu.A |= val;
                cpu.flagZ = (cpu.A == 0) ? 1 : 0;
                cpu.flagN = 0;
                cpu.flagH = 0;
                cpu.flagC = 0;
                increment = 2;
                cpu.cycles = 8;
                break;
            }

            // XOR u8
            case 0xEE: {
                uint8_t val = readMem(cpu.PC + 1);
                cpu.A ^= val;
                cpu.flagZ = (cpu.A == 0) ? 1 : 0;
                cpu.flagN = 0;
                cpu.flagH = 0;
                cpu.flagC = 0;
                increment = 2;
                cpu.cycles = 8;
                break;
            }

            // === IO Y LDH ===
            // LD (C), A / LD A, (C) / ADD SP, n / LD HL, SP+n
            case 0xE2: writeMem(0xFF00 + cpu.C, cpu.A); cpu.cycles = 8; break; 
            case 0xF2: cpu.A = readMem(0xFF00 + cpu.C); cpu.cycles = 8; break;
            case 0xE8: {
                int8_t offset = (int8_t)readMem(cpu.PC + 1);
                cpu.flagH = ((cpu.SP & 0xF) + (offset & 0xF) > 0xF);
                cpu.flagC = ((cpu.SP & 0xFF) + (offset & 0xFF) > 0xFF);
                cpu.SP += offset;
                cpu.flagZ = 0; cpu.flagN = 0;
                increment = 2; cpu.cycles = 16;
                break;
            }
            case 0xF8: {
                int8_t offset = (int8_t)readMem(cpu.PC + 1);
                cpu.flagH = ((cpu.SP & 0xF) + (offset & 0xF) > 0xF);
                cpu.flagC = ((cpu.SP & 0xFF) + (offset & 0xFF) > 0xFF);
                cpu.setHL(cpu.SP + offset);
                cpu.flagZ = 0; cpu.flagN = 0;
                increment = 2; cpu.cycles = 12;
                break;
            }
            case 0xF9: cpu.SP = cpu.getHL(); cpu.cycles = 8; break;

            // LD (u16), A
            case 0xEA: {
                uint16_t addr = readMem16(cpu.PC + 1);
                writeMem(addr, cpu.A);
                increment = 3;
                cpu.cycles = 16;
                break;
            }

            // LD A, (HL-)
            case 0x3A: {
                cpu.A = readMem(cpu.getHL());
                cpu.setHL(cpu.getHL() - 1);
                cpu.cycles = 8;
                break;
            }

            // LDH (n), A
            case 0xE0: {
                writeMem(0xFF00 + readMem(cpu.PC + 1), cpu.A);
                increment = 2;
                cpu.cycles = 12;
                break;
            }

            // LDH A, (n)
            case 0xF0: {
                cpu.A = readMem(0xFF00 + readMem(cpu.PC + 1));
                increment = 2;
                cpu.cycles = 12;
                break;
            }

            // LD A, (u16)
            case 0xFA: {
                uint16_t addr = readMem16(cpu.PC + 1);
                cpu.A = readMem(addr);
                increment = 3;
                cpu.cycles = 16;
                break;
            }

            // === PREFIJO CB (INSTRUCCIONES DE BITS) ===
            // Instrucciones Prefixed 0xCB
            case 0xCB: {
                uint8_t cb_opcode = readMem(cpu.PC + 1);
                increment = 2;
                uint8_t reg_idx = cb_opcode & 0x07;
                uint8_t* regs[] = {&cpu.B, &cpu.C, &cpu.D, &cpu.E, &cpu.H, &cpu.L, nullptr, &cpu.A};
                uint8_t val = (reg_idx == 6) ? readMem(cpu.getHL()) : *regs[reg_idx];
                
                if (cb_opcode < 0x40) { 
                    uint8_t type = (cb_opcode >> 3) & 0x07;
                    uint8_t res = 0;
                    switch (type) {
                        case 0: { uint8_t c = val >> 7; res = (val << 1) | c; cpu.flagC = c; break; }
                        case 1: { uint8_t c = val & 1; res = (val >> 1) | (c << 7); cpu.flagC = c; break; }
                        case 2: { uint8_t c = val >> 7; res = (val << 1) | cpu.flagC; cpu.flagC = c; break; }
                        case 3: { uint8_t c = val & 1; res = (val >> 1) | (cpu.flagC << 7); cpu.flagC = c; break; }
                        case 4: { cpu.flagC = val >> 7; res = val << 1; break; }
                        case 5: { cpu.flagC = val & 1; res = (val >> 1) | (val & 0x80); break; }
                        case 6: { res = (val << 4) | (val >> 4); cpu.flagC = 0; break; }
                        case 7: { cpu.flagC = val & 1; res = val >> 1; break; }
                    }
                    if (reg_idx == 6) writeMem(cpu.getHL(), res); else *regs[reg_idx] = res;

                    cpu.flagZ = (res == 0); cpu.flagN = 0; cpu.flagH = 0;
                    cpu.cycles = (reg_idx == 6) ? 16 : 8;
                } else if (cb_opcode < 0x80) { 
                    uint8_t bit = (cb_opcode >> 3) & 0x07;
                    cpu.flagZ = !(val & (1 << bit));
                    cpu.flagN = 0; cpu.flagH = 1;
                    cpu.cycles = (reg_idx == 6) ? 12 : 8;
                } else { 
                    uint8_t bit = (cb_opcode >> 3) & 0x07;
                    uint8_t res = val;
                    if (cb_opcode < 0xC0) res &= ~(1 << bit); else res |= (1 << bit);
                    if (reg_idx == 6) writeMem(cpu.getHL(), res); 
                    else if (regs[reg_idx]) *regs[reg_idx] = res;
                    cpu.cycles = (reg_idx == 6) ? 16 : 8;
                }
                break;
            }

            // RST - Restart (Call to fixed address)
            case 0xC7: case 0xCF: case 0xD7: case 0xDF: case 0xE7: case 0xEF: case 0xF7: case 0xFF: {
                uint16_t target = (opcode & 0x38);
                cpu.SP -= 2;
                writeMem16(cpu.SP, cpu.PC + 1);
                cpu.PC = target;
                increment = 0;
                cpu.cycles = 16;
                break;
            }

            // CP u8
            case 0xFE: {
                uint8_t val = readMem(cpu.PC + 1);
                uint8_t result = cpu.A - val;
                cpu.flagZ = (result == 0) ? 1 : 0;
                cpu.flagN = 1;
                cpu.flagH = ((cpu.A & 0x0F) < (val & 0x0F)) ? 1 : 0;
                cpu.flagC = (cpu.A < val) ? 1 : 0;
                increment = 2;
                cpu.cycles = 8;
                break;
            }

            // DI (Disable Interrupts)
            case 0xF3:
                cpu.ime = false;
                cpu.ei_delay = 0;
                cpu.cycles = 4;
                break;
            // EI (Enable Interrupts)
            case 0xFB:
                cpu.ei_delay = 2; // Se habilitarán tras la siguiente instrucción
                cpu.cycles = 4;
                break;

            // Instrucciones sin implementar
            default: {
                // Log unsupported opcode
                bool found = false;
                for (uint8_t op : unsupported_opcodes) {
                    if (op == opcode) {
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    unsupported_opcodes.push_back(opcode);
                    std::cerr << "\n[UNSUPPORTED OPCODE: 0x" << std::hex << (int)opcode << std::dec 
                              << " at PC=0x" << std::hex << cpu.PC << std::dec << "]\n";
                }
                cpu.cycles = 4;
                break;
            }
        }

        cpu.PC += increment;

        // Lógica de retraso de EI
        if (cpu.ei_delay > 0) {
            cpu.ei_delay--;
            if (cpu.ei_delay == 0) {
                cpu.ime = true;
            }
        }
    }

public:
    GameBoy() : memory(0x10000, 0), current_bank(1), joypad_buttons(0x0F), joypad_directions(0x0F) {}

    bool cargar_rom(const std::string& ruta_archivo) {
        std::ifstream archivo(ruta_archivo, std::ios::binary | std::ios::ate);
        if (!archivo.is_open()) return false;
        std::streamsize tamano = archivo.tellg();
        archivo.seekg(0, std::ios::beg);
        rom.resize(static_cast<size_t>(tamano));
        return (bool)archivo.read(reinterpret_cast<char*>(rom.data()), tamano);
    }

    void ejecutar() {
        bool corriendo = true;
        uint64_t instrucciones = 0;
        const double ms_per_frame = 1000.0 / 59.73;
        uint64_t frame_start_time = SDL_GetTicks();
        const int CYCLES_PER_FRAME = 70224; // 154 líneas * 456 ciclos
        frame_cycles = 0;

        while (corriendo) {
            if (instrucciones % 100 == 0) {
                SDL_Event event;
                while (SDL_PollEvent(&event)) {
                    if (event.type == SDL_EVENT_QUIT) corriendo = false;
                    else if (event.type == SDL_EVENT_KEY_DOWN) {
                        if (event.key.key == SDLK_RETURN && (event.key.mod & SDL_KMOD_ALT)) {
                            ppu.toggle_fullscreen();
                        } else {
                            manejar_teclado(event.key.key, true);
                        }
                    }
                    else if (event.type == SDL_EVENT_KEY_UP) manejar_teclado(event.key.key, false);
                }
            }

            ejecutar_instruccion();
            tick(cpu.cycles);
            manejar_interrupciones();

            if (frame_cycles >= CYCLES_PER_FRAME) {
                frame_cycles -= CYCLES_PER_FRAME;
                uint64_t now = SDL_GetTicks();
                uint64_t elapsed = now - frame_start_time;
                if (elapsed < ms_per_frame) SDL_Delay((uint32_t)(ms_per_frame - elapsed));
                frame_start_time = SDL_GetTicks();
            }
            instrucciones++;
            if (cpu.PC >= 0xFFFF) break;
        }
    }
};

int main(int argc, char* argv[]) {
    GameBoy emulador;
    std::string rom_path = "legend.gb";
    if (argc > 1) rom_path = argv[1];

    if (emulador.cargar_rom(rom_path)) emulador.ejecutar();
    else std::cerr << "ROM Fail\n";
    return 0;
}
