#pragma once

#include <cstddef>
#include <span>

#include "mesh/topology_data.hpp"

class HalfEdgeMesh;

// Short-lived operation object containing the validation and mutation needed
// to insert one face into a HalfEdgeMesh.
class FaceBuilder final {
  public:
   [[nodiscard]] static FaceHandle insert(HalfEdgeMesh &mesh,
                                          std::span<const VertexHandle> vertices,
                                          std::span<const godot::Vector2> corner_uvs);

   [[nodiscard]] static FaceHandle insert(HalfEdgeMesh &mesh,
                                          std::span<const VertexHandle> vertices);

  private:
   FaceBuilder(HalfEdgeMesh &mesh, std::span<const VertexHandle> vertices,
               std::span<const godot::Vector2> corner_uvs) noexcept;

   [[nodiscard]] FaceHandle run();

   [[nodiscard]] bool validate_vertices(std::span<VertexHandle> sorted_vertices) const noexcept;

   [[nodiscard]] bool resolve_edges() noexcept;

   [[nodiscard]] bool validate_vertex_fans() const noexcept;

   void construct_edges(FaceHandle face);

   void connect_topology(FaceHandle face) noexcept;

   [[nodiscard]] bool generate_planar_uvs(std::span<godot::Vector2> corner_uvs) const noexcept;

   HalfEdgeMesh &mMesh;
   std::span<const VertexHandle> mVertices;
   std::span<const godot::Vector2> mCornerUvs;
   std::span<HalfEdgeHandle> mFaceEdges;
   std::size_t mNewEdgeCount = 0;
};
