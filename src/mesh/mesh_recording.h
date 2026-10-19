#pragma once

#include <cstddef>
#include <cstdint>
#include <utility>

#include "mesh/topology_data.hpp"

class HalfEdgeMesh;

// Exactly reverses one committed HalfEdgeMesh recording.
class MeshPatch final {
  public:
   MeshPatch(const MeshPatch &) = delete;
   MeshPatch &operator=(const MeshPatch &) = delete;
   MeshPatch(MeshPatch &&) noexcept = default;
   MeshPatch &operator=(MeshPatch &&) noexcept = default;
   ~MeshPatch() = default;

   [[nodiscard]] std::size_t byte_size() const noexcept {
      return mVertices.byte_size() + mHalfEdges.byte_size() + mFaces.byte_size();
   }

  private:
   friend class HalfEdgeMesh;

   MeshPatch(VertexMap::Patch vertices, HalfEdgeMap::Patch half_edges, FaceMap::Patch faces,
             std::uint64_t revision_before, std::uint64_t revision_after) noexcept :
       mVertices(std::move(vertices)),
       mHalfEdges(std::move(half_edges)), mFaces(std::move(faces)),
       mRevisionBefore(revision_before), mRevisionAfter(revision_after) {}

   VertexMap::Patch mVertices;
   HalfEdgeMap::Patch mHalfEdges;
   FaceMap::Patch mFaces;
   std::uint64_t mRevisionBefore;
   std::uint64_t mRevisionAfter;
};

// An active mesh transaction. Destruction rolls back unless commit() was called.
class MeshRecording final {
  public:
   MeshRecording(const MeshRecording &) = delete;
   MeshRecording &operator=(const MeshRecording &) = delete;
   MeshRecording(MeshRecording &&other) noexcept;
   MeshRecording &operator=(MeshRecording &&other) = delete;
   ~MeshRecording() noexcept;

   [[nodiscard]] MeshPatch commit();
   void rollback() noexcept;
   [[nodiscard]] bool is_active() const noexcept { return mMesh != nullptr; }

  private:
   friend class HalfEdgeMesh;

   MeshRecording(HalfEdgeMesh &mesh, std::uint64_t revision_before) noexcept :
       mMesh(&mesh), mRevisionBefore(revision_before) {}

   HalfEdgeMesh *mMesh;
   std::uint64_t mRevisionBefore;
};
