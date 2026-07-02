// SPDX-License-Identifier: LGPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 Adrian Scripcă (YO6SSW)

// mm_ping — targeted send demo.
//
// Start several nodes. Each one, once it has connected peers, sends a targeted
// "ping" to one specific peer id; the addressed node replies with a "pong".
// Demonstrates mesh::send(peer_id, ...) routing across the mesh.
//
//   ./mm_ping

#include <multimaster/multimaster.hpp>

#include <chrono>
#include <iostream>
#include <string>
#include <thread>

namespace {
mm::bytes as_bytes(const std::string& s) {
    return mm::bytes(reinterpret_cast<const std::byte*>(s.data()), s.size());
}
std::string to_str(mm::bytes b) {
    return std::string(reinterpret_cast<const char*>(b.data()), b.size());
}
} // namespace

int main() {
    mm::mesh mesh(mm::mesh_config{});

    mm::callbacks cb;
    cb.onPeerConnected = [&mesh](mm::peer_id p) {
        std::cout << "connected to " << p.to_string().substr(0, 8) << ", sending ping\n";
        mesh.send(p, as_bytes("ping from " + mesh.id().to_string().substr(0, 8)));
    };
    cb.onMessage = [&mesh](mm::peer_id from, mm::bytes data) {
        std::string msg = to_str(data);
        std::cout << from.to_string().substr(0, 8) << "> " << msg << "\n";
        if (msg.rfind("ping", 0) == 0) {
            mesh.send(from, as_bytes("pong from " + mesh.id().to_string().substr(0, 8)));
        }
    };
    mesh.set_callbacks(std::move(cb));

    mesh.start();
    std::cout << "this node: " << mesh.id().to_string() << "\n";

    // Run for a while, then exit cleanly.
    std::this_thread::sleep_for(std::chrono::seconds(30));
    mesh.stop();
    return 0;
}
