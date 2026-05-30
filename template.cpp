#include <iostream>   // Para logs en consola
#include <vector>     // Para memoria dinámica
#include <fstream>    // Para cargar el archivo .gb
#include <string>     // Para rutas de archivos
#include <cstdint>    // Tipos estándar (uint8_t, uint16_t)
#include <iomanip>    // Formateo hexadecimal
#include <SDL3/SDL.h> // Gráficos y Ventana

// --- CONSTANTES DEL SISTEMA ---
const int SCREEN_WIDTH = 160;
const int SCREEN_HEIGHT = 144;
const int CYCLES_PER_FRAME = 70224; // Ciclos que la Game Boy ejecuta en 1/60 seg

// --- MAPA DE REGISTROS I/O (Input/Output) ---
// Estos registros son la interfaz entre el software y el hardware real.
enum IORegisters : uint16_t {
    REG_JOYP = 0xFF00, // Botones
    REG_SB   = 0xFF01, // Serial Transfer Data (Blargg escribe aquí el texto)
    REG_SC   = 0xFF02, // Serial Transfer Control (Dispara el envío de texto)
    REG_IF   = 0xFF0F, // Interrupt Flag (Indica qué interrupción se pide)
    REG_LCDC = 0xFF40, // Control de Pantalla
    REG_STAT = 0xFF41, // Estado de Pantalla
    REG_LY   = 0xFF44, // Línea actual que se está dibujando
    REG_DMA  = 0xFF46, // Copia rápida de Sprites
    REG_IE   = 0xFFFF  // Interrupt Enable (Habilita qué interrupciones procesar)
};

// =============================================================================
// CLASE CPU: El cerebro que procesa instrucciones
// =============================================================================
class CPU {
public:
    // Registros de 8 bits
    uint8_t A, B, C, D, E, H, L;
    // Registros de 16 bits
    uint16_t SP, PC; 
    // Banderas de estado
    uint8_t flagZ, flagN, flagH, flagC;
    
    int cycles;  // Ciclos consumidos por la instrucción actual
    bool halted; // Si la CPU está esperando una interrupción
    bool ime;    // Interrupt Master Enable

    // Valores iniciales tras el arranque (Post-BIOS)
    CPU() : A(0x01), B(0), C(0x13), D(0), E(0xD8), H(0x01), L(0x4D), 
            SP(0xFFFE), PC(0x0100), 
            flagZ(1), flagN(0), flagH(1), flagC(1),
            cycles(0), halted(false), ime(false) {}

    // Helpers para manejar registros combinados de 16 bits
    uint16_t getBC() const { return (B << 8) | C; }
    uint16_t getDE() const { return (D << 8) | E; }
    uint16_t getHL() const { return (H << 8) | L; }
    uint16_t getAF() const { return (A << 8) | (flagZ << 7 | flagN << 6 | flagH << 5 | flagC << 4); }

          // Setear flags basado en operaciones (Re-agregado para XOR/OR)
    void setFlagsZN(uint8_t result) {
        flagZ = (result == 0) ? 1 : 0;
        flagN = 0;
        flagH = 0;
    }
    void setBC(uint16_t val) { B = val >> 8; C = val & 0xFF; }
    void setDE(uint16_t val) { D = val >> 8; E = val & 0xFF; }
    void setHL(uint16_t val) { H = val >> 8; L = val & 0xFF; }
    void setAF(uint16_t val) { 
        A = val >> 8; 
        uint8_t f = val & 0xF0; // Los 4 bits bajos de F siempre son 0 en GB
        flagZ = (f >> 7) & 1; flagN = (f >> 6) & 1; flagH = (f >> 5) & 1; flagC = (f >> 4) & 1;
    }
};

// =============================================================================
// CLASE PPU: Generador de imagen
// =============================================================================
class PPU {
private:
    SDL_Window* window;
    SDL_Renderer* renderer;
    SDL_Texture* texture;
    uint32_t pixels[SCREEN_WIDTH * SCREEN_HEIGHT];

public:
    uint8_t ly;        // Línea actual
    int cycles_count;  // Sincronización con la CPU

