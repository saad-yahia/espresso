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
/** \file
 *  P3M algorithm for long range magnetic dipole-dipole interaction.
 *
 *  @note
 *      In general the magnetic dipole-dipole functions bear the same name than
 *      the charge-charge, but adding a "D" in front of the name and replacing
 *      "charge" by "dipole". In this way one can recognize the similarity of
 *      the functions while avoiding nasty confusions in their use.
 *
 *       By default the magnetic epsilon is metallic = 0.
 *
 *  The corresponding header file is p3m-dipolar.hpp.
 */

#include "electrostatics_magnetostatics/p3m-dipolar.hpp"

#ifdef DP3M

#include "cells.hpp"
#include "communication.hpp"
#include "debug.hpp"
#include "domain_decomposition.hpp"
#include "errorhandling.hpp"
#include "global.hpp"
#include "grid.hpp"
#include "integrate.hpp"
#include "particle_data.hpp"
#include "tuning.hpp"

#include <utils/strcat_alloc.hpp>
using Utils::strcat_alloc;
#include <utils/math/sinc.hpp>
using Utils::sinc;
#include <utils/constants.hpp>
#include <utils/math/sqr.hpp>

#include <boost/range/algorithm/min_element.hpp>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mpi.h>

/************************************************
 * DEFINES
 ************************************************/

/* MPI tags for the dipole-dipole p3m communications: */
/** Tag for communication in dp3m_init() -> dp3m_calc_send_mesh(). */
#define REQ_P3M_INIT_D 2001
/** Tag for communication in dp3m_gather_fft_grid(). */
#define REQ_P3M_GATHER_D 2011
/** Tag for communication in dp3m_spread_force_grid(). */
#define REQ_P3M_SPREAD_D 2021

/************************************************
 * variables
 ************************************************/

dp3m_data_struct dp3m;

/** \name Private Functions */
/************************************************************/
/*@{*/

/** Calculate for magnetic dipoles the properties of the send/recv sub-meshes
 *  of the local FFT mesh.
 *  In order to calculate the recv sub-meshes there is a communication of
 *  the margins between neighbouring nodes.
 */
static void dp3m_calc_send_mesh();

/** Initialize for magnetic dipoles the (inverse) mesh constant @ref
 *  P3MParameters::a "a" (@ref P3MParameters::ai "ai") and the
 *  cutoff for charge assignment @ref P3MParameters::cao_cut "cao_cut".
 *
 *  Function called by @ref dp3m_init() once and by @ref
 *  dp3m_scaleby_box_l() whenever the box_length changes.
 */
static void dp3m_init_a_ai_cao_cut();

/** Calculate for magnetic dipoles the spatial position of the left down mesh
 *  point of the local mesh, to be stored in
 *  @ref p3m_local_mesh::ld_pos "ld_pos".
 *
 *  Function called by @ref dp3m_calc_local_ca_mesh() once and by
 *  @ref dp3m_scaleby_box_l() whenever the box size changes.
 */
static void dp3m_calc_lm_ld_pos();

/** Gather FFT grid.
 *  After the charge assignment Each node needs to gather the
 *  information for the FFT grid in his spatial domain.
 */
static void dp3m_gather_fft_grid(double *mesh);

/** Spread force grid.
 *  After the k-space calculations each node needs to get all force
 *  information to reassign the forces from the grid to the particles.
 */
static void dp3m_spread_force_grid(double *mesh);

/** realloc charge assignment fields. */
static void dp3m_realloc_ca_fields(int newsize);

/** Checks for correctness for magnetic dipoles in P3M of the cao_cut,
 *  necessary when the box length changes
 */
static bool dp3m_sanity_checks_boxl();

/** Calculate properties of the local FFT mesh for the
 *   charge assignment process.
 */
static void dp3m_calc_local_ca_mesh();

/** Interpolate the P-th order charge assignment function from
 *  Hockney/Eastwood 5-189 (or 8-61). The following charge fractions
 *  are also tabulated in Deserno/Holm.
 */
static void dp3m_interpolate_dipole_assignment_function();

/** Shift the mesh points by mesh/2 */
static void dp3m_calc_meshift();

/** Calculate the Fourier transformed differential operator.
 *  Remark: This is done on the level of n-vectors and not k-vectors,
 *          i.e. the prefactor i*2*PI/L is missing!
 */
static void dp3m_calc_differential_operator();

/** Calculate the influence function optimized for the dipolar forces. */
static void dp3m_calc_influence_function_force();

/** Calculate the influence function optimized for the dipolar energy and
 *  torques.
 */
static void dp3m_calc_influence_function_energy();

/** Calculate the constants necessary to correct the dipolar energy to minimize
 *  the error.
 */
static void dp3m_compute_constants_energy_dipolar();

/** Calculate the aliasing sums for the optimal influence function.
 *
 *  Calculates the aliasing sums in the nominator and denominator of
 *  the expression for the optimal influence function (see
 *  Hockney/Eastwood: 8-22, p. 275).
 *
 *  \param  n           n-vector for which the aliasing sum is to be performed
 *  \param  nominator   aliasing sums in the nominator
 *  \retval denominator aliasing sum in the denominator
 */
static double dp3m_perform_aliasing_sums_force(const int n[3],
                                               double nominator[1]);
static double dp3m_perform_aliasing_sums_energy(const int n[3],
                                                double nominator[1]);

static double dp3m_k_space_error(double box_size, double prefac, int mesh,
                                 int cao, int n_c_part, double sum_q2,
                                 double alpha_L);
/*@}*/

/** Compute the dipolar surface terms */
static double calc_surface_term(int force_flag, int energy_flag,
                                const ParticleRange &particles);

/** \name P3M Tuning Functions */
/************************************************************/
/*@{*/

// These 3 functions are to tune the P3M code in the case of dipolar
// interactions

double P3M_DIPOLAR_real_space_error(double box_size, double prefac,
                                    double r_cut_iL, int n_c_part,
                                    double sum_q2, double alpha_L);
static void dp3m_tune_aliasing_sums(int nx, int ny, int nz, int mesh,
                                    double mesh_i, int cao, double alpha_L_i,
                                    double *alias1, double *alias2);

// To compute the value of alpha through a bibisection method from the formula
// 33 of JCP115,6351,(2001).
double dp3m_rtbisection(double box_size, double prefac, double r_cut_iL,
                        int n_c_part, double sum_q2, double x1, double x2,
                        double xacc, double tuned_accuracy);

/*@}*/

/************************************************************/
/* functions related to the correction of the dipolar p3m-energy */

static double dp3m_average_dipolar_self_energy(double box_l, int mesh);
static double dp3m_perform_aliasing_sums_dipolar_self_energy(const int n[3]);

/************************************************************/
/* functions related to the correction of the dipolar p3m-energy */

/*

// Do the sum over k<>0 where k={kx,ky,kz} with kx,ky,kz INTEGERS, of
// exp(-Utils::pi()**2*k**2/alpha**2/L**2)
static double dp3m_sumi1(double alpha_L){
       int k2,kx,ky,kz,kx2,ky2,limit=60;
       double suma,alpha_L2;

       alpha_L2= alpha_L* alpha_L;

       //fprintf(stderr,"alpha_L=%le\n",alpha_L);
       //fprintf(stderr,"Utils::pi()=%le\n",Utils::pi());


       suma=0.0;
       for(kx=-limit;kx<=limit;kx++){
         kx2=kx*kx;
       for(ky=-limit;ky<=limit;ky++){
         ky2=ky*ky;
       for(kz=-limit;kz<=limit;kz++){
           k2=kx2+ky2+kz*kz;
           suma+=exp(-Utils::pi()*Utils::pi()*k2/(alpha_L*alpha_L));
       }}}
       suma-=1; //It's easier to subtract the term k=0 later than put an if
inside the loops


         //fprintf(stderr,"suma=%le\n",suma);


   return suma;
}

*/

/************************************************************/

/*
// Do the sum over n<>0 where n={nx*L,ny*L,nz*L} with nx,ny,nz INTEGERS, of
// exp(-alpha_iL**2*n**2)
static double dp3m_sumi2(double alpha_L){
       int n2,nx,ny,nz,nx2,ny2,limit=60;
       double suma;



       suma=0.0;
       for(nx=-limit;nx<=limit;nx++){
         nx2=nx*nx;
       for(ny=-limit;ny<=limit;ny++){
         ny2=ny*ny;
       for(nz=-limit;nz<=limit;nz++){
           n2=nx2+ny2+nz*nz;
           suma+=exp(-alpha_L*alpha_L*n2);
       }}}
       suma-=1; //It's easier to subtract the term n=0 later than put an if
inside the loops



   return suma;
}

*/

dp3m_data_struct::dp3m_data_struct() {
  params.epsilon = P3M_EPSILON_MAGNETIC;

  /* local_mesh is uninitialized */
  /* sm is uninitialized */
  rs_mesh = nullptr;
  rs_mesh_dip[0] = nullptr;
  rs_mesh_dip[1] = nullptr;
  rs_mesh_dip[2] = nullptr;
  ks_mesh = nullptr;

  sum_dip_part = 0;
  sum_mu2 = 0.0;

  for (auto &i : int_caf)
    i = nullptr;
  pos_shift = 0.0;
  meshift = nullptr;

  d_op = nullptr;
  g_force = nullptr;
  g_energy = nullptr;

  ca_num = 0;
  ca_frac = nullptr;
  ca_fmp = nullptr;
  ks_pnum = 0;

  send_grid = nullptr;
  recv_grid = nullptr;

  energy_correction = 0.0;
}

void dp3m_deactivate() {
  dp3m.params.alpha = 0.0;
  dp3m.params.alpha_L = 0.0;
  dp3m.params.r_cut = 0.0;
  dp3m.params.r_cut_iL = 0.0;
  dp3m.params.mesh[0] = 0;
  dp3m.params.mesh[1] = 0;
  dp3m.params.mesh[2] = 0;
  dp3m.params.cao = 0;
}

void dp3m_init() {
  int n;

  if (dipole.prefactor <= 0.0) {
    dp3m.params.r_cut = 0.0;
    dp3m.params.r_cut_iL = 0.0;
    if (this_node == 0) {
      P3M_TRACE(
          fprintf(stderr, "0: dp3m_init: dipolar prefactor is zero.\n");
          fprintf(stderr, "   Magnetostatics of dipoles switched off!\n"));
    }
  } else {
    P3M_TRACE(fprintf(stderr, "%d: dp3m_init:\n", this_node));

    if (dp3m_sanity_checks(node_grid))
      return;

    P3M_TRACE(fprintf(stderr, "%d: dp3m_init: starting\n", this_node));

    P3M_TRACE(fprintf(stderr, "%d: mesh=%d, cao=%d, mesh_off=(%f,%f,%f)\n",
                      this_node, dp3m.params.mesh[0], dp3m.params.cao,
                      dp3m.params.mesh_off[0], dp3m.params.mesh_off[1],
                      dp3m.params.mesh_off[2]));
    dp3m.params.cao3 = dp3m.params.cao * dp3m.params.cao * dp3m.params.cao;

    /* initializes the (inverse) mesh constant dp3m.params.a (dp3m.params.ai)
     * and the cutoff for charge assignment dp3m.params.cao_cut */
    dp3m_init_a_ai_cao_cut();

    /* initialize ca fields to size CA_INCREMENT: dp3m.ca_frac and dp3m.ca_fmp
     */
    dp3m.ca_num = 0;
    if (dp3m.ca_num < CA_INCREMENT) {
      dp3m.ca_num = 0;
      dp3m_realloc_ca_fields(CA_INCREMENT);
    }

    dp3m_calc_local_ca_mesh();

    dp3m_calc_send_mesh();
    P3M_TRACE(p3m_p3m_print_local_mesh(dp3m.local_mesh));

    /* DEBUG */
    for (n = 0; n < n_nodes; n++) {
      /* MPI_Barrier(comm_cart); */
      if (n == this_node) {
        P3M_TRACE(p3m_p3m_print_send_mesh(dp3m.sm));
      }
    }

    dp3m.send_grid =
        Utils::realloc(dp3m.send_grid, sizeof(double) * dp3m.sm.max);
    dp3m.recv_grid =
        Utils::realloc(dp3m.recv_grid, sizeof(double) * dp3m.sm.max);

    /* fix box length dependent constants */
    dp3m_scaleby_box_l();

    if (dp3m.params.inter > 0)
      dp3m_interpolate_dipole_assignment_function();

    dp3m.pos_shift =
        std::floor((dp3m.params.cao - 1) / 2.0) - (dp3m.params.cao % 2) / 2.0;
    P3M_TRACE(fprintf(stderr, "%d: dipolar pos_shift = %f\n", this_node,
                      dp3m.pos_shift));

    /* FFT */
    P3M_TRACE(fprintf(stderr, "%d: dp3m.rs_mesh ADR=%p\n", this_node,
                      (void *)dp3m.rs_mesh));

    int ca_mesh_size =
        fft_init(&dp3m.rs_mesh, dp3m.local_mesh.dim, dp3m.local_mesh.margin,
                 dp3m.params.mesh, dp3m.params.mesh_off, &dp3m.ks_pnum,
                 dp3m.fft, node_grid, comm_cart);
    dp3m.ks_mesh = Utils::realloc(dp3m.ks_mesh, ca_mesh_size * sizeof(double));

    for (n = 0; n < 3; n++)
      dp3m.rs_mesh_dip[n] =
          Utils::realloc(dp3m.rs_mesh_dip[n], ca_mesh_size * sizeof(double));

    P3M_TRACE(fprintf(stderr, "%d: dp3m.rs_mesh_dip[0] ADR=%p\n", this_node,
                      (void *)dp3m.rs_mesh_dip[0]));
    P3M_TRACE(fprintf(stderr, "%d: dp3m.rs_mesh_dip[1] ADR=%p\n", this_node,
                      (void *)dp3m.rs_mesh_dip[1]));
    P3M_TRACE(fprintf(stderr, "%d: dp3m.rs_mesh_dip[2] ADR=%p\n", this_node,
                      (void *)dp3m.rs_mesh_dip[2]));

    /* k-space part: */

    dp3m_calc_differential_operator();

    dp3m_calc_influence_function_force();
    dp3m_calc_influence_function_energy();

    dp3m_count_magnetic_particles();

    P3M_TRACE(fprintf(stderr, "%d: p3m initialized\n", this_node));
  }
}

