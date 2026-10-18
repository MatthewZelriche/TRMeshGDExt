#pragma once

#include <cassert>
#include <cstddef>
#include <iterator>
#include <ranges>

#include "mesh/half_edge_mesh.h"

// A finite view over every outgoing half-edge in a vertex's connected fan,
// including boundary half-edges whose face is null. Every half-edge must have
// a valid twin, and boundary half-edges must participate in linked boundary
// loops. Invalid and isolated vertices produce an empty view. Any operation
// that changes face or half-edge topology invalidates active iterators.
class HalfEdgeMesh::HalfEdgesAroundVertexView :
    public std::ranges::view_interface<HalfEdgeMesh::HalfEdgesAroundVertexView> {
  public:
   class Iterator {
     public:
      using value_type = HalfEdgeHandle;
      using difference_type = std::ptrdiff_t;
      using iterator_concept = std::forward_iterator_tag;
      using iterator_category = std::forward_iterator_tag;

      Iterator() noexcept = default;

      HalfEdgeHandle operator*() const noexcept {
         assert(mCurrent != HalfEdgeHandle {});
         return mCurrent;
      }

      Iterator &operator++() noexcept {
         assert(mMesh != nullptr);
         assert(mCurrent != HalfEdgeHandle {});

         assert(mRemaining != 0 && "corrupt vertex fan does not terminate");
         --mRemaining;
         mCurrent = next(mMesh, mCurrent, mFirst);
         return *this;
      }

      Iterator operator++(int) noexcept {
         Iterator previous = *this;
         ++*this;
         return previous;
      }

      friend bool operator==(const Iterator &left, const Iterator &right) noexcept {
         return left.mMesh == right.mMesh && left.mFirst == right.mFirst &&
                left.mCurrent == right.mCurrent;
      }

      friend bool operator==(const Iterator &iterator, std::default_sentinel_t) noexcept {
         return iterator.mCurrent == HalfEdgeHandle {};
      }

     private:
      friend class HalfEdgesAroundVertexView;

      Iterator(const HalfEdgeMesh *mesh, HalfEdgeHandle first, std::size_t half_edge_count) noexcept
          : mMesh(mesh), mFirst(first), mCurrent(first), mRemaining(half_edge_count) {}

      const HalfEdgeMesh *mMesh = nullptr;
      HalfEdgeHandle mFirst;
      HalfEdgeHandle mCurrent;
      std::size_t mRemaining = 0;
   };

   HalfEdgesAroundVertexView() noexcept = default;

   Iterator begin() const noexcept {
      if (mMesh == nullptr) { return {}; }
      return {mMesh, first(mMesh, mVertex), mMesh->mHalfEdges.size()};
   }

   static std::default_sentinel_t end() noexcept { return {}; }

  private:
   friend class HalfEdgeMesh;

   HalfEdgesAroundVertexView(const HalfEdgeMesh *mesh, VertexHandle vertex) noexcept :
       mMesh(mesh), mVertex(vertex) {}

   static HalfEdgeHandle first(const HalfEdgeMesh *mesh, VertexHandle vertex) noexcept {
      const Vertex *vertex_data = mesh->mVertices.get<Vertex>(vertex);
      if (vertex_data == nullptr || vertex_data->outgoing == HalfEdgeHandle {}) { return {}; }

      [[maybe_unused]] const HalfEdge *outgoing =
          mesh->mHalfEdges.get<HalfEdge>(vertex_data->outgoing);
      assert(outgoing != nullptr && outgoing->origin == vertex && "vertex has invalid half-edge");
      return vertex_data->outgoing;
   }

   static HalfEdgeHandle next(const HalfEdgeMesh *mesh, HalfEdgeHandle half_edge,
                              HalfEdgeHandle first) noexcept {
      const HalfEdge *half_edge_data = mesh->mHalfEdges.get<HalfEdge>(half_edge);
      assert(half_edge_data != nullptr && "vertex iterator contains an invalid half-edge");
      assert(half_edge_data->twin != HalfEdgeHandle {} && "half-edge has no twin");

      const HalfEdge *twin = mesh->mHalfEdges.get<HalfEdge>(half_edge_data->twin);
      assert(twin != nullptr && "half-edge has an invalid twin");

      const HalfEdgeHandle next_half_edge = twin->next;
      const HalfEdge *next_data = mesh->mHalfEdges.get<HalfEdge>(next_half_edge);
      assert(next_data != nullptr && next_data->origin == half_edge_data->origin &&
             "half-edge traversal left the vertex fan");

      if (next_half_edge == first) { return {}; }
      return next_half_edge;
   }

   const HalfEdgeMesh *mMesh = nullptr;
   VertexHandle mVertex;
};

