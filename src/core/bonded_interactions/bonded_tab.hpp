/*
  Copyright (C) 2010-2018 The ESPResSo project
  Copyright (C) 2002,2003,2004,2005,2006,2007,2008,2009,2010
    Max-Planck-Institute for Polymer Research, Theory Group

  This file is part of ESPResSo.

  ESPResSo is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  ESPResSo is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/
#ifndef CORE_BONDED_INTERACTIONS_TABULATED_HPP
#define CORE_BONDED_INTERACTIONS_TABULATED_HPP

/** \file
 *  Routines to calculate the energy and/or force for particle bonds, angles
 *  and dihedrals via interpolation of lookup tables.
 *
 *  Implementation in \ref bonded_tab.cpp.
 */

#include "config.hpp"

#include "angle_common.hpp"
#include "bonded_interactions/bonded_interaction_data.hpp"
#include "bonded_interactions/dihedral.hpp"
#include "debug.hpp"
#include "particle_data.hpp"
#include <tuple>

#include <utils/constants.hpp>
#include <utils/math/sqr.hpp>

/** Set the parameters of a bonded tabulated potential.
 *  ia_params and force/energy tables are communicated to each node.
 *
 *  @param bond_type    Bond type for which the interaction is defined
 *  @param tab_type     Table type
 *  @param min          @copybrief TabulatedPotential::minval
 *  @param max          @copybrief TabulatedPotential::maxval
 *  @param energy       @copybrief TabulatedPotential::energy_tab
 *  @param force        @copybrief TabulatedPotential::force_tab
 *
 *  @retval ES_OK on success
 *  @retval ES_ERROR on error
 */
int tabulated_bonded_set_params(int bond_type,
                                TabulatedBondedInteraction tab_type, double min,
                                double max, std::vector<double> const &energy,
                                std::vector<double> const &force);

/** Compute a tabulated bond length force.
 *
 *  The force acts in the direction of the connecting vector between the
 *  particles. For distances smaller than the tabulated range it uses a linear
 *  extrapolation based on the first two tabulated force values.
 *
 *  @param[in]  iaparams  Bonded parameters for the pair interaction.
 *  @param[in]  dx        %Distance between the particles.
 *  @param[out] force     Force.
 *  @return whether the bond is broken
 */
inline bool calc_tab_bond_force(Bonded_ia_parameters const *const iaparams,
                                Utils::Vector3d const &dx,
                                Utils::Vector3d &force) {
  auto const *tab_pot = iaparams->p.tab.pot;
  auto const dist = dx.norm();

  if (dist < tab_pot->cutoff()) {
    auto const fac = tab_pot->force(dist) / dist;
    force = fac * dx;
    return false;
  }
  return true;
}

/** Compute a tabulated bond length energy.
 *
 *  For distances smaller than the tabulated range it uses a quadratic
 *  extrapolation based on the first two tabulated force values and the first
 *  tabulated energy value.
 *
 *  @param[in]  iaparams  Bonded parameters for the pair interaction.
 *  @param[in]  dx        %Distance between the particles.
 *  @param[out] _energy   Energy.
 *  @return whether the bond is broken
 */
inline bool tab_bond_energy(Bonded_ia_parameters const *const iaparams,
                            Utils::Vector3d const &dx, double *_energy) {
  auto const *tab_pot = iaparams->p.tab.pot;
  auto const dist = dx.norm();

  if (dist < tab_pot->cutoff()) {
    *_energy = tab_pot->energy(dist);
    return false;
  }
  return true;
}

/** Compute the three-body angle interaction force.
 *  @param  p_mid     Second/middle particle.
 *  @param  p_left    First/left particle.
 *  @param  p_right   Third/right particle.
 *  @param  iaparams  Bonded parameters for the angle interaction.
 *  @return Forces on the second, first and third particles, in that order.
 */
inline std::tuple<Utils::Vector3d, Utils::Vector3d, Utils::Vector3d>
calc_angle_3body_tabulated_forces(Particle const *const p_mid,
                                  Particle const *const p_left,
                                  Particle const *const p_right,
                                  Bonded_ia_parameters const *const iaparams) {

  auto forceFactor = [&iaparams](double const cos_phi) {
    auto const sin_phi = sqrt(1 - Utils::sqr(cos_phi));
#ifdef TABANGLEMINUS
    auto const phi = acos(-cos_phi);
#else
    auto const phi = acos(cos_phi);
#endif
    auto const *tab_pot = iaparams->p.tab.pot;
    auto const gradient = tab_pot->force(phi);
    return -gradient / sin_phi;
  };

  return calc_angle_generic_force(p_mid->r.p, p_left->r.p, p_right->r.p,
                                  forceFactor, true);
}

/** Compute the three-body angle interaction force.
 *  @param[in]  p_mid     Second/middle particle.
 *  @param[in]  p_left    First/left particle.
 *  @param[in]  p_right   Third/right particle.
 *  @param[in]  iaparams  Bonded parameters for the angle interaction.
 *  @param[out] f_mid     Force on @p p_mid.
 *  @param[out] f_left    Force on @p p_left.
 *  @param[out] f_right   Force on @p p_right.
 *  @retval false
 */
