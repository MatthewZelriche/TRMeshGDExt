#include <array>
#include <catch2/catch_test_macros.hpp>
#include <ranges>
#include <stdexcept>
#include <utility>
#include <vector>

#include "mesh/half_edge_mesh.h"

namespace {

struct RecordedTriangle {
   std::array<VertexHandle, 3> vertices;
   FaceHandle face;
};

RecordedTriangle add_triangle(HalfEdgeMesh &mesh) {
   RecordedTriangle result;
   result.vertices[0] = mesh.add_vertex({0, 0, 0});
   result.vertices[1] = mesh.add_vertex({1, 0, 0});
   result.vertices[2] = mesh.add_vertex({0, 1, 0});
   result.face = mesh.add_face(result.vertices);
   return result;
}

std::vector<HalfEdgeHandle> face_edges(const HalfEdgeMesh &mesh, FaceHandle face) {
   std::vector<HalfEdgeHandle> result;
   for (HalfEdgeHandle half_edge : mesh.half_edges(face)) { result.push_back(half_edge); }
   return result;
}

} // namespace

TEST_CASE("MeshRecording reverts and recomputes identical topology handles", "[recording][mesh]") {
   HalfEdgeMesh mesh;

   auto recording = mesh.begin_recording();
   const RecordedTriangle original = add_triangle(mesh);
   MeshPatch patch = recording.commit();
   REQUIRE(original.face != FaceHandle {});
   CHECK(patch.byte_size() > 0);

   mesh.revert(patch);
   const HalfEdgeMesh &reverted = mesh;
   for (VertexHandle vertex : original.vertices) { CHECK(reverted.position(vertex) == nullptr); }
   CHECK(std::ranges::empty(reverted.half_edges(original.face)));

   auto replay_recording = mesh.begin_recording();
   const RecordedTriangle replayed = add_triangle(mesh);
   [[maybe_unused]] MeshPatch replay_patch = replay_recording.commit();

   CHECK(replayed.vertices == original.vertices);
   CHECK(replayed.face == original.face);
}

TEST_CASE("MeshRecording destruction rolls back topology and attributes", "[recording][mesh]") {
   HalfEdgeMesh mesh;
   const VertexHandle retained = mesh.add_vertex({1, 2, 3});
   VertexHandle temporary;

   {
      [[maybe_unused]] auto recording = mesh.begin_recording();
      *mesh.position(retained) = {4, 5, 6};
      temporary = mesh.add_vertex({7, 8, 9});
   }

   const HalfEdgeMesh &reverted = mesh;
   REQUIRE(reverted.position(retained) != nullptr);
   CHECK(*reverted.position(retained) == (godot::Vector3 {1, 2, 3}));
   CHECK(reverted.position(temporary) == nullptr);
}

TEST_CASE("MeshRecording restores topology changed by an adjacent face", "[recording][mesh]") {
   HalfEdgeMesh mesh;
   const VertexHandle first = mesh.add_vertex({0, 0, 0});
   const VertexHandle second = mesh.add_vertex({1, 0, 0});
   const VertexHandle third = mesh.add_vertex({0, 1, 0});
   const VertexHandle fourth = mesh.add_vertex({1, 1, 0});
   const std::array base_vertices {first, second, third};
   const FaceHandle base = mesh.add_face(base_vertices);
   REQUIRE(base != FaceHandle {});

   const std::array adjacent_vertices {second, first, fourth};
   auto recording = mesh.begin_recording();
   const FaceHandle adjacent = mesh.add_face(adjacent_vertices);
   const std::vector<HalfEdgeHandle> original_edges = face_edges(mesh, adjacent);
   MeshPatch patch = recording.commit();
   REQUIRE(adjacent != FaceHandle {});

   mesh.revert(patch);
   CHECK(face_edges(mesh, adjacent).empty());
   CHECK(std::ranges::distance(mesh.half_edges(first)) == 2);
   CHECK(std::ranges::distance(mesh.half_edges(second)) == 2);
   CHECK(std::ranges::distance(mesh.half_edges(fourth)) == 0);
   CHECK(std::ranges::distance(mesh.half_edges(base)) == 3);

   auto replay_recording = mesh.begin_recording();
   const FaceHandle replayed = mesh.add_face(adjacent_vertices);
   [[maybe_unused]] MeshPatch replay_patch = replay_recording.commit();
   CHECK(replayed == adjacent);
   CHECK(face_edges(mesh, replayed) == original_edges);
}

