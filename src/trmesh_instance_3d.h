#pragma once

#include "mesh/half_edge_mesh.h"

#include <godot_cpp/classes/mesh_instance3d.hpp>

class TRMeshInstance3D : public godot::MeshInstance3D {
	GDCLASS(TRMeshInstance3D, godot::MeshInstance3D)

protected:
	static void _bind_methods();

private:
	HalfEdgeMesh mHalfEdgeMesh;

public:
	TRMeshInstance3D() = default;
	~TRMeshInstance3D() override = default;
};
