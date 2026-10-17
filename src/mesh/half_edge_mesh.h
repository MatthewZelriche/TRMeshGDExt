#pragma once

#include <span>

#include "mesh/topology_data.hpp"

// Engine-independent half-edge mesh storage.
class HalfEdgeMesh {
  public:
   [[nodiscard]] VertexHandle add_vertex(const godot::Vector3 &position) {
      return mVertices.emplace(Vertex(), position);
   }

   [[nodiscard]] FaceHandle add_face(std::span<const VertexHandle> vertices,
                                     std::span<const godot::Vector2> corner_uvs);

   [[nodiscard]] FaceHandle add_face(std::span<const VertexHandle> vertices);

   inline godot::Vector3 *position(VertexHandle vertex) noexcept {
      return mVertices.get<VertexPositionTag>(vertex);
   }

   inline const godot::Vector3 *position(VertexHandle vertex) const noexcept {
      return mVertices.get<VertexPositionTag>(vertex);
   }

   inline godot::Vector2 *uv(HalfEdgeHandle half_edge) noexcept {
      return mHalfEdges.get<HalfEdgeUVTag>(half_edge);
   }

   inline const godot::Vector2 *uv(HalfEdgeHandle half_edge) const noexcept {
      return mHalfEdges.get<HalfEdgeUVTag>(half_edge);
   }

  private:
   VertexMap mVertices;
   HalfEdgeMap mHalfEdges;
   FaceMap mFaces;
};
