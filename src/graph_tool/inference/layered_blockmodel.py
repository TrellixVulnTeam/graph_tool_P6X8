#! /usr/bin/env python
# -*- coding: utf-8 -*-
#
# graph_tool -- a general graph manipulation python module
#
# Copyright (C) 2006-2016 Tiago de Paula Peixoto <tiago@skewed.de>
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <http://www.gnu.org/licenses/>.

from __future__ import division, absolute_import, print_function
import sys
if sys.version_info < (3,):
    range = xrange

from .. import _degree, _prop, Graph, GraphView, libcore, _get_rng, PropertyMap, \
    conv_pickle_state
import random
from numpy import *
import numpy
from collections import defaultdict
from scipy.special import gammaln
import copy

from .. import group_vector_property, ungroup_vector_property, Vector_size_t, \
    perfect_prop_hash

from .. decorators import _wraps

from .. dl_import import dl_import
dl_import("from . import libgraph_tool_inference as libinference")

from .. generation import graph_union
from .. stats import vertex_hist

from . blockmodel import *
from . blockmodel import _bm_test
from . overlap_blockmodel import *

class LayeredBlockState(OverlapBlockState, BlockState):
    r"""The (possibly overlapping) block state of a given graph, where the edges are
    divided into discrete layers.

    This must be instantiated and used by functions such as :func:`mcmc_sweep`.

    Parameters
    ----------
    g : :class:`~graph_tool.Graph`
        Graph to be modelled.
    ec : :class:`~graph_tool.PropertyMap`
        Edge :class:`~graph_tool.PropertyMap` containing edge covariates that
        will split the network in discrete layers.
    eweight : :class:`~graph_tool.PropertyMap` (optional, default: ``None``)
        Edge multiplicities (for multigraphs or block graphs).
    vweight : :class:`~graph_tool.PropertyMap` (optional, default: ``None``)
        Vertex multiplicities (for block graphs).
    b : :class:`~graph_tool.PropertyMap` or :class:`numpy.ndarray` (optional, default: ``None``)
        Initial block labels on the vertices or half-edges. If not supplied, it
        will be randomly sampled.
    B : ``int`` (optional, default: ``None``)
        Number of blocks (or vertex groups). If not supplied it will be obtained
        from the parameter ``b``.
    clabel : :class:`~graph_tool.PropertyMap` (optional, default: ``None``)
        Constraint labels on the vertices. If supplied, vertices with different
        label values will not be clustered in the same group.
    pclabel : :class:`~graph_tool.PropertyMap` (optional, default: ``None``)
        Partition constraint labels on the vertices. This has the same
        interpretation as ``clabel``, but will be used to compute the partition
        description length.
    layers : ``bool`` (optional, default: ``False``)
        If ``layers == True``, the "independent layers" version of the model is
        used, instead of the "edge covariates" version.
    deg_corr : ``bool`` (optional, default: ``True``)
        If ``True``, the degree-corrected version of the blockmodel ensemble will
        be assumed, otherwise the traditional variant will be used.
    overlap : ``bool`` (optional, default: ``False``)
        If ``True``, the overlapping version of the model will be used.
    max_BE : ``int`` (optional, default: ``1000``)
        If the number of blocks exceeds this value, a sparse matrix is used for
        the block graph. Otherwise a dense matrix will be used.
    """

    def __init__(self, g, ec, eweight=None, vweight=None, b=None, B=None,
                 clabel=None, pclabel=False, layers=False, deg_corr=True,
                 overlap=False, **kwargs):
        self.g = g

        if kwargs.get("ec_done", False):
            self.ec = ec
        else:
            self.ec = ec = perfect_prop_hash([ec], "int32_t")[0]

        self.C = ec.fa.max() + 1
        self.layers = layers

        if "max_BE" in kwargs:
            del kwargs["max_BE"]
        max_BE = 0

        if vweight is None:
            vweight = g.new_vp("int", 1)

        if eweight is None:
            eweight = g.new_ep("int", 1)

        if not overlap:
            ldegs = kwargs.get("degs", libinference.simple_degs_t())
            if not isinstance(ldegs, libinference.simple_degs_t):
                tdegs = libinference.get_mapped_block_degs(self.g._Graph__graph,
                                                           ldegs, 0,
                                                           _prop("v", self.g,
                                                                 self.g.vertex_index.copy("int")))
            else:
                tdegs = libinference.simple_degs_t()

            total_state = BlockState(GraphView(g, skip_properties=True), b=b,
                                     B=B, eweight=eweight, vweight=vweight,
                                     clabel=clabel, pclabel=pclabel,
                                     deg_corr=deg_corr, max_BE=max_BE,
                                     degs=tdegs, **dmask(kwargs, ["degs"]))
        else:
            ldegs = None
            total_state = OverlapBlockState(g, b=b, B=B, eweight=eweight,
                                            vweight=vweight, clabel=clabel,
                                            pclabel=pclabel, deg_corr=deg_corr,
                                            max_BE=max_BE, **kwargs)
            self.base_g = total_state.base_g
            self.g = total_state.g

        self.total_state = total_state

        if overlap:
            self.base_ec = ec.copy()
            ec = total_state.eindex.copy()
            pmap(ec, self.ec)
            self.ec = ec.copy("int")

        self.E = total_state.E
        self.N = total_state.N

        if not overlap:
            self.eweight = total_state.eweight
            self.vweight = total_state.vweight

            self.is_weighted = total_state.is_weighted
            self.is_edge_weighted = total_state.is_edge_weighted
            self.is_vertex_weighted = total_state.is_vertex_weighted
        else:
            self.eweight = total_state.g.new_ep("int", 1)
            self.vweight = total_state.g.new_vp("int", 1)
            self.is_weighted = self.is_edge_weighted = self.is_vertex_weighted = False

        self.b = total_state.b
        self.B = total_state.B
        self.clabel = total_state.clabel
        self.bclabel = total_state.bclabel
        self.pclabel = total_state.pclabel

        self.deg_corr = deg_corr
        self.overlap = overlap

        self.vc = self.g.new_vp("vector<int>")
        self.vmap = self.g.new_vp("vector<int>")

        self.gs = []
        self.block_map = libinference.bmap_t()

        lweights = kwargs.get("lweights", g.new_vp("vector<int>"))

        for l in range(0, self.C):
            u = Graph(directed=g.is_directed())
            u.vp["b"] = u.new_vp("int")
            u.vp["weight"] = u.new_vp("int")
            u.ep["weight"] = u.new_ep("int")
            u.vp["brmap"] = u.new_vp("int")
            u.vp["vmap"] = u.new_vp("int")
            self.gs.append(u)

        libinference.split_layers(self.g._Graph__graph,
                                  _prop("e", self.g, self.ec),
                                  _prop("v", self.g, self.b),
                                  _prop("e", self.g, self.eweight),
                                  _prop("v", self.g, self.vweight),
                                  _prop("v", self.g, self.vc),
                                  _prop("v", self.g, self.vmap),
                                  _prop("v", self.g, lweights),
                                  [u._Graph__graph for u in self.gs],
                                  [_prop("v", u, u.vp["b"]) for u in self.gs],
                                  [_prop("e", u, u.ep["weight"]) for u in self.gs],
                                  [_prop("v", u, u.vp["weight"]) for u in self.gs],
                                  self.block_map,
                                  [_prop("v", u, u.vp["brmap"]) for u in self.gs],
                                  [_prop("v", u, u.vp["vmap"]) for u in self.gs])

        if self.g.get_vertex_filter()[0] is not None:
            for u in self.gs:
                u.set_vertex_filter(u.new_vp("bool", True))

        self.master = not self.layers

        if not overlap:
            self.degs = total_state.degs
            self.merge_map = total_state.merge_map

        self.layer_states = []

        self.max_BE = max_BE
        for l, u in enumerate(self.gs):
            state = self.__gen_state(l, u, ldegs)
            self.layer_states.append(state)

        self.bg = total_state.bg
        self.wr = total_state.wr
        self.mrs = total_state.mrs
        self.mrp = total_state.mrp
        self.mrm = total_state.mrm

        self.block_list = Vector_size_t()
        self.block_list.extend(arange(total_state.B, dtype="int"))

        self.__layer_entropy = kwargs.get("layer_entropy", None)

        if not self.overlap:
            self._state = \
                libinference.make_layered_block_state(total_state._state,
                                                      self)
        else:
            self._state = \
                libinference.make_layered_overlap_block_state(total_state._state,
                                                              self)
        del self.total_state._state

        if _bm_test():
            assert self.mrs.fa.sum() == self.eweight.fa.sum(), "inconsistent mrs!"

    def __get_base_u(self, u):
        node_index = u.vp["vmap"].copy("int64_t")
        pmap(node_index, self.total_state.node_index)
        base_u, nindex, vcount, ecount = \
            condensation_graph(u, node_index,
                               self_loops=True,
                               parallel_edges=True)[:4]
        rindex = zeros(nindex.a.max() + 1, dtype="int64")
        reverse_map(nindex, rindex)
        pmap(node_index, rindex)
        base_u.vp["vmap"] = nindex
        return base_u, node_index

    def __gen_state(self, l, u, ldegs):
        B = u.num_vertices() + 1
        if not self.overlap:
            if not isinstance(ldegs, libinference.simple_degs_t):
                degs = libinference.get_mapped_block_degs(u._Graph__graph,
                                                          ldegs, l + 1,
                                                           _prop("v", u,
                                                                 u.vp.vmap))
            else:
                degs = libinference.simple_degs_t()
            state = BlockState(u, b=u.vp["b"],
                               B=B,
                               eweight=u.ep["weight"],
                               vweight=u.vp["weight"],
                               deg_corr=self.deg_corr,
                               force_weighted=self.is_weighted,
                               degs=degs,
                               max_BE=self.max_BE)
        else:
            base_u, node_index = self.__get_base_u(u)
            state = OverlapBlockState(u, b=u.vp["b"].fa,
                                      B=B,
                                      vweight=u.vp["weight"],
                                      node_index=node_index,
                                      base_g=base_u,
                                      deg_corr=self.deg_corr,
                                      max_BE=self.max_BE)
        state.block_rmap = u.vp["brmap"]
        state.vmap = u.vp["vmap"]
        state.free_blocks = Vector_size_t()
        return state

    def __getstate__(self):
        state = dict(g=self.g,
                     ec=self.ec,
                     layers=self.layers,
                     eweight=self.eweight,
                     vweight=self.vweight,
                     b=self.b,
                     B=self.B,
                     clabel=self.clabel,
                     deg_corr=self.deg_corr)
        return state

    def __setstate__(self, state):
        conv_pickle_state(state)
        self.__init__(**state)
        return state

    def __copy__(self):
        return self.copy()

    def __deepcopy__(self, memo):
        g = copy.deepcopy(self.g, memo)
        ec = g.own_property(copy.deepcopy(self.ec, memo))
        return self.copy(g=g, ec=ec)

    def copy(self, g=None, eweight=None, vweight=None, b=None, B=None,
             deg_corr=None, clabel=None, pclabel=None, overlap=None,
             layers=None, ec=None, **kwargs):
        r"""Copies the block state. The parameters override the state properties, and
         have the same meaning as in the constructor."""
        lweights = self.g.new_vp("vector<int>")
        degs = None
        if not self.overlap:
            libinference.get_lweights(self.g._Graph__graph,
                                      _prop("v", self.g, self.vc),
                                      _prop("v", self.g, self.vmap),
                                      _prop("v", self.g, lweights),
                                      [_prop("v", state.g, state.vweight)
                                       for state in self.layer_states])
            if not isinstance(self.total_state.degs, libinference.simple_degs_t):
                degs = libinference.get_ldegs(self.g._Graph__graph,
                                              _prop("v", self.g, self.vc),
                                              _prop("v", self.g, self.vmap),
                                              [self.total_state.degs] +
                                              [state.degs for state
                                               in self.layer_states])
            else:
                degs = libinference.simple_degs_t()

        state = LayeredBlockState(self.g if g is None else g,
                                  ec=self.ec if ec is None else ec,
                                  eweight=self.eweight if eweight is None else eweight,
                                  vweight=self.vweight if vweight is None else vweight,
                                  b=self.b if b is None else b,
                                  B=(self.B if b is None else None) if B is None else B,
                                  clabel=self.clabel.fa if clabel is None else clabel,
                                  pclabel=self.pclabel if pclabel is None else pclabel,
                                  deg_corr=self.deg_corr if deg_corr is None else deg_corr,
                                  overlap=self.overlap if overlap is None else overlap,
                                  layers=self.layers if layers is None else layers,
                                  base_g=self.base_g if self.overlap else None,
                                  half_edges=self.total_state.half_edges if self.overlap else None,
                                  node_index=self.total_state.node_index if self.overlap else None,
                                  eindex=self.total_state.eindex if self.overlap else None,
                                  ec_done=ec is None,
                                  degs=degs, lweights=lweights,
                                  layer_entropy=self.__get_layer_entropy())
        return state

    def __repr__(self):
        return "<LayeredBlockState object with %d %sblocks, %d %s,%s for graph %s, at 0x%x>" % \
            (self.B, "overlapping " if self.overlap else "",
             self.C, "layers" if self.layers else "edge covariates",
             " degree corrected," if self.deg_corr else "",
             str(self.base_g if self.overlap else self.g), id(self))

    def get_bg(self):
        r"""Returns the block graph."""

        bg = Graph(directed=self.g.is_directed())
        mrs = bg.new_edge_property("int")
        ec = bg.new_edge_property("int")

        for l in range(self.C):
            u = GraphView(self.g, efilt=self.ec.a == l)
            ug = get_block_graph(u, self.B, self.b, self.vweight, self.eweight)
            uec = ug.new_edge_property("int")
            uec.a = l
            bg, props = graph_union(bg, ug,
                                    props=[(mrs, ug.ep["count"]),
                                           (ec, uec)],
                                    intersection=ug.vertex_index,
                                    include=True)
            mrs = props[0]
            ec = props[1]

        return bg, mrs, ec

    def get_block_state(self, b=None, vweight=False, deg_corr=False,
                        overlap=False, layers=None, **kwargs):
        r"""Returns a :class:`~graph_tool.inference.LayeredBlockState`` corresponding
        to the block graph. The parameters have the same meaning as the in the
        constructor."""

        bg, mrs, ec = self.get_bg()
        lweights = bg.new_vp("vector<int>")
        if not overlap and deg_corr and vweight:
            degs = libinference.get_layered_block_degs(self.g._Graph__graph,
                                                       _prop("e", self.g,
                                                             self.eweight),
                                                       _prop("v", self.g,
                                                             self.vweight),
                                                       _prop("e", self.g,
                                                             self.ec),
                                                       _prop("v", self.g,
                                                             self.b))
            libinference.get_blweights(self.g._Graph__graph,
                                       _prop("v", self.g, self.b),
                                       _prop("v", self.g, self.vc),
                                       _prop("v", self.g, self.vmap),
                                       _prop("v", bg, lweights),
                                       [_prop("v", state.g, state.vweight)
                                        for state in self.layer_states])
        else:
            degs = libinference.simple_degs_t()

        if vweight:
            layer_entropy = self.__get_layer_entropy()
        else:
            layer_entropy = None

        state = LayeredBlockState(bg, ec, eweight=mrs,
                                  vweight=bg.own_property(self.wr.copy()) if vweight else None,
                                  b=bg.vertex_index.copy("int") if b is None else b,
                                  deg_corr=deg_corr,
                                  overlap=overlap,
                                  max_BE=self.max_BE,
                                  layers=self.layers if layers is None else layers,
                                  ec_done=True,
                                  degs=degs, lweights=lweights,
                                  layer_entropy=layer_entropy, **kwargs)
        return state


    def get_edge_blocks(self):
        r"""Returns an edge property map which contains the block labels pairs for each
        edge."""
        if not self.overlap:
            raise ValueError("edge blocks only available if overlap == True")
        return self.total_state.get_edge_blocks()

    def get_overlap_blocks(self):
        r"""Returns the mixed membership of each vertex.

        Returns
        -------
        bv : :class:`~graph_tool.PropertyMap`
           A vector-valued vertex property map containing the block memberships
           of each node.
        bc_in : :class:`~graph_tool.PropertyMap`
           The labelled in-degrees of each node, i.e. how many in-edges belong
           to each group, in the same order as the ``bv`` property above.
        bc_out : :class:`~graph_tool.PropertyMap`
           The labelled out-degrees of each node, i.e. how many out-edges belong
           to each group, in the same order as the ``bv`` property above.
        bc_total : :class:`~graph_tool.PropertyMap`
           The labelled total degrees of each node, i.e. how many incident edges
           belong to each group, in the same order as the ``bv`` property above.

        """
        if not self.overlap:
            raise ValueError("overlap blocks only available if overlap == True")
        return self.total_state.get_overlap_blocks()

    def get_nonoverlap_blocks(self):
        r"""Returns a scalar-valued vertex property map with the block mixture
        represented as a single number."""

        if not self.overlap:
            return self.b.copy()
        else:
            return self.total_state.get_nonoverlap_blocks()

    def get_majority_blocks(self):
        r"""Returns a scalar-valued vertex property map with the majority block
        membership of each node."""

        if not self.overlap:
            return self.b.copy()
        else:
            return self.total_state.get_majority_blocks()

    def __get_layer_entropy(self):
        if self.__layer_entropy is None:
            if self.layers:
                # we need to include the membership of the nodes in each layer
                g = self.base_g if self.overlap else self.g
                ec = self.base_ec if self.overlap else self.ec
                be = group_vector_property([ec, ec])
                lstate = OverlapBlockState(g, b=be, deg_corr=False)
                self.__layer_entropy = lstate.entropy(dl=True, edges_dl=False) - \
                                       lstate.entropy(dl=False)
            else:
                self.__layer_entropy = 0
        return self.__layer_entropy

    def entropy(self, dl=True, partition_dl=True, degree_dl=True, edges_dl=True,
                dense=False, multigraph=True, dl_ent=False, deg_entropy=True,
                **kwargs):
        r"""Calculate the entropy associated with the current block partition. The
        meaning of the parameters are the same as in
        :meth:`graph_tool.inference.BlockState.entropy`.
        """

        if _bm_test() and kwargs.get("test", True):
            args = dict(**locals())
            args.update(**kwargs)
            del args["self"]
            del args["kwargs"]

        S = BlockState.entropy(self, dl=dl, partition_dl=partition_dl,
                               degree_dl=degree_dl, edges_dl=False,
                               dense=dense, multigraph=multigraph,
                               dl_ent=dl_ent, deg_entropy=deg_entropy,
                               **overlay(kwargs, test=False))

        if dl and edges_dl:
            actual_B = (self.wr.a > 0).sum()
            for state in self.layer_states:
                S += model_entropy(actual_B, 0, state.E,
                                   directed=self.g.is_directed(), nr=False)

        if dl:
            S += self.__get_layer_entropy()

        if _bm_test() and kwargs.get("test", True):
            assert not isnan(S) and not isinf(S), \
                "invalid entropy %g (%s) " % (S, str(args))

            Salt = self.copy().entropy(test=False, **args)
            assert abs(S - Salt) < 1e-6, \
                "entropy discrepancy after copying (%g %g)" % (S, Salt)

        return S

    def _mcmc_sweep_dispatch(self, mcmc_state):
        if not self.overlap:
            return libinference.mcmc_layered_sweep(mcmc_state, self._state,
                                                   _get_rng())
        else:
            dS, nmoves = libinference.mcmc_layered_overlap_sweep(mcmc_state,
                                                                 self._state,
                                                                 _get_rng())
            if self.__bundled:
                ret = libinference.mcmc_layered_overlap_bundled_sweep(mcmc_state,
                                                                      self._state,
                                                                      _get_rng())
                dS += ret[0]
                nmoves += ret[1]
            return dS, nmoves

    def mcmc_sweep(self, bundled=False, **kwargs):
        r"""Perform sweeps of a Metropolis-Hastings rejection sampling MCMC to sample
        network partitions. If ``bundled == True`` and the state is an
        overlapping one, the half-edges incident of the same node that belong to
        the same group are moved together. All remaining parameters are passed
        to :meth:`graph_tool.inference.BlockState.mcmc_sweep`."""

        self.__bundled = bundled
        return BlockState.mcmc_sweep(self, **kwargs)

    def _gibbs_sweep_dispatch(self, mcmc_state):
        if not self.overlap:
            return libinference.gibbs_layered_sweep(mcmc_state, self._state,
                                                    _get_rng())
        else:
            return libinference.gibbs_layered_overlap_sweep(mcmc_state,
                                                            self._state,
                                                            _get_rng())

    def _multicanonical_sweep_dispatch(self, mcmc_state):
        if not self.overlap:
            return libinference.multicanonical_layered_sweep(mcmc_state,
                                                             self._state,
                                                             _get_rng())
        else:
            return libinference.multicanonical_layered_overlap_sweep(mcmc_state,
                                                                     self._state,
                                                                     _get_rng())

    def _merge_sweep_dispatch(self, merge_state):
        if not self.overlap:
            return libinference.merge_layered_sweep(merge_state, self._state,
                                                    _get_rng())
        else:
            return libinference.vacate_layered_overlap_sweep(merge_state,
                                                             self._state,
                                                             _get_rng())

    def shrink(self, B, **kwargs):
        """Reduces the order of current state by progressively merging groups, until
        only ``B`` are left. All remaining keyword arguments are passed to
        :meth:`graph_tool.inference.BlockState.shrink` or
        :meth:`graph_tool.inference.OverlapBlockState.shrink`, as appropriate.

        This function leaves the current state untouched and returns instead a
        copy with the new partition.
        """

        if not self.overlap:
            return BlockState.shrink(self, B, **kwargs)
        else:
            return OverlapBlockState.shrink(self, B, **kwargs)

    def draw(self, **kwargs):
        """Convenience function to draw the current state. All keyword arguments are
        passed to :meth:`graph_tool.inference.BlockState.draw` or
        :meth:`graph_tool.inference.OverlapBlockState.draw`, as appropriate.
        """

        self.total_state.draw(**kwargs)


def init_layer_confined(g, ec):
    tmp_state = CovariateBlockState(g, ec=ec, B=g.num_vertices())
    tmp_state = tmp_state.copy(overlap=True)
    be = tmp_state.get_edge_blocks()
    ba = ungroup_vector_property(be, [0])[0]
    ba.fa = ba.fa + ec.fa * (ba.fa.max() + 1)
    continuous_map(ba)
    be = group_vector_property([ba, ba])
    return be