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
            
            // TODO: Implementar instrucciones aquí (LD, ADD, JP, CALL, etc.)
            
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