#include "client.hpp"

#include <iostream>

int main(int argc, char **argv) {
    std::cout << "[multiclient] target name: " << TARGET_NAME << '\n';
    std::cout << "[multiclient] starting benchmark for target: " << MULTICLIENT_TARGET << '\n';

    run_client(argc, argv);

    std::cout << "[multiclient] benchmark finished" << '\n';
    return 0;
}