inline bool calc_tab_angle_force(
    Particle const *const p_mid, Particle const *const p_left,
    Particle const *const p_right, Bonded_ia_parameters const *const iaparams,
    Utils::Vector3d &f_mid, Utils::Vector3d &f_left, Utils::Vector3d &f_right) {

  std::tie(f_mid, f_left, f_right) =
      calc_angle_3body_tabulated_forces(p_mid, p_left, p_right, iaparams);
  return false;
}

/** Compute the three-body angle interaction energy.
 *  It is assumed that the potential is tabulated
 *  for all angles between 0 and Pi.
 *
 *  @param[in]  p_mid     Second/middle particle.
 *  @param[in]  p_left    First/left particle.
 *  @param[in]  p_right   Third/right particle.
 *  @param[in]  iaparams  Bonded parameters for the angle interaction.
 *  @param[out] _energy   Energy.
 *  @retval false
 */
inline bool tab_angle_energy(Particle const *const p_mid,
                             Particle const *const p_left,
                             Particle const *const p_right,
                             Bonded_ia_parameters const *const iaparams,
                             double *_energy) {
  auto const vectors =
      calc_vectors_and_cosine(p_mid->r.p, p_left->r.p, p_right->r.p, true);
  auto const cos_phi = std::get<4>(vectors);
  /* calculate phi */
#ifdef TABANGLEMINUS
  auto const phi = acos(-cos_phi);
#else
  auto const phi = acos(cos_phi);
#endif
  *_energy = iaparams->p.tab.pot->energy(phi);
  return false;
}

/** Compute the four-body dihedral interaction force.
 *  This function is not tested yet.
 *
 *  @param[in]  p2        Second particle.
 *  @param[in]  p1        First particle.
 *  @param[in]  p3        Third particle.
 *  @param[in]  p4        Fourth particle.
 *  @param[in]  iaparams  Bonded parameters for the dihedral interaction.
 *  @param[out] force2    Force on particle 2.
 *  @param[out] force1    Force on particle 1.
 *  @param[out] force3    Force on particle 3.
 *  @return false
 */
inline bool
calc_tab_dihedral_force(Particle const *const p2, Particle const *const p1,
                        Particle const *const p3, Particle const *const p4,
                        Bonded_ia_parameters const *const iaparams,
                        Utils::Vector3d &force2, Utils::Vector3d &force1,
                        Utils::Vector3d &force3) {
  /* vectors for dihedral angle calculation */
  Utils::Vector3d v12, v23, v34, v12Xv23, v23Xv34;
  double l_v12Xv23, l_v23Xv34;
  /* dihedral angle, cosine of the dihedral angle, cosine of the bond angles */
  double phi, cos_phi;
  /* force factors */
  auto const *tab_pot = iaparams->p.tab.pot;

  /* dihedral angle */
  calc_dihedral_angle(p1, p2, p3, p4, v12, v23, v34, v12Xv23, &l_v12Xv23,
                      v23Xv34, &l_v23Xv34, &cos_phi, &phi);
  /* dihedral angle not defined - force zero */
  if (phi == -1.0) {
    force1 = {};
    force2 = {};
    force3 = {};
    return false;
  }

  /* calculate force components (directions) */
  auto const f1 = (v23Xv34 - cos_phi * v12Xv23) / l_v12Xv23;
  auto const f4 = (v12Xv23 - cos_phi * v23Xv34) / l_v23Xv34;

  auto const v23Xf1 = vector_product(v23, f1);
  auto const v23Xf4 = vector_product(v23, f4);
  auto const v34Xf4 = vector_product(v34, f4);
  auto const v12Xf1 = vector_product(v12, f1);

  /* table lookup */
  auto const fac = tab_pot->force(phi);

  /* store dihedral forces */
  force1 = fac * v23Xf1;
  force2 = fac * (v34Xf4 - v12Xf1 - v23Xf1);
  force3 = fac * (v12Xf1 - v23Xf4 - v34Xf4);

  return false;
}

/** Compute the four-body dihedral interaction energy.
 *  This function is not tested yet.
 *
 *  @param[in]  p2        Second particle.
 *  @param[in]  p1        First particle.
 *  @param[in]  p3        Third particle.
 *  @param[in]  p4        Fourth particle.
 *  @param[in]  iaparams  Bonded parameters for the dihedral interaction.
 *  @param[out] _energy   Energy.
 *  @return false
 */
inline bool tab_dihedral_energy(Particle const *const p2,
                                Particle const *const p1,
                                Particle const *const p3,
                                Particle const *const p4,
                                Bonded_ia_parameters const *const iaparams,
                                double *_energy) {
  /* vectors for dihedral calculations. */
  Utils::Vector3d v12, v23, v34, v12Xv23, v23Xv34;
  double l_v12Xv23, l_v23Xv34;
  /* dihedral angle, cosine of the dihedral angle */
  double phi, cos_phi;
  auto const *tab_pot = iaparams->p.tab.pot;
  calc_dihedral_angle(p1, p2, p3, p4, v12, v23, v34, v12Xv23, &l_v12Xv23,
                      v23Xv34, &l_v23Xv34, &cos_phi, &phi);
  *_energy = tab_pot->energy(phi);

  return false;
}

#endif
