#include "mesh/utils/face_builder.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <limits>
#include <ranges>

#include "mesh/half_edge_mesh.h"

FaceBuilder::FaceBuilder(HalfEdgeMesh &mesh, std::span<const VertexHandle> vertices,
                         std::span<const godot::Vector2> corner_uvs) noexcept :
    mMesh(mesh), mVertices(vertices), mCornerUvs(corner_uvs) {}

FaceHandle FaceBuilder::insert(HalfEdgeMesh &mesh, std::span<const VertexHandle> vertices,
                               std::span<const godot::Vector2> corner_uvs) {
   if (vertices.size() < 3 || corner_uvs.size() != vertices.size()) { return {}; }

   auto scratch_checkpoint = mesh.mScratch.checkpoint();
   return FaceBuilder(mesh, vertices, corner_uvs).run();
}

FaceHandle FaceBuilder::insert(HalfEdgeMesh &mesh, std::span<const VertexHandle> vertices) {
   if (vertices.size() < 3) { return {}; }

   auto scratch_checkpoint = mesh.mScratch.checkpoint();
   std::span<godot::Vector2> corner_uvs =
       mesh.mScratch.allocate_span<godot::Vector2>(vertices.size());
   FaceBuilder builder(mesh, vertices, corner_uvs);
   if (!builder.generate_planar_uvs(corner_uvs)) { return {}; }
   return builder.run();
}

FaceHandle FaceBuilder::run() {
   const std::size_t edge_count = mVertices.size();
   std::span<VertexHandle> sorted_vertices = mMesh.mScratch.allocate_span<VertexHandle>(edge_count);
   if (!validate_vertices(sorted_vertices)) { return {}; }

   mFaceEdges = mMesh.mScratch.allocate_span<HalfEdgeHandle>(edge_count);
   if (!resolve_edges() || !validate_vertex_fans()) { return {}; }

   assert(mNewEdgeCount <= (std::numeric_limits<std::size_t>::max() - mMesh.mHalfEdges.size()) / 2);

   // Mutation phase - validation is complete and this topology op should not fail
   // past this point.
   mMesh.mHalfEdges.ensure_capacity(mMesh.mHalfEdges.size() + (mNewEdgeCount * 2));
   const FaceHandle face = mMesh.mFaces.emplace(Face {});
   construct_edges(face);
   connect_topology(face);

   Face *face_data = mMesh.mFaces.get(face);
   assert(face_data != nullptr);
   face_data->boundary = mFaceEdges.front();
   return face;
}

bool FaceBuilder::validate_vertices(std::span<VertexHandle> sorted_vertices) const noexcept {
   assert(mVertices.size() == sorted_vertices.size());
   std::ranges::copy(mVertices, sorted_vertices.begin());
   std::ranges::sort(sorted_vertices);

   for (std::size_t index = 0; index < mVertices.size(); ++index) {
      if (!mMesh.mVertices.contains(mVertices[index])) { return false; }
      if (index > 0 && sorted_vertices[index] == sorted_vertices[index - 1]) { return false; }
   }

   return true;
}

bool FaceBuilder::resolve_edges() noexcept {
   assert(mVertices.size() == mFaceEdges.size());

   // An existing directed edge can be filled only when it is currently the
   // null-face boundary half-edge.
   for (std::size_t index = 0; index < mVertices.size(); ++index) {
      const VertexHandle origin = mVertices[index];
      const VertexHandle destination = mVertices[(index + 1) % mVertices.size()];
      const HalfEdgeHandle existing = mMesh.find_half_edge(origin, destination);

      if (existing == HalfEdgeHandle {}) {
         ++mNewEdgeCount;
         continue;
      }

      const HalfEdge *half_edge = mMesh.mHalfEdges.get<HalfEdge>(existing);
      assert(half_edge != nullptr);
      if (half_edge->face != FaceHandle {}) { return false; }

      const HalfEdge *twin = mMesh.mHalfEdges.get<HalfEdge>(half_edge->twin);
      const HalfEdge *previous = mMesh.mHalfEdges.get<HalfEdge>(half_edge->prev);
      const HalfEdge *next = mMesh.mHalfEdges.get<HalfEdge>(half_edge->next);
      assert(twin != nullptr && twin->twin == existing && twin->origin == destination);
      assert(twin->face != FaceHandle {});
      assert(previous != nullptr && previous->face == FaceHandle {});
      assert(next != nullptr && next->face == FaceHandle {});

      mFaceEdges[index] = existing;
   }

   return true;
}

