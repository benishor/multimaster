#include "multimaster/mesh.hpp"

#include "mesh_impl.hpp"

namespace mm {

mesh::mesh(mesh_config cfg) : impl_(std::make_unique<mesh_impl>(std::move(cfg))) {}
mesh::~mesh() = default;

mesh::mesh(mesh&&) noexcept            = default;
mesh& mesh::operator=(mesh&&) noexcept = default;

void mesh::set_callbacks(callbacks cb) { impl_->set_callbacks(std::move(cb)); }
void mesh::start() { impl_->start(); }
void mesh::stop() { impl_->stop(); }
bool mesh::is_running() const noexcept { return impl_->is_running(); }

void mesh::broadcast(bytes data) { impl_->broadcast(data); }
void mesh::send(peer_id dst, bytes data) { impl_->send(dst, data); }

peer_id   mesh::id() const noexcept { return impl_->id(); }
uint16_t mesh::listen_port() const noexcept { return impl_->listen_port(); }

std::vector<peer_id> mesh::connected_peers() const { return impl_->connected_peers(); }
std::vector<peer_id> mesh::known_peers() const { return impl_->known_peers(); }

} // namespace mm
