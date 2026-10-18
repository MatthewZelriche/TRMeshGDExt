#pragma once

#include <span>

#include "mesh/topology_data.hpp"
#include "utils/bump_allocator.hpp"

class FaceBuilder;

// Engine-independent half-edge mesh storage.
class HalfEdgeMesh {
  public:
   class HalfEdgesAroundVertexView;
   class HalfEdgesAroundFaceView;

   [[nodiscard]] VertexHandle add_vertex(const godot::Vector3 &position) {
      return mVertices.emplace(Vertex(), position);
   }

   // Adds a polygon in the supplied winding. corner_uvs[index] belongs to the
   // corner at vertices[index]. Returns an invalid handle when the input would
   // violate manifoldness or an other error would occur.
   [[nodiscard]] FaceHandle add_face(std::span<const VertexHandle> vertices,
                                     std::span<const godot::Vector2> corner_uvs);

   // Generates normalized planar UVs by projecting along the dominant face axis.
   [[nodiscard]] FaceHandle add_face(std::span<const VertexHandle> vertices);

   HalfEdgesAroundVertexView half_edges(VertexHandle vertex) const noexcept;

   HalfEdgesAroundFaceView half_edges(FaceHandle face) const noexcept;

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
   friend class FaceBuilder;

   HalfEdgeHandle find_half_edge(VertexHandle origin, VertexHandle destination) const noexcept;
   void link_half_edges(HalfEdgeHandle previous, HalfEdgeHandle next) noexcept;

   VertexMap mVertices;
   HalfEdgeMap mHalfEdges;
   FaceMap mFaces;
   BumpAllocator mScratch;
};

#include "mesh/topology_iterators.hpp"
