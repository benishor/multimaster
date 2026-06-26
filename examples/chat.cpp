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
mm::Bytes asBytes(const std::string& s) {
    return mm::Bytes(reinterpret_cast<const std::byte*>(s.data()), s.size());
}
std::string toStr(mm::Bytes b) {
    return std::string(reinterpret_cast<const char*>(b.data()), b.size());
}
} // namespace

int main(int argc, char** argv) {
    mm::MeshConfig cfg;
    if (argc > 1) cfg.groupName = argv[1];

    mm::Mesh mesh(cfg);

    mm::Callbacks cb;
    cb.onPeerConnected    = [](mm::PeerId p) { std::cout << "[+ connected]    " << p.toString() << "\n"; };
    cb.onPeerDisconnected = [](mm::PeerId p) { std::cout << "[- disconnected] " << p.toString() << "\n"; };
    cb.onPeerLost         = [](mm::PeerId p) { std::cout << "[x lost]         " << p.toString() << "\n"; };
    cb.onMessage          = [](mm::PeerId from, mm::Bytes data) {
        std::cout << from.toString().substr(0, 8) << "> " << toStr(data) << "\n";
    };
    cb.onError = [](const mm::Error& e) { std::cerr << "[error] " << e.what << "\n"; };
    mesh.setCallbacks(std::move(cb));

    mesh.start();
    std::cout << "this node: " << mesh.id().toString() << "  port: " << mesh.listenPort()
              << "  group: " << cfg.groupName << "\n";
    std::cout << "type messages and press enter (Ctrl-D to quit)\n";

    std::string line;
    while (std::getline(std::cin, line)) {
        mesh.broadcast(asBytes(line));
    }

    mesh.stop();
    return 0;
}