    PPU() : ly(0), cycles_count(456) {
        SDL_Init(SDL_INIT_VIDEO);
        window = SDL_CreateWindow("GB Template", SCREEN_WIDTH * 3, SCREEN_HEIGHT * 3, 0);
        renderer = SDL_CreateRenderer(window, NULL);
        texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, SCREEN_WIDTH, SCREEN_HEIGHT);
        SDL_SetRenderLogicalPresentation(renderer, SCREEN_WIDTH, SCREEN_HEIGHT, SDL_LOGICAL_PRESENTATION_LETTERBOX);
    }

    ~PPU() {
        SDL_DestroyTexture(texture); SDL_DestroyRenderer(renderer); SDL_DestroyWindow(window); SDL_Quit();
    }

    void update(int cycles, std::vector<uint8_t>& memory) {
        cycles_count -= cycles;
        if (cycles_count <= 0) {
            ly++; // Siguiente línea
            cycles_count = 456; // Una línea tarda 456 ciclos
            
            if (ly == 144) memory[REG_IF] |= 0x01; // Disparar VBlank
            
            if (ly > 153) {
                ly = 0; // Fin de frame
                SDL_UpdateTexture(texture, NULL, pixels, SCREEN_WIDTH * 4);
                SDL_RenderClear(renderer);
                SDL_RenderTexture(renderer, texture, NULL, NULL);
                SDL_RenderPresent(renderer);
            }
        }
    }
};

// =============================================================================
// CLASE GAMEBOY: El Bus de datos y lógica principal
// =============================================================================
class GameBoy {
private:
    std::vector<uint8_t> memory; // 64KB de RAM total
    std::vector<uint8_t> rom;    // Datos del cartucho
    
    CPU cpu;
    PPU ppu;

    // Lógica del Mapa de Memoria
    uint8_t readMem(uint16_t addr) {
        if (addr <= 0x7FFF) return rom[addr % rom.size()]; // ROM del juego
        if (addr == REG_LY) return ppu.ly;                 // Registro de video
        return memory[addr];                               // Resto de la RAM
    }

    void writeMem(uint16_t addr, uint8_t value) {
        // --- SALIDA SERIAL (Logs de Blargg) ---
        if (addr == REG_SB) memory[REG_SB] = value;
        if (addr == REG_SC && value == 0x81) {
            std::cout << (char)memory[REG_SB] << std::flush;
            memory[REG_SC] = 0; // Limpiar transferencia
            return;
        }

        // --- DMA TRANSFER (Copia de Sprites) ---
        // Vital para Tetris: copia 160 bytes desde ROM/RAM a la memoria de Sprites (OAM)
        if (addr == REG_DMA) {
            uint16_t src = value << 8;
            for(int i = 0; i < 0xA0; i++) memory[0xFE00 + i] = readMem(src + i);
            return;
        }

        // Escritura normal en RAM
        if (addr >= 0x8000) memory[addr] = value;
    }

    // Helpers de 16 bits para saltos y pila
    uint16_t readMem16(uint16_t addr) { return readMem(addr) | (readMem(addr+1) << 8); }
    void writeMem16(uint16_t addr, uint16_t val) { writeMem(addr, val & 0xFF); writeMem(addr+1, val >> 8); }

