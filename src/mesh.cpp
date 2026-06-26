#include "multimaster/mesh.hpp"

#include "mesh_impl.hpp"

namespace mm {

Mesh::Mesh(MeshConfig cfg) : impl_(std::make_unique<MeshImpl>(std::move(cfg))) {}
Mesh::~Mesh() = default;

Mesh::Mesh(Mesh&&) noexcept            = default;
Mesh& Mesh::operator=(Mesh&&) noexcept = default;

void Mesh::setCallbacks(Callbacks cb) { impl_->setCallbacks(std::move(cb)); }
void Mesh::start() { impl_->start(); }
void Mesh::stop() { impl_->stop(); }
bool Mesh::isRunning() const noexcept { return impl_->isRunning(); }

void Mesh::broadcast(Bytes data) { impl_->broadcast(data); }
void Mesh::send(PeerId dst, Bytes data) { impl_->send(dst, data); }

PeerId   Mesh::id() const noexcept { return impl_->id(); }
uint16_t Mesh::listenPort() const noexcept { return impl_->listenPort(); }

std::vector<PeerId> Mesh::connectedPeers() const { return impl_->connectedPeers(); }
std::vector<PeerId> Mesh::knownPeers() const { return impl_->knownPeers(); }

} // namespace mm
