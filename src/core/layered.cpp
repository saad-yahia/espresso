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
 *  Implementation of layered.hpp.
 */
#include "layered.hpp"
#include "cells.hpp"
#include "communication.hpp"
#include "constraints.hpp"
#include "debug.hpp"
#include "domain_decomposition.hpp"
#include "errorhandling.hpp"
#include "ghosts.hpp"
#include "global.hpp"

#include <cstring>
#include <mpi.h>

/* Organization: Layers only in one direction.
   ghost_bottom
   c1
   c2
   c3
   .
   .
   .
   cn
   ghost_top

   First, all nodes send downwards, then upwards. Within these actions,
   first the odd nodes send. For even n_nodes, this algorithm is straight
   forward: first the odd nodes send, the even receive, then vice versa.
   For odd n_nodes, we have
   1) 1->2 3->4 5->1
   2) 2->3 4->5
   So in the first round node 5 has to wait for node 1 to
   complete the send and get ready to receive. In other words,
   what physically happens is:
   1) 1->2 3->4 5->*
   2) *->1 2->3 4->*
   3) *->5
   This means that one pending receive has to be done in addition
   provided that all other transmissions can happen in parallel.

*/

/** whether we are the lowest node */
#define LAYERED_BOTTOM 1
/** whether we are the highest node */
#define LAYERED_TOP 2
/** same as PERIODIC(2) */
#define LAYERED_PERIODIC 4
#define LAYERED_BTM_MASK (LAYERED_BOTTOM | LAYERED_PERIODIC)
#define LAYERED_TOP_MASK (LAYERED_TOP | LAYERED_PERIODIC)
/** node has a neighbor above (modulo n_nodes) */
#define LAYERED_TOP_NEIGHBOR ((layered_flags & LAYERED_TOP_MASK) != LAYERED_TOP)
/** node has a neighbor below (modulo n_nodes) */
#define LAYERED_BTM_NEIGHBOR                                                   \
  ((layered_flags & LAYERED_BTM_MASK) != LAYERED_BOTTOM)

int layered_flags = 0;
int n_layers = -1, determine_n_layers = 1;
double layer_h = 0, layer_h_i = 0;

static int btm, top;

Cell *layered_position_to_cell(const Utils::Vector3d &pos) {
  int cpos = static_cast<int>(
                 std::floor((pos[2] - local_geo.my_left()[2]) * layer_h_i)) +
             1;
  if (cpos < 1) {
    if (!LAYERED_BTM_NEIGHBOR)
      cpos = 1;
    else
      return nullptr;
  } else if (cpos > n_layers) {
    /* not periodic, but at top */
    if (!LAYERED_TOP_NEIGHBOR)
      cpos = n_layers;
    else
      return nullptr;
  }
  return &(cells[cpos]);
}

void layered_topology_release() {
  CELL_TRACE(fprintf(stderr, "%d: layered_topology_release:\n", this_node));
  free_comm(&cell_structure.ghost_cells_comm);
  free_comm(&cell_structure.exchange_ghosts_comm);
  free_comm(&cell_structure.update_ghost_pos_comm);
  free_comm(&cell_structure.collect_ghost_force_comm);
}

