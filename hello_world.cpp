#include <iostream>
#include <vector>
#include <fstream>
#include <string>
#include <cstdint>
#include <iomanip>
#include <SDL3/SDL.h>

// Dimensiones de la Game Boy
const int SCREEN_WIDTH = 160;
const int SCREEN_HEIGHT = 144;

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

    CPU() : A(0), B(0), C(0), D(0), E(0), H(0), L(0), 
            SP(0xFFFE), PC(0x0100), 
            flagZ(0), flagN(0), flagH(0), flagC(0),
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

    // Setear flags basado en operaciones
    void setFlagsZN(uint8_t result) {
        flagZ = (result == 0) ? 1 : 0;
        flagN = 0;
        flagH = 0;
    }

    void printState() {
        std::cout << std::hex << std::setfill('0')
                  << "PC=" << std::setw(4) << PC
                  << " SP=" << std::setw(4) << SP
                  << " A=" << std::setw(2) << (int)A
                  << " B=" << std::setw(2) << (int)B
                  << " C=" << std::setw(2) << (int)C
                  << " D=" << std::setw(2) << (int)D
                  << " E=" << std::setw(2) << (int)E
                  << " H=" << std::setw(2) << (int)H
                  << " L=" << std::setw(2) << (int)L
                  << " FLAGS=" << std::setw(1) << (int)flagZ 
                  << std::setw(1) << (int)flagN 
                  << std::setw(1) << (int)flagH 
                  << std::setw(1) << (int)flagC
                  << " Halted=" << (halted ? "Yes" : "No")
                  << std::dec << std::endl;
    }
};

// ===================== CLASE PPU (PICTURE PROCESSING UNIT) =====================
class PPU {
private:
    SDL_Window* window;
    SDL_Renderer* renderer;
    SDL_Texture* texture;
    uint32_t pixels[SCREEN_WIDTH * SCREEN_HEIGHT];

public:
    uint8_t ly;        // Scanline actual (0xFF44)
    uint8_t stat;      // PPU Status (0xFF41)
    uint8_t lcdc;      // LCD Control (0xFF40)
    uint8_t scy, scx;  // Scroll Y, X
    int cycles_count;  // Contador para sincronizar con la CPU

