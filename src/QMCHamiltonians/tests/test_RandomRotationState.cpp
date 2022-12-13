//////////////////////////////////////////////////////////////////////////////////////
// This file is distributed under the University of Illinois/NCSA Open Source License.
// See LICENSE file in top directory for details.
//
// Copyright (c) 2022 QMCPACK developers.
//
// File developed by: Mark Dewing, mdewing@anl.gov, Argonne National Laboratory
//
//////////////////////////////////////////////////////////////////////////////////////

#include "catch.hpp"

#include "QMCHamiltonians/RandomRotationState.h"
#include <string>
#include <vector>

namespace qmcplusplus
{

namespace testing
{


TEST_CASE("RandomRotationState", "[hamiltonian]")
{
  RandomRotationState rs;
  RandomRotationState::TensorType rmat = rs.getRandomRotationMatrix();

  // Default rotation matrix should be the identity
  for (int i = 0; i < 3; i++)
    for (int j = 0; j < 3; j++)
      if (i == j)
        CHECK(rmat(i, j) == Approx(1.0));
      else
        CHECK(rmat(i, j) == Approx(0.0));
}

} // namespace testing
} // namespace qmcplusplus
