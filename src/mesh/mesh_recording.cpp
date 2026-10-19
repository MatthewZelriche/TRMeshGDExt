#include "mesh/mesh_recording.h"

#include <cassert>
#include <stdexcept>
#include <utility>

#include "mesh/half_edge_mesh.h"

MeshRecording::MeshRecording(MeshRecording &&other) noexcept :
    mMesh(std::exchange(other.mMesh, nullptr)), mRevisionBefore(other.mRevisionBefore) {}

MeshRecording::~MeshRecording() noexcept { rollback(); }

MeshPatch MeshRecording::commit() {
   if (mMesh == nullptr) { throw std::logic_error("the mesh recording is no longer active"); }

   MeshPatch patch = mMesh->commit_recording(mRevisionBefore);
   mMesh = nullptr;
   return patch;
}

void MeshRecording::rollback() noexcept {
   if (mMesh == nullptr) { return; }

   mMesh->rollback_recording(mRevisionBefore);
   mMesh = nullptr;
}

MeshRecording HalfEdgeMesh::begin_recording() {
   if (mIsRecording) { throw std::logic_error("the mesh is already recording"); }

   mVertices.begin_recording();
   try {
      mHalfEdges.begin_recording();
   } catch (...) {
      mVertices.cancel_recording();
      throw;
   }

   try {
      mFaces.begin_recording();
   } catch (...) {
      mHalfEdges.cancel_recording();
      mVertices.cancel_recording();
      throw;
   }

   mIsRecording = true;
   return MeshRecording {*this, mRevision};
}

void HalfEdgeMesh::revert(const MeshPatch &patch) {
   if (mIsRecording || mRevision != patch.mRevisionAfter) {
      throw std::logic_error("the patch does not match the current mesh state");
   }

   mVertices.revert(patch.mVertices);
   mHalfEdges.revert(patch.mHalfEdges);
   mFaces.revert(patch.mFaces);
   mRevision = patch.mRevisionBefore;
}

MeshPatch HalfEdgeMesh::commit_recording(std::uint64_t revision_before) {
   if (!mIsRecording) { throw std::logic_error("the mesh is not recording"); }

   // Even an empty recording is a distinct committed state, so its patch cannot be reused.
   if (mRevision == revision_before) { mark_mutated(); }

   MeshPatch patch {mVertices.commit_recording(), mHalfEdges.commit_recording(),
                    mFaces.commit_recording(), revision_before, mRevision};
   mIsRecording = false;
   return patch;
}

void HalfEdgeMesh::rollback_recording(std::uint64_t revision_before) noexcept {
   assert(mIsRecording);
   mVertices.rollback_recording();
   mHalfEdges.rollback_recording();
   mFaces.rollback_recording();
   mRevision = revision_before;
   mIsRecording = false;
}
