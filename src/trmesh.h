#pragma once

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/variant/string.hpp>

using namespace godot;

class TRMesh : public RefCounted {
	GDCLASS(TRMesh, RefCounted)

protected:
	static void _bind_methods();

public:
	TRMesh() = default;
	~TRMesh() override = default;

	String hello() const;
};
