#include "pg/engine.h"
#include <iostream>
#include <string>

int main(int argc, char* argv[]) {
    std::string db_prefix = "pg_cli_data";
    if (argc > 1) {
        db_prefix = argv[1];
    }

    std::cout << R"(
======================================================================
  ____  ____        ____  ___  ____ _____ ____ ____  _____ ____  
 / ___||  _ \ _ __ |  _ \/ _ \/ ___|_   _/ ___|  _ \| ____/ ___| 
| |    | |_) | '_ \| |_) | | | \___ \ | || |  _| |_) |  _| \___ \ 
| |___ |  __/| |_) |  __/| |_| |___) || || |_| |  _ <| |___ ___) |
 \____||_|   | .__/|_|    \___/|____/ |_| \____|_| \_\_____|____/ 
             |_|                                                  
       Toy PostgreSQL Storage Engine & Interactive SQL REPL
======================================================================
Type 'HELP;' for a list of commands, or 'EXIT;' to quit.
Database: )" << db_prefix << "\n\n";

    try {
        pg::Engine engine(db_prefix);
        std::string line;

        while (true) {
            if (engine.is_in_transaction()) {
                std::cout << "pg_cli (tx " << engine.current_tx_id() << ")> " << std::flush;
            } else {
                std::cout << "pg_cli> " << std::flush;
            }

            if (!std::getline(std::cin, line)) {
                break;
            }

            // Check exit
            std::string clean = line;
            size_t first = clean.find_first_not_of(" \t\r\n");
            if (first != std::string::npos) {
                size_t last = clean.find_last_not_of(" \t\r\n;");
                clean = clean.substr(first, (last - first + 1));
            }
            if (clean == "exit" || clean == "EXIT" || clean == "quit" || clean == "QUIT") {
                std::cout << "Goodbye.\n";
                break;
            }

            std::string output = engine.execute(line);
            if (!output.empty()) {
                std::cout << output;
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "Fatal engine error: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}