static void layered_prepare_comm(GhostCommunicator *comm, int data_parts) {
  int c, n;

  if (n_nodes > 1) {
    /* more than one node => no local transfers */

    /* how many communications to do: up even/odd, down even/odd */
    n = 4;
    /* one communication missing if not periodic but on border */
    if (!LAYERED_TOP_NEIGHBOR)
      n -= 2;
    if (!LAYERED_BTM_NEIGHBOR)
      n -= 2;

    prepare_comm(comm, data_parts, n);

    /* always sending/receiving 1 cell per time step */
    for (c = 0; c < n; c++) {
      comm->comm[c].part_lists = (Cell **)Utils::malloc(sizeof(Cell *));
      comm->comm[c].n_part_lists = 1;
      comm->comm[c].mpi_comm = comm_cart;
    }

    c = 0;

    CELL_TRACE(
        fprintf(stderr, "%d: ghostrec new comm of size %d\n", this_node, n));
    /* downwards */
    for (int even_odd = 0; even_odd < 2; even_odd++) {
      /* send */
      if (this_node % 2 == even_odd && LAYERED_BTM_NEIGHBOR) {
        comm->comm[c].type = GHOST_SEND;
        /* round 1 uses prefetched data and stores delayed data */
        if (c == 1)
          comm->comm[c].type |= GHOST_PREFETCH | GHOST_PSTSTORE;
        comm->comm[c].node = btm;
        if (data_parts == GHOSTTRANS_FORCE) {
          comm->comm[c].part_lists[0] = &cells[0];
          CELL_TRACE(fprintf(stderr, "%d: ghostrec send force to %d btmg\n",
                             this_node, btm));
        } else {
          comm->comm[c].part_lists[0] = &cells[1];

          /* if periodic and bottom or top, send shifted */
          comm->comm[c].shift[0] = comm->comm[c].shift[1] = 0;
          if (((layered_flags & LAYERED_BTM_MASK) == LAYERED_BTM_MASK) &&
              (data_parts & GHOSTTRANS_POSITION)) {
            comm->data_parts |= GHOSTTRANS_POSSHFTD;
            comm->comm[c].shift[2] = box_geo.length()[2];
          } else
            comm->comm[c].shift[2] = 0;
          CELL_TRACE(fprintf(stderr, "%d: ghostrec send to %d shift %f btml\n",
                             this_node, btm, comm->comm[c].shift[2]));
        }
        c++;
      }
      /* recv. Note we test r_node as we always have to test the sender
         as for odd n_nodes maybe we send AND receive. */
      if (top % 2 == even_odd && LAYERED_TOP_NEIGHBOR) {
        comm->comm[c].type = GHOST_RECV;
        /* round 0 prefetch send for round 1 and delay recvd data processing */
        if (c == 0)
          comm->comm[c].type |= GHOST_PREFETCH | GHOST_PSTSTORE;
        comm->comm[c].node = top;
        if (data_parts == GHOSTTRANS_FORCE) {
          comm->comm[c].part_lists[0] = &cells[n_layers];
          CELL_TRACE(fprintf(stderr, "%d: ghostrec get force from %d topl\n",
                             this_node, top));
        } else {
          comm->comm[c].part_lists[0] = &cells[n_layers + 1];
          CELL_TRACE(fprintf(stderr, "%d: ghostrec recv from %d topg\n",
                             this_node, top));
        }
        c++;
      }
    }

    CELL_TRACE(fprintf(stderr, "%d: ghostrec upwards\n", this_node));
    /* upwards */
    for (int even_odd = 0; even_odd < 2; even_odd++) {
      /* send */
      if (this_node % 2 == even_odd && LAYERED_TOP_NEIGHBOR) {
        comm->comm[c].type = GHOST_SEND;
        /* round 1 use prefetched data from round 0.
           But this time there may already have been two transfers downwards */
        if (c % 2 == 1)
          comm->comm[c].type |= GHOST_PREFETCH | GHOST_PSTSTORE;
        comm->comm[c].node = top;
        if (data_parts == GHOSTTRANS_FORCE) {
          comm->comm[c].part_lists[0] = &cells[n_layers + 1];
          CELL_TRACE(fprintf(stderr, "%d: ghostrec send force to %d topg\n",
                             this_node, top));
        } else {
          comm->comm[c].part_lists[0] = &cells[n_layers];

          /* if periodic and bottom or top, send shifted */
          comm->comm[c].shift[0] = comm->comm[c].shift[1] = 0;
          if (((layered_flags & LAYERED_TOP_MASK) == LAYERED_TOP_MASK) &&
              (data_parts & GHOSTTRANS_POSITION)) {
            comm->data_parts |= GHOSTTRANS_POSSHFTD;
            comm->comm[c].shift[2] = -box_geo.length()[2];
          } else
            comm->comm[c].shift[2] = 0;
          CELL_TRACE(fprintf(stderr, "%d: ghostrec send to %d shift %f topl\n",
                             this_node, top, comm->comm[c].shift[2]));
        }
        c++;
      }
      /* recv. Note we test r_node as we always have to test the sender
         as for odd n_nodes maybe we send AND receive. */
      if (btm % 2 == even_odd && LAYERED_BTM_NEIGHBOR) {
        comm->comm[c].type = GHOST_RECV;
        /* round 0 prefetch. But this time there may already have been two
         * transfers downwards */
        if (c % 2 == 0)
          comm->comm[c].type |= GHOST_PREFETCH | GHOST_PSTSTORE;
        comm->comm[c].node = btm;
        if (data_parts == GHOSTTRANS_FORCE) {
          comm->comm[c].part_lists[0] = &cells[1];
          CELL_TRACE(fprintf(stderr, "%d: ghostrec get force from %d btml\n",
                             this_node, btm));
        } else {
          comm->comm[c].part_lists[0] = &cells[0];
          CELL_TRACE(fprintf(stderr, "%d: ghostrec recv from %d btmg\n",
                             this_node, btm));
        }
        c++;
      }
    }
  } else {
    /* one node => local transfers, either 2 (up and down, periodic) or zero*/

    n = (layered_flags & LAYERED_PERIODIC) ? 2 : 0;

    prepare_comm(comm, data_parts, n);

    if (n != 0) {
      /* two cells: from and to */
      for (c = 0; c < n; c++) {
        comm->comm[c].part_lists = (Cell **)Utils::malloc(2 * sizeof(Cell *));
        comm->comm[c].n_part_lists = 2;
        comm->comm[c].mpi_comm = comm_cart;
        comm->comm[c].node = this_node;
      }

      c = 0;

      /* downwards */
      comm->comm[c].type = GHOST_LOCL;
      if (data_parts == GHOSTTRANS_FORCE) {
        comm->comm[c].part_lists[0] = &cells[0];
        comm->comm[c].part_lists[1] = &cells[n_layers];
      } else {
        comm->comm[c].part_lists[0] = &cells[1];
        comm->comm[c].part_lists[1] = &cells[n_layers + 1];
        /* here it is periodic */
        if (data_parts & GHOSTTRANS_POSITION)
          comm->data_parts |= GHOSTTRANS_POSSHFTD;
        comm->comm[c].shift[0] = comm->comm[c].shift[1] = 0;
        comm->comm[c].shift[2] = box_geo.length()[2];
      }
      c++;

      /* upwards */
      comm->comm[c].type = GHOST_LOCL;
      if (data_parts == GHOSTTRANS_FORCE) {
        comm->comm[c].part_lists[0] = &cells[n_layers + 1];
        comm->comm[c].part_lists[1] = &cells[1];
      } else {
        comm->comm[c].part_lists[0] = &cells[n_layers];
        comm->comm[c].part_lists[1] = &cells[0];
        /* here it is periodic */
        if (data_parts & GHOSTTRANS_POSITION)
          comm->data_parts |= GHOSTTRANS_POSSHFTD;
        comm->comm[c].shift[0] = comm->comm[c].shift[1] = 0;
        comm->comm[c].shift[2] = -box_geo.length()[2];
      }
    }
  }
}

