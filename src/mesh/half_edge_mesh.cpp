#include "mesh/half_edge_mesh.h"

#include <cassert>
#include <exception>
#include <utility>

#include "mesh/utils/face_builder.h"

HalfEdgeMesh::HalfEdgeMesh() noexcept : mRevision(next_revision()) {}

HalfEdgeMesh::HalfEdgeMesh(HalfEdgeMesh &&other) noexcept : HalfEdgeMesh() {
   *this = std::move(other);
}

HalfEdgeMesh &HalfEdgeMesh::operator=(HalfEdgeMesh &&other) noexcept {
   if (mIsRecording || other.mIsRecording) { std::terminate(); }
   if (this != &other) {
      mVertices = std::move(other.mVertices);
      mHalfEdges = std::move(other.mHalfEdges);
      mFaces = std::move(other.mFaces);
      mScratch = std::move(other.mScratch);
      mRevision = std::exchange(other.mRevision, next_revision());
   }
   return *this;
}

HalfEdgeMesh::~HalfEdgeMesh() {
   if (mIsRecording) { std::terminate(); }
}

VertexHandle HalfEdgeMesh::add_vertex(const godot::Vector3 &position) {
   const VertexHandle vertex = mVertices.emplace(Vertex(), position);
   mark_mutated();
   return vertex;
}

FaceHandle HalfEdgeMesh::add_face(std::span<const VertexHandle> vertices,
                                  std::span<const godot::Vector2> corner_uvs) {
   const FaceHandle face = FaceBuilder::insert(*this, vertices, corner_uvs);
   if (face != FaceHandle {}) { mark_mutated(); }
   return face;
}

FaceHandle HalfEdgeMesh::add_face(std::span<const VertexHandle> vertices) {
   const FaceHandle face = FaceBuilder::insert(*this, vertices);
   if (face != FaceHandle {}) { mark_mutated(); }
   return face;
}

HalfEdgeMesh::HalfEdgesAroundVertexView HalfEdgeMesh::half_edges(
    VertexHandle vertex) const noexcept {
   return {this, vertex};
}

HalfEdgeMesh::HalfEdgesAroundFaceView HalfEdgeMesh::half_edges(FaceHandle face) const noexcept {
   return {this, face};
}

HalfEdgeHandle HalfEdgeMesh::find_half_edge(VertexHandle origin,
                                            VertexHandle destination) const noexcept {
   for (HalfEdgeHandle half_edge_handle : half_edges(origin)) {
      const HalfEdge *half_edge = mHalfEdges.get<HalfEdge>(half_edge_handle);
      assert(half_edge != nullptr);
      const HalfEdge *twin = mHalfEdges.get<HalfEdge>(half_edge->twin);
      assert(twin != nullptr);

      if (twin->origin == destination) { return half_edge_handle; }
   }

   return {};
}

void HalfEdgeMesh::link_half_edges(HalfEdgeHandle previous, HalfEdgeHandle next) {
   HalfEdge *previous_data = mHalfEdges.get_mut<HalfEdge>(previous);
   HalfEdge *next_data = mHalfEdges.get_mut<HalfEdge>(next);
   assert(previous_data != nullptr && next_data != nullptr);
   previous_data->next = next;
   next_data->prev = previous;
}

void HalfEdgeMesh::mark_mutated() noexcept { mRevision = next_revision(); }

std::uint64_t HalfEdgeMesh::next_revision() noexcept {
   const std::uint64_t revision = sLastRevision.fetch_add(1, std::memory_order_relaxed) + 1;
   return revision;
}