    PPU() : ly(0), stat(0), lcdc(0x91), scy(0), scx(0), cycles_count(456) {
        SDL_Init(SDL_INIT_VIDEO);
        window = SDL_CreateWindow("Gemini GameBoy", SCREEN_WIDTH * 3, SCREEN_HEIGHT * 3, 0);
        renderer = SDL_CreateRenderer(window, NULL);
        texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, SCREEN_WIDTH, SCREEN_HEIGHT);
        SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_NEAREST);
    }

    ~PPU() {
        SDL_DestroyTexture(texture);
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
    }

    void render_scanline(const std::vector<uint8_t>& memory) {
        if (!(lcdc & 0x01)) return; // Background enable bit

        uint16_t tile_map_start = (lcdc & 0x08) ? 0x9C00 : 0x9800;
        uint16_t tile_data_start = (lcdc & 0x10) ? 0x8000 : 0x8800;
        bool unsigned_indices = (lcdc & 0x10);

        uint8_t y_pos = ly + scy;
        uint16_t tile_row = (y_pos / 8) * 32;

        for (int x = 0; x < SCREEN_WIDTH; x++) {
            uint8_t x_pos = x + scx;
            uint16_t tile_col = x_pos / 8;
            uint16_t tile_address = tile_map_start + tile_row + tile_col;
            
            int16_t tile_index;
            if (unsigned_indices) tile_index = memory[tile_address];
            else tile_index = (int8_t)memory[tile_address];

            uint16_t tile_data_address;
            if (unsigned_indices) tile_data_address = tile_data_start + (tile_index * 16);
            else tile_data_address = tile_data_start + ((tile_index + 128) * 16);

            uint8_t line = (y_pos % 8) * 2;
            uint8_t byte1 = memory[tile_data_address + line];
            uint8_t byte2 = memory[tile_data_address + line + 1];

            int color_bit = 7 - (x_pos % 8);
            int color_id = ((byte1 >> color_bit) & 1) | (((byte2 >> color_bit) & 1) << 1);

            // Paleta de grises
            uint32_t colors[] = { 0xFFFFFFFF, 0xFFAAAAAA, 0xFF555555, 0xFF000000 };
            pixels[ly * SCREEN_WIDTH + x] = colors[color_id];
        }
    }

    void update(int cycles, std::vector<uint8_t>& memory) {
        // Si el LCD está apagado, no hacemos nada
        if (!(lcdc & 0x80)) {
            ly = 0;
            cycles_count = 456;
            return;
        }

        cycles_count -= cycles;

        // Cada 456 ciclos de la CPU pasamos a la siguiente línea
        if (cycles_count <= 0) {
            if (ly < SCREEN_HEIGHT) render_scanline(memory);
            ly++;
            cycles_count = 456;

            if (ly >= 144) {
                stat = (stat & 0xFC) | 0x01; // Modo 1: VBlank
            } else {
                stat = (stat & 0xFC) | 0x02; // Modo simple: OAM/Transfer
            }

            if (ly > 153) {
                ly = 0;
                // Actualizar pantalla al final del frame
                SDL_UpdateTexture(texture, NULL, pixels, SCREEN_WIDTH * sizeof(uint32_t));
                SDL_RenderClear(renderer);
                SDL_RenderTexture(renderer, texture, NULL, NULL);
                SDL_RenderPresent(renderer);
                memory[0xFF0F] |= 0x01; // Interrupción VBlank
            }
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
    
    // Componentes
    CPU cpu;
    PPU ppu;
    Timer timer;
    
    // Para debug
    std::vector<uint8_t> unsupported_opcodes;

    // Verifica y muestra los caracteres enviados por el test de Blargg
    void depurar_puerto_serial() {
        // 0xFF02 es el registro SC (Serial Control)
        // Bit 7 = transferencia en progreso, Bit 1 = velocidad
        if ((memory[0xFF02] & 0x80) != 0) { 
            // 0xFF01 es el registro SB (Serial Data)
            char c = static_cast<char>(memory[0xFF01]); 
            
            if (c == '\n') {
                std::cout << std::endl;
            } else if (c >= 32 && c < 127) {
                std::cout << c;
            } else {
                // Otros caracteres de control se muestran en hex pero de forma más limpia
                std::cerr << "[SERIAL: 0x" << std::hex << (int)(uint8_t)c << std::dec << "]";
            }
            std::cout << std::flush;
            
            memory[0xFF02] &= 0x7F; // Limpia el bit de transferencia
        }
    }

    // Lee un valor de 8 bits de memoria
    uint8_t readMem(uint16_t addr) {
        // Redirección de registros de Timer
        if (addr == 0xFF04) return (timer.div_counter >> 8) & 0xFF;
        if (addr == 0xFF05) return timer.tima;
        if (addr == 0xFF06) return timer.tma;
        if (addr == 0xFF07) return timer.tac;

        if (addr == 0xFF41) return ppu.stat;
        // Redirección de registros de la PPU
        if (addr == 0xFF40) return ppu.lcdc;
        if (addr == 0xFF42) return ppu.scy;
        if (addr == 0xFF43) return ppu.scx;
        if (addr == 0xFF44) return ppu.ly; // El test suele leer esto

        // Mapeo de memoria con MBC1 básico
        if (addr <= 0x3FFF) return rom[addr]; // Banco 0 fijo
        if (addr >= 0x4000 && addr <= 0x7FFF) {
            // Banco 1 seleccionable
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
        // Redirección de registros de Timer
        if (addr == 0xFF04) { timer.div_counter = 0; return; }
        if (addr == 0xFF05) { timer.tima = value; return; }
        if (addr == 0xFF06) { timer.tma = value; return; }
        if (addr == 0xFF07) { timer.tac = value; return; }

        if (addr == 0xFF41) { ppu.stat = value; return; }
        // Redirección de registros de la PPU
        if (addr == 0xFF40) { ppu.lcdc = value; return; }
        if (addr == 0xFF42) { ppu.scy = value; return; }
        if (addr == 0xFF43) { ppu.scx = value; return; }
        if (addr == 0xFF44) { return; } // LY es de solo lectura para la CPU

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
        uint8_t ie = readMem(0xFFFF);
        uint8_t if_reg = readMem(0xFF0F);
        uint8_t pending = ie & if_reg;

        if (pending != 0) {
            cpu.halted = false; // Salir de HALT si hay interrupción pendiente
            if (cpu.ime) {
                for (int i = 0; i < 5; i++) {
                    if (pending & (1 << i)) {
                        servir_interrupcion(i);
                        break;
                    }
                }
            }
        }
    }

    // Salta al vector de interrupción correspondiente
    void servir_interrupcion(int bit) {
        cpu.ime = false;
        uint8_t if_reg = readMem(0xFF0F);
        writeMem(0xFF0F, if_reg & ~(1 << bit)); // Limpiar el flag en IF
        
        cpu.SP -= 2;
        writeMem16(cpu.SP, cpu.PC);
        
        uint16_t targets[] = {0x40, 0x48, 0x50, 0x58, 0x60};
        cpu.PC = targets[bit];
        cpu.cycles += 20; // Servir interrupción tarda 20 ciclos
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
            // NOP
            case 0x00:
                cpu.cycles = 4;
                break;

            // STOP
            case 0x10:
                increment = 2; // STOP es seguido por un byte 00
                cpu.cycles = 4;
                break;

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

            // Operaciones Aritméticas con (HL)
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

            // RETI (Return from Interrupt)
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
    GameBoy() : memory(0x10000, 0x00), rom(0x10000, 0x00), current_bank(1), cpu(), ppu(), timer() {}

    // Carga el ROM en la memoria del sistema
    bool cargar_rom(const std::string& ruta_archivo) {
        std::ifstream archivo(ruta_archivo, std::ios::binary | std::ios::ate);
        
        if (!archivo.is_open()) {
            std::cerr << "Error: No se pudo abrir el ROM: " << ruta_archivo << "\n";
            return false;
        }

        std::streamsize tamano = archivo.tellg();
        archivo.seekg(0, std::ios::beg);

        // Evita desbordar los 64KB si el archivo es más grande de lo esperado
        if (tamano > 0x10000) {
            tamano = 0x10000;
        }

        if (!archivo.read(reinterpret_cast<char*>(rom.data()), tamano)) {
            std::cerr << "Error al leer los datos del ROM.\n";
            return false;
        }

        return true;
    }

    // Muestra una representación visual simplificada de los Tiles en consola
    void visualizar_vram() {
        std::cout << "\n--- Visualización de Tiles (VRAM) ---\n";
        // Los tiles están en 0x8000-0x8FFF. Mostramos los primeros 8 tiles.
        for (int t = 0; t < 8; t++) {
            std::cout << "Tile " << t << ":\n";
            for (int y = 0; y < 8; y++) {
                uint16_t addr = 0x8000 + (t * 16) + (y * 2);
                uint8_t byte1 = memory[addr];
                uint8_t byte2 = memory[addr + 1];
                for (int x = 7; x >= 0; x--) {
                    int color = ((byte1 >> x) & 1) | (((byte2 >> x) & 1) << 1);
                    if (color == 0) std::cout << "  ";
                    else if (color == 1) std::cout << ". ";
                    else if (color == 2) std::cout << "x ";
                    else std::cout << "# ";
                }
                std::cout << "\n";
            }
        }
    }

    // Bucle principal de ejecución del emulador
    void ejecutar(bool debug = false) {
        std::cout << "Iniciando emulación del Game Boy...\n";
        std::cout << "Estado inicial de la CPU:\n";
        cpu.printState();
        std::cout << "\n";
        
        bool corriendo = true;
        long long instrucciones_ejecutadas = 0;

        while (corriendo) {
            // Manejo de eventos de SDL
            if (instrucciones_ejecutadas % 1000 == 0) {
                SDL_Event event;
                while (SDL_PollEvent(&event)) {
                    if (event.type == SDL_EVENT_QUIT) corriendo = false;
                }
            }

            ejecutar_instruccion();
            ppu.update(cpu.cycles, memory); // Sincronizamos la PPU con los ciclos usados
            timer.update(cpu.cycles, memory); // Sincronizamos el Timer
            manejar_interrupciones(); // Revisar IF/IE después de cada instrucción
            depurar_puerto_serial();

            instrucciones_ejecutadas++;

            // Limites de salida
            if (cpu.halted) {
              //  visualizar_vram(); // Al detenerse, mostramos qué hay en memoria de video
            }

            if (cpu.PC >= 0xFFFF) {
                std::cout << "\nPrograma Counter fuera de rango\n";
                break;
            }
        }

        std::cout << "\nEstado final de la CPU:\n";
        cpu.printState();
        std::cout << "\nTotal de instrucciones ejecutadas: " << instrucciones_ejecutadas << "\n";
    }

    void printCPUState() {
        cpu.printState();
    }
};

int main(int argc, char* argv[]) {
    GameBoy emulador;

    // ROM por defecto o desde argumentos
    std::string rom_path = "cpu_instrs.gb";
    bool debug = false;

    if (argc > 1) {
        rom_path = argv[1];
    }
    if (argc > 2) {
        debug = (std::string(argv[2]) == "debug");
    }

    std::cout << "Cargando ROM: " << rom_path << "\n";
    if (debug) std::cout << "Debug mode activado\n";
    std::cout << "\n";

    if (emulador.cargar_rom(rom_path)) {
        emulador.ejecutar(debug);
    } else {
        std::cerr << "No se pudo cargar el ROM\n";
        return 1;
    }

    return 0;
}
