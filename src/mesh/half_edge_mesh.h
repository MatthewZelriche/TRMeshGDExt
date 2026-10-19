#pragma once

#include <atomic>
#include <cstdint>
#include <span>

#include "mesh/mesh_recording.h"
#include "mesh/topology_data.hpp"
#include "utils/bump_allocator.hpp"

class FaceBuilder;

// Engine-independent half-edge mesh storage.
class HalfEdgeMesh {
  public:
   class HalfEdgesAroundVertexView;
   class HalfEdgesAroundFaceView;

   HalfEdgeMesh() noexcept;
   HalfEdgeMesh(const HalfEdgeMesh &) = delete;
   HalfEdgeMesh &operator=(const HalfEdgeMesh &) = delete;
   HalfEdgeMesh(HalfEdgeMesh &&other) noexcept;
   HalfEdgeMesh &operator=(HalfEdgeMesh &&other) noexcept;
   ~HalfEdgeMesh();

   [[nodiscard]] VertexHandle add_vertex(const godot::Vector3 &position);

   // Adds a polygon in the supplied winding. corner_uvs[index] belongs to the
   // corner at vertices[index]. Returns an invalid handle when the input would
   // violate manifoldness or an other error would occur.
   [[nodiscard]] FaceHandle add_face(std::span<const VertexHandle> vertices,
                                     std::span<const godot::Vector2> corner_uvs);

   // Generates normalized planar UVs by projecting along the dominant face axis.
   [[nodiscard]] FaceHandle add_face(std::span<const VertexHandle> vertices);

   [[nodiscard]] MeshRecording begin_recording();

   // Reverts the latest committed recording when the patch matches this exact mesh state.
   void revert(const MeshPatch &patch);

   HalfEdgesAroundVertexView half_edges(VertexHandle vertex) const noexcept;

   HalfEdgesAroundFaceView half_edges(FaceHandle face) const noexcept;

   // Writable pointers capture their complete row while recording. They must not be retained
   // across structural changes or recording boundaries.
   inline godot::Vector3 *position(VertexHandle vertex) {
      godot::Vector3 *result = mVertices.get_mut<VertexPositionTag>(vertex);
      if (result != nullptr) { mark_mutated(); }
      return result;
   }

   inline const godot::Vector3 *position(VertexHandle vertex) const noexcept {
      return mVertices.get<VertexPositionTag>(vertex);
   }

   inline godot::Vector2 *uv(HalfEdgeHandle half_edge) {
      godot::Vector2 *result = mHalfEdges.get_mut<HalfEdgeUVTag>(half_edge);
      if (result != nullptr) { mark_mutated(); }
      return result;
   }

   inline const godot::Vector2 *uv(HalfEdgeHandle half_edge) const noexcept {
      return mHalfEdges.get<HalfEdgeUVTag>(half_edge);
   }

  private:
   friend class FaceBuilder;
   friend class MeshRecording;

   HalfEdgeHandle find_half_edge(VertexHandle origin, VertexHandle destination) const noexcept;
   void link_half_edges(HalfEdgeHandle previous, HalfEdgeHandle next);
   void mark_mutated() noexcept;
   [[nodiscard]] MeshPatch commit_recording(std::uint64_t revision_before);
   void rollback_recording(std::uint64_t revision_before) noexcept;

   static std::uint64_t next_revision() noexcept;

   VertexMap mVertices;
   HalfEdgeMap mHalfEdges;
   FaceMap mFaces;
   BumpAllocator mScratch;
   std::uint64_t mRevision;
   bool mIsRecording = false;

   inline static std::atomic_uint64_t sLastRevision = 0;
};

#include "mesh/topology_iterators.hpp"