void dp3m_free_dipoles() {
  for (auto &i : dp3m.rs_mesh_dip)
    free(i);
  free(dp3m.ca_frac);
  free(dp3m.ca_fmp);
  free(dp3m.send_grid);
  free(dp3m.recv_grid);
  free(dp3m.rs_mesh);
  free(dp3m.ks_mesh);
}

double dp3m_average_dipolar_self_energy(double box_l, int mesh) {
  int i, ind, n[3];
  double node_phi = 0.0, phi = 0.0;
  double U2;

  int end[3];
  int size = 1;

  for (i = 0; i < 3; i++) {
    size *= dp3m.fft.plan[3].new_mesh[i];
    end[i] = dp3m.fft.plan[3].start[i] + dp3m.fft.plan[3].new_mesh[i];
  }

  for (n[0] = dp3m.fft.plan[3].start[0]; n[0] < end[0]; n[0]++) {
    for (n[1] = dp3m.fft.plan[3].start[1]; n[1] < end[1]; n[1]++) {
      for (n[2] = dp3m.fft.plan[3].start[2]; n[2] < end[2]; n[2]++) {
        ind = (n[2] - dp3m.fft.plan[3].start[2]) +
              dp3m.fft.plan[3].new_mesh[2] *
                  ((n[1] - dp3m.fft.plan[3].start[1]) +
                   (dp3m.fft.plan[3].new_mesh[1] *
                    (n[0] - dp3m.fft.plan[3].start[0])));

        if ((n[0] == 0) && (n[1] == 0) && (n[2] == 0))
          node_phi += 0.0;
        else if ((n[0] % (dp3m.params.mesh[0] / 2) == 0) &&
                 (n[1] % (dp3m.params.mesh[0] / 2) == 0) &&
                 (n[2] % (dp3m.params.mesh[0] / 2) == 0))
          node_phi += 0.0;
        else {
          U2 = dp3m_perform_aliasing_sums_dipolar_self_energy(n);
          node_phi +=
              dp3m.g_energy[ind] * U2 *
              (Utils::sqr(dp3m.d_op[n[0]]) + Utils::sqr(dp3m.d_op[n[1]]) +
               Utils::sqr(dp3m.d_op[n[2]]));
        }
      }
    }
  }

  MPI_Reduce(&node_phi, &phi, 1, MPI_DOUBLE, MPI_SUM, 0, comm_cart);

  phi *= Utils::pi() / 3. / box_l / pow(mesh, 3.0);

  return phi;
}

double dp3m_perform_aliasing_sums_dipolar_self_energy(const int n[3]) {
  double u_sum = 0.0;
  /* lots of temporary variables... */
  double f1, sx, sy, sz, mx, my, mz, nmx, nmy, nmz;
  int limit = P3M_BRILLOUIN + 5;

  f1 = 1.0 / (double)dp3m.params.mesh[0];

  for (mx = -limit; mx <= limit; mx++) {
    nmx = dp3m.meshift[n[0]] + dp3m.params.mesh[0] * mx;
    sx = pow(sinc(f1 * nmx), 2.0 * dp3m.params.cao);
    for (my = -limit; my <= limit; my++) {
      nmy = dp3m.meshift[n[1]] + dp3m.params.mesh[0] * my;
      sy = sx * pow(sinc(f1 * nmy), 2.0 * dp3m.params.cao);
      for (mz = -limit; mz <= limit; mz++) {
        nmz = dp3m.meshift[n[2]] + dp3m.params.mesh[0] * mz;
        sz = sy * pow(sinc(f1 * nmz), 2.0 * dp3m.params.cao);
        u_sum += sz;
      }
    }
  }
  return u_sum;
}

/******************
 * functions related to the parsing&tuning of the dipolar parameters
 ******************/

void dp3m_set_tune_params(double r_cut, int mesh, int cao, double alpha,
                          double accuracy, int n_interpol) {
  if (r_cut >= 0) {
    dp3m.params.r_cut = r_cut;
    dp3m.params.r_cut_iL = r_cut * (1. / box_geo.length()[0]);
  }

  if (mesh >= 0)
    dp3m.params.mesh[2] = dp3m.params.mesh[1] = dp3m.params.mesh[0] = mesh;

  if (cao >= 0)
    dp3m.params.cao = cao;

  if (alpha >= 0) {
    dp3m.params.alpha = alpha;
    dp3m.params.alpha_L = alpha * box_geo.length()[0];
  }

  if (accuracy >= 0)
    dp3m.params.accuracy = accuracy;

  if (n_interpol != -1)
    dp3m.params.inter = n_interpol;
}

/*****************************************************************************/

int dp3m_set_params(double r_cut, int mesh, int cao, double alpha,
                    double accuracy) {
  if (dipole.method != DIPOLAR_P3M && dipole.method != DIPOLAR_MDLC_P3M)
    Dipole::set_method_local(DIPOLAR_P3M);

  if (r_cut < 0)
    return -1;

  if (mesh < 0)
    return -2;

  if (cao < 1 || cao > 7 || cao > mesh)
    return -3;

  dp3m.params.r_cut = r_cut;
  dp3m.params.r_cut_iL = r_cut * (1. / box_geo.length()[0]);
  dp3m.params.mesh[2] = dp3m.params.mesh[1] = dp3m.params.mesh[0] = mesh;
  dp3m.params.cao = cao;

  if (alpha > 0) {
    dp3m.params.alpha = alpha;
    dp3m.params.alpha_L = alpha * box_geo.length()[0];
  } else if (alpha != -1.0)
    return -4;

  if (accuracy >= 0)
    dp3m.params.accuracy = accuracy;
  else if (accuracy != -1.0)
    return -5;

  mpi_bcast_coulomb_params();

  return 0;
}

int dp3m_set_mesh_offset(double x, double y, double z) {
  if (x < 0.0 || x > 1.0 || y < 0.0 || y > 1.0 || z < 0.0 || z > 1.0)
    return ES_ERROR;

  dp3m.params.mesh_off[0] = x;
  dp3m.params.mesh_off[1] = y;
  dp3m.params.mesh_off[2] = z;

  mpi_bcast_coulomb_params();

  return ES_OK;
}

/* We left the handling of the epsilon, due to portability reasons in
the future for the electrical dipoles, or if people wants to do
electrical dipoles alone using the magnetic code .. */

int dp3m_set_eps(double eps) {
  dp3m.params.epsilon = eps;

  mpi_bcast_coulomb_params();

  return ES_OK;
}

int dp3m_set_ninterpol(int n) {
  if (n < 0)
    return ES_ERROR;

  dp3m.params.inter = n;

  mpi_bcast_coulomb_params();

  return ES_OK;
}

/*****************************************************************************/

void dp3m_interpolate_dipole_assignment_function() {
  double dInterpol = 0.5 / (double)dp3m.params.inter;
  int i;
  long j;

  if (dp3m.params.inter == 0)
    return;

  P3M_TRACE(fprintf(stderr,
                    "dipolar %d - interpolating (%d) the order-%d "
                    "charge assignment function\n",
                    this_node, dp3m.params.inter, dp3m.params.cao));

  dp3m.params.inter2 = 2 * dp3m.params.inter + 1;

  for (i = 0; i < dp3m.params.cao; i++) {
    /* allocate memory for interpolation array */
    dp3m.int_caf[i] = Utils::realloc(
        dp3m.int_caf[i], sizeof(double) * (2 * dp3m.params.inter + 1));

    /* loop over all interpolation points */
    for (j = -dp3m.params.inter; j <= dp3m.params.inter; j++)
      dp3m.int_caf[i][j + dp3m.params.inter] =
          p3m_caf(i, j * dInterpol, dp3m.params.cao);
  }
}

void dp3m_dipole_assign(const ParticleRange &particles) {
  /* magnetic particle counter, dipole fraction counter */
  int cp_cnt = 0;

  /* prepare local FFT mesh */
  for (auto &i : dp3m.rs_mesh_dip)
    for (int j = 0; j < dp3m.local_mesh.size; j++)
      i[j] = 0.0;

  for (auto const &p : particles) {
    if (p.p.dipm != 0.0) {
      dp3m_assign_dipole(p.r.p.data(), p.p.dipm, p.calc_dip().data(), cp_cnt);
      cp_cnt++;
    }
  }

  dp3m_shrink_wrap_dipole_grid(cp_cnt);
}

void dp3m_assign_dipole(double const real_pos[3], double mu,
                        double const dip[3], int cp_cnt) {
  /* we do not really want to export these, but this function should be inlined
   */
  double p3m_caf(int i, double x, int cao_value);
  void dp3m_realloc_ca_fields(int size);

  int d, i0, i1, i2;
  double tmp0, tmp1;
  /* position of a particle in local mesh units */
  double pos;
  /* 1d-index of nearest mesh point */
  int nmp;
  /* distance to nearest mesh point */
  double dist[3];
  /* index for caf interpolation grid */
  int arg[3];
  /* index, index jumps for dp3m.rs_mesh array */
  int q_ind = 0;
  double cur_ca_frac_val, *cur_ca_frac;

  // make sure we have enough space
  if (cp_cnt >= dp3m.ca_num)
    dp3m_realloc_ca_fields(cp_cnt + 1);
  // do it here, since p3m_realloc_ca_fields may change the address of
  // dp3m.ca_frac
  cur_ca_frac = dp3m.ca_frac + dp3m.params.cao3 * cp_cnt;

  if (dp3m.params.inter == 0) {
    for (d = 0; d < 3; d++) {
      /* particle position in mesh coordinates */
      pos = ((real_pos[d] - dp3m.local_mesh.ld_pos[d]) * dp3m.params.ai[d]) -
            dp3m.pos_shift;
      /* nearest mesh point */
      nmp = (int)pos;
      /* distance to nearest mesh point */
      dist[d] = (pos - nmp) - 0.5;
      /* 3d-array index of nearest mesh point */
      q_ind = (d == 0) ? nmp : nmp + dp3m.local_mesh.dim[d] * q_ind;
    }

    if (cp_cnt >= 0)
      dp3m.ca_fmp[cp_cnt] = q_ind;

    for (i0 = 0; i0 < dp3m.params.cao; i0++) {
      tmp0 = p3m_caf(i0, dist[0], dp3m.params.cao);
      for (i1 = 0; i1 < dp3m.params.cao; i1++) {
        tmp1 = tmp0 * p3m_caf(i1, dist[1], dp3m.params.cao);
        for (i2 = 0; i2 < dp3m.params.cao; i2++) {
          cur_ca_frac_val = tmp1 * p3m_caf(i2, dist[2], dp3m.params.cao);
          if (cp_cnt >= 0)
            *(cur_ca_frac++) = cur_ca_frac_val;
          if (mu != 0.0) {
            dp3m.rs_mesh_dip[0][q_ind] += dip[0] * cur_ca_frac_val;
            dp3m.rs_mesh_dip[1][q_ind] += dip[1] * cur_ca_frac_val;
            dp3m.rs_mesh_dip[2][q_ind] += dip[2] * cur_ca_frac_val;
          }
          q_ind++;
        }
        q_ind += dp3m.local_mesh.q_2_off;
      }
      q_ind += dp3m.local_mesh.q_21_off;
    }
  } else {
    /* particle position in mesh coordinates */
    for (d = 0; d < 3; d++) {
      pos = ((real_pos[d] - dp3m.local_mesh.ld_pos[d]) * dp3m.params.ai[d]) -
            dp3m.pos_shift;
      nmp = (int)pos;
      arg[d] = (int)((pos - nmp) * dp3m.params.inter2);
      /* for the first dimension, q_ind is always zero, so this shifts correctly
       */
      q_ind = nmp + dp3m.local_mesh.dim[d] * q_ind;
    }
    if (cp_cnt >= 0)
      dp3m.ca_fmp[cp_cnt] = q_ind;

    for (i0 = 0; i0 < dp3m.params.cao; i0++) {
      tmp0 = dp3m.int_caf[i0][arg[0]];
      for (i1 = 0; i1 < dp3m.params.cao; i1++) {
        tmp1 = tmp0 * dp3m.int_caf[i1][arg[1]];
        for (i2 = 0; i2 < dp3m.params.cao; i2++) {
          cur_ca_frac_val = tmp1 * dp3m.int_caf[i2][arg[2]];
          if (cp_cnt >= 0)
            *(cur_ca_frac++) = cur_ca_frac_val;
          if (mu != 0.0) {
            dp3m.rs_mesh_dip[0][q_ind] += dip[0] * cur_ca_frac_val;
            dp3m.rs_mesh_dip[1][q_ind] += dip[1] * cur_ca_frac_val;
            dp3m.rs_mesh_dip[2][q_ind] += dip[2] * cur_ca_frac_val;
          }
          q_ind++;
        }
        q_ind += dp3m.local_mesh.q_2_off;
      }
      q_ind += dp3m.local_mesh.q_21_off;
    }
  }
}

void dp3m_shrink_wrap_dipole_grid(int n_dipoles) {
  if (n_dipoles < dp3m.ca_num)
    dp3m_realloc_ca_fields(n_dipoles);
}