void layered_topology_init(CellPList *old, Utils::Vector3i &grid,
                           const double range) {
  int c, p;

  cell_structure.type = CELL_STRUCTURE_LAYERED;
  cell_structure.particle_to_cell = [](const Particle &p) {
    return layered_position_to_cell(p.r.p);
  };

  /* check node grid. All we can do is 1x1xn. */
  if (grid[0] != 1 || grid[1] != 1) {
    runtimeErrorMsg() << "selected node grid is not suitable for layered cell "
                         "structure (needs 1x1x"
                      << n_nodes << " grid";
    grid[0] = grid[1] = 1;
    grid[2] = n_nodes;
  }

  if (this_node == 0 && determine_n_layers) {
    if (range > 0) {
      n_layers = (int)floor(local_geo.length()[2] / range);
      if (n_layers < 1) {
        runtimeErrorMsg() << "layered: maximal interaction range " << range
                          << " larger than local box length "
                          << local_geo.length()[2];
        n_layers = 1;
      }
      if (n_layers > max_num_cells - 2)
        n_layers = std::max(max_num_cells - 2, 0);
    } else
      n_layers = 1;
  }
  MPI_Bcast(&n_layers, 1, MPI_INT, 0, comm_cart);

  /* check whether node is top and/or bottom */
  layered_flags = 0;
  if (this_node == 0)
    layered_flags |= LAYERED_BOTTOM;
  if (this_node == n_nodes - 1)
    layered_flags |= LAYERED_TOP;

  if (box_geo.periodic(2))
    layered_flags |= LAYERED_PERIODIC;

  top = this_node + 1;
  if ((top == n_nodes) && (layered_flags & LAYERED_PERIODIC))
    top = 0;
  btm = this_node - 1;
  if ((btm == -1) && (layered_flags & LAYERED_PERIODIC))
    btm = n_nodes - 1;

  layer_h = local_geo.length()[2] / (double)(n_layers);
  layer_h_i = 1 / layer_h;

  cell_structure.max_range = {
      box_geo.periodic(0) ? 0.5 * box_geo.length()[0]
                          : std::numeric_limits<double>::infinity(),
      box_geo.periodic(1) ? 0.5 * box_geo.length()[1]
                          : std::numeric_limits<double>::infinity(),
      layer_h};

  /* allocate cells and mark them */
  realloc_cells(n_layers + 2);
  realloc_cellplist(&local_cells, local_cells.n = n_layers);
  for (c = 1; c <= n_layers; c++) {
    Cell *red[] = {&cells[c - 1]};
    Cell *black[] = {&cells[c + 1]};

    local_cells.cell[c - 1] = &cells.at(c);
    cells[c].m_neighbors = Neighbors<Cell *>(red, black);
  }

  realloc_cellplist(&ghost_cells, ghost_cells.n = 2);
  ghost_cells.cell[0] = &cells.front();
  ghost_cells.cell[1] = &cells.back();

  /* create communicators */
  layered_prepare_comm(&cell_structure.ghost_cells_comm, GHOSTTRANS_PARTNUM);
  layered_prepare_comm(&cell_structure.exchange_ghosts_comm,
                       GHOSTTRANS_PROPRTS | GHOSTTRANS_POSITION);
  layered_prepare_comm(&cell_structure.update_ghost_pos_comm,
                       GHOSTTRANS_POSITION);
  layered_prepare_comm(&cell_structure.collect_ghost_force_comm,
                       GHOSTTRANS_FORCE);

  /* copy particles */
  for (c = 0; c < old->n; c++) {
    Particle *part = old->cell[c]->part;
    int np = old->cell[c]->n;
    for (p = 0; p < np; p++) {
      Cell *nc = layered_position_to_cell(part[p].r.p);
      /* particle does not belong to this node. Just stow away
         somewhere for the moment */
      if (nc == nullptr)
        nc = local_cells.cell[0];
      append_unindexed_particle(nc, std::move(part[p]));
    }
  }
  for (c = 1; c <= n_layers; c++)
    update_local_particles(&cells[c]);

  CELL_TRACE(fprintf(stderr, "%d: layered_topology_init done\n", this_node));
}

