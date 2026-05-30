#include <iostream>   // Para salida de errores y logs en consola
#include <vector>     // Para manejar la memoria y la ROM de forma dinámica
#include <fstream>    // Para leer el archivo binario de la ROM
#include <string>     // Para manejar rutas de archivos
#include <cstdint>    // Para tipos de datos con tamaño exacto (uint8_t, uint16_t)
#include <iomanip>    // Para formatear números hexadecimales en la depuración
#include <SDL3/SDL.h> // Librería gráfica para la ventana y el audio

// Dimensiones de la pantalla original de la Game Boy (160x144 píxeles)
const int SCREEN_WIDTH = 160;
const int SCREEN_HEIGHT = 144;

// Direcciones de memoria de los registros de Entrada/Salida (I/O)
// Estos registros controlan el hardware (pantalla, botones, reloj, etc.)
enum IORegisters : uint16_t {
    REG_JOYP = 0xFF00, // Control de botones
    REG_DIV  = 0xFF04, // Registro divisor (incrementa a 16384Hz)
    REG_TIMA = 0xFF05, // Contador del Timer
    REG_TMA  = 0xFF06, // Valor de recarga del Timer
    REG_TAC  = 0xFF07, // Control del Timer (frecuencia y encendido)
    REG_IF   = 0xFF0F, // Flags de Interrupción (indica qué interrupción ocurrió)
    REG_LCDC = 0xFF40, // Control del LCD (encendido, capas, tamaño de sprites)
    REG_STAT = 0xFF41, // Estado del LCD (modos de la PPU)
    REG_SCY  = 0xFF42, // Scroll Y del fondo
    REG_SCX  = 0xFF43, // Scroll X del fondo
    REG_LY   = 0xFF44, // Scanline actual (0 a 153)
    REG_LYC  = 0xFF45, // Comparador de LY (dispara interrupciones STAT)
    REG_DMA  = 0xFF46, // Transferencia directa a memoria de sprites (OAM)
    REG_BGP  = 0xFF47, // Paleta del fondo
    REG_OBP0 = 0xFF48, // Paleta de sprites 0
    REG_OBP1 = 0xFF49, // Paleta de sprites 1
    REG_WY   = 0xFF4A, // Posición Y de la capa Ventana
    REG_WX   = 0xFF4B, // Posición X de la capa Ventana
    REG_IE   = 0xFFFF  // Habilitación de Interrupciones (indica cuáles procesar)
};

// ===================== CLASE CPU (LR35902) =====================
// El procesador de la Game Boy, una mezcla entre el Z80 y el Intel 8080.
class CPU {
public:
    // Registros generales de 8 bits
    uint8_t A, B, C, D, E, H, L;
    // Registros de control de 16 bits
    uint16_t SP, PC; // Stack Pointer y Program Counter
    // Registros de estado (Flags)
    uint8_t flagZ, flagN, flagH, flagC; // Zero, Negative, Half-Carry, Carry
    
    int cycles;   // Almacena cuántos ciclos consumió la última instrucción
    bool halted;  // Si la CPU está en modo de bajo consumo (instrucción HALT)
    bool ime;     // Interrupt Master Enable (habilitador maestro de interrupciones)

    // El constructor inicializa la CPU con los valores que tendría después
    // de ejecutar la BIOS interna de Nintendo (arranque en 0x0100).
    CPU() : A(0x01), B(0), C(0x13), D(0), E(0xD8), H(0x01), L(0x4D), 
            SP(0xFFFE), PC(0x0100), 
            flagZ(1), flagN(0), flagH(1), flagC(1),
            cycles(0), halted(false), ime(false) {}

    // Helper para obtener el par HL como una dirección de 16 bits
    uint16_t getHL() const { return (static_cast<uint16_t>(H) << 8) | L; }
    // Helper para establecer el par HL desde un valor de 16 bits
    void setHL(uint16_t val) { H = (val >> 8) & 0xFF; L = val & 0xFF; }
};

// ===================== CLASE PPU (PICTURE PROCESSING UNIT) =====================
// Encargada de generar la imagen píxel por píxel.
class PPU {
private:
    SDL_Window* window;
    SDL_Renderer* renderer;
    SDL_Texture* texture;
    uint32_t pixels[SCREEN_WIDTH * SCREEN_HEIGHT];

public:
    uint8_t ly, stat, lcdc, scy, scx; // Registros espejo de video
    int cycles_count; // Contador para saber cuándo terminar una línea (456 ciclos)

    PPU() : ly(0), stat(0), lcdc(0x91), scy(0), scx(0), cycles_count(456) {
        // Inicialización de SDL para mostrar la ventana del juego
        SDL_Init(SDL_INIT_VIDEO);
        window = SDL_CreateWindow("GB Template", SCREEN_WIDTH * 3, SCREEN_HEIGHT * 3, 0);
        renderer = SDL_CreateRenderer(window, NULL);
        texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, SCREEN_WIDTH, SCREEN_HEIGHT);
        // Mantenemos la nitidez de los píxeles (Nearest Neighbor)
        SDL_SetRenderLogicalPresentation(renderer, SCREEN_WIDTH, SCREEN_HEIGHT, SDL_LOGICAL_PRESENTATION_LETTERBOX);
    }

    ~PPU() {
        // Limpieza de recursos al cerrar el programa
        SDL_DestroyTexture(texture);
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
    }

    // Se llama después de cada instrucción de la CPU para avanzar el tiempo de video
    void update(int cycles, std::vector<uint8_t>& memory) {
        cycles_count -= cycles;
        if (cycles_count <= 0) {
            ly++; // Avanzamos a la siguiente línea de la pantalla
            cycles_count = 456; // Una línea tarda exactamente 456 ciclos T-states
            
            if (ly == 144) memory[REG_IF] |= 0x01; // Entramos en VBlank (Interrupción bit 0)
            
            if (ly > 153) {
                ly = 0; // Reiniciamos la pantalla después de 154 líneas
                // Dibujamos el búfer acumulado en la ventana de SDL
                SDL_UpdateTexture(texture, NULL, pixels, SCREEN_WIDTH * sizeof(uint32_t));
                SDL_RenderClear(renderer);
                SDL_RenderTexture(renderer, texture, NULL, NULL);
                SDL_RenderPresent(renderer);
            }
        }
    }
};

