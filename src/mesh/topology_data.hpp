#pragma once

#include "utils/slot_map.hpp"

#include <godot_cpp/variant/vector2.hpp>
#include <godot_cpp/variant/vector3.hpp>

struct VertexMapTag {};
struct HalfEdgeMapTag {};
struct FaceMapTag {};

struct VertexPositionTag {};
struct HalfEdgeUVTag {};

using VertexHandle = SlotHandle<VertexMapTag>;
using HalfEdgeHandle = SlotHandle<HalfEdgeMapTag>;
using FaceHandle = SlotHandle<FaceMapTag>;

struct Vertex {
   HalfEdgeHandle outgoing;
};

struct HalfEdge {
   VertexHandle origin;
   HalfEdgeHandle twin;
   HalfEdgeHandle prev;
   HalfEdgeHandle next;
   FaceHandle face;
};

struct Face {
   HalfEdgeHandle boundary;
};

using VertexMap = SlotMap<VertexMapTag, Col<Vertex>, Col<godot::Vector3, VertexPositionTag>>;
using HalfEdgeMap = SlotMap<HalfEdgeMapTag, Col<HalfEdge>, Col<godot::Vector2, HalfEdgeUVTag>>;
using FaceMap = SlotMap<FaceMapTag, Col<Face>>;