static void layered_append_particles(ParticleList *pl, ParticleList *up,
                                     ParticleList *dn) {
  int p;

  CELL_TRACE(fprintf(stderr, "%d: sorting in %d\n", this_node, pl->n));
  for (p = 0; p < pl->n; p++) {
    fold_position(pl->part[p].r.p, pl->part[p].l.i, box_geo);

    if (LAYERED_BTM_NEIGHBOR &&
        (get_mi_coord(pl->part[p].r.p[2], local_geo.my_left()[2],
                      box_geo.length()[2], box_geo.periodic(2)) < 0.0)) {
      CELL_TRACE(fprintf(stderr, "%d: leaving part %d for node below\n",
                         this_node, pl->part[p].p.identity));
      move_indexed_particle(dn, pl, p);
    } else if (LAYERED_TOP_NEIGHBOR &&
               (get_mi_coord(pl->part[p].r.p[2], local_geo.my_right()[2],
                             box_geo.length()[2],
                             box_geo.periodic(2)) >= 0.0)) {
      CELL_TRACE(fprintf(stderr, "%d: leaving part %d for node above\n",
                         this_node, pl->part[p].p.identity));
      move_indexed_particle(up, pl, p);
    } else
      move_indexed_particle(layered_position_to_cell(pl->part[p].r.p), pl, p);
    /* same particle again, as this is now a new one */
    if (p < pl->n)
      p--;
  }
  CELL_TRACE(fprintf(stderr, "%d: left over %d\n", this_node, pl->n));
}