bool FaceBuilder::validate_vertex_fans() const noexcept {
   assert(mVertices.size() == mFaceEdges.size());

   // A face may extend an existing connected fan through one or both of its
   // boundary edges. Merely touching a non-isolated vertex would create a
   // second, disconnected fan (bow-tie vertex) and is rejected as non-manifold.
   for (std::size_t index = 0; index < mVertices.size(); ++index) {
      const std::size_t incoming_index = (index + mVertices.size() - 1) % mVertices.size();
      const HalfEdgeHandle incoming = mFaceEdges[incoming_index];
      const HalfEdgeHandle outgoing = mFaceEdges[index];
      const bool incoming_reused = incoming != HalfEdgeHandle {};
      const bool outgoing_reused = outgoing != HalfEdgeHandle {};
      const Vertex *vertex = mMesh.mVertices.get<Vertex>(mVertices[index]);
      assert(vertex != nullptr);

      if (vertex->outgoing == HalfEdgeHandle {}) {
         assert(!incoming_reused && !outgoing_reused);
         continue;
      }

      if (!incoming_reused && !outgoing_reused) { return false; }

      if (incoming_reused && outgoing_reused) {
         const HalfEdge *incoming_data = mMesh.mHalfEdges.get<HalfEdge>(incoming);
         const HalfEdge *outgoing_data = mMesh.mHalfEdges.get<HalfEdge>(outgoing);
         assert(incoming_data != nullptr && outgoing_data != nullptr);
         if (incoming_data->next != outgoing || outgoing_data->prev != incoming) { return false; }
      }
   }

   return true;
}

void FaceBuilder::construct_edges(FaceHandle face) {
   assert(mVertices.size() == mCornerUvs.size() && mVertices.size() == mFaceEdges.size());

   // New edges get a paired null-face boundary half-edge. Reused boundary
   // half-edges become members of the new face.
   for (std::size_t index = 0; index < mVertices.size(); ++index) {
      HalfEdgeHandle &face_edge = mFaceEdges[index];
      if (face_edge != HalfEdgeHandle {}) {
         HalfEdge *half_edge = mMesh.mHalfEdges.get<HalfEdge>(face_edge);
         godot::Vector2 *uv = mMesh.mHalfEdges.get<HalfEdgeUVTag>(face_edge);
         assert(half_edge != nullptr && uv != nullptr);
         half_edge->face = face;
         *uv = mCornerUvs[index];
         continue;
      }

      const std::size_t next_index = (index + 1) % mVertices.size();
      face_edge = mMesh.mHalfEdges.emplace(HalfEdge {.origin = mVertices[index], .face = face},
                                           mCornerUvs[index]);
      const HalfEdgeHandle twin = mMesh.mHalfEdges.emplace(
          HalfEdge {.origin = mVertices[next_index], .twin = face_edge}, godot::Vector2 {});

      HalfEdge *half_edge = mMesh.mHalfEdges.get<HalfEdge>(face_edge);
      assert(half_edge != nullptr);
      half_edge->twin = twin;
   }
}

void FaceBuilder::connect_topology(FaceHandle face) noexcept {
   assert(mVertices.size() == mFaceEdges.size());

   // Repair the topology surrounding the new face. New face edges contribute
   // their opposite half-edge to the boundary; reused edges are removed from
   // it. When both incident edges are reused, this vertex becomes interior.
   for (std::size_t index = 0; index < mVertices.size(); ++index) {
      const std::size_t incoming_index = (index + mVertices.size() - 1) % mVertices.size();
      const HalfEdgeHandle incoming = mFaceEdges[incoming_index];
      const HalfEdgeHandle outgoing = mFaceEdges[index];
      HalfEdge *incoming_data = mMesh.mHalfEdges.get<HalfEdge>(incoming);
      HalfEdge *outgoing_data = mMesh.mHalfEdges.get<HalfEdge>(outgoing);
      assert(incoming_data != nullptr && outgoing_data != nullptr);

      const HalfEdge *incoming_twin = mMesh.mHalfEdges.get<HalfEdge>(incoming_data->twin);
      const HalfEdge *outgoing_twin = mMesh.mHalfEdges.get<HalfEdge>(outgoing_data->twin);
      assert(incoming_twin != nullptr && outgoing_twin != nullptr);
      const bool incoming_reused = incoming_twin->face != FaceHandle {};
      const bool outgoing_reused = outgoing_twin->face != FaceHandle {};

      if (!incoming_reused && !outgoing_reused) {
         mMesh.link_half_edges(outgoing_data->twin, incoming_data->twin);
      } else if (incoming_reused && !outgoing_reused) {
         mMesh.link_half_edges(outgoing_data->twin, incoming_data->next);
      } else if (!incoming_reused && outgoing_reused) {
         mMesh.link_half_edges(outgoing_data->prev, incoming_data->twin);
      }

      Vertex *vertex = mMesh.mVertices.get<Vertex>(mVertices[index]);
      assert(vertex != nullptr);
      vertex->outgoing = outgoing;
   }

   // Boundary links must be repaired before reused edges lose their old
   // boundary prev/next links to the new face cycle.
   for (std::size_t index = 0; index < mFaceEdges.size(); ++index) {
      const std::size_t previous_index = (index + mFaceEdges.size() - 1) % mFaceEdges.size();
      const std::size_t next_index = (index + 1) % mFaceEdges.size();
      HalfEdge *half_edge = mMesh.mHalfEdges.get<HalfEdge>(mFaceEdges[index]);
      assert(half_edge != nullptr);
      half_edge->prev = mFaceEdges[previous_index];
      half_edge->next = mFaceEdges[next_index];
      half_edge->face = face;
   }
}

