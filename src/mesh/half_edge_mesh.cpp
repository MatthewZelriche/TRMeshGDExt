#include "mesh/half_edge_mesh.h"

FaceHandle HalfEdgeMesh::add_face(std::span<const VertexHandle> vertices,
                                  std::span<const godot::Vector2> corner_uvs) {
   // TODO: Construct the face and its half-edge topology using the supplied corner UVs.
   return {};
}

FaceHandle HalfEdgeMesh::add_face(std::span<const VertexHandle> vertices) {
   // TODO: Generate corner UVs and construct the face and its half-edge topology.
   return {};
}

HalfEdgeMesh::HalfEdgesAroundVertexView HalfEdgeMesh::half_edges(
    VertexHandle vertex) const noexcept {
   return {this, vertex};
}