void layered_exchange_and_sort_particles(int global_flag,
                                         ParticleList *displaced_parts) {
  Particle *part;
  Cell *nc, *oc;
  int c, p, flag, redo;
  ParticleList send_buf_dn, send_buf_up;
  ParticleList recv_buf_up, recv_buf_dn;

  CELL_TRACE(fprintf(stderr, "%d:layered exchange and sort %d\n", this_node,
                     global_flag));

  /* sort local particles */
  for (p = 0; p < displaced_parts->n; p++) {
    part = &displaced_parts->part[p];

    if (n_nodes != 1 && LAYERED_BTM_NEIGHBOR &&
        (get_mi_coord(part->r.p[2], local_geo.my_left()[2], box_geo.length()[2],
                      box_geo.periodic(2)) < 0.0)) {
      CELL_TRACE(fprintf(stderr, "%d: send part %d down\n", this_node,
                         part->p.identity));
      move_indexed_particle(&send_buf_dn, displaced_parts, p);
      if (p < displaced_parts->n)
        p--;
    } else if (n_nodes != 1 && LAYERED_TOP_NEIGHBOR &&
               (get_mi_coord(part->r.p[2], local_geo.my_right()[2],
                             box_geo.length()[2],
                             box_geo.periodic(2)) >= 0.0)) {
      CELL_TRACE(fprintf(stderr, "%d: send part %d up\n", this_node,
                         part->p.identity));
      move_indexed_particle(&send_buf_up, displaced_parts, p);
      if (p < displaced_parts->n)
        p--;
    }
  }

  for (;;) {
    /* transfer */
    if (n_nodes > 1) {
      if (this_node % 2 == 0) {
        /* send down */
        if (LAYERED_BTM_NEIGHBOR) {
          CELL_TRACE(
              fprintf(stderr, "%d: send dn %d\n", this_node, send_buf_dn.n));
          send_particles(&send_buf_dn, btm);
        }
        if (LAYERED_TOP_NEIGHBOR) {
          recv_particles(&recv_buf_up, top);
          CELL_TRACE(
              fprintf(stderr, "%d: recv up %d\n", this_node, recv_buf_up.n));
        }
        /* send up */
        if (LAYERED_TOP_NEIGHBOR) {
          CELL_TRACE(fprintf(stderr, "%d: send up\n", this_node));
          send_particles(&send_buf_up, top);
        }
        if (LAYERED_BTM_NEIGHBOR) {
          recv_particles(&recv_buf_dn, btm);
          CELL_TRACE(
              fprintf(stderr, "%d: recv dn %d\n", this_node, recv_buf_dn.n));
        }
      } else {
        if (LAYERED_TOP_NEIGHBOR) {
          CELL_TRACE(fprintf(stderr, "%d: recv up\n", this_node));
          recv_particles(&recv_buf_up, top);
        }
        if (LAYERED_BTM_NEIGHBOR) {
          CELL_TRACE(
              fprintf(stderr, "%d: send dn %d\n", this_node, send_buf_dn.n));
          send_particles(&send_buf_dn, btm);
        }
        if (LAYERED_BTM_NEIGHBOR) {
          CELL_TRACE(fprintf(stderr, "%d: recv dn\n", this_node));
          recv_particles(&recv_buf_dn, btm);
        }
        if (LAYERED_TOP_NEIGHBOR) {
          CELL_TRACE(fprintf(stderr, "%d: send up\n", this_node));
          send_particles(&send_buf_up, top);
        }
      }
    } else {
      if (recv_buf_up.n != 0 || recv_buf_dn.n != 0 || send_buf_dn.n != 0 ||
          send_buf_up.n != 0) {
        fprintf(stderr,
                "1 node but transfer buffers are not empty. send up "
                "%d, down %d, recv up %d recv dn %d\n",
                send_buf_up.n, send_buf_dn.n, recv_buf_up.n, recv_buf_dn.n);
        errexit();
      }
    }
    layered_append_particles(&recv_buf_up, &send_buf_up, &send_buf_dn);
    layered_append_particles(&recv_buf_dn, &send_buf_up, &send_buf_dn);

    /* handshake redo */
    flag = (send_buf_up.n != 0 || send_buf_dn.n != 0);

    CELL_TRACE(if (flag) fprintf(
        stderr, "%d: requesting another exchange round\n", this_node));

    if (global_flag == CELL_GLOBAL_EXCHANGE) {
      MPI_Allreduce(&flag, &redo, 1, MPI_INT, MPI_MAX, comm_cart);
      if (!redo)
        break;
      CELL_TRACE(fprintf(stderr, "%d: another exchange round\n", this_node));
    } else {
      if (flag) {
        runtimeErrorMsg() << "layered_exchange_and_sort_particles: particle "
                             "moved more than one cell";

        /* sort left over particles into border cells */
        CELL_TRACE(fprintf(stderr, "%d: emergency sort\n", this_node));
        while (send_buf_up.n > 0)
          move_indexed_particle(&cells[1], &send_buf_up, 0);
        while (send_buf_dn.n > 0)
          move_indexed_particle(&cells[n_layers], &send_buf_dn, 0);
      }
      break;
    }
  }

  realloc_particlelist(&recv_buf_up, 0);
  realloc_particlelist(&recv_buf_dn, 0);
}