#ifdef ROTATION
/* Assign the torques obtained from k-space */
static void P3M_assign_torques(double prefac, int d_rs,
                               const ParticleRange &particles) {
  /* particle counter, charge fraction counter */
  int cp_cnt = 0, cf_cnt = 0;
  /* index, index jumps for dp3m.rs_mesh array */
  int q_ind;
  int q_m_off = (dp3m.local_mesh.dim[2] - dp3m.params.cao);
  int q_s_off =
      dp3m.local_mesh.dim[2] * (dp3m.local_mesh.dim[1] - dp3m.params.cao);

  cp_cnt = 0;
  cf_cnt = 0;

  for (auto &p : particles) {
    if ((p.p.dipm) != 0.0) {
      const Utils::Vector3d dip = p.calc_dip();
      q_ind = dp3m.ca_fmp[cp_cnt];
      for (int i0 = 0; i0 < dp3m.params.cao; i0++) {
        for (int i1 = 0; i1 < dp3m.params.cao; i1++) {
          for (int i2 = 0; i2 < dp3m.params.cao; i2++) {
            /*
            The following line would fill the torque with the k-space electric
            field (without the self-field term) [notice the minus sign!]:
                                p.f.torque[d_rs] -=
                prefac*dp3m.ca_frac[cf_cnt]*dp3m.rs_mesh[q_ind];
            Since the torque is the dipole moment cross-product with E, we
            have:
            */
            switch (d_rs) {
            case 0: // E_x
              p.f.torque[1] -=
                  dip[2] * prefac * dp3m.ca_frac[cf_cnt] * dp3m.rs_mesh[q_ind];
              p.f.torque[2] +=
                  dip[1] * prefac * dp3m.ca_frac[cf_cnt] * dp3m.rs_mesh[q_ind];
              break;
            case 1: // E_y
              p.f.torque[0] +=
                  dip[2] * prefac * dp3m.ca_frac[cf_cnt] * dp3m.rs_mesh[q_ind];
              p.f.torque[2] -=
                  dip[0] * prefac * dp3m.ca_frac[cf_cnt] * dp3m.rs_mesh[q_ind];
              break;
            case 2: // E_z
              p.f.torque[0] -=
                  dip[1] * prefac * dp3m.ca_frac[cf_cnt] * dp3m.rs_mesh[q_ind];
              p.f.torque[1] +=
                  dip[0] * prefac * dp3m.ca_frac[cf_cnt] * dp3m.rs_mesh[q_ind];
            }
            q_ind++;
            cf_cnt++;
          }
          q_ind += q_m_off;
        }
        q_ind += q_s_off;
      }
      cp_cnt++;

      ONEPART_TRACE(if (p.p.identity == check_id) fprintf(
          stderr, "%d: OPT: P3M  f = (%.3e,%.3e,%.3e) in dir %d\n", this_node,
          p.f.f[0], p.f.f[1], p.f.f[2], d_rs));
    }
  }
}
#endif

/* Assign the dipolar forces obtained from k-space */
static void dp3m_assign_forces_dip(double prefac, int d_rs,
                                   const ParticleRange &particles) {
  /* particle counter, charge fraction counter */
  int cp_cnt = 0, cf_cnt = 0;
  /* index, index jumps for dp3m.rs_mesh array */
  int q_ind;
  int q_m_off = (dp3m.local_mesh.dim[2] - dp3m.params.cao);
  int q_s_off =
      dp3m.local_mesh.dim[2] * (dp3m.local_mesh.dim[1] - dp3m.params.cao);

  cp_cnt = 0;
  cf_cnt = 0;

  for (auto &p : particles) {
    if ((p.p.dipm) != 0.0) {
      const Utils::Vector3d dip = p.calc_dip();
      q_ind = dp3m.ca_fmp[cp_cnt];
      for (int i0 = 0; i0 < dp3m.params.cao; i0++) {
        for (int i1 = 0; i1 < dp3m.params.cao; i1++) {
          for (int i2 = 0; i2 < dp3m.params.cao; i2++) {
            p.f.f[d_rs] += prefac * dp3m.ca_frac[cf_cnt] *
                           (dp3m.rs_mesh_dip[0][q_ind] * dip[0] +
                            dp3m.rs_mesh_dip[1][q_ind] * dip[1] +
                            dp3m.rs_mesh_dip[2][q_ind] * dip[2]);
            q_ind++;
            cf_cnt++;
          }
          q_ind += q_m_off;
        }
        q_ind += q_s_off;
      }
      cp_cnt++;

      ONEPART_TRACE(if (p.p.identity == check_id) fprintf(
          stderr, "%d: OPT: P3M  f = (%.3e,%.3e,%.3e) in dir %d\n", this_node,
          p.f.f[0], p.f.f[1], p.f.f[2], d_rs));
    }
  }
}

/*****************************************************************************/

double dp3m_calc_kspace_forces(int force_flag, int energy_flag,
                               const ParticleRange &particles) {
  int i, d, d_rs, ind, j[3];
  /**************************************************************/
  /* k-space energy */
  double dipole_prefac;
  double surface_term = 0.0;
  double k_space_energy_dip = 0.0, node_k_space_energy_dip = 0.0;
  double tmp0, tmp1;

  P3M_TRACE(fprintf(stderr, "%d: dipolar p3m_perform(%d,%d):\n", this_node,
                    force_flag, energy_flag));

  dipole_prefac =
      dipole.prefactor /
      (double)(dp3m.params.mesh[0] * dp3m.params.mesh[1] * dp3m.params.mesh[2]);

  if (dp3m.sum_mu2 > 0) {
    /* Gather information for FFT grid inside the nodes domain (inner local
     * mesh) and Perform forward 3D FFT (Charge Assignment Mesh). */
    dp3m_gather_fft_grid(dp3m.rs_mesh_dip[0]);
    dp3m_gather_fft_grid(dp3m.rs_mesh_dip[1]);
    dp3m_gather_fft_grid(dp3m.rs_mesh_dip[2]);
    fft_perform_forw(dp3m.rs_mesh_dip[0], dp3m.fft, comm_cart);
    fft_perform_forw(dp3m.rs_mesh_dip[1], dp3m.fft, comm_cart);
    fft_perform_forw(dp3m.rs_mesh_dip[2], dp3m.fft, comm_cart);
    // Note: after these calls, the grids are in the order yzx and not xyz
    // anymore!!!
  }

  /* === k-space calculations === */
  P3M_TRACE(fprintf(stderr, "%d: dipolar p3m_perform: k-space\n", this_node));

  /* === k-space energy calculation  === */
  if (energy_flag) {
    /*********************
       Dipolar energy
    **********************/
    if (dp3m.sum_mu2 > 0) {
      P3M_TRACE(fprintf(stderr,
                        "%d: dipolar p3m start energy calculation: k-space\n",
                        this_node));

      /* i*k differentiation for dipolar gradients:
       * |(\Fourier{\vect{mu}}(k)\cdot \vect{k})|^2 */
      ind = 0;
      i = 0;
      for (j[0] = 0; j[0] < dp3m.fft.plan[3].new_mesh[0]; j[0]++) {
        for (j[1] = 0; j[1] < dp3m.fft.plan[3].new_mesh[1]; j[1]++) {
          for (j[2] = 0; j[2] < dp3m.fft.plan[3].new_mesh[2]; j[2]++) {
            node_k_space_energy_dip +=
                dp3m.g_energy[i] *
                (Utils::sqr(dp3m.rs_mesh_dip[0][ind] *
                                dp3m.d_op[j[2] + dp3m.fft.plan[3].start[2]] +
                            dp3m.rs_mesh_dip[1][ind] *
                                dp3m.d_op[j[0] + dp3m.fft.plan[3].start[0]] +
                            dp3m.rs_mesh_dip[2][ind] *
                                dp3m.d_op[j[1] + dp3m.fft.plan[3].start[1]]) +
                 Utils::sqr(dp3m.rs_mesh_dip[0][ind + 1] *
                                dp3m.d_op[j[2] + dp3m.fft.plan[3].start[2]] +
                            dp3m.rs_mesh_dip[1][ind + 1] *
                                dp3m.d_op[j[0] + dp3m.fft.plan[3].start[0]] +
                            dp3m.rs_mesh_dip[2][ind + 1] *
                                dp3m.d_op[j[1] + dp3m.fft.plan[3].start[1]]));
            ind += 2;
            i++;
          }
        }
      }
      node_k_space_energy_dip *=
          dipole_prefac * Utils::pi() / box_geo.length()[0];
      MPI_Reduce(&node_k_space_energy_dip, &k_space_energy_dip, 1, MPI_DOUBLE,
                 MPI_SUM, 0, comm_cart);

      dp3m_compute_constants_energy_dipolar();

      P3M_TRACE(fprintf(stderr, "%d: dp3m.params.epsilon=%lf\n", this_node,
                        dp3m.params.epsilon));

      if (this_node == 0) {
        /* self energy correction */
        P3M_TRACE(fprintf(stderr, "%d: *dp3m.energy_correction=%20.15lf\n",
                          this_node, dp3m.energy_correction));
#ifdef P3M_DEBUG
        double a = k_space_energy_dip;
#endif
        k_space_energy_dip -=
            dipole.prefactor *
            (dp3m.sum_mu2 * 2 *
             pow(dp3m.params.alpha_L * (1. / box_geo.length()[0]), 3) *
             Utils::sqrt_pi_i() / 3.0);

        double volume =
            box_geo.length()[0] * box_geo.length()[1] * box_geo.length()[2];
        k_space_energy_dip += dipole.prefactor * dp3m.energy_correction /
                              volume; /* add the dipolar energy correction due
                                         to systematic Madelung-Self effects */

        P3M_TRACE(fprintf(stderr, "%d: energy correction: %lf\n", this_node,
                          k_space_energy_dip - a));
      }

      P3M_TRACE(fprintf(stderr,
                        "%d: dipolar p3m end energy calculation: k-space\n",
                        this_node));
    }
  } // if (energy_flag)

  /* === k-space force calculation  === */
  if (force_flag) {
    /****************************
     * DIPOLAR TORQUES (k-space)
     ****************************/
    if (dp3m.sum_mu2 > 0) {
#ifdef ROTATION
      P3M_TRACE(fprintf(stderr,
                        "%d: dipolar p3m start torques calculation: k-space\n",
                        this_node));

      /* fill in ks_mesh array for torque calculation */
      ind = 0;
      i = 0;

      for (j[0] = 0; j[0] < dp3m.fft.plan[3].new_mesh[0]; j[0]++) { // j[0]=n_y
        for (j[1] = 0; j[1] < dp3m.fft.plan[3].new_mesh[1];
             j[1]++) { // j[1]=n_z
          for (j[2] = 0; j[2] < dp3m.fft.plan[3].new_mesh[2];
               j[2]++) { // j[2]=n_x
            // tmp0 = Re(mu)*k,   tmp1 = Im(mu)*k

            tmp0 = dp3m.rs_mesh_dip[0][ind] *
                       dp3m.d_op[j[2] + dp3m.fft.plan[3].start[2]] +
                   dp3m.rs_mesh_dip[1][ind] *
                       dp3m.d_op[j[0] + dp3m.fft.plan[3].start[0]] +
                   dp3m.rs_mesh_dip[2][ind] *
                       dp3m.d_op[j[1] + dp3m.fft.plan[3].start[1]];

            tmp1 = dp3m.rs_mesh_dip[0][ind + 1] *
                       dp3m.d_op[j[2] + dp3m.fft.plan[3].start[2]] +
                   dp3m.rs_mesh_dip[1][ind + 1] *
                       dp3m.d_op[j[0] + dp3m.fft.plan[3].start[0]] +
                   dp3m.rs_mesh_dip[2][ind + 1] *
                       dp3m.d_op[j[1] + dp3m.fft.plan[3].start[1]];

            /* the optimal influence function is the same for torques
               and energy */

            dp3m.ks_mesh[ind] = tmp0 * dp3m.g_energy[i];
            dp3m.ks_mesh[ind + 1] = tmp1 * dp3m.g_energy[i];
            ind += 2;
            i++;
          }
        }
      }

      /* Force component loop */
      for (d = 0; d < 3; d++) {
        d_rs = (d + dp3m.ks_pnum) % 3;
        ind = 0;
        for (j[0] = 0; j[0] < dp3m.fft.plan[3].new_mesh[0]; j[0]++) {
          for (j[1] = 0; j[1] < dp3m.fft.plan[3].new_mesh[1]; j[1]++) {
            for (j[2] = 0; j[2] < dp3m.fft.plan[3].new_mesh[2]; j[2]++) {
              dp3m.rs_mesh[ind] = dp3m.d_op[j[d] + dp3m.fft.plan[3].start[d]] *
                                  dp3m.ks_mesh[ind];
              ind++;
              dp3m.rs_mesh[ind] = dp3m.d_op[j[d] + dp3m.fft.plan[3].start[d]] *
                                  dp3m.ks_mesh[ind];
              ind++;
            }
          }
        }

        /* Back FFT force component mesh */
        fft_perform_back(dp3m.rs_mesh, false, dp3m.fft, comm_cart);
        /* redistribute force component mesh */
        dp3m_spread_force_grid(dp3m.rs_mesh);
        /* Assign force component from mesh to particle */
        P3M_assign_torques(dipole_prefac *
                               (2 * Utils::pi() / box_geo.length()[0]),
                           d_rs, particles);
      }
      P3M_TRACE(fprintf(stderr, "%d: done torque calculation.\n", this_node));
#endif /*if def ROTATION */

      /***************************
         DIPOLAR FORCES (k-space)
      ****************************/
      P3M_TRACE(fprintf(stderr,
                        "%d: dipolar p3m start forces calculation: k-space\n",
                        this_node));

      // Compute forces after torques because the algorithm below overwrites the
      // grids dp3m.rs_mesh_dip !
      // Note: I'll do here 9 inverse FFTs. By symmetry, we can reduce this
      // number to 6 !
      /* fill in ks_mesh array for force calculation */
      ind = 0;
      i = 0;
      for (j[0] = 0; j[0] < dp3m.fft.plan[3].new_mesh[0]; j[0]++) { // j[0]=n_y
        for (j[1] = 0; j[1] < dp3m.fft.plan[3].new_mesh[1];
             j[1]++) { // j[1]=n_z
          for (j[2] = 0; j[2] < dp3m.fft.plan[3].new_mesh[2];
               j[2]++) { // j[2]=n_x
            // tmp0 = Im(mu)*k,   tmp1 = -Re(mu)*k
            tmp0 = dp3m.rs_mesh_dip[0][ind + 1] *
                       dp3m.d_op[j[2] + dp3m.fft.plan[3].start[2]] +
                   dp3m.rs_mesh_dip[1][ind + 1] *
                       dp3m.d_op[j[0] + dp3m.fft.plan[3].start[0]] +
                   dp3m.rs_mesh_dip[2][ind + 1] *
                       dp3m.d_op[j[1] + dp3m.fft.plan[3].start[1]];
            tmp1 = dp3m.rs_mesh_dip[0][ind] *
                       dp3m.d_op[j[2] + dp3m.fft.plan[3].start[2]] +
                   dp3m.rs_mesh_dip[1][ind] *
                       dp3m.d_op[j[0] + dp3m.fft.plan[3].start[0]] +
                   dp3m.rs_mesh_dip[2][ind] *
                       dp3m.d_op[j[1] + dp3m.fft.plan[3].start[1]];
            dp3m.ks_mesh[ind] = tmp0 * dp3m.g_force[i];
            dp3m.ks_mesh[ind + 1] = -tmp1 * dp3m.g_force[i];
            ind += 2;
            i++;
          }
        }
      }

      /* Force component loop */
      for (d = 0; d < 3; d++) { /* direction in k-space: */
        d_rs = (d + dp3m.ks_pnum) % 3;
        ind = 0;
        for (j[0] = 0; j[0] < dp3m.fft.plan[3].new_mesh[0];
             j[0]++) { // j[0]=n_y
          for (j[1] = 0; j[1] < dp3m.fft.plan[3].new_mesh[1];
               j[1]++) { // j[1]=n_z
            for (j[2] = 0; j[2] < dp3m.fft.plan[3].new_mesh[2];
                 j[2]++) { // j[2]=n_x
              tmp0 = dp3m.d_op[j[d] + dp3m.fft.plan[3].start[d]] *
                     dp3m.ks_mesh[ind];
              dp3m.rs_mesh_dip[0][ind] =
                  dp3m.d_op[j[2] + dp3m.fft.plan[3].start[2]] * tmp0;
              dp3m.rs_mesh_dip[1][ind] =
                  dp3m.d_op[j[0] + dp3m.fft.plan[3].start[0]] * tmp0;
              dp3m.rs_mesh_dip[2][ind] =
                  dp3m.d_op[j[1] + dp3m.fft.plan[3].start[1]] * tmp0;
              ind++;
              tmp0 = dp3m.d_op[j[d] + dp3m.fft.plan[3].start[d]] *
                     dp3m.ks_mesh[ind];
              dp3m.rs_mesh_dip[0][ind] =
                  dp3m.d_op[j[2] + dp3m.fft.plan[3].start[2]] * tmp0;
              dp3m.rs_mesh_dip[1][ind] =
                  dp3m.d_op[j[0] + dp3m.fft.plan[3].start[0]] * tmp0;
              dp3m.rs_mesh_dip[2][ind] =
                  dp3m.d_op[j[1] + dp3m.fft.plan[3].start[1]] * tmp0;
              ind++;
            }
          }
        }
        /* Back FFT force component mesh */
        fft_perform_back(dp3m.rs_mesh_dip[0], false, dp3m.fft, comm_cart);
        fft_perform_back(dp3m.rs_mesh_dip[1], false, dp3m.fft, comm_cart);
        fft_perform_back(dp3m.rs_mesh_dip[2], false, dp3m.fft, comm_cart);
        /* redistribute force component mesh */
        dp3m_spread_force_grid(dp3m.rs_mesh_dip[0]);
        dp3m_spread_force_grid(dp3m.rs_mesh_dip[1]);
        dp3m_spread_force_grid(dp3m.rs_mesh_dip[2]);
        /* Assign force component from mesh to particle */
        dp3m_assign_forces_dip(
            dipole_prefac * pow(2 * Utils::pi() / box_geo.length()[0], 2), d_rs,
            particles);
      }

      P3M_TRACE(fprintf(stderr,
                        "%d: dipolar p3m end forces calculation: k-space\n",
                        this_node));

    } /* of if (dp3m.sum_mu2>0 */
  }   /* of if(force_flag) */

  if (dp3m.params.epsilon != P3M_EPSILON_METALLIC) {
    surface_term = calc_surface_term(force_flag, energy_flag, particles);
    if (this_node == 0)
      k_space_energy_dip += surface_term;
  }

  return k_space_energy_dip;
}

