// mm_ping — targeted send demo.
//
// Start several nodes. Each one, once it has connected peers, sends a targeted
// "ping" to one specific peer id; the addressed node replies with a "pong".
// Demonstrates Mesh::send(PeerId, ...) routing across the mesh.
//
//   ./mm_ping

#include <multimaster/multimaster.hpp>

#include <chrono>
#include <iostream>
#include <string>
#include <thread>

namespace {
mm::Bytes asBytes(const std::string& s) {
    return mm::Bytes(reinterpret_cast<const std::byte*>(s.data()), s.size());
}
std::string toStr(mm::Bytes b) {
    return std::string(reinterpret_cast<const char*>(b.data()), b.size());
}
} // namespace

int main() {
    mm::Mesh mesh(mm::MeshConfig{});

    mm::Callbacks cb;
    cb.onPeerConnected = [&mesh](mm::PeerId p) {
        std::cout << "connected to " << p.toString().substr(0, 8) << ", sending ping\n";
        mesh.send(p, asBytes("ping from " + mesh.id().toString().substr(0, 8)));
    };
    cb.onMessage = [&mesh](mm::PeerId from, mm::Bytes data) {
        std::string msg = toStr(data);
        std::cout << from.toString().substr(0, 8) << "> " << msg << "\n";
        if (msg.rfind("ping", 0) == 0) {
            mesh.send(from, asBytes("pong from " + mesh.id().toString().substr(0, 8)));
        }
    };
    mesh.setCallbacks(std::move(cb));

    mesh.start();
    std::cout << "this node: " << mesh.id().toString() << "\n";

    // Run for a while, then exit cleanly.
    std::this_thread::sleep_for(std::chrono::seconds(30));
    mesh.stop();
    return 0;
}