bool FaceBuilder::generate_planar_uvs(std::span<godot::Vector2> corner_uvs) const noexcept {
   assert(mVertices.size() == corner_uvs.size());
   if (mVertices.empty()) { return true; }

   godot::Vector3 normal {};
   godot::Vector3 minimum;
   godot::Vector3 maximum;

   const godot::Vector3 *first_position = mMesh.mVertices.get<VertexPositionTag>(mVertices.front());
   if (first_position == nullptr) { return false; }
   minimum = *first_position;
   maximum = *first_position;

   for (std::size_t index = 0; index < mVertices.size(); ++index) {
      const godot::Vector3 *current = mMesh.mVertices.get<VertexPositionTag>(mVertices[index]);
      const godot::Vector3 *next =
          mMesh.mVertices.get<VertexPositionTag>(mVertices[(index + 1) % mVertices.size()]);
      if (current == nullptr || next == nullptr) { return false; }

      normal.x += (current->y - next->y) * (current->z + next->z);
      normal.y += (current->z - next->z) * (current->x + next->x);
      normal.z += (current->x - next->x) * (current->y + next->y);

      minimum.x = std::min(minimum.x, current->x);
      minimum.y = std::min(minimum.y, current->y);
      minimum.z = std::min(minimum.z, current->z);
      maximum.x = std::max(maximum.x, current->x);
      maximum.y = std::max(maximum.y, current->y);
      maximum.z = std::max(maximum.z, current->z);
   }

   const auto absolute_x = std::abs(normal.x);
   const auto absolute_y = std::abs(normal.y);
   const auto absolute_z = std::abs(normal.z);
   enum class DroppedAxis : std::uint8_t { X, Y, Z };
   DroppedAxis dropped_axis = DroppedAxis::Z;

   if (absolute_x == 0 && absolute_y == 0 && absolute_z == 0) {
      const auto x_extent = maximum.x - minimum.x;
      const auto y_extent = maximum.y - minimum.y;
      const auto z_extent = maximum.z - minimum.z;
      if (x_extent <= y_extent && x_extent <= z_extent) {
         dropped_axis = DroppedAxis::X;
      } else if (y_extent <= z_extent) {
         dropped_axis = DroppedAxis::Y;
      }
   } else if (absolute_x >= absolute_y && absolute_x >= absolute_z) {
      dropped_axis = DroppedAxis::X;
   } else if (absolute_y >= absolute_z) {
      dropped_axis = DroppedAxis::Y;
   }

   for (std::size_t index = 0; index < mVertices.size(); ++index) {
      const godot::Vector3 &position = *mMesh.mVertices.get<VertexPositionTag>(mVertices[index]);
      switch (dropped_axis) {
         case DroppedAxis::X: corner_uvs[index] = {position.y, position.z}; break;
         case DroppedAxis::Y: corner_uvs[index] = {position.x, position.z}; break;
         case DroppedAxis::Z: corner_uvs[index] = {position.x, position.y}; break;
      }
   }

   godot::Vector2 uv_minimum = corner_uvs.front();
   godot::Vector2 uv_maximum = corner_uvs.front();
   for (const godot::Vector2 &uv : corner_uvs) {
      uv_minimum.x = std::min(uv_minimum.x, uv.x);
      uv_minimum.y = std::min(uv_minimum.y, uv.y);
      uv_maximum.x = std::max(uv_maximum.x, uv.x);
      uv_maximum.y = std::max(uv_maximum.y, uv.y);
   }

   const auto scale = std::max(uv_maximum.x - uv_minimum.x, uv_maximum.y - uv_minimum.y);
   if (scale > 0) {
      for (godot::Vector2 &uv : corner_uvs) {
         uv.x = (uv.x - uv_minimum.x) / scale;
         uv.y = (uv.y - uv_minimum.y) / scale;
      }
   }

   return true;
}