// ===================== CLASE GAMEBOY (BUS DE DATOS) =====================
// Actúa como la placa base, conectando CPU, PPU y Memoria.
class GameBoy {
private:
    std::vector<uint8_t> memory; // Memoria interna (WRAM, VRAM, Registros)
    std::vector<uint8_t> rom;    // Cartucho del juego
    
    CPU cpu;
    PPU ppu;

    // Lee un byte de la dirección especificada (Mapa de Memoria)
    uint8_t readMem(uint16_t addr) {
        // 0x0000 - 0x7FFF: Cartucho (ROM)
        if (addr <= 0x7FFF) return rom[addr % rom.size()];
        // Registro LY (Scanline actual) está en la PPU
        if (addr == REG_LY) return ppu.ly;
        return memory[addr];
    }

    // Escribe un byte en la dirección especificada
    void writeMem(uint16_t addr, uint8_t value) {
        if (addr >= 0x8000) memory[addr] = value;
        // DMA (Direct Memory Access): Copia masiva de datos para los Sprites
        if (addr == REG_DMA) {
            uint16_t src = value << 8;
            for(int i=0; i<0xA0; i++) memory[0xFE00+i] = readMem(src+i);
        }
    }

    // Ciclo de Fetch-Decode-Execute (Obtener, Decodificar y Ejecutar instrucción)
    void ejecutar_instruccion() {
        uint8_t opcode = readMem(cpu.PC); // Fetch
        uint8_t increment = 1;

        // Decode (Traducir qué debe hacer la CPU)
        switch (opcode) {
            case 0x00: cpu.cycles = 4; break; // NOP
            
            // TODO: Implementar el resto de los 256 opcodes aquí
            
            default:
                std::cout << "Opcode no soportado: 0x" << std::hex << (int)opcode << std::endl;
                cpu.halted = true;
                break;
        }
        cpu.PC += increment; // Avanzar el contador de programa
    }

    // Verifica si hay interrupciones pendientes (VBlank, LCD, Timer, Serial, Joypad)
    void manejar_interrupciones() {
        uint8_t pending = readMem(REG_IE) & readMem(REG_IF); // Solo las habilitadas y ocurridas
        if (pending && cpu.ime) {
            // TODO: Salto a vectores de interrupción ($40, $48, $50, $58, $60)
        }
    }

public:
    GameBoy() : memory(0x10000, 0) {} // Inicializamos 64KB de RAM

    // Carga el archivo .gb en el vector de la ROM
    bool cargar_rom(const std::string& path) {
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file.is_open()) return false;
        std::streamsize size = file.tellg();
        file.seekg(0, std::ios::beg);
        rom.resize(size);
        return (bool)file.read(reinterpret_cast<char*>(rom.data()), size);
    }

    // El bucle infinito donde ocurre la magia
    void ejecutar() {
        bool corriendo = true;
        // Sincronización para correr a ~60 FPS (Frecuencia real: 59.73Hz)
        const double ms_per_frame = 1000.0 / 59.73;
        uint64_t frame_start = SDL_GetTicks();
        int frame_cycles = 0;

        while (corriendo) {
            SDL_Event event; // Manejo de eventos del sistema operativo (cerrar ventana)
            while (SDL_PollEvent(&event)) {
                if (event.type == SDL_EVENT_QUIT) corriendo = false;
            }

            if (!cpu.halted) {
                ejecutar_instruccion(); // Paso 1: CPU
                int c = cpu.cycles;
                ppu.update(c, memory);  // Paso 2: Pantalla
                manejar_interrupciones(); // Paso 3: Interrupts
                frame_cycles += c;
            }

            // Si ya procesamos los ciclos de un frame completo (70,224 ciclos T-states)
            // limitamos la velocidad para que no corra demasiado rápido.
            if (frame_cycles >= 70224) {
                frame_cycles -= 70224;
                uint64_t now = SDL_GetTicks();
                uint64_t elapsed = now - frame_start;
                
                // Si terminamos el frame antes de los 16.7ms, esperamos
                if (elapsed < ms_per_frame) SDL_Delay((uint32_t)(ms_per_frame - elapsed));
                frame_start = SDL_GetTicks();
            }
        }
    }
};

int main(int argc, char* argv[]) { // Punto de entrada del programa
    if (argc < 2) {
        std::cout << "Uso: emu.exe juego.gb" << std::endl;
        return 1;
    }

    GameBoy gb; // Creamos la instancia del emulador
    
    // Intentamos cargar el archivo pasado por consola
    std::string rom_path = "01-special.gb";
    if (argc > 1) rom_path = argv[1];
    if (gb.cargar_rom(rom_path)) {
        gb.ejecutar(); // Iniciamos el bucle
    } else {
        std::cerr << "Error cargando ROM" << std::endl;
    }

    return 0;
}