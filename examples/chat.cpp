// SPDX-License-Identifier: LGPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 Adrian Scripcă (YO6SSW)

// mm_chat — a tiny broadcast chat over the multimaster mesh.
//
// On a LAN, peers discover each other automatically via multicast:
//
//   ./mm_chat [groupName]
//
// To connect across the internet (WAN), multicast doesn't apply — give each node
// the public host:port of one or more peers as *static peers*. They are dialed
// and the connection is kept up persistently (reconnect with backoff):
//
//   ./mm_chat [groupName] [--port N] [--psk SECRET] [host:port ...]
//
// Pass --psk SECRET (the same value on every node) to encrypt and authenticate
// the mesh: only nodes holding the secret can join, and all traffic is encrypted.
//
// Example, two machines over the internet (peers find each other once either
// side can reach the other; listing both ends on both is fine and de-duped):
//
//   # machine A (public IP a.a.a.a), listen on a fixed port so B can reach it
//   ./mm_chat myroom --port 45000 b.b.b.b:45000
//   # machine B (public IP b.b.b.b)
//   ./mm_chat myroom --port 45000 a.a.a.a:45000
//
// NOTE: the listen port must be reachable from the peer — open it in the
// firewall and, behind NAT, forward it to this host. multimaster does not do NAT
// traversal; one reachable endpoint per pair is enough.

#include <multimaster/multimaster.hpp>

#include <cstdlib>
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

// Parse "host:port" (IPv4 or hostname) into a seed_peer. Returns false if malformed.
bool parse_endpoint(const std::string& arg, mm::seed_peer& out) {
    auto colon = arg.rfind(':');
    if (colon == std::string::npos || colon == 0 || colon + 1 >= arg.size()) return false;
    out.host = arg.substr(0, colon);
    out.port = static_cast<uint16_t>(std::strtoul(arg.c_str() + colon + 1, nullptr, 10));
    return out.port != 0;
}
} // namespace

int main(int argc, char** argv) {
    mm::mesh_config cfg;

    // argv[1] (optional, not starting with '-') is the group name.
    int i = 1;
    if (i < argc && argv[i][0] != '-') cfg.groupName = argv[i++];

    // Remaining args: --port N to fix the listen port, and host:port WAN peers.
    for (; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--port" && i + 1 < argc) {
            cfg.listenPort = static_cast<uint16_t>(std::strtoul(argv[++i], nullptr, 10));
            continue;
        }
        if (a == "--psk" && i + 1 < argc) {
            cfg.psk = argv[++i]; // non-empty => encrypted + authenticated mesh
            continue;
        }
        if (a == "--identity" && i + 1 < argc) {
            cfg.identityFile = argv[++i]; // persistent self-certifying identity
            continue;
        }
        if (a == "--name" && i + 1 < argc) {
            cfg.nodeName = argv[++i]; // signed, gossiped nickname
            continue;
        }
        if (a == "--trust" && i + 1 < argc) {
            // --trust <pubkeyhex>[=label] : admit this node key (+ optional label)
            std::string v = argv[++i];
            auto        eq = v.find('=');
            mm::trusted_node t;
            t.publicKeyHex = v.substr(0, eq);
            if (eq != std::string::npos) t.label = v.substr(eq + 1);
            cfg.trustedKeys.push_back(std::move(t));
            continue;
        }
        mm::seed_peer ep;
        if (parse_endpoint(a, ep)) {
            cfg.staticPeers.push_back(ep); // persistently maintained WAN peer
        } else {
            std::cerr << "ignoring unrecognized argument: " << a << "\n";
        }
    }

    mm::mesh mesh(cfg);

    // Prefer a node's human name (local label or signed nickname) over its hex id.
    auto who = [&mesh](mm::peer_id p) {
        auto n = mesh.node_name(p);
        return n.empty() ? p.to_string().substr(0, 8) : n;
    };

    mm::callbacks cb;
    cb.onPeerConnected    = [&](mm::peer_id p) { std::cout << "[+ connected]    " << who(p) << "\n"; };
    cb.onPeerDisconnected = [&](mm::peer_id p) { std::cout << "[- disconnected] " << who(p) << "\n"; };
    cb.onMemberJoined    = [&](mm::peer_id p) { std::cout << "[+ connected]    " << who(p) << "\n"; };
    cb.onMemberLeft = [&](mm::peer_id p) { std::cout << "[- disconnected] " << who(p) << "\n"; };
    cb.onPeerLost         = [&](mm::peer_id p) { std::cout << "[x lost]         " << who(p) << "\n"; };
    cb.onMessage          = [&](mm::peer_id from, mm::bytes data) {
        std::cout << who(from) << "> " << to_str(data) << "\n";
    };
    cb.onError = [](const mm::error& e) { std::cerr << "[error] " << e.what << "\n"; };
    mesh.set_callbacks(std::move(cb));

    mesh.start();
    std::cout << "this node: " << mesh.id().to_string() << "  port: " << mesh.listen_port()
              << "  group: " << cfg.groupName << (cfg.psk.empty() ? "" : "  [secured]") << "\n";
    if (auto pub = mesh.identity_public_key(); !pub.empty()) {
        std::cout << "identity"  << (cfg.nodeName.empty() ? "" : " \"" + cfg.nodeName + "\"")
                  << " pubkey: " << pub << "  (share for peers' --trust)\n";
    }
    if (!cfg.staticPeers.empty()) {
        std::cout << "static peers:";
        for (const auto& s : cfg.staticPeers) std::cout << " " << s.host << ":" << s.port;
        std::cout << "\n";
    }
    std::cout << "type messages and press enter (Ctrl-D to quit)\n";

    std::string line;
    while (std::getline(std::cin, line)) {
        mesh.broadcast(as_bytes(line));
    }

    mesh.stop();
    return 0;
}
