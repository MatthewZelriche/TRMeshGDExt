#pragma once

#include "utils/slot_map.hpp"

// Engine-independent half-edge mesh storage.
class HalfEdgeMesh {
  public:
   HalfEdgeMesh() = default;
   ~HalfEdgeMesh() = default;

  private:
   struct PlaceholderMapTag {};

   SlotMap<PlaceholderMapTag, Col<float>> mPlaceholder;
};
