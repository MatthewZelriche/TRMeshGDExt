#include "mesh/half_edge_mesh.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <ranges>

namespace {

std::size_t half_edge_count(const HalfEdgeMesh &mesh, VertexHandle vertex) {
   return static_cast<std::size_t>(std::ranges::distance(mesh.half_edges(vertex)));
}

bool has_uv(const HalfEdgeMesh &mesh, VertexHandle vertex, const godot::Vector2 &expected) {
   for (HalfEdgeHandle half_edge : mesh.half_edges(vertex)) {
      const godot::Vector2 *uv = mesh.uv(half_edge);
      REQUIRE(uv != nullptr);
      if (*uv == expected) { return true; }
   }
   return false;
}

} // namespace

TEST_CASE("HalfEdgeMesh creates paired half-edges and a boundary loop") {
   HalfEdgeMesh mesh;
   const VertexHandle a = mesh.add_vertex({0, 0, 0});
   const VertexHandle b = mesh.add_vertex({1, 0, 0});
   const VertexHandle c = mesh.add_vertex({0, 1, 0});
   const std::array vertices {a, b, c};
   const std::array corner_uvs {
       godot::Vector2 {0.25, 0.25},
       godot::Vector2 {0.75, 0.25},
       godot::Vector2 {0.25, 0.75},
   };

   const FaceHandle face = mesh.add_face(vertices, corner_uvs);

   REQUIRE(face != FaceHandle {});
   REQUIRE(half_edge_count(mesh, a) == 2);
   REQUIRE(half_edge_count(mesh, b) == 2);
   REQUIRE(half_edge_count(mesh, c) == 2);
   REQUIRE(has_uv(mesh, a, corner_uvs[0]));
   REQUIRE(has_uv(mesh, b, corner_uvs[1]));
   REQUIRE(has_uv(mesh, c, corner_uvs[2]));

   std::size_t corner = 0;
   for (HalfEdgeHandle half_edge : mesh.half_edges(face)) {
      REQUIRE(*mesh.uv(half_edge) == corner_uvs[corner]);
      ++corner;
   }
   REQUIRE(corner == 3);
}

TEST_CASE("HalfEdgeMesh splices an adjacent face into its boundary") {
   HalfEdgeMesh mesh;
   const VertexHandle a = mesh.add_vertex({0, 0, 0});
   const VertexHandle b = mesh.add_vertex({1, 0, 0});
   const VertexHandle c = mesh.add_vertex({0, 1, 0});
   const VertexHandle d = mesh.add_vertex({1, 1, 0});
   const std::array first {a, b, c};
   const std::array second {b, a, d};

   const FaceHandle first_face = mesh.add_face(first);
   const FaceHandle second_face = mesh.add_face(second);
   REQUIRE(first_face != FaceHandle {});
   REQUIRE(second_face != FaceHandle {});

   REQUIRE(half_edge_count(mesh, a) == 3);
   REQUIRE(half_edge_count(mesh, b) == 3);
   REQUIRE(half_edge_count(mesh, c) == 2);
   REQUIRE(half_edge_count(mesh, d) == 2);
   REQUIRE(std::ranges::distance(mesh.half_edges(first_face)) == 3);
   REQUIRE(std::ranges::distance(mesh.half_edges(second_face)) == 3);
}

TEST_CASE("HalfEdgeMesh rejects an occupied directed edge without mutation") {
   HalfEdgeMesh mesh;
   const VertexHandle a = mesh.add_vertex({0, 0, 0});
   const VertexHandle b = mesh.add_vertex({1, 0, 0});
   const VertexHandle c = mesh.add_vertex({0, 1, 0});
   const VertexHandle d = mesh.add_vertex({1, 1, 0});
   const std::array first {a, b, c};
   const std::array conflicting {a, b, d};

   REQUIRE(mesh.add_face(first) != FaceHandle {});
   REQUIRE(mesh.add_face(conflicting) == FaceHandle {});

   REQUIRE(half_edge_count(mesh, a) == 2);
   REQUIRE(half_edge_count(mesh, b) == 2);
   REQUIRE(half_edge_count(mesh, c) == 2);
   REQUIRE(half_edge_count(mesh, d) == 0);
}