/************************************************************/

double calc_surface_term(int force_flag, int energy_flag,
                         const ParticleRange &particles) {
  const double pref = dipole.prefactor * 4 * M_PI * (1. / box_geo.length()[0]) *
                      (1. / box_geo.length()[1]) * (1. / box_geo.length()[2]) /
                      (2 * dp3m.params.epsilon + 1);
  double suma, a[3];
  double en;

  auto const n_local_part = particles.size();

  // We put all the dipolar momenta in a the arrays mx,my,mz according to the
  // id-number of the particles
  std::vector<double> mx(n_local_part);
  std::vector<double> my(n_local_part);
  std::vector<double> mz(n_local_part);

  int ip = 0;
  for (auto const &p : particles) {
    const Utils::Vector3d dip = p.calc_dip();
    mx[ip] = dip[0];
    my[ip] = dip[1];
    mz[ip] = dip[2];
    ip++;
  }

  // we will need the sum of all dipolar momenta vectors
  a[0] = 0.0;
  a[1] = 0.0;
  a[2] = 0.0;

  for (int i = 0; i < n_local_part; i++) {
    a[0] += mx[i];
    a[1] += my[i];
    a[2] += mz[i];
  }

  MPI_Allreduce(MPI_IN_PLACE, a, 3, MPI_DOUBLE, MPI_SUM, comm_cart);

  if (energy_flag) {

    suma = 0.0;
    for (int i = 0; i < n_local_part; i++) {
      suma += mx[i] * a[0] + my[i] * a[1] + mz[i] * a[2];
    }
    MPI_Allreduce(MPI_IN_PLACE, &suma, 1, MPI_DOUBLE, MPI_SUM, comm_cart);
    en = 0.5 * pref * suma;

  } else {
    en = 0;
  }
#ifdef ROTATION
  if (force_flag) {

    std::vector<double> sumix(n_local_part);
    std::vector<double> sumiy(n_local_part);
    std::vector<double> sumiz(n_local_part);

    for (int i = 0; i < n_local_part; i++) {
      sumix[i] = my[i] * a[2] - mz[i] * a[1];
      sumiy[i] = mz[i] * a[0] - mx[i] * a[2];
      sumiz[i] = mx[i] * a[1] - my[i] * a[0];
    }

    ip = 0;
    for (auto &p : particles) {
      p.f.torque[0] -= pref * sumix[ip];
      p.f.torque[1] -= pref * sumiy[ip];
      p.f.torque[2] -= pref * sumiz[ip];
      ip++;
    }
  }
#endif

  return en;
}

/************************************************************/
void dp3m_gather_fft_grid(double *themesh) {
  int s_dir, r_dir, evenodd;
  MPI_Status status;
  double *tmp_ptr;

  P3M_TRACE(fprintf(stderr, "%d: dp3m_gather_fft_grid:\n", this_node));

  auto const node_neighbors = calc_node_neighbors(comm_cart);
  auto const node_pos = calc_node_pos(comm_cart);

  /* direction loop */
  for (s_dir = 0; s_dir < 6; s_dir++) {
    if (s_dir % 2 == 0)
      r_dir = s_dir + 1;
    else
      r_dir = s_dir - 1;
    /* pack send block */
    if (dp3m.sm.s_size[s_dir] > 0)
      fft_pack_block(themesh, dp3m.send_grid, dp3m.sm.s_ld[s_dir],
                     dp3m.sm.s_dim[s_dir], dp3m.local_mesh.dim, 1);

    /* communication */
    if (node_neighbors[s_dir] != this_node) {
      for (evenodd = 0; evenodd < 2; evenodd++) {
        if ((node_pos[s_dir / 2] + evenodd) % 2 == 0) {
          if (dp3m.sm.s_size[s_dir] > 0)
            MPI_Send(dp3m.send_grid, dp3m.sm.s_size[s_dir], MPI_DOUBLE,
                     node_neighbors[s_dir], REQ_P3M_GATHER_D, comm_cart);
        } else {
          if (dp3m.sm.r_size[r_dir] > 0)
            MPI_Recv(dp3m.recv_grid, dp3m.sm.r_size[r_dir], MPI_DOUBLE,
                     node_neighbors[r_dir], REQ_P3M_GATHER_D, comm_cart,
                     &status);
        }
      }
    } else {
      tmp_ptr = dp3m.recv_grid;
      dp3m.recv_grid = dp3m.send_grid;
      dp3m.send_grid = tmp_ptr;
    }
    /* add recv block */
    if (dp3m.sm.r_size[r_dir] > 0) {
      p3m_add_block(dp3m.recv_grid, themesh, dp3m.sm.r_ld[r_dir],
                    dp3m.sm.r_dim[r_dir], dp3m.local_mesh.dim);
    }
  }
}

/************************************************************/

void dp3m_spread_force_grid(double *themesh) {
  int s_dir, r_dir, evenodd;
  MPI_Status status;
  double *tmp_ptr;
  P3M_TRACE(fprintf(stderr, "%d: dipolar p3m_spread_force_grid:\n", this_node));

  auto const node_neighbors = calc_node_neighbors(comm_cart);
  auto const node_pos = calc_node_pos(comm_cart);

  /* direction loop */
  for (s_dir = 5; s_dir >= 0; s_dir--) {
    if (s_dir % 2 == 0)
      r_dir = s_dir + 1;
    else
      r_dir = s_dir - 1;
    /* pack send block */
    if (dp3m.sm.s_size[s_dir] > 0)
      fft_pack_block(themesh, dp3m.send_grid, dp3m.sm.r_ld[r_dir],
                     dp3m.sm.r_dim[r_dir], dp3m.local_mesh.dim, 1);
    /* communication */
    if (node_neighbors[r_dir] != this_node) {
      for (evenodd = 0; evenodd < 2; evenodd++) {
        if ((node_pos[r_dir / 2] + evenodd) % 2 == 0) {
          if (dp3m.sm.r_size[r_dir] > 0)
            MPI_Send(dp3m.send_grid, dp3m.sm.r_size[r_dir], MPI_DOUBLE,
                     node_neighbors[r_dir], REQ_P3M_SPREAD_D, comm_cart);
        } else {
          if (dp3m.sm.s_size[s_dir] > 0)
            MPI_Recv(dp3m.recv_grid, dp3m.sm.s_size[s_dir], MPI_DOUBLE,
                     node_neighbors[s_dir], REQ_P3M_SPREAD_D, comm_cart,
                     &status);
        }
      }
    } else {
      tmp_ptr = dp3m.recv_grid;
      dp3m.recv_grid = dp3m.send_grid;
      dp3m.send_grid = tmp_ptr;
    }
    /* un pack recv block */
    if (dp3m.sm.s_size[s_dir] > 0) {
      fft_unpack_block(dp3m.recv_grid, themesh, dp3m.sm.s_ld[s_dir],
                       dp3m.sm.s_dim[s_dir], dp3m.local_mesh.dim, 1);
    }
  }
}

/*****************************************************************************/

void dp3m_realloc_ca_fields(int newsize) {
  newsize = ((newsize + CA_INCREMENT - 1) / CA_INCREMENT) * CA_INCREMENT;
  if (newsize == dp3m.ca_num)
    return;
  if (newsize < CA_INCREMENT)
    newsize = CA_INCREMENT;

  P3M_TRACE(fprintf(
      stderr,
      "%d: p3m_realloc_ca_fields: dipolar,  old_size=%d -> new_size=%d\n",
      this_node, dp3m.ca_num, newsize));
  dp3m.ca_num = newsize;
  dp3m.ca_frac = Utils::realloc(dp3m.ca_frac, dp3m.params.cao3 * dp3m.ca_num *
                                                  sizeof(double));
  dp3m.ca_fmp = Utils::realloc(dp3m.ca_fmp, dp3m.ca_num * sizeof(int));
}

/*****************************************************************************/

void dp3m_calc_meshift() {
  int i;
  double dmesh;
  dmesh = (double)dp3m.params.mesh[0];
  dp3m.meshift =
      Utils::realloc(dp3m.meshift, dp3m.params.mesh[0] * sizeof(double));
  for (i = 0; i < dp3m.params.mesh[0]; i++)
    dp3m.meshift[i] = i - std::round(i / dmesh) * dmesh;
}

/*****************************************************************************/

void dp3m_calc_differential_operator() {
  int i;
  double dmesh;

  dmesh = (double)dp3m.params.mesh[0];
  dp3m.d_op = Utils::realloc(dp3m.d_op, dp3m.params.mesh[0] * sizeof(double));

  for (i = 0; i < dp3m.params.mesh[0]; i++)
    dp3m.d_op[i] = (double)i - std::round((double)i / dmesh) * dmesh;

  dp3m.d_op[dp3m.params.mesh[0] / 2] = 0;
}

/*****************************************************************************/

