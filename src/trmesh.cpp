#include "trmesh.h"

void TRMesh::_bind_methods() {
	ClassDB::bind_method(D_METHOD("hello"), &TRMesh::hello);
}

String TRMesh::hello() const {
	return "Hello from TRMesh GDExtension";
}
