// mm_chat — a tiny LAN broadcast chat over the multimaster mesh.
//
// Run it in two or more terminals (same LAN / same host). Each node discovers
// the others automatically; lines you type are broadcast to everyone.
//
//   ./mm_chat [groupName]

#include <multimaster/multimaster.hpp>

#include <atomic>
#include <cstring>
#include <iostream>
#include <string>

namespace {
mm::bytes as_bytes(const std::string& s) {
    return mm::bytes(reinterpret_cast<const std::byte*>(s.data()), s.size());
}
std::string to_str(mm::bytes b) {
    return std::string(reinterpret_cast<const char*>(b.data()), b.size());
}
} // namespace

int main(int argc, char** argv) {
    mm::mesh_config cfg;
    if (argc > 1) cfg.groupName = argv[1];

    mm::mesh mesh(cfg);

    mm::callbacks cb;
    cb.onPeerConnected    = [](mm::peer_id p) { std::cout << "[+ connected]    " << p.to_string() << "\n"; };
    cb.onPeerDisconnected = [](mm::peer_id p) { std::cout << "[- disconnected] " << p.to_string() << "\n"; };
    cb.onPeerLost         = [](mm::peer_id p) { std::cout << "[x lost]         " << p.to_string() << "\n"; };
    cb.onMessage          = [](mm::peer_id from, mm::bytes data) {
        std::cout << from.to_string().substr(0, 8) << "> " << to_str(data) << "\n";
    };
    cb.onError = [](const mm::error& e) { std::cerr << "[error] " << e.what << "\n"; };
    mesh.set_callbacks(std::move(cb));

    mesh.start();
    std::cout << "this node: " << mesh.id().to_string() << "  port: " << mesh.listen_port()
              << "  group: " << cfg.groupName << "\n";
    std::cout << "type messages and press enter (Ctrl-D to quit)\n";

    std::string line;
    while (std::getline(std::cin, line)) {
        mesh.broadcast(as_bytes(line));
    }

    mesh.stop();
    return 0;
}