void dp3m_calc_influence_function_force() {
  int i, n[3], ind;
  int end[3];
  int size = 1;
  double fak1, fak2;
  double nominator[1] = {0.0}, denominator = 0.0;

  dp3m_calc_meshift();

  for (i = 0; i < 3; i++) {
    size *= dp3m.fft.plan[3].new_mesh[i];
    end[i] = dp3m.fft.plan[3].start[i] + dp3m.fft.plan[3].new_mesh[i];
  }
  dp3m.g_force = Utils::realloc(dp3m.g_force, size * sizeof(double));
  fak1 = dp3m.params.mesh[0] * dp3m.params.mesh[0] * dp3m.params.mesh[0] * 2.0 /
         (box_geo.length()[0] * box_geo.length()[0]);

  for (n[0] = dp3m.fft.plan[3].start[0]; n[0] < end[0]; n[0]++)
    for (n[1] = dp3m.fft.plan[3].start[1]; n[1] < end[1]; n[1]++)
      for (n[2] = dp3m.fft.plan[3].start[2]; n[2] < end[2]; n[2]++) {
        ind = (n[2] - dp3m.fft.plan[3].start[2]) +
              dp3m.fft.plan[3].new_mesh[2] *
                  ((n[1] - dp3m.fft.plan[3].start[1]) +
                   (dp3m.fft.plan[3].new_mesh[1] *
                    (n[0] - dp3m.fft.plan[3].start[0])));

        if ((n[0] == 0) && (n[1] == 0) && (n[2] == 0))
          dp3m.g_force[ind] = 0.0;
        else if ((n[0] % (dp3m.params.mesh[0] / 2) == 0) &&
                 (n[1] % (dp3m.params.mesh[0] / 2) == 0) &&
                 (n[2] % (dp3m.params.mesh[0] / 2) == 0))
          dp3m.g_force[ind] = 0.0;
        else {
          denominator = dp3m_perform_aliasing_sums_force(n, nominator);
          fak2 = nominator[0];
          fak2 /=
              pow(Utils::sqr(dp3m.d_op[n[0]]) + Utils::sqr(dp3m.d_op[n[1]]) +
                      Utils::sqr(dp3m.d_op[n[2]]),
                  3) *
              Utils::sqr(denominator);
          dp3m.g_force[ind] = fak1 * fak2;
        }
      }
}

/*****************************************************************************/

double dp3m_perform_aliasing_sums_force(const int n[3], double nominator[1]) {
  double denominator = 0.0;
  /* lots of temporary variables... */
  double sx, sy, sz, f1, f2, f3, mx, my, mz, nmx, nmy, nmz, nm2, expo;
  double limit = 30;
  double n_nm;
  double n_nm3;

  nominator[0] = 0.0;

  f1 = 1.0 / (double)dp3m.params.mesh[0];
  f2 = Utils::sqr(Utils::pi() / (dp3m.params.alpha_L));

  for (mx = -P3M_BRILLOUIN; mx <= P3M_BRILLOUIN; mx++) {
    nmx = dp3m.meshift[n[0]] + dp3m.params.mesh[0] * mx;
    sx = pow(sinc(f1 * nmx), 2.0 * dp3m.params.cao);
    for (my = -P3M_BRILLOUIN; my <= P3M_BRILLOUIN; my++) {
      nmy = dp3m.meshift[n[1]] + dp3m.params.mesh[0] * my;
      sy = sx * pow(sinc(f1 * nmy), 2.0 * dp3m.params.cao);
      for (mz = -P3M_BRILLOUIN; mz <= P3M_BRILLOUIN; mz++) {
        nmz = dp3m.meshift[n[2]] + dp3m.params.mesh[0] * mz;
        sz = sy * pow(sinc(f1 * nmz), 2.0 * dp3m.params.cao);

        nm2 = Utils::sqr(nmx) + Utils::sqr(nmy) + Utils::sqr(nmz);
        expo = f2 * nm2;
        f3 = (expo < limit) ? sz * exp(-expo) / nm2 : 0.0;

        n_nm = dp3m.d_op[n[0]] * nmx + dp3m.d_op[n[1]] * nmy +
               dp3m.d_op[n[2]] * nmz;
        n_nm3 = n_nm * n_nm * n_nm;

        nominator[0] += f3 * n_nm3;
        denominator += sz;
      }
    }
  }
  return denominator;
}

/*****************************************************************************/

void dp3m_calc_influence_function_energy() {
  int i, n[3], ind;
  int end[3];
  int size = 1;
  double fak1, fak2;
  double nominator[1] = {0.0}, denominator = 0.0;

  dp3m_calc_meshift();

  for (i = 0; i < 3; i++) {
    size *= dp3m.fft.plan[3].new_mesh[i];
    end[i] = dp3m.fft.plan[3].start[i] + dp3m.fft.plan[3].new_mesh[i];
  }
  dp3m.g_energy = Utils::realloc(dp3m.g_energy, size * sizeof(double));
  fak1 = dp3m.params.mesh[0] * dp3m.params.mesh[0] * dp3m.params.mesh[0] * 2.0 /
         (box_geo.length()[0] * box_geo.length()[0]);

  for (n[0] = dp3m.fft.plan[3].start[0]; n[0] < end[0]; n[0]++)
    for (n[1] = dp3m.fft.plan[3].start[1]; n[1] < end[1]; n[1]++)
      for (n[2] = dp3m.fft.plan[3].start[2]; n[2] < end[2]; n[2]++) {
        ind = (n[2] - dp3m.fft.plan[3].start[2]) +
              dp3m.fft.plan[3].new_mesh[2] *
                  ((n[1] - dp3m.fft.plan[3].start[1]) +
                   (dp3m.fft.plan[3].new_mesh[1] *
                    (n[0] - dp3m.fft.plan[3].start[0])));

        if ((n[0] == 0) && (n[1] == 0) && (n[2] == 0))
          dp3m.g_energy[ind] = 0.0;
        else if ((n[0] % (dp3m.params.mesh[0] / 2) == 0) &&
                 (n[1] % (dp3m.params.mesh[0] / 2) == 0) &&
                 (n[2] % (dp3m.params.mesh[0] / 2) == 0))
          dp3m.g_energy[ind] = 0.0;
        else {
          denominator = dp3m_perform_aliasing_sums_energy(n, nominator);
          fak2 = nominator[0];
          fak2 /=
              pow(Utils::sqr(dp3m.d_op[n[0]]) + Utils::sqr(dp3m.d_op[n[1]]) +
                      Utils::sqr(dp3m.d_op[n[2]]),
                  2) *
              Utils::sqr(denominator);
          dp3m.g_energy[ind] = fak1 * fak2;
        }
      }
}

/*****************************************************************************/

double dp3m_perform_aliasing_sums_energy(const int n[3], double nominator[1]) {
  double denominator = 0.0;
  /* lots of temporary variables... */
  double sx, sy, sz, f1, f2, f3, mx, my, mz, nmx, nmy, nmz, nm2, expo;
  double limit = 30;
  double n_nm;
  double n_nm2;

  nominator[0] = 0.0;

  f1 = 1.0 / (double)dp3m.params.mesh[0];
  f2 = Utils::sqr(Utils::pi() / (dp3m.params.alpha_L));

  for (mx = -P3M_BRILLOUIN; mx <= P3M_BRILLOUIN; mx++) {
    nmx = dp3m.meshift[n[0]] + dp3m.params.mesh[0] * mx;
    sx = pow(sinc(f1 * nmx), 2.0 * dp3m.params.cao);
    for (my = -P3M_BRILLOUIN; my <= P3M_BRILLOUIN; my++) {
      nmy = dp3m.meshift[n[1]] + dp3m.params.mesh[0] * my;
      sy = sx * pow(sinc(f1 * nmy), 2.0 * dp3m.params.cao);
      for (mz = -P3M_BRILLOUIN; mz <= P3M_BRILLOUIN; mz++) {
        nmz = dp3m.meshift[n[2]] + dp3m.params.mesh[0] * mz;
        sz = sy * pow(sinc(f1 * nmz), 2.0 * dp3m.params.cao);

        nm2 = Utils::sqr(nmx) + Utils::sqr(nmy) + Utils::sqr(nmz);
        expo = f2 * nm2;
        f3 = (expo < limit) ? sz * exp(-expo) / nm2 : 0.0;

        n_nm = dp3m.d_op[n[0]] * nmx + dp3m.d_op[n[1]] * nmy +
               dp3m.d_op[n[2]] * nmz;
        n_nm2 = n_nm * n_nm;
        nominator[0] += f3 * n_nm2;
        denominator += sz;
      }
    }
  }
  return denominator;
}

/*****************************************************************************/

/************************************************
 * Functions for dipolar P3M Parameter tuning
 * This tuning is based on the P3M tuning of the charges
 * which in turn is based on the P3M_tune by M. Deserno
 ************************************************/

#define P3M_TUNE_MAX_CUTS 50

/*****************************************************************************/

/** @copybrief p3m_get_accuracy
 *
 *  The real space error is tuned such that it contributes half of the
 *  total error, and then the Fourier space error is calculated.
 *  If an optimal alpha is not found, the value 0.1 is used as fallback.
 *  @param[in]  mesh       @copybrief P3MParameters::mesh
 *  @param[in]  cao        @copybrief P3MParameters::cao
 *  @param[in]  r_cut_iL   @copybrief P3MParameters::r_cut_iL
 *  @param[out] _alpha_L   @copybrief P3MParameters::alpha_L
 *  @param[out] _rs_err    real space error
 *  @param[out] _ks_err    Fourier space error
 *  @returns Error magnitude
 */
double dp3m_get_accuracy(int mesh, int cao, double r_cut_iL, double *_alpha_L,
                         double *_rs_err, double *_ks_err) {
  double rs_err, ks_err;
  double alpha_L;
  P3M_TRACE(fprintf(stderr, "dp3m_get_accuracy: mesh %d, cao %d, r_cut %f ",
                    mesh, cao, r_cut_iL));

  /* calc maximal real space error for setting */

  // Alpha cannot be zero in the dipolar case because real_space formula breaks
  // down
  rs_err = P3M_DIPOLAR_real_space_error(box_geo.length()[0], dipole.prefactor,
                                        r_cut_iL, dp3m.sum_dip_part,
                                        dp3m.sum_mu2, 0.001);

  if (M_SQRT2 * rs_err > dp3m.params.accuracy) {
    /* assume rs_err = ks_err -> rs_err = accuracy/sqrt(2.0) -> alpha_L */
    alpha_L = dp3m_rtbisection(
        box_geo.length()[0], dipole.prefactor, r_cut_iL, dp3m.sum_dip_part,
        dp3m.sum_mu2, 0.0001 * box_geo.length()[0], 5.0 * box_geo.length()[0],
        0.0001, dp3m.params.accuracy);

  }

  else
    /* even alpha=0 is ok, however, we cannot choose it since it kills the
       k-space error formula.
       Anyways, this very likely NOT the optimal solution */
    alpha_L = 0.1;

  *_alpha_L = alpha_L;
  /* calculate real space and k-space error for this alpha_L */

  rs_err = P3M_DIPOLAR_real_space_error(box_geo.length()[0], dipole.prefactor,
                                        r_cut_iL, dp3m.sum_dip_part,
                                        dp3m.sum_mu2, alpha_L);
  ks_err = dp3m_k_space_error(box_geo.length()[0], dipole.prefactor, mesh, cao,
                              dp3m.sum_dip_part, dp3m.sum_mu2, alpha_L);

  *_rs_err = rs_err;
  *_ks_err = ks_err;
  P3M_TRACE(fprintf(stderr, "dipolar tuning resulting: %f -> %f %f\n", alpha_L,
                    rs_err, ks_err));
  return sqrt(Utils::sqr(rs_err) + Utils::sqr(ks_err));
}

/*****************************************************************************/

/** @copybrief p3m_mcr_time
 *
 *  @param[in]  mesh            @copybrief P3MParameters::mesh
 *  @param[in]  cao             @copybrief P3MParameters::cao
 *  @param[in]  r_cut_iL        @copybrief P3MParameters::r_cut_iL
 *  @param[in]  alpha_L         @copybrief P3MParameters::alpha_L
 *
 *  @returns The integration time in case of success, otherwise
 *           -@ref P3M_TUNE_FAIL
 */
static double dp3m_mcr_time(int mesh, int cao, double r_cut_iL,
                            double alpha_L) {
  /* rounded up 2000/n_charges timing force evaluations */
  int int_num = (1999 + dp3m.sum_dip_part) / dp3m.sum_dip_part;

  /* broadcast p3m parameters for test run */
  if (dipole.method != DIPOLAR_P3M && dipole.method != DIPOLAR_MDLC_P3M)
    Dipole::set_method_local(DIPOLAR_P3M);
  dp3m.params.r_cut_iL = r_cut_iL;
  dp3m.params.mesh[0] = dp3m.params.mesh[1] = dp3m.params.mesh[2] = mesh;
  dp3m.params.cao = cao;
  dp3m.params.alpha_L = alpha_L;
  dp3m_scaleby_box_l();
  /* initialize p3m structures */
  mpi_bcast_coulomb_params();
  /* perform force calculation test */
  double int_time = time_force_calc(int_num);
  if (int_time == -1) {
    return -P3M_TUNE_FAIL;
  }
  return int_time;
}

/*****************************************************************************/

/** @copybrief p3m_mc_time
 *
 *  The @p _r_cut_iL is determined via a simple bisection.
 *
 *  @param[out] log             log output
 *  @param[in]  mesh            @copybrief P3MParameters::mesh
 *  @param[in]  cao             @copybrief P3MParameters::cao
 *  @param[in]  r_cut_iL_min    lower bound for @p _r_cut_iL
 *  @param[in]  r_cut_iL_max    upper bound for @p _r_cut_iL
 *  @param[out] _r_cut_iL       @copybrief P3MParameters::r_cut_iL
 *  @param[out] _alpha_L        @copybrief P3MParameters::alpha_L
 *  @param[out] _accuracy       @copybrief P3MParameters::accuracy
 *
 *  @returns The integration time in case of success, otherwise
 *           -@ref P3M_TUNE_FAIL, -@ref P3M_TUNE_ACCURACY_TOO_LARGE,
 *           -@ref P3M_TUNE_CAO_TOO_LARGE, -@ref P3M_TUNE_ELCTEST, or
 *           -@ref P3M_TUNE_CUTOFF_TOO_LARGE
 */
