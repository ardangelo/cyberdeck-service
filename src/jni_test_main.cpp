#include "jni/uart_jni_bridge.hpp"
#include <iostream>
#include <iomanip>
#include <csignal>
#include <thread>
#include <chrono>
#include <string>

volatile sig_atomic_t g_stop = 0;

void signal_handler(int sig) {
    std::cout << "\nReceived signal " << sig << ", shutting down..." << std::endl;
    g_stop = 1;
}

void print_hex(const std::vector<uint8_t>& data) {
    for (auto byte : data) {
        std::cout << std::hex << std::setw(2) << std::setfill('0')
                  << static_cast<int>(byte) << " ";
    }
    std::cout << std::dec << std::endl;
}

int main(int argc, char** argv) {
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <device_path> [jar_path]" << std::endl;
        std::cerr << "Example: " << argv[0] << " /dev/ttyS1 /tmp/libpebble3-jvm-fat.jar" << std::endl;
        return 1;
    }

    const char* device_path = argv[1];
    const char* jar_path = argc > 2 ? argv[2] : "/tmp/libpebble3-jvm-fat.jar";

    std::cout << "=== Cyberdeck Service - JNI Bridge Test ===" << std::endl;
    std::cout << "Device: " << device_path << std::endl;
    std::cout << "JAR:    " << jar_path << std::endl;
    std::cout << std::endl;

    try {
        // Create bridge
        std::cout << "Creating JNI bridge..." << std::endl;
        cyberdeck::UartJniBridge bridge(jar_path);

        // Track connection state
        bool ppog_connected = false;

        static constexpr uint16_t K230_ENDPOINT = 0xCD00;
        static constexpr uint8_t K230_CHANNEL_TEST = 0x00;

        // Set callbacks
        bridge.set_endpoint_callback([&bridge](uint16_t endpoint, const std::vector<uint8_t>& data) {
            std::cout << "\n>>> Endpoint 0x" << std::hex << endpoint << std::dec
                      << " received " << data.size() << " bytes:" << std::endl;
            std::cout << "    Hex: ";
            print_hex(data);

            // Handle K230 endpoint (0xCD00): first byte is channel
            if (endpoint == K230_ENDPOINT && !data.empty()) {
                uint8_t channel = data[0];
                size_t payload_len = data.size() - 1;

                std::cout << "    Channel: 0x" << std::hex << static_cast<int>(channel)
                          << std::dec << ", payload: " << payload_len << " bytes" << std::endl;

                // Print payload as text
                if (payload_len > 0) {
                    std::cout << "    Text: ";
                    for (size_t i = 1; i < data.size(); i++) {
                        std::cout << static_cast<char>(data[i]);
                    }
                    std::cout << std::endl;
                }

                // Respond on test channel: "[N] bytes received"
                std::string reply = std::to_string(payload_len) + " bytes received";
                std::vector<uint8_t> response;
                response.push_back(K230_CHANNEL_TEST);
                response.insert(response.end(), reply.begin(), reply.end());
                bridge.send_message(K230_ENDPOINT, response);
                std::cout << "    <<< Replied: " << reply << std::endl;
            }

            std::cout << std::endl;
        });

        bridge.set_connected_callback([]() {
            std::cout << "UART connected to firmware, waiting for PPoG handshake..." << std::endl;
        });

        bridge.set_session_opened_callback([&ppog_connected]() {
            ppog_connected = true;
            std::cout << "\n=== PPoG Handshake Complete ===" << std::endl;
            std::cout << "Protocol session opened, ready for messages!\n" << std::endl;

            std::cout << "Firmware test commands:" << std::endl;
            std::cout << "  k230 ppog test          - Send test packet through PPoG/PP stack" << std::endl;
            std::cout << "  k230 ppouart test       - Send raw PPOUART packet (bypasses protocol)" << std::endl;
            std::cout << "  k230 stats              - Show PPOUART statistics" << std::endl;
            std::cout << std::endl;
        });

        bridge.set_disconnected_callback([]() {
            std::cout << "\n✗ Protocol session closed" << std::endl;
        });

        // Enable unknown/custom endpoint forwarding
        bridge.set_allow_unknown_endpoints(true);

        // Initialize
        std::cout << "Initializing..." << std::endl;
        if (!bridge.init(device_path)) {
            std::cerr << "Failed to initialize bridge" << std::endl;
            return 1;
        }

        // Connect
        if (!bridge.connect()) {
            std::cerr << "Failed to connect" << std::endl;
            return 1;
        }

        // Wait for PPoG handshake to complete (callback sets ppog_connected)
        while (!ppog_connected) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        // Main loop
        while (!g_stop) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        std::cout << "\nShutting down..." << std::endl;
        bridge.disconnect();

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }

    std::cout << "Goodbye!" << std::endl;
    return 0;
}