// A finite view over the half-edges forming a face boundary, in face order.
// Invalid faces produce an empty view. Any operation that changes face or
// half-edge topology invalidates active iterators.
class HalfEdgeMesh::HalfEdgesAroundFaceView :
    public std::ranges::view_interface<HalfEdgeMesh::HalfEdgesAroundFaceView> {
  public:
   class Iterator {
     public:
      using value_type = HalfEdgeHandle;
      using difference_type = std::ptrdiff_t;
      using iterator_concept = std::forward_iterator_tag;
      using iterator_category = std::forward_iterator_tag;

      Iterator() noexcept = default;

      HalfEdgeHandle operator*() const noexcept {
         assert(mCurrent != HalfEdgeHandle {});
         return mCurrent;
      }

      Iterator &operator++() noexcept {
         assert(mMesh != nullptr);
         assert(mCurrent != HalfEdgeHandle {});
         assert(mRemaining != 0 && "corrupt face boundary does not terminate");

         --mRemaining;
         mCurrent = next(mMesh, mCurrent, mFirst);
         return *this;
      }

      Iterator operator++(int) noexcept {
         Iterator previous = *this;
         ++*this;
         return previous;
      }

      friend bool operator==(const Iterator &left, const Iterator &right) noexcept {
         return left.mMesh == right.mMesh && left.mFirst == right.mFirst &&
                left.mCurrent == right.mCurrent;
      }

      friend bool operator==(const Iterator &iterator, std::default_sentinel_t) noexcept {
         return iterator.mCurrent == HalfEdgeHandle {};
      }

     private:
      friend class HalfEdgesAroundFaceView;

      Iterator(const HalfEdgeMesh *mesh, HalfEdgeHandle first, std::size_t half_edge_count) noexcept
          : mMesh(mesh), mFirst(first), mCurrent(first), mRemaining(half_edge_count) {}

      const HalfEdgeMesh *mMesh = nullptr;
      HalfEdgeHandle mFirst;
      HalfEdgeHandle mCurrent;
      std::size_t mRemaining = 0;
   };

   HalfEdgesAroundFaceView() noexcept = default;

   Iterator begin() const noexcept {
      if (mMesh == nullptr) { return {}; }
      return {mMesh, first(mMesh, mFace), mMesh->mHalfEdges.size()};
   }

   static std::default_sentinel_t end() noexcept { return {}; }

  private:
   friend class HalfEdgeMesh;

   HalfEdgesAroundFaceView(const HalfEdgeMesh *mesh, FaceHandle face) noexcept :
       mMesh(mesh), mFace(face) {}

   static HalfEdgeHandle first(const HalfEdgeMesh *mesh, FaceHandle face) noexcept {
      const Face *face_data = mesh->mFaces.get(face);
      if (face_data == nullptr) { return {}; }

      const HalfEdge *boundary = mesh->mHalfEdges.get<HalfEdge>(face_data->boundary);
      assert(boundary != nullptr && boundary->face == face &&
             "face has an invalid boundary half-edge");
      return face_data->boundary;
   }

   static HalfEdgeHandle next(const HalfEdgeMesh *mesh, HalfEdgeHandle half_edge,
                              HalfEdgeHandle first) noexcept {
      const HalfEdge *half_edge_data = mesh->mHalfEdges.get<HalfEdge>(half_edge);
      assert(half_edge_data != nullptr && "face iterator contains an invalid half-edge");

      const HalfEdgeHandle next_half_edge = half_edge_data->next;
      const HalfEdge *next_data = mesh->mHalfEdges.get<HalfEdge>(next_half_edge);
      assert(next_data != nullptr && next_data->face == half_edge_data->face &&
             "half-edge traversal left the face boundary");

      if (next_half_edge == first) { return {}; }
      return next_half_edge;
   }

   const HalfEdgeMesh *mMesh = nullptr;
   FaceHandle mFace;
};

static_assert(std::ranges::forward_range<HalfEdgeMesh::HalfEdgesAroundVertexView>);
static_assert(std::ranges::forward_range<HalfEdgeMesh::HalfEdgesAroundFaceView>);
