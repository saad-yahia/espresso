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
#ifndef AFFINITY_H
#define AFFINITY_H

/** \file
 *  Routines to calculate the affinity force for a particle pair.
 *  \ref forces.cpp
 */

#include "grid.hpp"
#include "integrate.hpp"
#include "nonbonded_interactions/nonbonded_interaction_data.hpp"
#include "particle_data.hpp"
#include "random.hpp"

#ifdef AFFINITY

int affinity_set_params(int part_type_a, int part_type_b, int afftype,
                        double kappa, double r0, double Kon, double Koff,
                        double maxBond, double cut);

/** Calculate soft-sphere potential force between particle p1 and p2 */
inline void add_affinity_pair_force(Particle *const p1, Particle *const p2,
                                    IA_parameters const *const ia_params,
                                    Utils::Vector3d const &d, double dist,
                                    Utils::Vector3d &force) {

  // The affinity potential has the first argument affinity.type. This is to
  // differentiate between different implementations. For example one
  // implementation can take into account the detachment force, another not.
  int aff_type_extracted = 0;
  int period_for_output = -1;
  if (ia_params->affinity.type > 10) {
    aff_type_extracted = ia_params->affinity.type % 10;
    period_for_output = ia_params->affinity.type - aff_type_extracted;
  } else {
    aff_type_extracted = ia_params->affinity.type;
  }

  auto const unfolded_pos =
      unfolded_position(p1->r.p, p1->l.i, box_geo.length());
  auto const vec = p1->p.bond_site - unfolded_pos;
  auto const len = vec.norm();

  if (aff_type_extracted == 1) {
    /************************
     *
     * Here I can implement the affinity force.
     * I have the position of the particle - p1, and under p1->p.bond_site I
     * have the coordinate of the bond_site. Also, under d[3] I have the vector
     * towards the constraint meaning that force on p1 should be in the
     * direction of d[3].
     *
     * Algorithm:
     * 1. First check is, whether I am in the cut-off radius: ?dist <
     *    affinity.cut?.
     * 2. Then I check whether there exists a bond from the current particle:
     *    ?bond_site != -1?
     * 3. If yes, then I maintain the bond. I put the forces and afterwards I
     *    decide whether the bond will break or not.
     * 4. If no, I maintain the creation of a bond. First I check whether I am
     *    in the area of possible bond creation: ?dist < affinity.r0?
     * 5. If yes, I run the decision algorithm for bond creation and I either
     *    create or does not create the bond.
     * 6. If I am not in the area of possible bond creation I do nothing
     *
     * comments:
     * - strength of the force is proportional to the difference of
     *   actual bond length and relaxed bond length
     * - bond is always created, no probability is involved
     * - if bond length reaches maxBond, the bond immediately ruptures,
     *   no probability is involved
     *********************/
    double fac = 0.0;
    if (dist < ia_params->affinity.cut) { // Checking whether I am inside the
                                          // interaction cut-off radius.
      if (dist > 0.0) {
        // printf("bond_site: %f %f
        // %f\n",p1->p.bond_site[0],p1->p.bond_site[1],p1->p.bond_site[2]);
        if ((p1->p.bond_site[0] >= 0) && (p1->p.bond_site[1] >= 0) &&
            (p1->p.bond_site[2] >= 0)) // Checking whether any bond exists
        {                              // Bond exists
          if (len > ia_params->affinity.r0) {
            fac = ia_params->affinity.kappa * (len - ia_params->affinity.r0);
            // printf("len %f r0 %f\n",len, ia_params->affinity.r0);
          } else {
            fac = 0.0;
          }
          // double ftemp = 0;
          force += (fac / len) * vec;
          // printf("%f ",ftemp);
          // Decision whether I should break the bond: if the bond length is
          // greater than maxBond, it breaks.
          if (len > ia_params->affinity.maxBond) {
            p1->p.bond_site = {-1, -1, -1};
          }
        } else if (dist < ia_params->affinity
                              .r0) { // Bond does not exist, we are inside
                                     // of possible bond creation area,
                                     // let's talk about creating a bond
          // This implementation creates bond always
          p1->p.bond_site = unfolded_pos - d;
        }
      }
    }
  }
  if (aff_type_extracted == 2) { // second implementation of affinity
    /************************
     *
     * Here I can implement the affinity force.
     * I have the position of the particle - p1, and under p1->p.bond_site I
     * have the coordinate of the bond_site. Also, under d[3] I have the vector
     * towards the constraint meaning that force on p1 should be in the
     * direction of d[3].
     *
     * Algorithm:
     * 1. First check is whether I am in the cut-off radius: ?dist <
     *    affinity.cut?.
     * 2. Then I check whether there exists a bond from the current particle:
     *    ?bond_site != -1?
     * 3. If yes, then I maintain the bond. I put the forces and afterwards I
     *    decide whether the bond will break or not.
     * 4. If no, I maintain the creation of a bond. First I check whether I am
     *    in the area of possible bond creation: ?dist < affinity.r0?
     * 5. If yes, I run the decision algorithm for bond creation and I either
     *    create or does not create the bond.
     * 6. If I am not in the area of possible bond creation I do nothing
     *
     *
     * comments:
     * - strength of the force is proportional to the difference of actual
     *   bond length and relaxed bond length
     * - bond is created with probability 1-exp(-Kon*timestep)
     * - maxBond is not used, we use probability 1-exp(-Koff*timestep) to
     *   break the bond
     * - Koff depends on the bond length via Koff = K0*exp(F/Fd) =
     *   K0*exp(kappa(r-r0)/Fd)
     * - here, ia_params->Koff gives us K_0, off rate when bond is relaxed
     * - here, maxBond is used as detachment force F_d
     * - the original check for ensuring, that particle flows out of the
     *   cut-off radius and the bond remains active is replaced with fixed
     *   check, that bond length must not be greater that 0.8 cut_off
     *********************/
    double fac = 0.0;
    if (dist < ia_params->affinity.cut) { // Checking whether I am inside the
                                          // interaction cut-off radius.
      if (dist > 0.0) {
        // printf("bond_site: %f %f
        // %f\n",p1->p.bond_site[0],p1->p.bond_site[1],p1->p.bond_site[2]);
        if ((p1->p.bond_site[0] >= 0) && (p1->p.bond_site[1] >= 0) &&
            (p1->p.bond_site[2] >= 0)) // Checking whether any bond exists
        {                              // Bond exists
          if (len > ia_params->affinity.r0) {
            fac = ia_params->affinity.kappa * (len - ia_params->affinity.r0);
            // printf("len %f r0 %f\n",len, ia_params->affinity.r0);
          } else {
            fac = 0.0;
          }
          // double ftemp = 0;
          force += (fac / len) * vec;
          // ftemp += abs((fac / len) * vec);
          // if (ftemp > 0.000000000000001) printf("%f ",fac);
          // Decision whether I should break the bond:
          // First, force exerted on bond is stored in fac
          double tmpF = fac;
          // Then, zero force off rate K_0 is stored at ia_params_Koff
          double tmpK0 = ia_params->affinity.Koff;
          // Then, detachment force is stored in  ia_params->affinity.maxBond
          double tmpFd = ia_params->affinity.maxBond;
          // Then, compute Koff
          double tmpKoff = tmpK0 * exp(tmpF / tmpFd);
          // Finally, compute Poff
          double Poff = 1.0 - exp(-tmpKoff * time_step);
          // printf("%f ", Poff);
          if (len <
              0.8 *
                  ia_params->affinity.cut) { // in other implementation, maxBond
                                             // is used here. However, in this
                                             // implementation, we need maxBond
                                             // for setting detachment force F_d
            double decide = d_random();
            if (decide < Poff) {
              p1->p.bond_site = {-1, -1, -1};
            }
          } else {
            p1->p.bond_site = {-1, -1, -1};
            // printf("breaking: out of cut");
          }
          // Checkpoint output:
          if (period_for_output > 0)
            if (((int)floor(sim_time / time_step) % period_for_output == 0) &&
                (len > ia_params->affinity.r0)) {
              FILE *fp;
              double tmpPon = 1.0 - exp(-ia_params->affinity.Kon * time_step);
              fp = fopen("affinity_check.dat", "a");
              fprintf(fp, "sim_time %f, period_for_output %d aff type: %d ",
                      sim_time, period_for_output, aff_type_extracted);
              fprintf(fp,
                      "Pon %f, Kon %f, particle %d, Poff = %f, F = %f, Koff = "
                      "%f, K0 = %f, len = %f \n",
                      tmpPon, ia_params->affinity.Kon, p1->p.identity, Poff,
                      tmpF, tmpKoff, tmpK0, len);
              fclose(fp);
            }
        } else if (dist < ia_params->affinity
                              .r0) { // Bond does not exist, we are inside
                                     // of possible bond creation area,
                                     // let's talk about creating a bond
          double Pon = 1.0 - exp(-ia_params->affinity.Kon * time_step);
          // The probability is given by function Pon(x)= 1 - e^(-x) where x is
          // Kon*dt.
          double decide = d_random();
          if (decide <
              Pon) { // the bond will be created only with probability Pon.
            p1->p.bond_site = unfolded_pos - d;
          } else {
            // printf("In range, not creating: Pon = %f, decide = %f", Pon,
            // decide);
          }
        }
      }
    }
  }
  if (aff_type_extracted == 3) {
    /************************
     *
     * Here I can implement the affinity force.
     * I have the position of the particle - p1, and under p1->p.bond_site I
     * have the coordinate of the bond_site. Also, under d[3] I have the vector
     * towards the constraint meaning that force on p1 should be in the
     * direction of d[3].
     *
     * Algorithm:
     * 1. First check is whether I am in the cut-off radius: ?dist <
     *    affinity.cut?.
     * 2. Then I check whether there exists a bond from the current particle:
     *    ?bond_site != -1?
     * 3. If yes, then I maintain the bond. I put the forces and afterwards I
     *    decide whether the bond will break or not.
     * 4. If no, I maintain the creation of a bond. First I check whether I am
     *    in the area of possible bond creation: ?dist < affinity.r0?
     * 5. If yes, I run the decision algorithm for bond creation and I either
     *    create or does not create the bond.
     * 6. If I am not in the area of possible bond creation I do nothing
     *
     *
     * comments:
     * - strength of the force is proportional to the difference of
     *   actual bond length and relaxed bond length
     * - bond is created with probability 1-exp(-Kon*timestep)
     * - bond is ruptured with probability 1-exp(-Koff*timestep)
     * - to break the bond Koff is given as parameter, is not
     *   dependent on the force nor the bond length
     * - here, maxBond stands for ensuring, that particle flows out of the
     *   cut-off radius and the bond remains active
     * - maxBond should be always less than cut_off radius
     *********************/
    double fac = 0.0;
    if ((dist < ia_params->affinity.cut)) { // Checking whether I am inside
                                            // the interaction cut-off radius.
      if (dist > 0.0) {
        // printf("bond_site: %f %f
        // %f\n",p1->p.bond_site[0],p1->p.bond_site[1],p1->p.bond_site[2]);
        if ((p1->p.bond_site[0] >= 0) && (p1->p.bond_site[1] >= 0) &&
            (p1->p.bond_site[2] >= 0)) // Checking whether any bond exists
        {                              // Bond exists
          if (len > ia_params->affinity.r0) {
            fac = ia_params->affinity.kappa * (len - ia_params->affinity.r0) /
                  len;
            // printf("len %f r0 %f\n",len, ia_params->affinity.r0);
          } else
            fac = 0.0;
          // double ftemp = 0;
          force += fac * vec;
          // printf("%f ",ftemp);
          // Decision whether I should break the bond:
          // The random decision algorithm is much more complicated with Fd
          // detachment force etc. Here, I use much simpler rule, the same as
          // with Kon, except that the probability of bond breakage increases
          // with prolongation of the bond. If the bond reaches

          double Poff = 1.0 - exp(-ia_params->affinity.Koff * time_step);
          if (len < ia_params->affinity.maxBond) {
            double decide = d_random();
            if (decide < Poff) {
              p1->p.bond_site = {-1, -1, -1};
            }

          } else {
            p1->p.bond_site = {-1, -1, -1};
            // printf("breaking: out of cut");
          }
        } else if (dist < ia_params->affinity
                              .r0) { // Bond does not exist, we are inside
                                     // of possible bond creation area,
                                     // let's talk about creating a bond
          double Pon = 1.0 - exp(-ia_params->affinity.Kon * time_step);
          // The probability is given by function Pon(x)= 1 - e^(-x) where x is
          // Kon*dt.
          double decide = d_random();
          if (decide <
              Pon) { // the bond will be created only with probability Pon.
            p1->p.bond_site = unfolded_pos - d;
          }
        }
      }
    }
  }
  if (aff_type_extracted == 4) {
    /************************
     *
     * Here I can implement the affinity force.
     * I have the position of the particle - p1, and under p1->p.bond_site I
     * have the coordinate of the bond_site. Also, under d[3] I have the vector
     * towards the constraint meaning that force on p1 should be in the
     * direction of d[3].
     *
     * Algorithm:
     * 1. First check is whether I am in the cut-off radius: ?dist <
     *    affinity.cut?.
     * 2. Then I check whether there exists a bond from the current particle:
     *    ?bond_site != -1?
     * 3. If yes, then I maintain the bond. I put the forces and afterwards I
     *    decide whether the bond will break or not.
     * 4. If no, I maintain the creation of a bond. First I check whether I am
     *    in the area of possible bond creation: ?dist < affinity.r0?
     * 5. If yes, I run the decision algorithm for bond creation and I either
     *    create or does not create the bond.
     * 6. If I am not in the area of possible bond creation I do nothing
     *
     *
     * comments:
     * - strength of the force is proportional to the actual bond length
     * - bond is created with probability 1-exp(-Kon*timestep)
     * - maxBond is not used, we use probability 1-exp(-Koff*timestep) to
     *   break the bond
     * - Koff depends on the bond length via Koff = K0*exp(F/Fd) =
     *   K0*exp(kappa*r/Fd)
     * - here, ia_params->Koff gives us K_0, off rate when bond is relaxed
     * - here, maxBond is used as detachment force F_d
     * - the original check for ensuring, that particle flows out of the
     *   cut-off radius and the bond remains active is replaced with fixed
     *   check, that bond length must not be greater that 0.8 cut_off
     *********************/
    double fac = 0.0;
    if (dist < ia_params->affinity.cut) { // Checking whether I am inside the
                                          // interaction cut-off radius.
      if (dist > 0.0) {
        // printf("bond_site: %f %f
        // %f\n",p1->p.bond_site[0],p1->p.bond_site[1],p1->p.bond_site[2]);
        if ((p1->p.bond_site[0] >= 0) && (p1->p.bond_site[1] >= 0) &&
            (p1->p.bond_site[2] >= 0)) // Checking whether any bond exists
        {                              // Bond exists
          fac = ia_params->affinity.kappa * len;
          // double ftemp = 0;
          force += (fac / len) * vec;
          // Decision whether I should break the bond:
          // First, force exerted on bond is stored in fac
          double tmpF = fac;
          // Then, zero force off rate K_0 is stored at ia_params_Koff
          double tmpK0 = ia_params->affinity.Koff;
          // Then, detachment force is stored in  ia_params->affinity.maxBond
          double tmpFd = ia_params->affinity.maxBond;
          // Then, compute Koff
          double tmpKoff = tmpK0 * exp(tmpF / tmpFd);
          // Finally, compute Poff
          double Poff = 1.0 - exp(-tmpKoff * time_step);
          // printf("%f ", Poff);
          if (len <
              0.8 *
                  ia_params->affinity.cut) { // in other implementation, maxBond
                                             // is used here. However, in this
                                             // implementation, we need maxBond
                                             // for setting detachment force F_d
            double decide = d_random();
            if (decide < Poff) {
              p1->p.bond_site = {-1, -1, -1};
            }

          } else {
            p1->p.bond_site = {-1, -1, -1};
            // printf("breaking: out of cut");
          }
          // Checkpoint output:
          if (period_for_output > 0)
            if ((int)floor(sim_time / time_step) % period_for_output == 0) {
              FILE *fp;
              double tmpPon = 1.0 - exp(-ia_params->affinity.Kon * time_step);
              fp = fopen("affinity_check.dat", "a");
              fprintf(fp, "sim_time %f, period_for_output %d aff type: %d ",
                      sim_time, period_for_output, aff_type_extracted);
              fprintf(fp,
                      "Pon %f, Kon %f, particle %d, Poff = %f, F = %f, Koff = "
                      "%f, K0 = %f, len = %f \n",
                      tmpPon, ia_params->affinity.Kon, p1->p.identity, Poff,
                      tmpF, tmpKoff, tmpK0, len);
              fclose(fp);
            }
        } else if (dist < ia_params->affinity
                              .r0) { // Bond does not exist, we are inside
                                     // of possible bond creation area,
                                     // let's talk about creating a bond
          double Pon = 1.0 - exp(-ia_params->affinity.Kon * time_step);
          // The probability is given by function Pon(x)= 1 - e^(-x) where x is
          // Kon*dt.
          double decide = d_random();
          if (decide <
              Pon) { // the bond will be created only with probability Pon.
            p1->p.bond_site = unfolded_pos - d;
          } else {
            // printf("In range, not creating: Pon = %f, decide = %f", Pon,
            // decide);
          }
        }
      }
    }
  }
  if (aff_type_extracted == 5) { // second implementation of affinity
    /************************
     *
     * Here I can implement the affinity force.
     * I have the position of the particle - p1, and under p1->p.bond_site I
     * have the coordinate of the bond_site. Also, under d[3] I have the vector
     * towards the constraint meaning that force on p1 should be in the
     * direction of d[3].
     *
     * Algorithm:
     * 1. First check is whether I am in the cut-off radius: ?dist <
     *    affinity.cut?.
     * 2. Then I check whether there exists a bond from the current particle:
     *    ?bond_site != -1?
     * 3. If yes, then I maintain the bond. I put the forces and afterwards I
     *    decide whether the bond will break or not.
     * 4. If no, I maintain the creation of a bond. First I check whether I am
     *    in the area of possible bond creation: ?dist < affinity.r0?
     * 5. If yes, I run the decision algorithm for bond creation and I either
     *    create or does not create the bond.
     * 6. If I am not in the area of possible bond creation I do nothing
     *
     *
     * comments:
     * - strength of the force is proportional to the difference of actual
     *   bond length and 75% of the relaxed bond length
     * - bond is created with probability 1-exp(-Kon*timestep)
     * - maxBond is not used, we use probability 1-exp(-Koff*timestep) to
     *   break the bond
     * - Koff depends on the bond length via Koff = K0*exp(F/Fd) =
     *   K0*exp(kappa(r-0.75*r0)/Fd)
     * - here, ia_params->Koff gives us K_0, off rate when bond is relaxed
     * - here, maxBond is used as detachment force F_d
     * - the original check for ensuring, that particle flows out of the
     *   cut-off radius and the bond remains active is replaced with fixed
     *   check, that bond length must not be greater that 0.8 cut_off
     *********************/
    double fac = 0.0;
    if (dist < ia_params->affinity.cut) { // Checking whether I am inside the
                                          // interaction cut-off radius.
      if (dist > 0.0) {
        // printf("bond_site: %f %f
        // %f\n",p1->p.bond_site[0],p1->p.bond_site[1],p1->p.bond_site[2]);
        if ((p1->p.bond_site[0] >= 0) && (p1->p.bond_site[1] >= 0) &&
            (p1->p.bond_site[2] >= 0)) // Checking whether any bond exists
        {                              // Bond exists
          if (len > 0.75 * (ia_params->affinity.r0)) {
            fac = ia_params->affinity.kappa *
                  (len - 0.75 * (ia_params->affinity.r0));
            // printf("len %f r0 %f\n",len, ia_params->affinity.r0);
          } else
            fac = 0.0;
          // double ftemp = 0;
          force += (fac / len) * vec;
          // if (ftemp > 0.000000000000001) printf("%f ",fac);
          // Decision whether I should break the bond:
          // First, force exerted on bond is stored in fac
          double tmpF = fac;
          // Then, zero force off rate K_0 is stored at ia_params_Koff
          double tmpK0 = ia_params->affinity.Koff;
          // Then, detachment force is stored in  ia_params->affinity.maxBond
          double tmpFd = ia_params->affinity.maxBond;
          // Then, compute Koff
          double tmpKoff = tmpK0 * exp(tmpF / tmpFd);
          // Finally, compute Poff
          double Poff = 1.0 - exp(-tmpKoff * time_step);
          // printf("%f ", Poff);
          if (len <
              0.8 *
                  ia_params->affinity.cut) { // in other implementation, maxBond
                                             // is used here. However, in this
                                             // implementation, we need maxBond
                                             // for setting detachment force F_d
            double decide = d_random();
            if (decide < Poff) {
              p1->p.bond_site = {-1, -1, -1};
            }

          } else {
            p1->p.bond_site = {-1, -1, -1};
            // printf("breaking: out of cut");
          }
          // Checkpoint output:
          if (period_for_output > 0)
            if (((int)floor(sim_time / time_step) % period_for_output == 0) &&
                (len > ia_params->affinity.r0)) {
              FILE *fp;
              double tmpPon = 1.0 - exp(-ia_params->affinity.Kon * time_step);
              fp = fopen("affinity_check.dat", "a");
              fprintf(fp, "sim_time %f, period_for_output %d aff type: %d ",
                      sim_time, period_for_output, aff_type_extracted);
              fprintf(fp,
                      "Pon %f, Kon %f, particle %d, Poff = %f, F = %f, Koff = "
                      "%f, K0 = %f, len = %f \n",
                      tmpPon, ia_params->affinity.Kon, p1->p.identity, Poff,
                      tmpF, tmpKoff, tmpK0, len);
              fclose(fp);
            }
        } else if (dist < ia_params->affinity
                              .r0) { // Bond does not exist, we are inside
                                     // of possible bond creation area,
                                     // let's talk about creating a bond
          double Pon = 1.0 - exp(-ia_params->affinity.Kon * time_step);
          // The probability is given by function Pon(x)= 1 - e^(-x) where x is
          // Kon*dt.
          double decide = d_random();
          if (decide <
              Pon) { // the bond will be created only with probability Pon.
            p1->p.bond_site = unfolded_pos - d;
          } else {
            // printf("In range, not creating: Pon = %f, decide = %f", Pon,
            // decide);
          }
        }
      }
    }
  }
  if (aff_type_extracted == 6) { // second implementation of affinity
    /************************
     *
     * Here I can implement the affinity force.
     * I have the position of the particle - p1, and under p1->p.bond_site I
     * have the coordinate of the bond_site. Also, under d[3] I have the vector
     * towards the constraint meaning that force on p1 should be in the
     * direction of d[3].
     *
     * Algorithm:
     * 1. First check is whether I am in the cut-off radius: ?dist <
     *    affinity.cut?.
     * 2. Then I check whether there exists a bond from the current particle:
     *    ?bond_site != -1?
     * 3. If yes, then I maintain the bond. I put the forces and afterwards I
     *    decide whether the bond will break or not.
     * 4. If no, I maintain the creation of a bond. First I check whether I am
     *    in the area of possible bond creation: ?dist < affinity.r0?
     * 5. If yes, I run the decision algorithm for bond creation and I either
     *    create or does not create the bond.
     * 6. If I am not in the area of possible bond creation I do nothing
     *
     *
     * comments:
     * - strength of the force is proportional to the difference of
     *   actual bond length and the relaxed bond length
     * - bond is created with probability 1-exp(-Kon*timestep)
     * - maxBond is not used, we use probability 1-exp(-Koff*timestep) to
     *   break the bond
     * - Koff depends on the bond length via Koff = K0*exp(F/Fd) =
     *   K0*exp(kappa(r-0.75*r0)/Fd)
     * - here, ia_params->Koff gives us K_0, off rate when bond is relaxed
     * - here, maxBond is used as detachment force F_d
     * - the original check for ensuring, that particle flows out of the
     *   cut-off radius and the bond remains active is replaced with fixed
     *   check, that bond length must not be greater that 0.8 cut_off
     *********************/
    double fac = 0.0;
    if (dist < ia_params->affinity.cut) { // Checking whether I am inside the
                                          // interaction cut-off radius.
      if (dist > 0.0) {
        // printf("bond_site: %f %f
        // %f\n",p1->p.bond_site[0],p1->p.bond_site[1],p1->p.bond_site[2]);
        if ((p1->p.bond_site[0] >= 0) && (p1->p.bond_site[1] >= 0) &&
            (p1->p.bond_site[2] >= 0)) // Checking whether any bond exists
        {                              // Bond exists
          if (len > 1.0 * (ia_params->affinity.r0)) {
            fac = ia_params->affinity.kappa *
                  (len - 1.0 * (ia_params->affinity.r0));
            // printf("len %f r0 %f\n",len, ia_params->affinity.r0);
          } else
            fac = 0.0;
          // double ftemp = 0;
          force += (fac / len) * vec;
          // if (ftemp > 0.000000000000001) printf("%f ",fac);
          // Decision whether I should break the bond:
          // First, force exerted on bond is stored in fac
          double tmpF = fac;
          // Then, zero force off rate K_0 is stored at ia_params_Koff
          double tmpK0 = ia_params->affinity.Koff;
          // Then, detachment force is stored in  ia_params->affinity.maxBond
          double tmpFd = ia_params->affinity.maxBond;
          // Then, compute Koff
          double tmpKoff = tmpK0 * exp(tmpF / tmpFd);
          // Finally, compute Poff
          double Poff = 1.0 - exp(-tmpKoff * time_step);
          // printf("%f ", Poff);
          if (len <
              0.8 *
                  ia_params->affinity.cut) { // in other implementation, maxBond
                                             // is used here. However, in this
                                             // implementation, we need maxBond
                                             // for setting detachment force F_d
            double decide = d_random();
            if (decide < Poff) {
              p1->p.bond_site = {-1, -1, -1};
            }

          } else {
            p1->p.bond_site = {-1, -1, -1};
            // printf("breaking: out of cut");
          }
          // Checkpoint output:
          if (period_for_output > 0)
            if (((int)floor(sim_time / time_step) % period_for_output == 0) &&
                (len > ia_params->affinity.r0)) {
              FILE *fp;
              double tmpPon = 1.0 - exp(-ia_params->affinity.Kon * time_step);
              fp = fopen("affinity_check.dat", "a");
              fprintf(fp, "sim_time %f, period_for_output %d aff type: %d ",
                      sim_time, period_for_output, aff_type_extracted);
              fprintf(fp,
                      "Pon %f, Kon %f, particle %d, Poff = %f, F = %f, Koff = "
                      "%f, K0 = %f, len = %f \n",
                      tmpPon, ia_params->affinity.Kon, p1->p.identity, Poff,
                      tmpF, tmpKoff, tmpK0, len);
              fclose(fp);
            }
        } else if (dist < ia_params->affinity
                              .r0) { // Bond does not exist, we are inside
                                     // of possible bond creation area,
                                     // let's talk about creating a bond
          double Pon = 1.0 - exp(-ia_params->affinity.Kon * time_step);
          // The probability is given by function Pon(x)= 1 - e^(-x) where x is
          // Kon*dt.
          double decide = d_random();
          if (decide <
              Pon) { // the bond will be created only with probability Pon.
            p1->p.bond_site = unfolded_pos - d;
          } else {
            // printf("In range, not creating: Pon = %f, decide = %f", Pon,
            // decide);
          }
        }
      }
    }
  }
}

#endif /* ifdef AFFINITY */
#endif