    // Ciclo de ejecución
    void ejecutar_instruccion() {
        uint8_t opcode = readMem(cpu.PC);
        uint8_t increment = 1; // Cuántos bytes avanzar el PC

        switch (opcode) {
            case 0x00: cpu.cycles = 4; break; // NOP
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
            case 0xC3: {
                uint16_t addr = readMem16(cpu.PC + 1);
                cpu.PC = addr;
                increment = 0;
                cpu.cycles = 16;
                break;
            }
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

            case 0x06: cpu.B = readMem(cpu.PC + 1); increment = 2; cpu.cycles = 8; break;
            case 0x0E: cpu.C = readMem(cpu.PC + 1); increment = 2; cpu.cycles = 8; break;
            case 0x16: cpu.D = readMem(cpu.PC + 1); increment = 2; cpu.cycles = 8; break;
            case 0x1E: cpu.E = readMem(cpu.PC + 1); increment = 2; cpu.cycles = 8; break;
            case 0x26: cpu.H = readMem(cpu.PC + 1); increment = 2; cpu.cycles = 8; break;
            case 0x2E: cpu.L = readMem(cpu.PC + 1); increment = 2; cpu.cycles = 8; break;
            case 0x3E: cpu.A = readMem(cpu.PC + 1); increment = 2; cpu.cycles = 8; break;
            // TODO: Implementar instrucciones aquí (LD, ADD, JP, CALL, etc.)
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

             // DI (Disable Interrupts)
            case 0xF3:
                cpu.ime = false;
               // cpu.ei_delay = 0;
                cpu.cycles = 4;
                break;
            // EI (Enable Interrupts)
            case 0xFB:
                //cpu.ei_delay = 2; // Se habilitarán tras la siguiente instrucción
                cpu.cycles = 4;
                break;

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

             // LD A, (rr)
            case 0x0A: cpu.A = readMem(cpu.getBC()); cpu.cycles = 8; break;
            case 0x1A: cpu.A = readMem(cpu.getDE()); cpu.cycles = 8; break;

            // DEC rr
            case 0x0B: cpu.setBC(cpu.getBC() - 1); cpu.cycles = 8; break;
            case 0x1B: cpu.setDE(cpu.getDE() - 1); cpu.cycles = 8; break;
            case 0x2B: cpu.setHL(cpu.getHL() - 1); cpu.cycles = 8; break;
            case 0x3B: cpu.SP--; cpu.cycles = 8; break;

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

             // === CARGAS DE 16 BITS ===
            // LD (HL-), A
            case 0x32:
                writeMem(cpu.getHL(), cpu.A);
                cpu.setHL(cpu.getHL() - 1);
                cpu.cycles = 8;
                break;
            
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
            case 0xBE: { // CP (HL)
                uint8_t val = readMem(cpu.getHL());
                uint8_t result = cpu.A - val;
                cpu.flagZ = (result == 0) ? 1 : 0;
                cpu.flagN = 1;
                cpu.flagH = ((cpu.A & 0x0F) < (val & 0x0F)) ? 1 : 0;
                cpu.flagC = (cpu.A < val) ? 1 : 0;
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


            
            // === SALTOS ABSOLUTOS ===
            // JP u16 (salto incondicional)

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

            default:
                std::cout << "\nOpcode no soportado: 0x" << std::hex << (int)opcode 
                          << " en PC: 0x" << cpu.PC << std::dec << std::endl;
                cpu.halted = true;
                break;
        }
        cpu.PC += increment;
    }

public:
    GameBoy() : memory(0x10000, 0) {}

    bool cargar_rom(const std::string& path) {
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file.is_open()) return false;
        std::streamsize size = file.tellg();
        file.seekg(0, std::ios::beg);
        rom.resize(size);
        return (bool)file.read(reinterpret_cast<char*>(rom.data()), size);
    }

    void ejecutar() {
        bool corriendo = true;
        const double ms_per_frame = 1000.0 / 59.73;
        uint64_t frame_start = SDL_GetTicks();
        int frame_cycles = 0;

        while (corriendo) {
            SDL_Event event;
            while (SDL_PollEvent(&event)) {
                if (event.type == SDL_EVENT_QUIT) corriendo = false;
            }

            if (!cpu.halted) {
                ejecutar_instruccion();
                int c = cpu.cycles;
                ppu.update(c, memory); // La pantalla avanza con la CPU
                frame_cycles += c;
            }

            // Sincronización de velocidad
            if (frame_cycles >= CYCLES_PER_FRAME) {
                frame_cycles -= CYCLES_PER_FRAME;
                uint64_t now = SDL_GetTicks();
                uint64_t elapsed = now - frame_start;
                if (elapsed < ms_per_frame) SDL_Delay((uint32_t)(ms_per_frame - elapsed));
                frame_start = SDL_GetTicks();
            }
        }
    }
};

// --- PUNTO DE ENTRADA ---
int main(int argc, char* argv[]) {
    std::string rom_path = "01-special.gb"; // Test de Blargg por defecto
    if (argc > 1) rom_path = argv[1];

    GameBoy gb;
    std::cout << "Cargando: " << rom_path << "..." << std::endl;
    if (gb.cargar_rom(rom_path)) {
        gb.ejecutar();
    } else {
        std::cerr << "Error: No se encontro el archivo ROM." << std::endl;
    }

    return 0;
}