TEST_CASE("MeshRecording rolls back while unwinding an exception", "[recording][mesh]") {
   HalfEdgeMesh mesh;
   VertexHandle temporary;

   CHECK_THROWS_AS(
       [&] {
          auto recording = mesh.begin_recording();
          temporary = mesh.add_vertex({1, 2, 3});
          throw std::runtime_error("expected failure");
       }(),
       std::runtime_error);

   CHECK(std::as_const(mesh).position(temporary) == nullptr);
}

TEST_CASE("HalfEdgeMesh rejects nested and stale recordings", "[recording][mesh]") {
   HalfEdgeMesh mesh;
   auto first_recording = mesh.begin_recording();
   CHECK_THROWS_AS((void)mesh.begin_recording(), std::logic_error);
   const VertexHandle first = mesh.add_vertex({0, 0, 0});
   MeshPatch first_patch = first_recording.commit();

   auto second_recording = mesh.begin_recording();
   [[maybe_unused]] const VertexHandle second = mesh.add_vertex({1, 0, 0});
   MeshPatch second_patch = second_recording.commit();

   CHECK_THROWS_AS(mesh.revert(first_patch), std::logic_error);
   mesh.revert(second_patch);
   mesh.revert(first_patch);
   CHECK(std::as_const(mesh).position(first) == nullptr);
   CHECK_THROWS_AS(mesh.revert(first_patch), std::logic_error);
}

TEST_CASE("HalfEdgeMesh rejects a patch from another mesh", "[recording][mesh]") {
   HalfEdgeMesh first;
   HalfEdgeMesh second;
   auto recording = first.begin_recording();
   [[maybe_unused]] const VertexHandle vertex = first.add_vertex({0, 0, 0});
   MeshPatch patch = recording.commit();

   CHECK_THROWS_AS(second.revert(patch), std::logic_error);
}

TEST_CASE("Non-const attribute access is recorded", "[recording][mesh]") {
   HalfEdgeMesh mesh;
   const VertexHandle vertex = mesh.add_vertex({1, 2, 3});

   auto recording = mesh.begin_recording();
   *mesh.position(vertex) = {4, 5, 6};
   MeshPatch patch = recording.commit();
   CHECK(*std::as_const(mesh).position(vertex) == (godot::Vector3 {4, 5, 6}));

   mesh.revert(patch);
   CHECK(*std::as_const(mesh).position(vertex) == (godot::Vector3 {1, 2, 3}));
}

TEST_CASE("Const attribute access does not add patch data", "[recording][mesh]") {
   HalfEdgeMesh mesh;
   const VertexHandle vertex = mesh.add_vertex({1, 2, 3});

   auto recording = mesh.begin_recording();
   REQUIRE(std::as_const(mesh).position(vertex) != nullptr);
   MeshPatch patch = recording.commit();

   CHECK(patch.byte_size() == 0);
   mesh.revert(patch);
}

TEST_CASE("Non-const UV access is recorded", "[recording][mesh]") {
   HalfEdgeMesh mesh;
   const RecordedTriangle triangle = add_triangle(mesh);
   REQUIRE(triangle.face != FaceHandle {});
   const HalfEdgeHandle half_edge = face_edges(mesh, triangle.face).front();
   const godot::Vector2 original = *std::as_const(mesh).uv(half_edge);

   auto recording = mesh.begin_recording();
   *mesh.uv(half_edge) = {0.25F, 0.75F};
   MeshPatch patch = recording.commit();

   mesh.revert(patch);
   CHECK(*std::as_const(mesh).uv(half_edge) == original);
}

TEST_CASE("An empty committed recording can only be reverted once", "[recording][mesh]") {
   HalfEdgeMesh mesh;
   auto recording = mesh.begin_recording();
   MeshPatch patch = recording.commit();

   mesh.revert(patch);
   CHECK_THROWS_AS(mesh.revert(patch), std::logic_error);
}