static double dp3m_mc_time(char **log, int mesh, int cao, double r_cut_iL_min,
                           double r_cut_iL_max, double *_r_cut_iL,
                           double *_alpha_L, double *_accuracy) {
  double int_time;
  double r_cut_iL;
  double rs_err, ks_err, mesh_size, k_cut;
  int i, n_cells;
  char b[3 * ES_INTEGER_SPACE + 3 * ES_DOUBLE_SPACE + 128];

  /* initial checks. */
  mesh_size = box_geo.length()[0] / (double)mesh;
  k_cut = mesh_size * cao / 2.0;

  auto const min_box_l = *boost::min_element(box_geo.length());
  auto const min_local_box_l = *boost::min_element(local_geo.length());

  if (cao >= mesh || k_cut >= std::min(min_box_l, min_local_box_l) - skin) {
    /* print result */
    sprintf(b, "%-4d %-3d  cao too large for this mesh\n", mesh, cao);
    *log = strcat_alloc(*log, b);
    return -P3M_TUNE_CAO_TOO_LARGE;
  }

  /* Either low and high boundary are equal (for fixed cut), or the low border
     is initially 0 and therefore
     has infinite error estimate, as required. Therefore if the high boundary
     fails, there is no possible r_cut */
  if ((*_accuracy = dp3m_get_accuracy(mesh, cao, r_cut_iL_max, _alpha_L,
                                      &rs_err, &ks_err)) >
      dp3m.params.accuracy) {
    /* print result */
    sprintf(b, "%-4d %-3d %.5e %.5e %.5e %.3e %.3e accuracy not achieved\n",
            mesh, cao, r_cut_iL_max, *_alpha_L, *_accuracy, rs_err, ks_err);
    *log = strcat_alloc(*log, b);
    return -P3M_TUNE_ACCURACY_TOO_LARGE;
  }

  for (;;) {
    P3M_TRACE(fprintf(stderr, "dp3m_mc_time: interval [%f,%f]\n", r_cut_iL_min,
                      r_cut_iL_max));
    r_cut_iL = 0.5 * (r_cut_iL_min + r_cut_iL_max);

    if (r_cut_iL_max - r_cut_iL_min < P3M_RCUT_PREC)
      break;

    /* bisection */
    if (dp3m_get_accuracy(mesh, cao, r_cut_iL, _alpha_L, &rs_err, &ks_err) >
        dp3m.params.accuracy)
      r_cut_iL_min = r_cut_iL;
    else
      r_cut_iL_max = r_cut_iL;
  }
  /* final result is always the upper interval boundary, since only there
     we know that the desired minimal accuracy is obtained */
  *_r_cut_iL = r_cut_iL = r_cut_iL_max;

  /* check whether we are running P3M+DLC, and whether we leave a reasonable gap
   * space */
  if (dipole.method == DIPOLAR_MDLC_P3M) {
    runtimeErrorMsg() << "dipolar P3M: tuning when dlc needs to be fixed";
  }

  /* check whether this radius is too large, so that we would use less cells
   * than allowed */
  n_cells = 1;
  for (i = 0; i < 3; i++)
    n_cells *= (int)(floor(local_geo.length()[i] /
                           (r_cut_iL * box_geo.length()[0] + skin)));
  if (n_cells < min_num_cells) {
    P3M_TRACE(fprintf(
        stderr, "dp3m_mc_time: mesh %d cao %d r_cut %f reject n_cells %d\n",
        mesh, cao, r_cut_iL, n_cells));
    /* print result */
    sprintf(b, "%-4d %-3d %.5e %.5e %.5e %.3e %.3e radius dangerously high\n\n",
            mesh, cao, r_cut_iL_max, *_alpha_L, *_accuracy, rs_err, ks_err);
    *log = strcat_alloc(*log, b);
    return -P3M_TUNE_CUTOFF_TOO_LARGE;
  }

  int_time = dp3m_mcr_time(mesh, cao, r_cut_iL, *_alpha_L);
  if (int_time == -P3M_TUNE_FAIL) {
    *log = strcat_alloc(*log, "tuning failed, test integration not possible\n");
    return int_time;
  }

  *_accuracy =
      dp3m_get_accuracy(mesh, cao, r_cut_iL, _alpha_L, &rs_err, &ks_err);

  P3M_TRACE(fprintf(stderr, "dp3m_mc_time: mesh %d cao %d r_cut %f time %f\n",
                    mesh, cao, r_cut_iL, int_time));
  /* print result */
  sprintf(b, "%-4d %-3d %.5e %.5e %.5e %.3e %.3e %-8d\n", mesh, cao, r_cut_iL,
          *_alpha_L, *_accuracy, rs_err, ks_err, (int)int_time);
  *log = strcat_alloc(*log, b);
  return int_time;
}

/*****************************************************************************/

/** @copybrief p3m_m_time
 *
 *  @p _cao should contain an initial guess, which is then adapted by stepping
 *  up and down.
 *
 *  @param[out]     log             log output
 *  @param[in]      mesh            @copybrief P3MParameters::mesh
 *  @param[in]      cao_min         lower bound for @p _cao
 *  @param[in]      cao_max         upper bound for @p _cao
 *  @param[in,out]  _cao            initial guess for the
 *                                  @copybrief P3MParameters::cao
 *  @param[in]      r_cut_iL_min    lower bound for @p _r_cut_iL
 *  @param[in]      r_cut_iL_max    upper bound for @p _r_cut_iL
 *  @param[out]     _r_cut_iL       @copybrief P3MParameters::r_cut_iL
 *  @param[out]     _alpha_L        @copybrief P3MParameters::alpha_L
 *  @param[out]     _accuracy       @copybrief P3MParameters::accuracy
 *
 *  @returns The integration time in case of success, otherwise
 *           -@ref P3M_TUNE_FAIL or -@ref P3M_TUNE_CAO_TOO_LARGE */
static double dp3m_m_time(char **log, int mesh, int cao_min, int cao_max,
                          int *_cao, double r_cut_iL_min, double r_cut_iL_max,
                          double *_r_cut_iL, double *_alpha_L,
                          double *_accuracy) {
  double best_time = -1, tmp_r_cut_iL = -1., tmp_alpha_L = 0.0,
         tmp_accuracy = 0.0;
  /* in which direction improvement is possible. Initially, we don't know it
   * yet.
   */
  int final_dir = 0;
  int cao = *_cao;

  P3M_TRACE(fprintf(
      stderr,
      "dp3m_m_time: Dmesh=%d, Dcao_min=%d, Dcao_max=%d, Drmin=%f, Drmax=%f\n",
      mesh, cao_min, cao_max, r_cut_iL_min, r_cut_iL_max));
  /* the initial step sets a timing mark. If there is no valid r_cut, we can
     only try
     to increase cao to increase the obtainable precision of the far formula. */
  double tmp_time;
  do {
    tmp_time = dp3m_mc_time(log, mesh, cao, r_cut_iL_min, r_cut_iL_max,
                            &tmp_r_cut_iL, &tmp_alpha_L, &tmp_accuracy);
    /* bail out if the force evaluation is not working */
    if (tmp_time == -P3M_TUNE_FAIL)
      return tmp_time;
    /* cao is too large for this grid, but still the accuracy cannot be
     * achieved, give up */
    if (tmp_time == -P3M_TUNE_CAO_TOO_LARGE) {
      P3M_TRACE(fprintf(stderr, "dp3m_m_time: no possible cao found\n"));
      return tmp_time;
    }
    /* we have a valid time, start optimising from there */
    if (tmp_time >= 0) {
      best_time = tmp_time;
      *_r_cut_iL = tmp_r_cut_iL;
      *_alpha_L = tmp_alpha_L;
      *_accuracy = tmp_accuracy;
      *_cao = cao;
      break;
    }
    /* the required accuracy could not be obtained, try higher caos. Therefore
       optimisation can only be
       obtained with even higher caos, but not lower ones */
    P3M_TRACE(fprintf(stderr, "dp3m_m_time: "
                              "doesn't give precision, step up\n"));
    cao++;
    final_dir = 1;
  } while (cao <= cao_max);
  /* with this mesh, the required accuracy cannot be obtained. */
  if (cao > cao_max)
    return -P3M_TUNE_CAO_TOO_LARGE;

  /* at the boundaries, only the opposite direction can be used for optimisation
   */
  if (cao == cao_min)
    final_dir = 1;
  else if (cao == cao_max)
    final_dir = -1;

  P3M_TRACE(
      fprintf(stderr, "dp3m_m_time: final constraints dir %d\n", final_dir));

  if (final_dir == 0) {
    /* check in which direction we can optimise. Both directions are possible */
    double dir_times[3];
    for (final_dir = -1; final_dir <= 1; final_dir += 2) {
      dir_times[final_dir + 1] = tmp_time =
          dp3m_mc_time(log, mesh, cao + final_dir, r_cut_iL_min, r_cut_iL_max,
                       &tmp_r_cut_iL, &tmp_alpha_L, &tmp_accuracy);
      /* bail out on errors, as usual */
      if (tmp_time == -P3M_TUNE_FAIL)
        return tmp_time;
      /* in this direction, we cannot optimise, since we get into precision
       * trouble */
      if (tmp_time < 0)
        continue;

      if (tmp_time < best_time) {
        best_time = tmp_time;
        *_r_cut_iL = tmp_r_cut_iL;
        *_alpha_L = tmp_alpha_L;
        *_accuracy = tmp_accuracy;
        *_cao = cao + final_dir;
      }
    }
    /* choose the direction which was optimal, if any of the two */
    if (dir_times[0] == best_time) {
      final_dir = -1;
    } else if (dir_times[2] == best_time) {
      final_dir = 1;
    } else {
      /* no improvement in either direction, however if one is only marginally
       * worse, we can still try*/
      /* down is possible and not much worse, while up is either illegal or even
       * worse */
      if ((dir_times[0] >= 0 && dir_times[0] < best_time + P3M_TIME_GRAN) &&
          (dir_times[2] < 0 || dir_times[2] > dir_times[0]))
        final_dir = -1;
      /* same for up */
      else if ((dir_times[2] >= 0 &&
                dir_times[2] < best_time + P3M_TIME_GRAN) &&
               (dir_times[0] < 0 || dir_times[0] > dir_times[2]))
        final_dir = 1;
      else {
        /* really no chance for optimisation */
        P3M_TRACE(fprintf(stderr,
                          "dp3m_m_time: Dmesh=%d final Dcao=%d time=%f\n", mesh,
                          cao, best_time));
        return best_time;
      }
    }
    /* we already checked the initial cao and its neighbor */
    cao += 2 * final_dir;
  } else {
    /* here some constraint is active, and we only checked the initial cao
     * itself */
    cao += final_dir;
  }

  P3M_TRACE(
      fprintf(stderr, "dp3m_m_time: optimise in direction %d\n", final_dir));

  /* move cao into the optimisation direction until we do not gain anymore. */
  for (; cao >= cao_min && cao <= cao_max; cao += final_dir) {
    tmp_time = dp3m_mc_time(log, mesh, cao, r_cut_iL_min, r_cut_iL_max,
                            &tmp_r_cut_iL, &tmp_alpha_L, &tmp_accuracy);
    /* bail out on errors, as usual */
    if (tmp_time == -P3M_TUNE_FAIL)
      return tmp_time;
    /* if we cannot meet the precision anymore, give up */
    if (tmp_time < 0)
      break;

    if (tmp_time < best_time) {
      best_time = tmp_time;
      *_r_cut_iL = tmp_r_cut_iL;
      *_alpha_L = tmp_alpha_L;
      *_accuracy = tmp_accuracy;
      *_cao = cao;
    }
    /* no hope of further optimisation */
    else if (tmp_time > best_time + P3M_TIME_GRAN)
      break;
  }
  P3M_TRACE(fprintf(stderr,
                    "dp3m_m_time: "
                    "Dmesh=%d final Dcao=%d Dr_cut=%f time=%f\n",
                    mesh, *_cao, *_r_cut_iL, best_time));
  return best_time;
}

