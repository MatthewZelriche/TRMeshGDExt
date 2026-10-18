#include "mesh/half_edge_mesh.h"

#include <cassert>

#include "mesh/utils/face_builder.h"

FaceHandle HalfEdgeMesh::add_face(std::span<const VertexHandle> vertices,
                                  std::span<const godot::Vector2> corner_uvs) {
   return FaceBuilder::insert(*this, vertices, corner_uvs);
}

FaceHandle HalfEdgeMesh::add_face(std::span<const VertexHandle> vertices) {
   return FaceBuilder::insert(*this, vertices);
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

void HalfEdgeMesh::link_half_edges(HalfEdgeHandle previous, HalfEdgeHandle next) noexcept {
   HalfEdge *previous_data = mHalfEdges.get<HalfEdge>(previous);
   HalfEdge *next_data = mHalfEdges.get<HalfEdge>(next);
   assert(previous_data != nullptr && next_data != nullptr);
   previous_data->next = next;
   next_data->prev = previous;
}
