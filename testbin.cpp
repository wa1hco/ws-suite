#include <iostream>
#include <fstream>
#include <string>

int main() {
    std::string path = "C:/Users/73w3s/AppData/Local/WSJT-X/callsign_states.bin";
    std::ifstream f(path, std::ios::binary);

    if (!f) {
        std::cerr << "Cannot open BIN file\n";
        return 1;
    }

    uint32_t count = 0;
    f.read(reinterpret_cast<char*>(&count), sizeof(count));
    std::cout << "BIN entry count: " << count << "\n";

    bool found_WA4TED = false;
    bool found_KE9ZZ  = false;

    for (uint32_t i = 0; i < count; ++i) {
        uint8_t len = 0;
        f.read(reinterpret_cast<char*>(&len), 1);

        std::string call(len, '\0');
        f.read(&call[0], len);

        char state[2];
        f.read(state, 2);

        if (call == "WA4TED") {
            found_WA4TED = true;
            std::cout << "FOUND WA4TED, state = " << state[0] << state[1] << "\n";
        }

        if (call == "KE9ZZ") {
            found_KE9ZZ = true;
            std::cout << "FOUND KE9ZZ, state = " << state[0] << state[1] << "\n";
        }
    }

    if (!found_WA4TED) std::cout << "WA4TED NOT FOUND in BIN\n";
    if (!found_KE9ZZ)  std::cout << "KE9ZZ NOT FOUND in BIN\n";

    return 0;
}