TEST_CASE("HalfEdgeMesh rejects a disconnected fan at an existing vertex") {
   HalfEdgeMesh mesh;
   const VertexHandle a = mesh.add_vertex({0, 0, 0});
   const VertexHandle b = mesh.add_vertex({1, 0, 0});
   const VertexHandle c = mesh.add_vertex({0, 1, 0});
   const VertexHandle d = mesh.add_vertex({-1, 0, 0});
   const VertexHandle e = mesh.add_vertex({0, -1, 0});
   const std::array first {a, b, c};
   const std::array disconnected {a, d, e};

   REQUIRE(mesh.add_face(first) != FaceHandle {});
   REQUIRE(mesh.add_face(disconnected) == FaceHandle {});

   REQUIRE(half_edge_count(mesh, a) == 2);
   REQUIRE(half_edge_count(mesh, d) == 0);
   REQUIRE(half_edge_count(mesh, e) == 0);
}

TEST_CASE("HalfEdgeMesh can consume an entire boundary loop") {
   HalfEdgeMesh mesh;
   const VertexHandle a = mesh.add_vertex({0, 0, 0});
   const VertexHandle b = mesh.add_vertex({1, 0, 0});
   const VertexHandle c = mesh.add_vertex({0, 1, 0});
   const std::array front {a, b, c};
   const std::array back {b, a, c};

   REQUIRE(mesh.add_face(front) != FaceHandle {});
   REQUIRE(mesh.add_face(back) != FaceHandle {});

   REQUIRE(half_edge_count(mesh, a) == 2);
   REQUIRE(half_edge_count(mesh, b) == 2);
   REQUIRE(half_edge_count(mesh, c) == 2);
}

TEST_CASE("HalfEdgeMesh generates normalized planar UVs") {
   HalfEdgeMesh mesh;
   const VertexHandle a = mesh.add_vertex({0, 0, 2});
   const VertexHandle b = mesh.add_vertex({2, 0, 2});
   const VertexHandle c = mesh.add_vertex({0, 2, 2});
   const std::array vertices {a, b, c};

   REQUIRE(mesh.add_face(vertices) != FaceHandle {});
   REQUIRE(has_uv(mesh, b, {1, 0}));
   REQUIRE(has_uv(mesh, c, {0, 1}));
}

TEST_CASE("HalfEdgeMesh builds a closed tetrahedral manifold") {
   HalfEdgeMesh mesh;
   const VertexHandle a = mesh.add_vertex({0, 0, 0});
   const VertexHandle b = mesh.add_vertex({1, 0, 0});
   const VertexHandle c = mesh.add_vertex({0, 1, 0});
   const VertexHandle d = mesh.add_vertex({0, 0, 1});
   const std::array faces {
       std::array {a, b, c},
       std::array {b, a, d},
       std::array {c, b, d},
       std::array {a, c, d},
   };

   for (const auto &vertices : faces) {
      const FaceHandle face = mesh.add_face(vertices);
      REQUIRE(face != FaceHandle {});
      REQUIRE(std::ranges::distance(mesh.half_edges(face)) == 3);
   }

   REQUIRE(half_edge_count(mesh, a) == 3);
   REQUIRE(half_edge_count(mesh, b) == 3);
   REQUIRE(half_edge_count(mesh, c) == 3);
   REQUIRE(half_edge_count(mesh, d) == 3);
}

TEST_CASE("HalfEdgeMesh rejects malformed face inputs without mutation") {
   HalfEdgeMesh mesh;
   const VertexHandle a = mesh.add_vertex({0, 0, 0});
   const VertexHandle b = mesh.add_vertex({1, 0, 0});
   const VertexHandle c = mesh.add_vertex({0, 1, 0});
   const std::array too_short {a, b};
   const std::array duplicate {a, b, a};
   const std::array valid_vertices {a, b, c};
   const std::array wrong_uv_count {godot::Vector2 {0, 0}, godot::Vector2 {1, 0}};

   REQUIRE(mesh.add_face(too_short) == FaceHandle {});
   REQUIRE(mesh.add_face(duplicate) == FaceHandle {});
   REQUIRE(mesh.add_face(valid_vertices, wrong_uv_count) == FaceHandle {});
   REQUIRE(mesh.add_face(std::array {a, b, VertexHandle {}}) == FaceHandle {});

   REQUIRE(half_edge_count(mesh, a) == 0);
   REQUIRE(half_edge_count(mesh, b) == 0);
   REQUIRE(half_edge_count(mesh, c) == 0);
}