int dp3m_adaptive_tune(char **logger) {
  /** Tuning of dipolar P3M. The algorithm basically determines the mesh, cao
   *  and then the real space cutoff, in this nested order.
   *
   *  For each mesh, the cao optimal for the mesh tested previously is used as
   *  an initial guess, and the algorithm tries whether increasing or decreasing
   *  it leads to a better solution. This is efficient, since the optimal cao
   *  only changes little with the meshes in general.
   *
   *  The real space cutoff for a given mesh and cao is determined via a
   *  bisection on the error estimate, which determines where the error
   *  estimate equals the required accuracy. Therefore the smallest possible,
   *  i.e. fastest real space cutoff is determined.
   *
   *  Both the search over mesh and cao stop to search in a specific direction
   *  once the computation time is significantly higher than the currently
   *  known optimum.
   */
  int mesh_max, mesh = -1, tmp_mesh;
  double r_cut_iL_min, r_cut_iL_max, r_cut_iL = -1, tmp_r_cut_iL = 0.0;
  int cao_min, cao_max, cao = -1, tmp_cao;

  double alpha_L = -1, tmp_alpha_L = 0.0;
  double accuracy = -1, tmp_accuracy = 0.0;
  double time_best = 1e20, tmp_time;
  char b[3 * ES_INTEGER_SPACE + 3 * ES_DOUBLE_SPACE + 128];

  P3M_TRACE(fprintf(stderr, "%d: dp3m_adaptive_tune\n", this_node));

  /* preparation */
  mpi_call(dp3m_count_magnetic_particles);
  dp3m_count_magnetic_particles();

  /* Print Status */
  sprintf(b,
          "Dipolar P3M tune parameters: Accuracy goal = %.5e prefactor "
          "= %.5e\n",
          dp3m.params.accuracy, dipole.prefactor);
  *logger = strcat_alloc(*logger, b);
  sprintf(b, "System: box_l = %.5e # charged part = %d Sum[q_i^2] = %.5e\n",
          box_geo.length()[0], dp3m.sum_dip_part, dp3m.sum_mu2);
  *logger = strcat_alloc(*logger, b);

  if (dp3m.sum_dip_part == 0) {
    *logger = strcat_alloc(
        *logger, "no dipolar particles in the system, cannot tune dipolar P3M");
    return ES_ERROR;
  }

  /* parameter ranges */
  if (dp3m.params.mesh[0] == 0) {
    double expo;
    expo = log(pow((double)dp3m.sum_dip_part, (1.0 / 3.0))) / log(2.0);

    tmp_mesh = (int)(pow(2.0, (double)((int)expo)) + 0.1);
    /* this limits the tried meshes if the accuracy cannot
       be obtained with smaller meshes, but normally not all these
       meshes have to be tested */
    mesh_max = tmp_mesh * 256;
    /* avoid using more than 1 GB of FFT arrays (per default, see config.hpp) */
    if (mesh_max > P3M_MAX_MESH)
      mesh_max = P3M_MAX_MESH;
  } else {
    tmp_mesh = mesh_max = dp3m.params.mesh[0];

    sprintf(b, "fixed mesh %d\n", dp3m.params.mesh[0]);
    *logger = strcat_alloc(*logger, b);
  }

  if (dp3m.params.r_cut_iL == 0.0) {
    auto const min_box_l = *boost::min_element(box_geo.length());
    auto const min_local_box_l = *boost::min_element(local_geo.length());

    r_cut_iL_min = 0;
    r_cut_iL_max = std::min(min_local_box_l, min_box_l / 2) - skin;
    r_cut_iL_min *= (1. / box_geo.length()[0]);
    r_cut_iL_max *= (1. / box_geo.length()[0]);
  } else {
    r_cut_iL_min = r_cut_iL_max = dp3m.params.r_cut_iL;

    sprintf(b, "fixed r_cut_iL %f\n", dp3m.params.r_cut_iL);
    *logger = strcat_alloc(*logger, b);
  }

  if (dp3m.params.cao == 0) {
    cao_min = 1;
    cao_max = 7;
    cao = 3;
  } else {
    cao_min = cao_max = cao = dp3m.params.cao;

    sprintf(b, "fixed cao %d\n", dp3m.params.cao);
    *logger = strcat_alloc(*logger, b);
  }
  *logger = strcat_alloc(*logger, "Dmesh cao Dr_cut_iL   Dalpha_L     Derr     "
                                  "    Drs_err    Dks_err    time [ms]\n");

  /* mesh loop */
  for (; tmp_mesh <= mesh_max; tmp_mesh += 2) {
    tmp_cao = cao;
    tmp_time =
        dp3m_m_time(logger, tmp_mesh, cao_min, cao_max, &tmp_cao, r_cut_iL_min,
                    r_cut_iL_max, &tmp_r_cut_iL, &tmp_alpha_L, &tmp_accuracy);
    /* some error occurred during the tuning force evaluation */
    if (tmp_time == -1)
      return ES_ERROR;
    /* this mesh does not work at all */
    if (tmp_time < 0)
      continue;

    /* the optimum r_cut for this mesh is the upper limit for higher meshes,
       everything else is slower */
    r_cut_iL_max = tmp_r_cut_iL;

    /* new optimum */
    if (tmp_time < time_best) {
      time_best = tmp_time;
      mesh = tmp_mesh;
      cao = tmp_cao;
      r_cut_iL = tmp_r_cut_iL;
      alpha_L = tmp_alpha_L;
      accuracy = tmp_accuracy;
    }
    /* no hope of further optimisation */
    else if (tmp_time > time_best + P3M_TIME_GRAN)
      break;
  }

  P3M_TRACE(fprintf(stderr, "finished tuning\n"));
  if (time_best == 1e20) {
    *logger = strcat_alloc(
        *logger,
        "failed to tune dipolar P3M parameters to required accuracy\n");
    return ES_ERROR;
  }

  /* set tuned p3m parameters */
  dp3m.params.r_cut_iL = r_cut_iL;
  dp3m.params.mesh[0] = dp3m.params.mesh[1] = dp3m.params.mesh[2] = mesh;
  dp3m.params.cao = cao;
  dp3m.params.alpha_L = alpha_L;
  dp3m.params.accuracy = accuracy;
  dp3m_scaleby_box_l();
  /* broadcast tuned p3m parameters */
  mpi_bcast_coulomb_params();
  /* Tell the user about the outcome */
  sprintf(b,
          "\nresulting parameters: mesh: %d, cao: %d, r_cut_iL: %.4e,"
          "\n                      alpha_L: %.4e, accuracy: %.4e, time: %.0f\n",
          mesh, cao, r_cut_iL, alpha_L, accuracy, time_best);
  *logger = strcat_alloc(*logger, b);
  return ES_OK;
}

/*****************************************************************************/

void p3m_print_dp3m_struct(P3MParameters ps) {
  fprintf(stderr, "%d: dipolar P3MParameters:\n", this_node);
  fprintf(stderr, "   alpha_L=%f, r_cut_iL=%f\n", ps.alpha_L, ps.r_cut_iL);
  fprintf(stderr, "   mesh=(%d,%d,%d), mesh_off=(%.4f,%.4f,%.4f)\n", ps.mesh[0],
          ps.mesh[1], ps.mesh[2], ps.mesh_off[0], ps.mesh_off[1],
          ps.mesh_off[2]);
  fprintf(stderr, "   Dcao=%d, Dinter=%d, Depsilon=%f\n", ps.cao, ps.inter,
          ps.epsilon);
  fprintf(stderr, "   Dcao_cut=(%f,%f,%f)\n", ps.cao_cut[0], ps.cao_cut[1],
          ps.cao_cut[2]);
  fprintf(stderr, "   Da=(%f,%f,%f), Dai=(%f,%f,%f)\n", ps.a[0], ps.a[1],
          ps.a[2], ps.ai[0], ps.ai[1], ps.ai[2]);
}

/*****************************************************************************/

void dp3m_count_magnetic_particles() {
  double node_sums[2], tot_sums[2];

  for (int i = 0; i < 2; i++) {
    node_sums[i] = 0.0;
    tot_sums[i] = 0.0;
  }

  for (auto const &p : local_cells.particles()) {
    if (p.p.dipm != 0.0) {
      node_sums[0] += p.calc_dip().norm2();
      node_sums[1] += 1.0;
    }
  }

  MPI_Allreduce(node_sums, tot_sums, 2, MPI_DOUBLE, MPI_SUM, comm_cart);
  dp3m.sum_mu2 = tot_sums[0];
  dp3m.sum_dip_part = (int)(tot_sums[1] + 0.1);
}

REGISTER_CALLBACK(dp3m_count_magnetic_particles)

/*****************************************************************************/

/* Following are the two functions for computing the error in dipolar P3M and
   tune the parameters to minimize the time with the desired accuracy.


   This functions are called by the functions: dp3m_get_accuracy().

*/

/*****************************************************************************/

static double dp3m_k_space_error(double box_size, double prefac, int mesh,
                                 int cao, int n_c_part, double sum_q2,
                                 double alpha_L) {
  int nx, ny, nz;
  double he_q = 0.0, mesh_i = 1. / mesh, alpha_L_i = 1. / alpha_L;
  double alias1, alias2, n2, cs;

  for (nx = -mesh / 2; nx < mesh / 2; nx++)
    for (ny = -mesh / 2; ny < mesh / 2; ny++)
      for (nz = -mesh / 2; nz < mesh / 2; nz++)
        if ((nx != 0) || (ny != 0) || (nz != 0)) {
          n2 = Utils::sqr(nx) + Utils::sqr(ny) + Utils::sqr(nz);
          cs = p3m_analytic_cotangent_sum(nx, mesh_i, cao) *
               p3m_analytic_cotangent_sum(ny, mesh_i, cao) *
               p3m_analytic_cotangent_sum(nz, mesh_i, cao);
          dp3m_tune_aliasing_sums(nx, ny, nz, mesh, mesh_i, cao, alpha_L_i,
                                  &alias1, &alias2);
          double d = alias1 - Utils::sqr(alias2 / cs) / (n2 * n2 * n2);
          /* at high precisions, d can become negative due to extinction;
             also, don't take values that have no significant digits left*/
          if (d > 0 && (fabs(d / alias1) > ROUND_ERROR_PREC))
            he_q += d;
        }

  return 8. * Utils::pi() * Utils::pi() / 3. * sum_q2 *
         sqrt(he_q / (double)n_c_part) /
         (box_size * box_size * box_size * box_size);
}

void dp3m_tune_aliasing_sums(int nx, int ny, int nz, int mesh, double mesh_i,
                             int cao, double alpha_L_i, double *alias1,
                             double *alias2) {
  int mx, my, mz;
  double nmx, nmy, nmz;
  double fnmx, fnmy, fnmz;

  double ex, ex2, nm2, U2, factor1;

  factor1 = Utils::sqr(Utils::pi() * alpha_L_i);

  *alias1 = *alias2 = 0.0;
  for (mx = -P3M_BRILLOUIN; mx <= P3M_BRILLOUIN; mx++) {
    fnmx = mesh_i * (nmx = nx + mx * mesh);
    for (my = -P3M_BRILLOUIN; my <= P3M_BRILLOUIN; my++) {
      fnmy = mesh_i * (nmy = ny + my * mesh);
      for (mz = -P3M_BRILLOUIN; mz <= P3M_BRILLOUIN; mz++) {
        fnmz = mesh_i * (nmz = nz + mz * mesh);

        nm2 = Utils::sqr(nmx) + Utils::sqr(nmy) + Utils::sqr(nmz);
        ex2 = Utils::sqr(ex = exp(-factor1 * nm2));

        U2 = pow(sinc(fnmx) * sinc(fnmy) * sinc(fnmz), 2.0 * cao);

        *alias1 += ex2 * nm2;
        *alias2 += U2 * ex * pow((nx * nmx + ny * nmy + nz * nmz), 3.) / nm2;
      }
    }
  }
}

//----------------------------------------------------------
//  Function used to calculate the value of the errors
//  for the REAL part of the force in terms of the splitting parameter alpha of
//  Ewald
//  based on the formulas 33, the paper of Zuowei-HolmJCP, 115,6351,(2001).

//  Please, notice than in this more refined approach we don't use
//  formulas 37, but 33 which maintains all the powers in alpha

//------------------------------------------------------------

double P3M_DIPOLAR_real_space_error(double box_size, double prefac,
                                    double r_cut_iL, int n_c_part,
                                    double sum_q2, double alpha_L) {
  double d_error_f, d_cc, d_dc, d_rcut2, d_con;
  double d_a2, d_c, d_RCUT;

  d_RCUT = r_cut_iL * box_size;
  d_rcut2 = d_RCUT * d_RCUT;

  d_a2 = alpha_L * alpha_L / (box_size * box_size);

  d_c = sum_q2 * exp(-d_a2 * d_RCUT * d_RCUT);

  d_cc = 4.0 * d_a2 * d_a2 * d_rcut2 * d_rcut2 + 6.0 * d_a2 * d_rcut2 + 3.0;

  d_dc = 8.0 * d_a2 * d_a2 * d_a2 * d_rcut2 * d_rcut2 * d_rcut2 +
         20.0 * d_a2 * d_a2 * d_rcut2 * d_rcut2 + 30 * d_a2 * d_rcut2 + 15.0;

  d_con = 1.0 / sqrt(box_size * box_size * box_size * d_a2 * d_a2 * d_rcut2 *
                     d_rcut2 * d_rcut2 * d_rcut2 * d_RCUT * (double)n_c_part);

  d_error_f = d_c * d_con *
              sqrt((13. / 6.) * d_cc * d_cc + (2. / 15.) * d_dc * d_dc -
                   (13. / 15.) * d_cc * d_dc);

  return d_error_f;
}

/*****************************************************************************/

// Using bisection find the root of a function "func-tuned_accuracy/sqrt(2.)"
// known to lie between x1 and x2. The root, returned as rtbis, will be refined
// until its accuracy is +-xacc.

double dp3m_rtbisection(double box_size, double prefac, double r_cut_iL,
                        int n_c_part, double sum_q2, double x1, double x2,
                        double xacc, double tuned_accuracy) {
  int j;
  double dx, f, fmid, xmid, rtb, constant, JJ_RTBIS_MAX = 40;

  constant = tuned_accuracy / sqrt(2.);

  f = P3M_DIPOLAR_real_space_error(box_size, prefac, r_cut_iL, n_c_part, sum_q2,
                                   x1) -
      constant;
  fmid = P3M_DIPOLAR_real_space_error(box_size, prefac, r_cut_iL, n_c_part,
                                      sum_q2, x2) -
         constant;
  if (f * fmid >= 0.0)
    fprintf(stderr,
            "Root must be bracketed for bisection in dp3m_rtbisection\n");
  rtb = f < 0.0 ? (dx = x2 - x1, x1)
                : (dx = x1 - x2,
                   x2); // Orient the search dx, and set rtb to x1 or x2 ...
  for (j = 1; j <= JJ_RTBIS_MAX; j++) {
    fmid = P3M_DIPOLAR_real_space_error(box_size, prefac, r_cut_iL, n_c_part,
                                        sum_q2, xmid = rtb + (dx *= 0.5)) -
           constant;
    if (fmid <= 0.0)
      rtb = xmid;
    if (fabs(dx) < xacc || fmid == 0.0)
      return rtb;
  }
  fprintf(stderr, "Too many bisections in JJ_rtbissection\n");
  return -9999999.9999;
}

