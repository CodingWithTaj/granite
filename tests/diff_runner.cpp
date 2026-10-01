// Reads one SQL statement per line; prints the JSON of its last result per line.
#include <iostream>
#include "../src/database.h"
using namespace granite;
int main() {
    MemFile d, w;
    Database db(d, w, Bugs{}, 8);
    db.autoCheckpointFrames = 40;
    db.open();
    std::string line;
    while (std::getline(std::cin, line)) {
        auto r = db.execute(line);
        std::string err = db.check();
        if (!err.empty()) { std::cout << "{\"corrupt\":" << jsonString(err) << "}\n"; continue; }
        std::cout << (r.empty() ? std::string("{}") : resultJson(r.back())) << "\n";
    }
}