/************************************************************/

void dp3m_calc_lm_ld_pos() {
  int i;
  for (i = 0; i < 3; i++) {
    dp3m.local_mesh.ld_pos[i] =
        (dp3m.local_mesh.ld_ind[i] + dp3m.params.mesh_off[i]) *
        dp3m.params.a[i];
  }
}

/************************************************************/

void dp3m_init_a_ai_cao_cut() {
  int i;
  for (i = 0; i < 3; i++) {
    dp3m.params.ai[i] = (double)dp3m.params.mesh[i] / box_geo.length()[i];
    dp3m.params.a[i] = 1.0 / dp3m.params.ai[i];
    dp3m.params.cao_cut[i] = 0.5 * dp3m.params.a[i] * dp3m.params.cao;
  }
}

/************************************************************/

void dp3m_calc_local_ca_mesh() {
  int i;
  int ind[3];
  /* total skin size */
  double full_skin[3];

  for (i = 0; i < 3; i++)
    full_skin[i] =
        dp3m.params.cao_cut[i] + skin + dp3m.params.additional_mesh[i];

  /* inner left down grid point (global index) */
  for (i = 0; i < 3; i++)
    dp3m.local_mesh.in_ld[i] = (int)ceil(
        local_geo.my_left()[i] * dp3m.params.ai[i] - dp3m.params.mesh_off[i]);
  /* inner up right grid point (global index) */
  for (i = 0; i < 3; i++)
    dp3m.local_mesh.in_ur[i] = (int)floor(
        local_geo.my_right()[i] * dp3m.params.ai[i] - dp3m.params.mesh_off[i]);

  /* correct roundof errors at boundary */
  for (i = 0; i < 3; i++) {
    if ((local_geo.my_right()[i] * dp3m.params.ai[i] -
         dp3m.params.mesh_off[i]) -
            dp3m.local_mesh.in_ur[i] <
        ROUND_ERROR_PREC)
      dp3m.local_mesh.in_ur[i]--;
    if (1.0 +
            (local_geo.my_left()[i] * dp3m.params.ai[i] -
             dp3m.params.mesh_off[i]) -
            dp3m.local_mesh.in_ld[i] <
        ROUND_ERROR_PREC)
      dp3m.local_mesh.in_ld[i]--;
  }
  /* inner grid dimensions */
  for (i = 0; i < 3; i++)
    dp3m.local_mesh.inner[i] =
        dp3m.local_mesh.in_ur[i] - dp3m.local_mesh.in_ld[i] + 1;
  /* index of left down grid point in global mesh */
  for (i = 0; i < 3; i++)
    dp3m.local_mesh.ld_ind[i] =
        (int)ceil((local_geo.my_left()[i] - full_skin[i]) * dp3m.params.ai[i] -
                  dp3m.params.mesh_off[i]);
  /* spacial position of left down mesh point */
  dp3m_calc_lm_ld_pos();
  /* left down margin */
  for (i = 0; i < 3; i++)
    dp3m.local_mesh.margin[i * 2] =
        dp3m.local_mesh.in_ld[i] - dp3m.local_mesh.ld_ind[i];
  /* up right grid point */
  for (i = 0; i < 3; i++)
    ind[i] = (int)floor((local_geo.my_right()[i] + full_skin[i]) *
                            dp3m.params.ai[i] -
                        dp3m.params.mesh_off[i]);
  /* correct roundof errors at up right boundary */
  for (i = 0; i < 3; i++)
    if (((local_geo.my_right()[i] + full_skin[i]) * dp3m.params.ai[i] -
         dp3m.params.mesh_off[i]) -
            ind[i] ==
        0)
      ind[i]--;
  /* up right margin */
  for (i = 0; i < 3; i++)
    dp3m.local_mesh.margin[(i * 2) + 1] = ind[i] - dp3m.local_mesh.in_ur[i];

  /* grid dimension */
  dp3m.local_mesh.size = 1;
  for (i = 0; i < 3; i++) {
    dp3m.local_mesh.dim[i] = ind[i] - dp3m.local_mesh.ld_ind[i] + 1;
    dp3m.local_mesh.size *= dp3m.local_mesh.dim[i];
  }
  /* reduce inner grid indices from global to local */
  for (i = 0; i < 3; i++)
    dp3m.local_mesh.in_ld[i] = dp3m.local_mesh.margin[i * 2];
  for (i = 0; i < 3; i++)
    dp3m.local_mesh.in_ur[i] =
        dp3m.local_mesh.margin[i * 2] + dp3m.local_mesh.inner[i];

  dp3m.local_mesh.q_2_off = dp3m.local_mesh.dim[2] - dp3m.params.cao;
  dp3m.local_mesh.q_21_off =
      dp3m.local_mesh.dim[2] * (dp3m.local_mesh.dim[1] - dp3m.params.cao);
}

/*****************************************************************************/

bool dp3m_sanity_checks_boxl() {
  int i;
  bool ret = false;
  for (i = 0; i < 3; i++) {
    /* check k-space cutoff */
    if (dp3m.params.cao_cut[i] >= 0.5 * box_geo.length()[i]) {
      runtimeErrorMsg() << "dipolar P3M_init: k-space cutoff "
                        << dp3m.params.cao_cut[i]
                        << " is larger than half of box dimension "
                        << box_geo.length()[i];
      ret = true;
    }
    if (dp3m.params.cao_cut[i] >= local_geo.length()[i]) {
      runtimeErrorMsg() << "dipolar P3M_init: k-space cutoff "
                        << dp3m.params.cao_cut[i]
                        << " is larger than local box dimension "
                        << local_geo.length()[i];
      ret = true;
    }
  }
  return ret;
}

/*****************************************************************************/

bool dp3m_sanity_checks(const Utils::Vector3i &grid) {
  bool ret = false;

  if (!box_geo.periodic(0) || !box_geo.periodic(1) || !box_geo.periodic(2)) {
    runtimeErrorMsg() << "dipolar P3M requires periodicity 1 1 1";
    ret = true;
  }
  /*
  if (n_nodes != 1) {
      runtimeErrorMsg() <<"dipolar P3M does not run in parallel";
    ret = true;
  } */
  if (cell_structure.type != CELL_STRUCTURE_DOMDEC) {
    runtimeErrorMsg() << "dipolar P3M at present requires the domain "
                         "decomposition cell system";
    ret = true;
  }

  if ((box_geo.length()[0] != box_geo.length()[1]) ||
      (box_geo.length()[1] != box_geo.length()[2])) {
    runtimeErrorMsg() << "dipolar P3M requires a cubic box";
    ret = true;
  }

  if ((dp3m.params.mesh[0] != dp3m.params.mesh[1]) ||
      (dp3m.params.mesh[1] != dp3m.params.mesh[2])) {
    runtimeErrorMsg() << "dipolar P3M requires a cubic mesh";
    ret = true;
  }

  if (dp3m_sanity_checks_boxl())
    ret = true;

  if (dp3m.params.mesh[0] == 0) {
    runtimeErrorMsg() << "dipolar P3M_init: mesh size is not yet set";
    ret = true;
  }
  if (dp3m.params.cao == 0) {
    runtimeErrorMsg() << "dipolar P3M_init: cao is not yet set";
    ret = true;
  }
  if (grid[0] < grid[1] || grid[1] < grid[2]) {
    runtimeErrorMsg()
        << "dipolar P3M_init: node grid must be sorted, largest first";
    ret = true;
  }

  return ret;
}

/*****************************************************************************/

void dp3m_calc_send_mesh() {
  int i, j, evenodd;
  int done[3] = {0, 0, 0};
  MPI_Status status;
  /* send grids */
  for (i = 0; i < 3; i++) {
    for (j = 0; j < 3; j++) {
      /* left */
      dp3m.sm.s_ld[i * 2][j] = 0 + done[j] * dp3m.local_mesh.margin[j * 2];
      if (j == i)
        dp3m.sm.s_ur[i * 2][j] = dp3m.local_mesh.margin[j * 2];
      else
        dp3m.sm.s_ur[i * 2][j] = dp3m.local_mesh.dim[j] -
                                 done[j] * dp3m.local_mesh.margin[(j * 2) + 1];
      /* right */
      if (j == i)
        dp3m.sm.s_ld[(i * 2) + 1][j] = dp3m.local_mesh.in_ur[j];
      else
        dp3m.sm.s_ld[(i * 2) + 1][j] =
            0 + done[j] * dp3m.local_mesh.margin[j * 2];
      dp3m.sm.s_ur[(i * 2) + 1][j] =
          dp3m.local_mesh.dim[j] -
          done[j] * dp3m.local_mesh.margin[(j * 2) + 1];
    }
    done[i] = 1;
  }
  dp3m.sm.max = 0;
  for (i = 0; i < 6; i++) {
    dp3m.sm.s_size[i] = 1;
    for (j = 0; j < 3; j++) {
      dp3m.sm.s_dim[i][j] = dp3m.sm.s_ur[i][j] - dp3m.sm.s_ld[i][j];
      dp3m.sm.s_size[i] *= dp3m.sm.s_dim[i][j];
    }
    if (dp3m.sm.s_size[i] > dp3m.sm.max)
      dp3m.sm.max = dp3m.sm.s_size[i];
  }
  /* communication */
  auto const node_neighbors = calc_node_neighbors(comm_cart);
  auto const node_pos = calc_node_pos(comm_cart);

  for (i = 0; i < 6; i++) {
    if (i % 2 == 0)
      j = i + 1;
    else
      j = i - 1;
    if (node_neighbors[i] != this_node) {
      /* two step communication: first all even positions than all odd */
      for (evenodd = 0; evenodd < 2; evenodd++) {
        if ((node_pos[i / 2] + evenodd) % 2 == 0)
          MPI_Send(&(dp3m.local_mesh.margin[i]), 1, MPI_INT, node_neighbors[i],
                   REQ_P3M_INIT_D, comm_cart);
        else
          MPI_Recv(&(dp3m.local_mesh.r_margin[j]), 1, MPI_INT,
                   node_neighbors[j], REQ_P3M_INIT_D, comm_cart, &status);
      }
    } else {
      dp3m.local_mesh.r_margin[j] = dp3m.local_mesh.margin[i];
    }
  }
  /* recv grids */
  for (i = 0; i < 3; i++)
    for (j = 0; j < 3; j++) {
      if (j == i) {
        dp3m.sm.r_ld[i * 2][j] =
            dp3m.sm.s_ld[i * 2][j] + dp3m.local_mesh.margin[2 * j];
        dp3m.sm.r_ur[i * 2][j] =
            dp3m.sm.s_ur[i * 2][j] + dp3m.local_mesh.r_margin[2 * j];
        dp3m.sm.r_ld[(i * 2) + 1][j] = dp3m.sm.s_ld[(i * 2) + 1][j] -
                                       dp3m.local_mesh.r_margin[(2 * j) + 1];
        dp3m.sm.r_ur[(i * 2) + 1][j] =
            dp3m.sm.s_ur[(i * 2) + 1][j] - dp3m.local_mesh.margin[(2 * j) + 1];
      } else {
        dp3m.sm.r_ld[i * 2][j] = dp3m.sm.s_ld[i * 2][j];
        dp3m.sm.r_ur[i * 2][j] = dp3m.sm.s_ur[i * 2][j];
        dp3m.sm.r_ld[(i * 2) + 1][j] = dp3m.sm.s_ld[(i * 2) + 1][j];
        dp3m.sm.r_ur[(i * 2) + 1][j] = dp3m.sm.s_ur[(i * 2) + 1][j];
      }
    }
  for (i = 0; i < 6; i++) {
    dp3m.sm.r_size[i] = 1;
    for (j = 0; j < 3; j++) {
      dp3m.sm.r_dim[i][j] = dp3m.sm.r_ur[i][j] - dp3m.sm.r_ld[i][j];
      dp3m.sm.r_size[i] *= dp3m.sm.r_dim[i][j];
    }
    if (dp3m.sm.r_size[i] > dp3m.sm.max)
      dp3m.sm.max = dp3m.sm.r_size[i];
  }
}

/************************************************/

void dp3m_scaleby_box_l() {
  if (dipole.prefactor < 0.0) {
    runtimeErrorMsg() << "Dipolar prefactor has to be >=0";
    return;
  }

  dp3m.params.r_cut = dp3m.params.r_cut_iL * box_geo.length()[0];
  dp3m.params.alpha = dp3m.params.alpha_L * (1. / box_geo.length()[0]);
  dp3m_init_a_ai_cao_cut();
  dp3m_calc_lm_ld_pos();
  dp3m_sanity_checks_boxl();

  dp3m_calc_influence_function_force();
  dp3m_calc_influence_function_energy();
}

/*****************************************************************************/

/** function to give the dipolar-P3M energy correction */
void dp3m_compute_constants_energy_dipolar() {
  double Eself, Ukp3m;

  if (dp3m.energy_correction != 0.0)
    return;

  P3M_TRACE(fprintf(stderr, "%d: dp3m_compute_constants_energy_dipolar().\n",
                    this_node));

  double volume =
      box_geo.length()[0] * box_geo.length()[1] * box_geo.length()[2];
  Ukp3m = dp3m_average_dipolar_self_energy(box_geo.length()[0],
                                           dp3m.params.mesh[0]) *
          volume;

  P3M_TRACE(
      fprintf(stderr, "%d: Average Dipolar Energy = %lf.\n", this_node, Ukp3m));

  Eself = -(2 * pow(dp3m.params.alpha_L, 3) * Utils::sqrt_pi_i() / 3.0);

  dp3m.energy_correction =
      -dp3m.sum_mu2 * (Ukp3m + Eself + 2. * Utils::pi() / 3.);
}

/*****************************************************************************/

/*****************************************************************************/

#endif /* DP3M */
